// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbManager
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import java.io.File
import java.net.URLClassLoader
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import org.json.JSONObject

// The production UsbSourceManager against a scripted bus: the test plugs, unplugs, grants and answers prompts, and
// reads back what the host's poll would. The manager is a process-wide object, so each case runs in a class loader
// of its own (see main) and starts from a fresh manager, tracker, clock and engine.

private const val RTL_A = "/dev/bus/usb/001/002"
private const val RTL_B = "/dev/bus/usb/001/003"
private const val AIRSPY_A = "/dev/bus/usb/001/004"
private const val AIRSPY_B = "/dev/bus/usb/001/005"
private const val SERIAL = "26A464DC"

private fun rtl(name: String = RTL_A) = UsbDevice(name, 0x0bda, 0x2838, "00000001", "RTL2838UHIDIR")

private fun airspy(name: String = AIRSPY_A, serial: String = SERIAL) = UsbDevice(name, 0x1d50, 0x60a1, serial, "AIRSPY")

// One per case: the manager registers its receiver once, on the first context it sees.
private class Rig {
    val context = Context()
    val usb: UsbManager get() = context.usb

    init {
        // This thread is the main thread: broadcasts and the service's start run here, and the Qt thread's requests
        // wait in Handler.pending until drain().
        Looper.mainThread = Thread.currentThread()
        UsbSourceManager.initializeAttachments(context)
    }

    fun plug(device: UsbDevice) {
        usb.plug(device)
        context.broadcast(Intent(UsbManager.ACTION_USB_DEVICE_ATTACHED).putExtra(UsbManager.EXTRA_DEVICE, device))
    }

    fun unplug(device: UsbDevice) {
        usb.unplug(device.deviceName)
        context.broadcast(Intent(UsbManager.ACTION_USB_DEVICE_DETACHED).putExtra(UsbManager.EXTRA_DEVICE, device))
    }

    fun prompt(index: Int): Pair<UsbDevice, PendingIntent> {
        check(index < usb.prompts.size) { "expected prompt $index, saw ${usb.prompts.size}" }
        return usb.prompts[index]
    }

    /** The framework's reply to prompt [index]: its intent, with the device and the answer added. */
    fun answer(index: Int, granted: Boolean) {
        val (device, intent) = prompt(index)
        if (granted) usb.granted += device.deviceName
        context.broadcast(
            intent.intent.copy().putExtra(UsbManager.EXTRA_DEVICE, device).putExtra(UsbManager.EXTRA_PERMISSION_GRANTED, granted)
        )
    }

    fun connect(device: UsbDevice) {
        plug(device)
        usb.granted += device.deviceName
        UsbSourceManager.requestAccessForSource(context, UsbReceiverKind.sourceOf(device.vendorId, device.productId), "")
        check(record().optBoolean("ready")) { "the receiver connects: ${record()}" }
    }

    /** The service's take of [source] for a new session, as DecoderService.startDecoding() makes it. */
    fun take(source: String = "usb") = UsbSourceManager.takeForSession(context, source)

    /** Runs what other threads posted to the main thread. */
    fun drain() = Handler.drain()

    fun record() = JSONObject(UsbSourceManager.deviceStatus())
    fun text() = record().optString("text")
    fun kind() = record().optString("kind")
}

// #647 B9: one PendingIntent identity per request, its generation carried back; only its own answer counts.
private fun eachPromptHasItsOwnIdentity() {
    val rig = Rig()
    rig.plug(rtl())
    UsbSourceManager.requestAccess(rig.context)
    val (_, first) = rig.prompt(0)
    check(first.requestCode == 1 && first.intent.getIntExtra("io.github.arancormonk.dsdneo.extra.USB_REQUEST_GENERATION", 0) == 1) {
        "the first prompt is generation 1, as request code and extra"
    }
    check(first.flags and PendingIntent.FLAG_UPDATE_CURRENT == 0) { "no prompt rewrites another's extras" }
    check(first.flags and PendingIntent.FLAG_CANCEL_CURRENT != 0 && first.flags and PendingIntent.FLAG_MUTABLE != 0)
    check(rig.record().optBoolean("pending") && rig.record().optString("requestedSource") == "usb")
    UsbSourceManager.requestAccess(rig.context)
    check(rig.usb.prompts.size == 1) { "a prompt inside its wait is not repeated" }
    // A reply that names another generation (a replay, or an old dialog) is not this request's answer.
    val forged = first.intent.copy().putExtra("io.github.arancormonk.dsdneo.extra.USB_REQUEST_GENERATION", 7)
    rig.usb.granted += RTL_A
    rig.context.broadcast(forged.putExtra(UsbManager.EXTRA_DEVICE, rtl()).putExtra(UsbManager.EXTRA_PERMISSION_GRANTED, true))
    check(!rig.record().optBoolean("ready") && rig.record().optBoolean("pending")) { "another generation's reply is ignored" }
    rig.answer(0, granted = true)
    check(rig.record().optBoolean("ready") && !rig.record().optBoolean("pending")) { "the prompt's own answer opens it" }
}

// #647 B9: no answer for 60 s retires the prompt on the poll alone; a late reply then changes nothing.
private fun anUnansweredPromptExpiresOnThePoll() {
    val rig = Rig()
    rig.plug(rtl())
    UsbSourceManager.requestAccess(rig.context)
    SystemClock.nowMs += UsbRequestState.TIMEOUT_MS - 1
    check(rig.record().optBoolean("pending"))
    SystemClock.nowMs += 1
    val record = rig.record()
    check(!record.optBoolean("pending") && record.optString("text") == "No answer to the permission request") { "$record" }
    check(record.optString("kind") == "") { "an expiry is no device failure" }
    check(rig.usb.prompts.size == 1) { "expiry never prompts again" }
    rig.answer(0, granted = true)
    check(!rig.record().optBoolean("ready")) { "a reply after expiry is ignored" }
    UsbSourceManager.requestAccess(rig.context)
    check(rig.usb.prompts.size == 1) { "the device is now permitted, so Connect opens it without a prompt" }
    check(rig.record().optBoolean("ready"))
}

// #647 B9: unplugging the requested device before it opens retires its prompt and says so; a re-plug prompts at once.
private fun aDetachRetiresThePromptAndAReplugAsksAgain() {
    val rig = Rig()
    val first = rtl(RTL_A)
    rig.plug(first)
    UsbSourceManager.requestAccess(rig.context)
    rig.unplug(first)
    check(!rig.record().optBoolean("pending")) { "the detach retires the prompt" }
    check(rig.text() == "Device detached: RTL2838UHIDIR" && rig.kind() == "detached") { "the detach says so: ${rig.record()}" }
    check(DsdNative.stopCauses.isEmpty()) { "an unopened device's detach stops nothing" }
    rig.plug(rtl(RTL_B))
    UsbSourceManager.requestAccess(rig.context)
    check(rig.usb.prompts.size == 2) { "Connect prompts for the re-plugged device at once" }
    check(rig.prompt(1).second.requestCode == 2) { "as a new request" }
    rig.answer(0, granted = true)
    check(!rig.record().optBoolean("ready")) { "the old prompt's late answer is ignored" }
    rig.answer(1, granted = true)
    check(rig.record().optBoolean("ready") && rig.text() == "Ready: RTL2838UHIDIR")
}

// #647: a granted Airspy with another serial moves the request on to the next Airspy, as the same request.
private fun theSerialSearchHopsWithinItsRequest() {
    val rig = Rig()
    rig.plug(airspy(AIRSPY_A, "0000000000000001"))
    rig.plug(airspy(AIRSPY_B, SERIAL))
    UsbSourceManager.requestAccessForSource(rig.context, "airspy", SERIAL)
    check(rig.prompt(0).first.deviceName == AIRSPY_A) { "the first unpermitted Airspy is asked first" }
    rig.answer(0, granted = true)
    check(rig.usb.prompts.size == 2 && rig.prompt(1).first.deviceName == AIRSPY_B) { "the search moves to the next Airspy" }
    check(rig.prompt(1).second.requestCode == rig.prompt(0).second.requestCode) { "the hop keeps the request's generation" }
    check(rig.record().optBoolean("pending") && !rig.record().optBoolean("ready"))
    rig.answer(0, granted = true)
    check(!rig.record().optBoolean("ready") && rig.usb.prompts.size == 2) { "a repeated answer for the first Airspy is ignored" }
    rig.answer(1, granted = true)
    val record = rig.record()
    check(record.optBoolean("ready") && record.optString("serial") == SERIAL) { "the requested Airspy connects: $record" }
    check(DsdNative.airspyFd >= 0)
}

// #647 B2: an attach replaces the diagnostic only for the receiver the request waits for, never a live connection's.
private fun anAttachReportsOnlyWhatTheRequestWaitsFor() {
    val rig = Rig()
    rig.plug(rtl(RTL_A))
    check(rig.text() == "RTL2838UHIDIR attached" && rig.kind() == "") { "with no request, any receiver reports: ${rig.record()}" }
    UsbSourceManager.requestAccessForSource(rig.context, "airspy", SERIAL)
    check(rig.text() == "Requested Airspy serial not found" && rig.kind() == "detached")
    rig.plug(rtl(RTL_B))
    check(rig.text() == "Requested Airspy serial not found" && rig.kind() == "detached") { "an RTL attach keeps the Airspy diagnostic" }
    rig.plug(airspy(AIRSPY_A, SERIAL))
    check(rig.text() == "Requested Airspy serial not found") { "an Airspy whose serial cannot be read yet keeps it too" }
    // The edges are still recorded for auto-start, whatever the diagnostic says.
    check(UsbSourceManager.takeAttachment() == "airspy:$AIRSPY_A") { "the newest attachment, with its kind" }
}

private fun anAttachClearsTheFailureItAnswers() {
    val rig = Rig()
    UsbSourceManager.requestAccessForSource(rig.context, "usb", "")
    check(rig.text() == "No RTL-SDR attached" && rig.kind() == "detached")
    rig.plug(rtl())
    check(rig.text() == "RTL2838UHIDIR attached" && rig.kind() == "") { "the awaited receiver replaces it, kind and all" }
}

private fun anAttachNeverOverwritesALiveConnection() {
    val rig = Rig()
    rig.connect(rtl(RTL_A))
    rig.plug(rtl(RTL_B))
    check(rig.text() == "Ready: RTL2838UHIDIR" && rig.record().optBoolean("ready")) { "a second dongle leaves the connection's text" }
    check(UsbSourceManager.takeAttachment() == "usb:$RTL_B")
}

// #647 B10: a plain Connect asks for the request already made, its serial included, before the bus.
private fun aPlainConnectFollowsTheRequest() {
    val rig = Rig()
    rig.plug(airspy())
    UsbSourceManager.requestAccess(rig.context)
    check(rig.prompt(0).first.deviceName == AIRSPY_A) { "a fresh install with only an Airspy asks for the Airspy" }
    check(rig.record().optString("requestedSource") == "airspy")
}

private fun aPlainConnectKeepsTheRequestedSerial() {
    val rig = Rig()
    rig.plug(rtl())
    rig.usb.granted += RTL_A
    UsbSourceManager.requestAccessForSource(rig.context, "airspy", SERIAL)
    UsbSourceManager.requestAccess(rig.context)
    check(!rig.record().optBoolean("ready") && rig.usb.prompts.isEmpty()) { "Connect never opens the RTL-SDR instead" }
    check(rig.text() == "Requested Airspy serial not found" && rig.record().optString("requestedSerial") == SERIAL)
}

private fun nothingIsRequestedBeforeTheFirstRequest() {
    val rig = Rig()
    rig.plug(rtl())
    val record = rig.record()
    check(record.optString("requestedSource") == "" && record.optString("requestedSerial") == "" && !record.optBoolean("pending")) {
        "an attach alone requests nothing: $record"
    }
}

// #647 B4, B11: a refused switch is feedback, and the next request that finds the receiver ready rewrites Ready.
private fun aRefusedSwitchIsFeedbackAndReadyIsRewritten() {
    val rig = Rig()
    rig.connect(rtl())
    DsdNative.running = true
    UsbSourceManager.requestAccessForSource(rig.context, "airspy", "")
    check(rig.text() == "Stop the current receiver before switching USB devices" && rig.kind() == "refused") { "${rig.record()}" }
    check(rig.record().optBoolean("ready") && rig.record().optString("source") == "usb") { "the connection stays" }
    UsbSourceManager.requestAccessForSource(rig.context, "usb", "")
    check(rig.text() == "Ready: RTL2838UHIDIR" && rig.kind() == "") { "the ready request rewrites the record: ${rig.record()}" }
    check(rig.record().optString("requestedSource") == "usb")
}

// #647 B8: a detach of the held connection stops the engine only for a session that uses it, and always releases.
// "session" is also the order a detach during a start takes: configure and the detach broadcast both run on the main
// thread, so the detach is handled once configure returned and the native record names the session's receiver.
private fun aDetachStopsOnlyTheSessionThatUsesTheReceiver(owner: String) {
    val rig = Rig()
    val device = rtl()
    rig.connect(device)
    DsdNative.sessionUsesLocalReceiver = owner == "session"
    DsdNative.fdInUse = owner == "descriptor"
    rig.unplug(device)
    DsdNative.fdInUse = false
    DsdNative.sessionUsesLocalReceiver = false
    val stops = if (owner == "none") emptyList() else listOf("detached")
    check(DsdNative.stopCauses == stops) { "$owner: stops ${DsdNative.stopCauses}" }
    check(!rig.record().optBoolean("ready") && DsdNative.usbFd == -1) { "$owner: the connection is released either way" }
    check(rig.text() == "Device detached" && rig.kind() == "detached")
    awaitClosed(rig.usb.opened.single(), "$owner: the connection closes once nothing uses it")
}

private fun awaitClosed(connection: UsbDeviceConnection, what: String) {
    val deadline = System.nanoTime() + 2_000_000_000L
    while (!connection.closed && System.nanoTime() < deadline) Thread.sleep(10)
    check(connection.closed) { what }
}

// #647: a retained receiver unplugged during an rtl_tcp, UDP or file session is closed at once, not held until that
// session ends (or leaked after the release timeout while it runs on).
private fun aDetachDuringAnotherSessionClosesTheConnection() {
    val rig = Rig()
    val device = rtl()
    rig.connect(device)
    DsdNative.running = true
    rig.unplug(device)
    check(DsdNative.stopCauses.isEmpty()) { "the other session keeps running" }
    awaitClosed(rig.usb.opened.single(), "the connection closes while the other session runs")
}

// #647: a session configured for the local receiver is stopped by the detach, and its connection stays open while the
// engine holds the descriptor. #782: only the descriptor itself counts. The detach cleared the slot first, so a session
// that has not taken it yet never will, and its connection closes at once.
private fun aConfiguredSessionKeepsTheConnectionUntilItEnds() {
    val rig = Rig()
    val device = rtl()
    rig.connect(device)
    DsdNative.sessionUsesLocalReceiver = true
    val connection = rig.usb.opened.single()
    DsdNative.held = intArrayOf(connection.fileDescriptor, -1)
    rig.unplug(device)
    check(DsdNative.stopCauses == listOf("detached"))
    Thread.sleep(300)
    check(!connection.closed) { "the connection stays open while the session's engine holds its descriptor" }
    DsdNative.held = intArrayOf(-1, -1)
    awaitClosed(connection, "the connection closes once the engine let go of it")

    // Configured, but the descriptor not taken: nothing can take it now, so the close does not wait for the session.
    val second = rtl(RTL_B)
    rig.connect(second)
    DsdNative.stopCauses.clear()
    rig.unplug(second)
    check(DsdNative.stopCauses == listOf("detached")) { "the configured session is stopped" }
    awaitClosed(rig.usb.opened.last(), "a descriptor nothing holds closes while the session ends")
}

private fun anUnrelatedDetachChangesNothing() {
    val rig = Rig()
    rig.connect(rtl(RTL_A))
    DsdNative.sessionUsesLocalReceiver = true
    val other = rtl(RTL_B)
    rig.plug(other)
    rig.unplug(other)
    check(DsdNative.stopCauses.isEmpty() && rig.record().optBoolean("ready")) { "another dongle's detach leaves the session alone" }
}


private fun await(latch: CountDownLatch, what: String) {
    check(latch.await(2, TimeUnit.SECONDS)) { what }
}

/** Runs [work] on a thread of its own, as the Qt thread calls the broker; returns it started. */
private fun onAnotherThread(work: () -> Unit): Thread = Thread(work, "qt").also { it.start() }

// #782 WP7: each session after the first gets a connection of its own, reopened without a prompt while permission
// holds; the retired one is closed and the new descriptor published before configure.
private fun aSecondSessionGetsAFreshDescriptor() {
    val rig = Rig()
    rig.connect(rtl())
    val first = rig.usb.opened.single()
    val taken = rig.take()
    check(taken.failureKind == "" && taken.device == RTL_A && taken.connection == 1L && taken.epoch == 1L) {
        "the first session takes the connection opened for it: $taken"
    }
    check(rig.usb.opened.size == 1 && DsdNative.usbFd == first.fileDescriptor) { "and opens nothing new" }
    val again = rig.take()
    check(again.failureKind == "" && again.connection == 2L && again.device == RTL_A && again.epoch == 1L) {
        "the second session takes a new connection to the same attachment: $again"
    }
    val second = rig.usb.opened.last()
    check(rig.usb.opened.size == 2 && first.closed && !second.closed) { "the retired connection is closed" }
    check(DsdNative.usbFd == second.fileDescriptor && second.fileDescriptor != first.fileDescriptor) {
        "the new descriptor is published: ${DsdNative.usbFd}"
    }
    check(!DsdNative.resetFirst) { "no reset is asked of a descriptor that owes none" }
    check(rig.usb.prompts.isEmpty()) { "no prompt while permission holds" }
    val record = rig.record()
    check(record.optBoolean("ready") && record.optLong("connection") == 2L && record.optString("device") == RTL_A &&
        record.optLong("epoch") == 1L) { "the broker publishes the new connection: $record" }
    check(rig.take().connection == 3L) { "and every later session reopens again" }
}

// #782: the device is no longer listed when the start reopens it (its detach broadcast is still queued behind the
// start on the main thread). The start fails as usb_gone, and the broker reads not ready.
private fun aDeviceGoneMidSwapFailsTheStart() {
    val rig = Rig()
    val device = rtl()
    rig.connect(device)
    rig.take()
    val old = rig.usb.opened.single()
    rig.usb.unplug(RTL_A)
    val taken = rig.take()
    check(taken.failureKind == UsbSessionTake.USB_GONE) { "the start fails as gone: $taken" }
    check(old.closed && DsdNative.usbFd == -1) { "the retired connection is closed and its slot cleared" }
    check(!rig.record().optBoolean("ready") && rig.kind() == "detached") { "the broker reads not ready: ${rig.record()}" }
    // The queued broadcast then finds no connection and no session to stop.
    rig.context.broadcast(Intent(UsbManager.ACTION_USB_DEVICE_DETACHED).putExtra(UsbManager.EXTRA_DEVICE, device))
    check(DsdNative.stopCauses.isEmpty()) { "nothing is stopped" }
    // A detach that was handled before the start leaves nothing to take: the start goes on, as before.
    check(rig.take().failureKind == "" && rig.take().connection == 0L) { "no connection, nothing to take" }
}

// #782: permission was revoked since the last session. The start fails as usb_permission without prompting; Connect
// then takes the normal request path, which prompts.
private fun aRevokedPermissionFailsTheStart() {
    val rig = Rig()
    rig.connect(rtl())
    rig.take()
    rig.usb.granted.remove(RTL_A)
    val taken = rig.take()
    check(taken.failureKind == UsbSessionTake.USB_PERMISSION) { "the start fails for permission: $taken" }
    check(rig.usb.opened.size == 1 && rig.usb.opened.single().closed) { "nothing is reopened; the old one is closed" }
    check(!rig.record().optBoolean("ready") && rig.kind() == "permission") { "${rig.record()}" }
    check(rig.usb.prompts.isEmpty()) { "the start itself never prompts" }
    UsbSourceManager.requestAccess(rig.context)
    check(rig.usb.prompts.size == 1 && rig.prompt(0).first.deviceName == RTL_A) { "Retry asks for permission again" }
}

// #782: Android cannot open the device again. usb_open_failed, and the broker reads not ready.
private fun aFailedReopenFailsTheStart() {
    val rig = Rig()
    rig.connect(rtl())
    rig.take()
    rig.usb.refuseOpen = true
    val taken = rig.take()
    check(taken.failureKind == UsbSessionTake.USB_OPEN_FAILED) { "$taken" }
    check(!rig.record().optBoolean("ready") && rig.kind() == "open_failed" && DsdNative.usbFd == -1) { "${rig.record()}" }
    rig.usb.refuseOpen = false
    UsbSourceManager.requestAccess(rig.context)
    check(rig.record().optBoolean("ready")) { "Retry opens it again on the normal path" }
}

// #782: the previous session still owns a receiver: the start is refused as still stopping, and nothing is touched.
private fun aStartWhileThePreviousSessionHoldsTheReceiverIsRefused(owner: String) {
    val rig = Rig()
    rig.connect(rtl())
    rig.take()
    DsdNative.sessionUsesLocalReceiver = owner == "session"
    DsdNative.fdInUse = owner == "descriptor"
    val taken = rig.take()
    check(taken.failureKind == UsbSessionTake.PREVIOUS_STOPPING) { "$owner: $taken" }
    check(rig.usb.opened.size == 1 && !rig.usb.opened.single().closed && rig.record().optBoolean("ready")) {
        "$owner: the connection is left alone"
    }
}

// #782: the Airspy gets the same rule through its own slot.
private fun anAirspySessionGetsAFreshDescriptor() {
    val rig = Rig()
    rig.connect(airspy())
    rig.take("airspy")
    val first = rig.usb.opened.single()
    val again = rig.take("airspy")
    check(again.failureKind == "" && first.closed && DsdNative.airspyFd == rig.usb.opened.last().fileDescriptor) {
        "the Airspy is reopened: $again, fd ${DsdNative.airspyFd}"
    }
    check(DsdNative.usbFd == -1) { "the RTL-SDR slot is not touched" }
    check(rig.take("usb").connection == 0L) { "an RTL-SDR start takes nothing from an Airspy connection" }
}

// #782: a serial-specific request keeps its serial through the reopen, and a different device under the same name is
// not taken for it.
private fun aSerialRequestKeepsItsSerial() {
    val rig = Rig()
    rig.plug(airspy(AIRSPY_A, "0000000000000001"))
    rig.plug(airspy(AIRSPY_B, SERIAL))
    rig.usb.granted += AIRSPY_A
    rig.usb.granted += AIRSPY_B
    UsbSourceManager.requestAccessForSource(rig.context, "airspy", SERIAL)
    rig.take("airspy")
    check(rig.take("airspy").device == AIRSPY_B) { "the requested Airspy is reopened" }
    val record = rig.record()
    check(record.optString("serial") == SERIAL && record.optString("requestedSerial") == SERIAL) {
        "the serial stays the request's: $record"
    }
    // The name now belongs to another Airspy: not the one the request asked for.
    rig.usb.unplug(AIRSPY_B)
    rig.usb.plug(airspy(AIRSPY_B, "0000000000000002"))
    rig.usb.granted += AIRSPY_B
    check(rig.take("airspy").failureKind == UsbSessionTake.USB_GONE) { "another serial under the name is not taken" }
}

// #782: a session configured for the receiver, not yet running, owns it: a switch is refused as during a run.
private fun aConfiguredSessionRefusesASwitch(owner: String) {
    val rig = Rig()
    rig.plug(airspy())
    rig.usb.granted += AIRSPY_A
    rig.connect(rtl())
    DsdNative.running = false
    DsdNative.sessionUsesLocalReceiver = owner == "session"
    DsdNative.fdInUse = owner == "descriptor"
    UsbSourceManager.requestAccessForSource(rig.context, "airspy", "")
    check(rig.kind() == "refused" && rig.record().optString("source") == "usb") { "$owner: ${rig.record()}" }
    check(rig.usb.opened.size == 1 && !rig.usb.opened.single().closed) { "$owner: the connection stays" }
}

// #782 WP4: the Qt thread's requests are run by the broker's owner, in order; switches back and forth leave one open
// connection, the last one asked for, and close the others.
private fun concurrentSwitchesAreSerialized() {
    val rig = Rig()
    rig.plug(rtl())
    rig.plug(airspy())
    rig.usb.granted += RTL_A
    rig.usb.granted += AIRSPY_A
    val qt = onAnotherThread {
        UsbSourceManager.requestAccessForSource(rig.context, "usb", "")
        UsbSourceManager.requestAccessForSource(rig.context, "airspy", "")
        UsbSourceManager.requestAccessForSource(rig.context, "usb", "")
    }
    qt.join(2000)
    check(rig.usb.opened.isEmpty() && !rig.record().optBoolean("ready")) { "nothing changes off the main thread" }
    rig.drain()
    val record = rig.record()
    check(record.optBoolean("ready") && record.optString("source") == "usb") { "the last request wins: $record" }
    check(rig.usb.opened.size == 3) { "each switch opened its receiver once: ${rig.usb.opened.size}" }
    awaitClosed(rig.usb.opened[0], "the first RTL-SDR connection closes")
    awaitClosed(rig.usb.opened[1], "the Airspy connection closes")
    check(!rig.usb.opened[2].closed && DsdNative.usbFd == rig.usb.opened[2].fileDescriptor && DsdNative.airspyFd == -1) {
        "one connection stays open, published"
    }
}

// #782: a released connection waits only on its own descriptor, never on a later session's flags or descriptor.
private fun aDeferredCloseWaitsOnlyOnItsOwnDescriptor() {
    val rig = Rig()
    val first = rtl(RTL_A)
    rig.connect(first)
    val old = rig.usb.opened.single()
    // A session runs on it: the global flags say a session uses a receiver and a descriptor is wrapped.
    DsdNative.held = intArrayOf(old.fileDescriptor, -1)
    DsdNative.sessionUsesLocalReceiver = true
    DsdNative.fdInUse = true
    rig.unplug(first)
    Thread.sleep(200)
    check(!old.closed) { "held: the retired connection stays open" }
    // A replacement attaches and a session runs on it, holding its own descriptor; the global flags stay raised
    // throughout, as they would for the new session.
    rig.connect(rtl(RTL_B))
    val replacement = rig.usb.opened.last()
    DsdNative.held = intArrayOf(replacement.fileDescriptor, -1)
    awaitClosed(old, "the retired connection closes once its own descriptor is let go, whatever the new session holds")
    check(!replacement.closed) { "the replacement stays open" }
}

// #782 WP4: the Qt thread's attachment poll lists the bus outside the lock. A listing older than an attach applied
// meanwhile is dropped, so it never clears the newer pending edge, and the broadcast never waits on the poll.
private fun aStaleListingNeverClearsANewerAttach() {
    val rig = Rig()
    val listed = CountDownLatch(1)
    val resume = CountDownLatch(1)
    rig.usb.listHook = { listed.countDown(); resume.await(5, TimeUnit.SECONDS) }
    var first = "unset"
    val qt = onAnotherThread { first = UsbSourceManager.takeAttachment() }
    await(listed, "the poll lists the bus")
    rig.usb.listHook = null
    val attached = CountDownLatch(1)
    val main = Thread { rig.plug(rtl()); attached.countDown() }.also { it.start() }
    await(attached, "the attach broadcast is handled while the poll's listing is held up")
    main.join()
    resume.countDown()
    qt.join(2000)
    check(first == "") { "the stale listing is dropped: $first" }
    check(UsbSourceManager.takeAttachment() == "usb:$RTL_A") { "the attach is still pending for the next poll" }
}

// #782 WP4: unplugged and plugged back in (a new device name) while the poll's listing was held up.
private fun aStaleListingAcrossAReplug() {
    val rig = Rig()
    rig.plug(rtl(RTL_A))
    val listed = CountDownLatch(1)
    val resume = CountDownLatch(1)
    rig.usb.listHook = { listed.countDown(); resume.await(5, TimeUnit.SECONDS) }
    var first = "unset"
    val qt = onAnotherThread { first = UsbSourceManager.takeAttachment() }
    await(listed, "the poll lists the bus")
    rig.usb.listHook = null
    val main = Thread { rig.unplug(rtl(RTL_A)); rig.plug(rtl(RTL_B)) }.also { it.start() }
    main.join(2000)
    check(!main.isAlive) { "the replug is handled while the poll's listing is held up" }
    resume.countDown()
    qt.join(2000)
    check(first == "") { "a listing from before the replug is dropped, not taken as the old device: $first" }
    check(UsbSourceManager.takeAttachment() == "usb:$RTL_B") { "the replugged device is pending" }
    check(rig.record().optLong("epoch") == 0L) { "nothing is connected" }
}

// #782 WP4: a detach while the listing was held up: the old device is never reported attached.
private fun aStaleListingAcrossADetach() {
    val rig = Rig()
    rig.plug(rtl(RTL_A))
    val listed = CountDownLatch(1)
    val resume = CountDownLatch(1)
    rig.usb.listHook = { listed.countDown(); resume.await(5, TimeUnit.SECONDS) }
    var first = "unset"
    val qt = onAnotherThread { first = UsbSourceManager.takeAttachment() }
    await(listed, "the poll lists the bus")
    rig.usb.listHook = null
    val main = Thread { rig.unplug(rtl(RTL_A)) }.also { it.start() }
    main.join(2000)
    check(!main.isAlive) { "the detach is handled while the poll's listing is held up" }
    resume.countDown()
    qt.join(2000)
    check(first == "" && UsbSourceManager.takeAttachment() == "") { "the unplugged device is never reported: $first" }
}

// #782 WP4 / F5: a serial read (a binder call) held up inside an attach never holds the broker's lock: the host's poll
// and another broadcast go on meanwhile.
private fun aBlockedSerialReadBlocksNothing() {
    val rig = Rig()
    rig.connect(rtl(RTL_A))
    val other = rtl(RTL_B)
    rig.plug(other)
    val slow = airspy()
    val reading = CountDownLatch(1)
    val resume = CountDownLatch(1)
    slow.serialHook = { reading.countDown(); resume.await(5, TimeUnit.SECONDS) }
    rig.usb.plug(slow)
    val attach = onAnotherThread {
        UsbSourceManager.reportAttachment(rig.context, Intent(UsbManager.ACTION_USB_DEVICE_ATTACHED).putExtra(UsbManager.EXTRA_DEVICE, slow))
    }
    await(reading, "the attach reads the serial")
    val answered = CountDownLatch(1)
    val receiver = Thread {
        UsbSourceManager.deviceStatus()
        UsbSourceManager.takeAttachment()
        rig.context.broadcast(Intent(UsbManager.ACTION_USB_DEVICE_DETACHED).putExtra(UsbManager.EXTRA_DEVICE, other))
        answered.countDown()
    }.also { it.start() }
    await(answered, "the poll and the detach broadcast go on while the serial read is held up")
    resume.countDown()
    attach.join(2000)
    receiver.join(2000)
    check(rig.record().optBoolean("ready")) { "the connection is untouched" }
}

private val CASES = linkedMapOf<String, () -> Unit>(
    "identity" to ::eachPromptHasItsOwnIdentity,
    "expiry" to ::anUnansweredPromptExpiresOnThePoll,
    "replug" to ::aDetachRetiresThePromptAndAReplugAsksAgain,
    "hop" to ::theSerialSearchHopsWithinItsRequest,
    "attach_scope" to ::anAttachReportsOnlyWhatTheRequestWaitsFor,
    "attach_clears" to ::anAttachClearsTheFailureItAnswers,
    "attach_connected" to ::anAttachNeverOverwritesALiveConnection,
    "connect_order" to ::aPlainConnectFollowsTheRequest,
    "connect_serial" to ::aPlainConnectKeepsTheRequestedSerial,
    "no_request" to ::nothingIsRequestedBeforeTheFirstRequest,
    "refusal" to ::aRefusedSwitchIsFeedbackAndReadyIsRewritten,
    "detach_unused" to { aDetachStopsOnlyTheSessionThatUsesTheReceiver("none") },
    "detach_session" to { aDetachStopsOnlyTheSessionThatUsesTheReceiver("session") },
    "detach_descriptor" to { aDetachStopsOnlyTheSessionThatUsesTheReceiver("descriptor") },
    "detach_other_session" to ::aDetachDuringAnotherSessionClosesTheConnection,
    "detach_configured" to ::aConfiguredSessionKeepsTheConnectionUntilItEnds,
    "detach_unrelated" to ::anUnrelatedDetachChangesNothing,
    "swap_second_session" to ::aSecondSessionGetsAFreshDescriptor,
    "swap_gone" to ::aDeviceGoneMidSwapFailsTheStart,
    "swap_permission" to ::aRevokedPermissionFailsTheStart,
    "swap_open_failed" to ::aFailedReopenFailsTheStart,
    "swap_previous_session" to { aStartWhileThePreviousSessionHoldsTheReceiverIsRefused("session") },
    "swap_previous_descriptor" to { aStartWhileThePreviousSessionHoldsTheReceiverIsRefused("descriptor") },
    "swap_airspy" to ::anAirspySessionGetsAFreshDescriptor,
    "swap_serial" to ::aSerialRequestKeepsItsSerial,
    "switch_configured" to { aConfiguredSessionRefusesASwitch("session") },
    "switch_descriptor" to { aConfiguredSessionRefusesASwitch("descriptor") },
    "switch_concurrent" to ::concurrentSwitchesAreSerialized,
    "close_own_descriptor" to ::aDeferredCloseWaitsOnlyOnItsOwnDescriptor,
    "stale_attach" to ::aStaleListingNeverClearsANewerAttach,
    "stale_replug" to ::aStaleListingAcrossAReplug,
    "stale_detach" to ::aStaleListingAcrossADetach,
    "blocked_serial" to ::aBlockedSerialReadBlocksNothing,
)

/** Entry point inside a case's own class loader. */
fun runCase(name: String) {
    CASES.getValue(name)()
}

fun main() {
    val classpath = System.getProperty("java.class.path").split(File.pathSeparator).map { File(it).toURI().toURL() }
    for (name in CASES.keys) {
        // Parent is the platform loader, so every class here, the manager's objects included, loads afresh.
        URLClassLoader(classpath.toTypedArray(), ClassLoader.getPlatformClassLoader()).use { loader ->
            val case = loader.loadClass("io.github.arancormonk.dsdneo.UsbSourceManagerTestKt").getMethod("runCase", String::class.java)
            try {
                case.invoke(null, name)
            } catch (e: java.lang.reflect.InvocationTargetException) {
                throw IllegalStateException("UsbSourceManagerTest $name: ${e.targetException.message}", e.targetException)
            }
        }
        println("UsbSourceManagerTest: $name OK")
    }
}
