// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.os.SystemClock
import java.io.File
import java.net.URLClassLoader
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
        UsbSourceManager.initializeAttachments(context)
    }

    fun plug(device: UsbDevice) {
        usb.plug(device)
        context.broadcast(Intent(UsbManager.ACTION_USB_DEVICE_ATTACHED).putExtra(UsbManager.EXTRA_DEVICE, device))
    }

    fun unplug(device: UsbDevice) {
        usb.deviceList.remove(device.deviceName)
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
private fun aDetachStopsOnlyTheSessionThatUsesTheReceiver(owner: String) {
    val rig = Rig()
    val device = rtl()
    rig.connect(device)
    DsdNative.sessionUsesLocalReceiver = owner == "session"
    DsdNative.fdInUse = owner == "descriptor"
    DecoderService.configuringLocalReceiver = owner == "configuring"
    rig.unplug(device)
    DsdNative.fdInUse = false
    val stops = if (owner == "none") emptyList() else listOf("detached")
    check(DsdNative.stopCauses == stops) { "$owner: stops ${DsdNative.stopCauses}" }
    check(DecoderService.detachesNoted == if (owner == "configuring") 1 else 0) { "$owner: the configuring session hears of it" }
    check(!rig.record().optBoolean("ready") && DsdNative.usbFd == -1) { "$owner: the connection is released either way" }
    check(rig.text() == "Device detached" && rig.kind() == "detached")
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
    "detach_configuring" to { aDetachStopsOnlyTheSessionThatUsesTheReceiver("configuring") },
    "detach_unrelated" to ::anUnrelatedDetachChangesNothing,
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
