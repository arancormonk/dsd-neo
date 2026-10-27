// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

import android.app.NotificationManager
import android.content.Intent
import android.os.Handler
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import org.json.JSONObject

// #782: a start's local receiver gets a USB connection of its own, taken between IDLE->STARTING and nativeConfigure()
// (UsbSourceManager.takeForSession(), scripted here). A take that fails ends the start before configure with the
// attempt's failure kind; a retirement before or during the take ends it without configuring anything.

private var nextStartId = 1
private var nextAttempt = 1L
private val shade get() = NotificationManager.device
private val RTL = arrayOf("-i", "rtl:0:851.0125M:0:0:24")
private val AIRSPY = arrayOf("-i", "airspy:26a464dc:0:0:0", "--airspy-serial", "26A464DC")
private val NETWORK = arrayOf("-i", "rtltcp:127.0.0.1:1234")

private fun await(latch: CountDownLatch) {
    check(latch.await(5, TimeUnit.SECONDS)) { "service test boundary timed out" }
}

private fun newService(): DecoderService {
    DsdNative.prepareRun()
    shade.clear()
    Handler.pending.clear()
    Handler.timeouts.clear()
    return DecoderService().also { it.onCreate() }
}

/** What DecoderHostAndroid::start() does: retire every earlier attempt, then dispatch this one. */
private fun hostStart(service: DecoderService, args: Array<String>): Pair<Long, Intent> {
    val attempt = nextAttempt++
    DecoderService.retireAttempts(attempt - 1)
    DecoderService.startDecoder(service, args, attempt)
    return attempt to service.intents.remove()
}

private fun deliver(service: DecoderService, intent: Intent): Int {
    val id = nextStartId++
    service.deliver(intent, id)
    return id
}

private fun record() = JSONObject(DecoderService.lifecycleStatus())

/** A start that ran nothing has left the foreground, stopped against its own start id and released everything. */
private fun endsQuietly(service: DecoderService, id: Int, what: String) {
    Handler.drain()
    check(DsdNative.enteredRun.count == 1L) { "$what: an engine run started" }
    check(DecoderService.stateName() == "IDLE") { "$what: the service is not idle" }
    check(service.stoppedIds.toList() == listOf(id)) { "$what: the service must stop against its start" }
    check(!service.foreground) { "$what: the service stayed in the foreground" }
    service.onDestroy()
    Handler.drain()
    check(shade.shown.isEmpty()) { "$what: a notification outlived the start" }
    check(service.power.created.none { it.isHeld }) { "$what: the wake lock is still held" }
}

private fun finish(service: DecoderService) {
    val stopped = service.stoppedIds.size
    DsdNative.finishRun.countDown()
    val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5)
    while (service.stoppedIds.size <= stopped) {
        check(System.nanoTime() < deadline) { "the run's end never reached the service" }
        Handler.pending.poll(10, TimeUnit.MILLISECONDS)?.run()
    }
    service.onDestroy()
    Handler.drain()
    check(DecoderService.stateName() == "IDLE")
}

// Each start of a local receiver takes it before configure, on the start's own thread; other inputs take nothing.
// The session's record names the connection it took, which its receiver error belongs to.
private fun theReceiverIsTakenBeforeConfigure() {
    for ((args, source) in listOf(RTL to "usb", AIRSPY to "airspy")) {
        val service = newService()
        var configuredAtTake = -1
        UsbSourceManager.answer = {
            configuredAtTake = DsdNative.configured.size
            check(DecoderService.stateName() == "STARTING") { "$source: the take runs while STARTING" }
            UsbSessionTake(device = "/dev/bus/usb/001/007", epoch = 3L, connection = 5L)
        }
        val (attempt, intent) = hostStart(service, args)
        deliver(service, intent)
        await(DsdNative.enteredRun)
        check(UsbSourceManager.takes.toList() == listOf(source)) { "$source: one take: ${UsbSourceManager.takes}" }
        check(configuredAtTake == 0 && DsdNative.configured.size == 1) { "$source: the take comes before configure" }
        val running = record()
        check(running.optLong("attemptId") == attempt && running.optString("usbDevice") == "/dev/bus/usb/001/007" &&
            running.optLong("usbEpoch") == 3L && running.optLong("usbConnection") == 5L) {
            "$source: the record names the connection the session took: $running"
        }
        finish(service)
    }
    val service = newService()
    val (_, intent) = hostStart(service, NETWORK)
    deliver(service, intent)
    await(DsdNative.enteredRun)
    check(UsbSourceManager.takes.isEmpty()) { "an rtl_tcp start takes no USB receiver" }
    check(record().optLong("usbConnection") == 0L && record().optString("usbDevice") == "") {
        "and names no connection: ${record()}"
    }
    finish(service)
}

// A take that fails ends the start before configure, as the attempt's failure: the kind for the host, the service's
// text, the take's reason in the diagnostics.
private fun aFailedTakeFailsTheAttempt() {
    val texts = mapOf(
        UsbSessionTake.USB_GONE to "resource-20",
        UsbSessionTake.USB_PERMISSION to "resource-21",
        UsbSessionTake.USB_OPEN_FAILED to "resource-22",
        UsbSessionTake.PREVIOUS_STOPPING to "resource-9",
    )
    for ((kind, text) in texts) {
        val service = newService()
        UsbSourceManager.answer = { UsbSessionTake.failed(kind, "usb: the take failed as $kind") }
        val (attempt, intent) = hostStart(service, RTL)
        val id = deliver(service, intent)
        check(DsdNative.configured.isEmpty()) { "$kind: nothing is configured" }
        val failed = record()
        check(failed.optLong("attemptId") == attempt && failed.optString("failureKind") == kind &&
            failed.optString("lastError") == text && failed.optLong("reason") == DsdNative.RUN_FAILED.toLong()) {
            "$kind: the attempt fails with its kind: $failed"
        }
        check("usb: the take failed as $kind" in DsdNative.diagnostics) { "$kind: ${DsdNative.diagnostics}" }
        endsQuietly(service, id, kind)
    }
}

// Task 4 review (minor 5): a retirement between IDLE->STARTING and the take neither opens a USB connection nor pays
// for a bootstrap.
private fun aRetirementBeforeTheTakeOpensNothing() {
    val service = newService()
    val (attempt, intent) = hostStart(service, RTL)
    // The wake lock is acquired after IDLE->STARTING, before the take: the host's grace runs out there.
    service.power.acquireHook = {
        check(DecoderService.stateName() == "STARTING")
        check(!DecoderService.retireAttempts(attempt)) { "a starting attempt can still be cancelled" }
    }
    val id = deliver(service, intent)
    service.power.acquireHook = null
    check(UsbSourceManager.takes.isEmpty()) { "no receiver is taken: ${UsbSourceManager.takes}" }
    check(DsdNative.configured.isEmpty() && DsdNative.discards == 0) { "nothing is configured, so nothing is discarded" }
    val cancelled = record()
    check(cancelled.optLong("attemptId") == attempt && cancelled.optLong("reason") == DsdNative.RUN_CANCELLED.toLong() &&
        cancelled.optString("failureKind") == "") { "the attempt ends cancelled: $cancelled" }
    check("service: start attempt $attempt retired before configuration; cancelled" in DsdNative.diagnostics) {
        DsdNative.diagnostics
    }
    endsQuietly(service, id, "retired before the take")
}

// The retirement lands while the take reopens the receiver: configure is not paid for.
private fun aRetirementDuringTheTakeConfiguresNothing() {
    val service = newService()
    val (attempt, intent) = hostStart(service, RTL)
    UsbSourceManager.answer = {
        check(!DecoderService.retireAttempts(attempt)) { "a starting attempt can still be cancelled" }
        UsbSessionTake(device = "/dev/bus/usb/001/007", connection = 2L)
    }
    val id = deliver(service, intent)
    check(UsbSourceManager.takes.toList() == listOf("usb")) { "the take ran" }
    check(DsdNative.configured.isEmpty() && DsdNative.discards == 0) { "nothing is configured" }
    check(record().optLong("reason") == DsdNative.RUN_CANCELLED.toLong()) { "the attempt ends cancelled: ${record()}" }
    endsQuietly(service, id, "retired during the take")
}

fun main(args: Array<String>) {
    val tests = linkedMapOf<String, () -> Unit>(
        "take_before_configure" to ::theReceiverIsTakenBeforeConfigure,
        "take_failure" to ::aFailedTakeFailsTheAttempt,
        "retire_before_take" to ::aRetirementBeforeTheTakeOpensNothing,
        "retire_during_take" to ::aRetirementDuringTheTakeConfiguresNothing,
    )
    for ((name, test) in tests) {
        if (args.isNotEmpty() && name !in args) continue
        test()
        println("DecoderServiceUsbTest: $name OK")
    }
}
