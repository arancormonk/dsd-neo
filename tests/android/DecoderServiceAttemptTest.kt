// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

import android.app.ForegroundServiceStartNotAllowedException
import android.app.NotificationManager
import android.content.Intent
import android.os.Build
import android.os.Handler
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import org.json.JSONObject

// #782: start attempts. The host gives each start an id and retires the ones it gave up on (its 2 s grace ran out, a
// dismissal, Retry, a start of something else); the intents here are delivered late, as a busy main thread would.

private var nextStartId = 1
private var nextAttempt = 1L
private val shade get() = NotificationManager.device
private val RTL = arrayOf("-i", "rtl:0")
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

/** What DecoderHostAndroid::start() does: retire every earlier attempt, then dispatch this one, which is queued. */
private fun hostStart(service: DecoderService, args: Array<String>): Pair<Long, Intent> {
    val attempt = nextAttempt++
    DecoderService.retireAttempts(attempt - 1)
    DecoderService.startDecoder(service, args, attempt)
    return attempt to service.intents.remove()
}

/**
 * #786: what the host's roaming start does: hostStart() with the user stop generation and the foreground epoch its
 * transaction captured ([expected] and [epoch], by default the ones now).
 */
private fun roamStart(
    service: DecoderService,
    args: Array<String>,
    expected: Long = DecoderService.userStopGeneration(),
    epoch: Long = ForegroundEpoch.current()
): Pair<Long, Intent> {
    val attempt = nextAttempt++
    DecoderService.retireAttempts(attempt - 1)
    val status = DecoderService.startDecoder(service, args, attempt, expected, epoch)
    check(status == DecoderService.START_HANDED_OVER) { "the roaming start was not handed over: $status" }
    return attempt to service.intents.remove()
}

private fun deliver(service: DecoderService, intent: Intent): Int {
    val id = nextStartId++
    service.deliver(intent, id)
    return id
}

private fun record() = JSONObject(DecoderService.lifecycleStatus())

private fun neverRan(what: String) {
    check(DsdNative.enteredRun.count == 1L) { "$what: an engine run started" }
}

/** A start that ran nothing has taken the service out of the foreground and stopped it against its own start id. */
private fun endsQuietly(service: DecoderService, id: Int, what: String) {
    Handler.drain()
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

// The grace ran out and the host retired the attempt; the user dismissed the card (a second retirement). The START
// arrives afterwards: it never configures or runs anything.
private fun timeoutThenDismiss() {
    val service = newService()
    val (attempt, late) = hostStart(service, RTL)
    check(!DecoderService.retireAttempts(attempt)) { "an undelivered attempt was never taken" }
    check(!DecoderService.retireAttempts(attempt)) { "the dismissal finds it retired" }
    val before = record()
    val id = deliver(service, late)
    check(DsdNative.configured.isEmpty()) { "a retired attempt was configured" }
    neverRan("dismissed")
    check("service: start attempt $attempt rejected as retired or superseded (state IDLE)" in DsdNative.diagnostics) {
        DsdNative.diagnostics
    }
    val after = record()
    check(after.optLong("sessionId") == before.optLong("sessionId") && after.optString("lastError") == "") {
        "a rejected attempt is no session: $after"
    }
    endsQuietly(service, id, "dismissed")
}

// The grace ran out; Retry dismissed it and started again. Both STARTs arrive late, the old one first: only the retry
// runs.
private fun timeoutThenRetry() {
    val service = newService()
    val (first, firstIntent) = hostStart(service, RTL)
    check(!DecoderService.retireAttempts(first))
    check(!DecoderService.retireAttempts(first))
    val (retry, retryIntent) = hostStart(service, RTL)
    deliver(service, firstIntent)
    check(DsdNative.configured.isEmpty()) { "the retired attempt was configured" }
    deliver(service, retryIntent)
    await(DsdNative.enteredRun)
    check(DsdNative.configured.size == 1) { "only the retry configures: ${DsdNative.configured}" }
    check(DecoderService.stateName() == "RUNNING")
    check(record().optLong("attemptId") == retry) { "the record names the retry: ${record()}" }
    finish(service)
}

// The grace ran out and the user started another source. The new START arrives first and runs; the old one, later
// still, is rejected without touching the running session.
private fun timeoutThenAnotherSource() {
    val service = newService()
    val (old, oldIntent) = hostStart(service, RTL)
    check(!DecoderService.retireAttempts(old))
    val (other, otherIntent) = hostStart(service, NETWORK)
    deliver(service, otherIntent)
    await(DsdNative.enteredRun)
    deliver(service, oldIntent)
    check(DsdNative.configured.toList() == listOf(NETWORK.toList())) {
        "only the other source runs: ${DsdNative.configured}"
    }
    check(DecoderService.stateName() == "RUNNING")
    val running = record()
    check(running.optLong("attemptId") == other && running.optString("lastError") == "") {
        "the late START leaves the running session alone: $running"
    }
    check(service.foreground) { "the running session keeps its notification" }
    finish(service)
}

private fun cancelledDuringConfigure(service: DecoderService, attempt: Long, id: Int, what: String) {
    neverRan(what)
    check(DsdNative.discards == 1) { "$what: the configured session was not given up" }
    val cancelled = record()
    val reason = cancelled.optLong("reason")
    check(cancelled.optLong("attemptId") == attempt && reason == DsdNative.RUN_CANCELLED.toLong()) {
        "$what: the attempt ends cancelled: $cancelled"
    }
    check(DsdNative.diagnostics.any { it.startsWith("service: start attempt $attempt retired during configuration") }) {
        DsdNative.diagnostics
    }
    endsQuietly(service, id, what)
}

// The host retires the attempt while nativeConfigure() runs, which also clears the native stop flags.
private fun retireDuringConfigure() {
    val service = newService()
    val (attempt, intent) = hostStart(service, RTL)
    DsdNative.configureHook = {
        check(!DecoderService.retireAttempts(attempt)) { "a configuring attempt can still be cancelled" }
        check(DecoderService.stateName() == "STARTING")
    }
    val id = deliver(service, intent)
    cancelledDuringConfigure(service, attempt, id, "during configure")
}

// The retirement lands after nativeConfigure() returned, just before the worker would be published.
private fun retireJustBeforeWorkerPublication() {
    val service = newService()
    val (attempt, intent) = hostStart(service, RTL)
    DsdNative.diagnosticHook = { line ->
        if (line.startsWith("service: engine configuration returned")) {
            check(!DecoderService.retireAttempts(attempt)) { "an unpublished attempt can still be cancelled" }
        }
    }
    val id = deliver(service, intent)
    cancelledDuringConfigure(service, attempt, id, "before publication")
}

// The host's expiry came from an IDLE record read before the service took the START. Its retirement lands once the
// run is published: the run goes on, and the host is told so it follows it instead of reporting a failure.
private fun timeoutFromAnEarlierIdleSnapshot() {
    val service = newService()
    val (attempt, intent) = hostStart(service, RTL)
    val snapshot = record()
    check(snapshot.optString("state") == "IDLE")
    deliver(service, intent)
    await(DsdNative.enteredRun)
    check(DecoderService.retireAttempts(attempt)) { "a published attempt is reported taken" }
    check(DecoderService.stateName() == "RUNNING") { "and keeps running" }
    check(DsdNative.discards == 0 && DsdNative.stops.isEmpty())
    val now = record()
    check(now.optLong("sessionId") == snapshot.optLong("sessionId") + 1 && now.optLong("attemptId") == attempt) {
        "the record the host reads next is the attempt's: $now"
    }
    finish(service)
}

// The record carries the attempt and its attempt-scoped failure kind, empty unless the start failed for one.
private fun theRecordNamesTheAttempt() {
    val service = newService()
    val (attempt, intent) = hostStart(service, RTL)
    DsdNative.configureCode = -3
    val id = deliver(service, intent)
    val failed = record()
    val kind = failed.optString("failureKind")
    check(failed.optLong("attemptId") == attempt && failed.has("failureKind") && kind == "") {
        "a configure failure names its attempt and no attempt failure kind: $failed"
    }
    check(failed.optLong("reason") == DsdNative.RUN_FAILED.toLong())
    endsQuietly(service, id, "configure failure")
}

// A process died with a START undelivered, and the platform redelivers it into the process it restarted, whose host
// counts attempts from 1 again. That START carries the dead process's token: it never configures, and it leaves the
// attempt counter alone, so the new host's own first start runs.
private fun anEarlierProcessStartIsRejected() {
    val service = newService()
    val earlier = Intent(service, DecoderService::class.java)
        .setAction(DecoderService.ACTION_START)
        .putExtra(DecoderService.EXTRA_ARGS, RTL)
        .putExtra(DecoderService.EXTRA_ATTEMPT, nextAttempt + 5)
        .putExtra(DecoderService.EXTRA_PROCESS, DecoderService.processToken xor 1L)
    val before = record()
    val id = deliver(service, earlier)
    check(DsdNative.configured.isEmpty()) { "an earlier process's START was configured" }
    neverRan("earlier process")
    check(DsdNative.diagnostics.any { it.startsWith("service: start attempt ${nextAttempt + 5} rejected: sent by an earlier process") }) {
        DsdNative.diagnostics
    }
    check(record().optLong("sessionId") == before.optLong("sessionId")) { "a rejected START is no session: ${record()}" }
    endsQuietly(service, id, "earlier process")

    val host = newService()
    val (attempt, intent) = hostStart(host, RTL)
    deliver(host, intent)
    await(DsdNative.enteredRun)
    check(DsdNative.configured.size == 1) { "the host's own start configures: ${DsdNative.configured}" }
    check(record().optLong("attemptId") == attempt) { "the host's attempt $attempt runs: ${record()}" }
    finish(host)
}

// #786: the record a start a user stop superseded leaves: its own attempt, ended cancelled with "superseded" set and
// nothing to report as a failure, so the host follows it to Idle instead of showing a failure card or waiting out its
// grace.
private fun supersededRecord(attempt: Long, what: String) {
    val superseded = record()
    check(
        superseded.optLong("attemptId") == attempt && superseded.optBoolean("superseded") &&
            superseded.optLong("reason") == DsdNative.RUN_CANCELLED.toLong() &&
            superseded.optString("lastError") == "" && superseded.optString("failureKind") == ""
    ) { "$what: the attempt ends superseded: $superseded" }
    check(DecoderService.retireAttempts(attempt)) { "$what: the host's retirement finds the attempt ended on its own" }
}

/**
 * The stop the user's request queued on [stopped] arrives once the superseded start is done, at the instance the
 * platform creates for it: nothing is left to stop, and its delivery is not counted again.
 */
private fun theQueuedStopFindsNothing(stopped: DecoderService, generation: Long, what: String) {
    val intent = stopped.intents.remove()
    check(stopped.intents.isEmpty()) { "$what: more than the user's Stop was queued" }
    val service = DecoderService().also { it.onCreate() }
    DsdNative.diagnostics.clear()
    val id = deliver(service, intent)
    check(DsdNative.stops.isEmpty()) { "$what: the queued Stop found a run: ${DsdNative.stops}" }
    check(DecoderService.userStopGeneration() == generation) { "$what: the delivered Stop was counted twice" }
    check(DsdNative.diagnostics.any { it.endsWith("cause=user state=IDLE: no run to cancel") }) { DsdNative.diagnostics }
    endsQuietly(service, id, "$what, queued Stop")
}

// #786: the user stopped listening after the roaming transaction read the stop generation, and before the service
// took its START (the service was IDLE, so the Stop had nothing to cancel). The START never configures anything.
private fun roamSupersededBeforeAcceptance() {
    val service = newService()
    val generation = DecoderService.userStopGeneration()
    val (attempt, intent) = roamStart(service, RTL, generation)
    DecoderService.stopDecoder(service)
    check(DecoderService.userStopGeneration() == generation + 1) { "a Stop while IDLE still moves the generation" }
    val before = record()
    val id = deliver(service, intent)
    check(DsdNative.configured.isEmpty() && UsbSourceManager.takes.isEmpty()) { "a superseded START took the service" }
    check(DsdNative.inits == 0 && DsdNative.startupArms == 0) { "a superseded START went on to open a device" }
    check(service.power.created.isEmpty()) { "a superseded START took a wake lock" }
    neverRan("superseded before acceptance")
    check(record().optLong("sessionId") == before.optLong("sessionId") + 1) { "the attempt has a record of its own" }
    supersededRecord(attempt, "before acceptance")
    check(
        "service: start attempt $attempt superseded by a user stop before it was accepted; nothing ran" in
            DsdNative.diagnostics
    ) { DsdNative.diagnostics }
    endsQuietly(service, id, "before acceptance")
    theQueuedStopFindsNothing(service, generation + 1, "before acceptance")
}

// The roam START arrives while the service is still busy, after the user's Stop moved the generation: it is turned
// away silently, as a retired one is, with no busy failure on the record of the run that holds the service.
private fun roamSupersededWhileBusy() {
    val service = newService()
    val (manual, manualIntent) = hostStart(service, RTL)
    deliver(service, manualIntent)
    await(DsdNative.enteredRun)
    val generation = DecoderService.userStopGeneration()
    val (attempt, intent) = roamStart(service, RTL, generation)
    DecoderService.stopDecoder(service)
    check(DecoderService.stateName() == "STOPPING")
    // Its own intent only repeats the stop; the run is already STOPPING.
    check(service.intents.remove().action == DecoderService.ACTION_STOP)
    val before = record()
    deliver(service, intent)
    val after = record()
    check(DsdNative.configured.size == 1) { "a superseded START was configured: ${DsdNative.configured}" }
    check(
        after.optLong("sessionId") == before.optLong("sessionId") && after.optLong("attemptId") == manual &&
            after.optString("lastError") == "" && !after.optBoolean("superseded")
    ) { "the busy run's record is left alone: $after" }
    check("service: start attempt $attempt rejected: superseded by a user stop (state STOPPING)" in DsdNative.diagnostics) {
        DsdNative.diagnostics
    }
    // Without the stop, the same START is the busy failure it always was.
    val (late, lateIntent) = roamStart(service, RTL)
    deliver(service, lateIntent)
    check(record().optString("lastError") != "") { "a START that finds the service busy, unsuperseded, is refused busy: ${record()}" }
    check(late > attempt)
    finish(service)
}

// The user's Stop lands while the service takes the start's receiver, before configuration: nothing is configured.
private fun roamSupersededBeforeConfigure() {
    val service = newService()
    val (attempt, intent) = roamStart(service, RTL)
    UsbSourceManager.answer = {
        DecoderService.stopDecoder(service)
        UsbSessionTake()
    }
    val id = deliver(service, intent)
    check(UsbSourceManager.takes.toList() == listOf(UsbReceiverKind.RTL)) { "the receiver take is where the Stop lands" }
    check(DsdNative.configured.isEmpty() && DsdNative.discards == 0) { "a superseded start was configured" }
    supersededRecord(attempt, "before configure")
    check("service: start attempt $attempt superseded by a user stop before configuration; cancelled" in DsdNative.diagnostics) {
        DsdNative.diagnostics
    }
    val generation = DecoderService.userStopGeneration()
    endsQuietly(service, id, "before configure")
    theQueuedStopFindsNothing(service, generation, "before configure")
}

private fun supersededDuringConfigure(service: DecoderService, attempt: Long, id: Int, what: String) {
    neverRan(what)
    check(DsdNative.configured.size == 1 && DsdNative.discards == 1) { "$what: the configured session was not given up" }
    check(DsdNative.stops.isEmpty()) { "$what: nothing was running to stop" }
    supersededRecord(attempt, what)
    check(DsdNative.diagnostics.any {
        it.startsWith("service: start attempt $attempt superseded by a user stop during configuration; cancelled")
    }) { DsdNative.diagnostics }
    val generation = DecoderService.userStopGeneration()
    endsQuietly(service, id, what)
    theQueuedStopFindsNothing(service, generation, what)
}

// The user's Stop lands while nativeConfigure() runs: the service is STARTING, so the Stop is only counted and ordered
// behind the start. The configured session is discarded at the RUNNING commit; its worker never starts.
private fun roamSupersededDuringConfigure() {
    val service = newService()
    val (attempt, intent) = roamStart(service, RTL)
    DsdNative.configureHook = {
        DecoderService.stopDecoder(service)
        check(DecoderService.stateName() == "STARTING") { "configuration runs while STARTING" }
    }
    val id = deliver(service, intent)
    supersededDuringConfigure(service, attempt, id, "during configure")
}

// The user's Stop lands after nativeConfigure() returned, just before the worker would be published.
private fun roamSupersededJustBeforePublication() {
    val service = newService()
    val (attempt, intent) = roamStart(service, RTL)
    DsdNative.diagnosticHook = { line ->
        if (line.startsWith("service: engine configuration returned")) {
            DecoderService.stopDecoder(service)
        }
    }
    val id = deliver(service, intent)
    supersededDuringConfigure(service, attempt, id, "before publication")
}

// A roam start's setup can still fail after the user's Stop superseded it (the destination's channel CSV is bad, its
// receiver will not open): the user stopped, so the record says superseded with no failure, never a failure card. The
// same failure of a roam start nobody stopped is still a failure.
private fun roamSupersededThenSetupFails() {
    val failing = newService()
    val (held, heldIntent) = roamStart(failing, RTL)
    DsdNative.configureCode = -3
    val heldId = deliver(failing, heldIntent)
    val failed = record()
    check(
        failed.optLong("attemptId") == held && !failed.optBoolean("superseded") &&
            failed.optLong("reason") == DsdNative.RUN_FAILED.toLong() && failed.optString("lastError") != ""
    ) { "a roam start nobody stopped still fails: $failed" }
    endsQuietly(failing, heldId, "unsuperseded configure failure")

    val configuring = newService()
    val (attempt, intent) = roamStart(configuring, RTL)
    DsdNative.configureCode = -3
    DsdNative.configureHook = { DecoderService.stopDecoder(configuring) }
    val id = deliver(configuring, intent)
    neverRan("configure failure")
    check(DsdNative.discards == 0) { "a configuration that failed left nothing to give up" }
    supersededRecord(attempt, "configure failure")
    check(DsdNative.diagnostics.any {
        it.startsWith("service: start attempt $attempt superseded by a user stop; its setup failure is not shown")
    }) { DsdNative.diagnostics }
    var generation = DecoderService.userStopGeneration()
    endsQuietly(configuring, id, "configure failure")
    theQueuedStopFindsNothing(configuring, generation, "configure failure")

    val taking = newService()
    val (taken, takeIntent) = roamStart(taking, RTL)
    UsbSourceManager.answer = {
        DecoderService.stopDecoder(taking)
        UsbSessionTake.failed(UsbSessionTake.USB_OPEN_FAILED, "open failed")
    }
    val takeId = deliver(taking, takeIntent)
    check(DsdNative.configured.isEmpty()) { "a start whose receiver failed was configured" }
    supersededRecord(taken, "receiver failure")
    generation = DecoderService.userStopGeneration()
    endsQuietly(taking, takeId, "receiver failure")
    theQueuedStopFindsNothing(taking, generation, "receiver failure")
}

// The host cancels its roam (the user disabled roaming or chose a site) while the destination configures: the existing
// retirement cancels it, and the record says retired, not superseded, since no user stop came.
private fun roamRetiredBeforeCommit() {
    val service = newService()
    val generation = DecoderService.userStopGeneration()
    val (attempt, intent) = roamStart(service, RTL, generation)
    DsdNative.configureHook = {
        check(!DecoderService.retireAttempts(attempt)) { "a configuring roam start can still be cancelled" }
    }
    val id = deliver(service, intent)
    neverRan("retired roam")
    check(DsdNative.discards == 1) { "the retired roam's configuration was not given up" }
    val cancelled = record()
    check(
        cancelled.optLong("attemptId") == attempt && !cancelled.optBoolean("superseded") &&
            cancelled.optLong("reason") == DsdNative.RUN_CANCELLED.toLong()
    ) { "a retired roam ends cancelled, not superseded: $cancelled" }
    check(DsdNative.diagnostics.any { it.startsWith("service: start attempt $attempt retired during configuration") }) {
        DsdNative.diagnostics
    }
    check(DecoderService.userStopGeneration() == generation) { "a retirement is no user stop" }
    endsQuietly(service, id, "retired roam")
}

// A roam start whose generation still holds runs as any start does, and a superseded attempt before it leaves nothing
// behind in its record. A user Stop after it published RUNNING stops the run, as for any run.
private fun roamWithAnUnchangedGenerationRuns() {
    val service = newService()
    val (attempt, intent) = roamStart(service, RTL)
    deliver(service, intent)
    await(DsdNative.enteredRun)
    check(DecoderService.stateName() == "RUNNING")
    val running = record()
    check(running.optLong("attemptId") == attempt && !running.optBoolean("superseded")) {
        "the roam start runs, unsuperseded: $running"
    }
    DecoderService.stopDecoder(service)
    check(DsdNative.stops == listOf(true)) { "a user Stop after the commit stops the run" }
    check(DecoderService.stateName() == "STOPPING")
    finish(service)
    check(!record().optBoolean("superseded")) { "a run the user stopped was not superseded: ${record()}" }
}

// #786 class sweep: a roam start the user's Stop superseded, the host retired, or leaving the foreground fenced never
// starts its worker and never ends failed, at whichever point it is cancelled: as it is accepted, before configuration
// (during the receiver take), during a setup step that then fails (nativeInit, the receiver take, nativeConfigure),
// during a configuration that succeeds, or just before the worker would be published.
private enum class RoamCancel(val wording: String) {
    STOP("superseded by a user stop"),
    RETIRE("retired"),
    FOREGROUND("superseded by leaving the foreground")
}

private enum class RoamPoint {
    ACCEPTANCE,
    BEFORE_CONFIGURE,
    INIT_FAILS,
    TAKE_FAILS,
    CONFIGURE_RUNS,
    CONFIGURE_FAILS,
    BEFORE_PUBLICATION
}

private fun cancelRoam(how: RoamCancel, service: DecoderService, attempt: Long) {
    when (how) {
        RoamCancel.STOP -> DecoderService.stopDecoder(service)
        RoamCancel.RETIRE -> check(!DecoderService.retireAttempts(attempt)) { "the roam start was past retirement" }
        RoamCancel.FOREGROUND -> ForegroundEpoch.advance()
    }
}

/** Sets up [point]'s boundary to cancel the start [how]; the receiver take and the native steps are scripted. */
private fun armRoamCancel(how: RoamCancel, point: RoamPoint, service: DecoderService, attempt: Long) {
    val cancel = { cancelRoam(how, service, attempt) }
    when (point) {
        RoamPoint.ACCEPTANCE -> cancel()
        RoamPoint.BEFORE_CONFIGURE -> UsbSourceManager.answer = { cancel(); UsbSessionTake() }
        RoamPoint.INIT_FAILS -> {
            DsdNative.initHook = cancel
            DsdNative.initCode = -4
        }
        RoamPoint.TAKE_FAILS -> UsbSourceManager.answer = {
            cancel()
            UsbSessionTake.failed(UsbSessionTake.USB_OPEN_FAILED, "open failed")
        }
        RoamPoint.CONFIGURE_RUNS -> DsdNative.configureHook = cancel
        RoamPoint.CONFIGURE_FAILS -> {
            DsdNative.configureHook = cancel
            DsdNative.configureCode = -3
        }
        RoamPoint.BEFORE_PUBLICATION -> DsdNative.diagnosticHook = { line ->
            if (line.startsWith("service: engine configuration returned")) cancel()
        }
    }
}

private fun roamCancelledAt(how: RoamCancel, point: RoamPoint) {
    val what = "${how.name.lowercase()} at ${point.name.lowercase()}"
    val service = newService()
    val generation = DecoderService.userStopGeneration()
    val before = record()
    val (attempt, intent) = roamStart(service, RTL)
    armRoamCancel(how, point, service, attempt)
    val id = deliver(service, intent)
    neverRan(what)
    val configured = point == RoamPoint.CONFIGURE_RUNS || point == RoamPoint.CONFIGURE_FAILS ||
        point == RoamPoint.BEFORE_PUBLICATION
    check(DsdNative.configured.size == (if (configured) 1 else 0)) { "$what: configured ${DsdNative.configured}" }
    val discarded = point == RoamPoint.CONFIGURE_RUNS || point == RoamPoint.BEFORE_PUBLICATION
    check(DsdNative.discards == (if (discarded) 1 else 0)) { "$what: discards ${DsdNative.discards}" }
    val ended = record()
    if (how == RoamCancel.RETIRE && point == RoamPoint.ACCEPTANCE) {
        // Retired before it arrived: no session at all, as #782's undelivered retirements.
        check(ended.optLong("sessionId") == before.optLong("sessionId")) { "$what: a retired START opened: $ended" }
    } else {
        check(
            ended.optLong("attemptId") == attempt && ended.optLong("reason") == DsdNative.RUN_CANCELLED.toLong() &&
                ended.optString("lastError") == "" && ended.optString("failureKind") == "" &&
                ended.optBoolean("superseded") == (how != RoamCancel.RETIRE)
        ) { "$what: the attempt ends cancelled, never failed: $ended" }
        check(DsdNative.diagnostics.any { it.startsWith("service: start attempt $attempt ${how.wording}") }) {
            "$what: ${DsdNative.diagnostics}"
        }
    }
    val moved = DecoderService.userStopGeneration()
    check(moved == generation + (if (how == RoamCancel.STOP) 1 else 0)) { "$what: only the user's Stop is counted" }
    endsQuietly(service, id, what)
    if (how == RoamCancel.STOP) {
        theQueuedStopFindsNothing(service, moved, what)
    } else {
        check(service.intents.isEmpty()) { "$what: nothing else was queued" }
    }
}

private fun roamCancelMatrix() {
    for (how in RoamCancel.values()) {
        for (point in RoamPoint.values()) {
            roamCancelledAt(how, point)
        }
    }
}

// The roam START finds the service busy after the host retired it or the app left the foreground: it is turned away
// silently, with no busy failure on the record of the run that holds the service (the user's Stop: above).
private fun roamCancelledWhileBusy() {
    for (how in listOf(RoamCancel.RETIRE, RoamCancel.FOREGROUND)) {
        val service = newService()
        val (manual, manualIntent) = hostStart(service, RTL)
        deliver(service, manualIntent)
        await(DsdNative.enteredRun)
        val (attempt, intent) = roamStart(service, RTL)
        cancelRoam(how, service, attempt)
        val before = record()
        deliver(service, intent)
        val after = record()
        check(DsdNative.configured.size == 1) { "$how: a cancelled START was configured: ${DsdNative.configured}" }
        check(
            after.optLong("sessionId") == before.optLong("sessionId") && after.optLong("attemptId") == manual &&
                after.optString("lastError") == "" && !after.optBoolean("superseded")
        ) { "$how: the busy run's record is left alone: $after" }
        val expected = if (how == RoamCancel.RETIRE) {
            "service: start attempt $attempt rejected as retired or superseded (state RUNNING)"
        } else {
            "service: start attempt $attempt rejected: superseded by leaving the foreground (state RUNNING)"
        }
        check(expected in DsdNative.diagnostics) { "$how: ${DsdNative.diagnostics}" }
        finish(service)
    }
}

// The native record already says a configure failed while the service, still STARTING, has not decided the start: a
// cancel can still turn it into a cancelled start. The record the host reads carries no failure until the service's
// own verdict, which for a start nobody cancelled is the failure.
private fun aConfigureFailureWaitsForTheServiceVerdict() {
    for (cancelled in listOf(false, true)) {
        val service = newService()
        val (attempt, intent) = roamStart(service, RTL)
        var during: JSONObject? = null
        DsdNative.configureCode = -3
        DsdNative.configureHook = {
            val session = record().optLong("sessionId")
            DsdNative.lifecycle = longArrayOf(session, 0L, DsdNative.RUN_FAILED.toLong(), -1L, 0L, 6L, -3L, 0L, 0L)
            during = record()
            if (cancelled) DecoderService.stopDecoder(service)
        }
        val id = deliver(service, intent)
        val mid = checkNotNull(during)
        check(
            mid.optString("state") == "STARTING" && mid.optLong("reason") == DsdNative.RUN_PENDING.toLong() &&
                mid.optString("lastError") == ""
        ) { "cancelled=$cancelled: the native failure shows before the service decided: $mid" }
        val ended = record()
        if (cancelled) {
            supersededRecord(attempt, "native failure, then superseded")
        } else {
            check(ended.optLong("reason") == DsdNative.RUN_FAILED.toLong() && ended.optString("lastError") != "") {
                "the start nobody cancelled fails: $ended"
            }
        }
        val generation = DecoderService.userStopGeneration()
        endsQuietly(service, id, "native failure, cancelled=$cancelled")
        if (cancelled) theQueuedStopFindsNothing(service, generation, "native failure, superseded")
    }
}

// A manual start carries no expectation: leaving the foreground and a retirement of an older attempt leave it alone.
private fun manualStartIgnoresTheFences() {
    val service = newService()
    val (attempt, intent) = hostStart(service, RTL)
    val epoch = intent.getLongExtra(DecoderService.EXTRA_EXPECTED_FOREGROUND_EPOCH, 0L)
    check(epoch == DecoderService.NO_FOREGROUND_EXPECTATION) { "a manual start carries no foreground expectation" }
    ForegroundEpoch.advance()
    DsdNative.configureHook = { ForegroundEpoch.advance() }
    deliver(service, intent)
    await(DsdNative.enteredRun)
    check(DecoderService.stateName() == "RUNNING" && record().optLong("attemptId") == attempt) { "the manual run" }
    finish(service)
}

// #786 B2: a START the platform refuses never reaches the service, and the host is told at once.
private fun theStartStatusSaysWhetherItWasHandedOver() {
    val service = newService()
    check(DecoderService.startDecoder(service, RTL) == DecoderService.START_HANDED_OVER)
    val manual = service.intents.remove()
    check(manual.getLongExtra(DecoderService.EXTRA_EXPECTED_STOP_GENERATION, 0L) == DecoderService.NO_STOP_EXPECTATION) {
        "a manual start carries no stop-generation expectation"
    }
    check(DecoderService.startDecoder(service, RTL, 0L, 7L, 3L) == DecoderService.START_HANDED_OVER)
    val roam = service.intents.remove()
    check(
        roam.getLongExtra(DecoderService.EXTRA_EXPECTED_STOP_GENERATION, 0L) == 7L &&
            roam.getLongExtra(DecoderService.EXTRA_EXPECTED_FOREGROUND_EPOCH, 0L) == 3L
    ) { "a roaming start carries its expectations" }
    val sdk = Build.VERSION.SDK_INT
    try {
        Build.VERSION.SDK_INT = Build.VERSION_CODES.S
        val refusals = listOf(
            ForegroundServiceStartNotAllowedException("background") to DecoderService.START_NOT_ALLOWED,
            IllegalStateException("not allowed") to DecoderService.START_ILLEGAL_STATE,
            SecurityException("refused") to DecoderService.START_SECURITY,
            UnsupportedOperationException("vendor refusal") to DecoderService.START_UNEXPECTED,
        )
        for ((failure, expected) in refusals) {
            service.dispatchFailure = failure
            DsdNative.diagnostics.clear()
            val status = DecoderService.startDecoder(service, RTL, 41L)
            check(status == expected) { "${failure.javaClass.simpleName}: status $status, expected $expected" }
            check("service: start attempt 41 could not be handed over (status $expected)" in DsdNative.diagnostics) {
                DsdNative.diagnostics
            }
        }
        service.dispatchFailure = null
        service.dispatchReturnsNull = true
        check(DecoderService.startDecoder(service, RTL, 42L) == DecoderService.START_NO_SERVICE) {
            "a start no service received was not handed over"
        }
        service.dispatchReturnsNull = false
    } finally {
        Build.VERSION.SDK_INT = sdk
        service.dispatchFailure = null
        service.dispatchReturnsNull = false
    }
    check(service.intents.isEmpty()) { "a refused START was queued" }
    // A failed JNI call answers 0, so 0 must never read as handed over: every status is nonzero and distinct.
    val statuses = listOf(
        DecoderService.START_HANDED_OVER,
        DecoderService.START_NOT_ALLOWED,
        DecoderService.START_ILLEGAL_STATE,
        DecoderService.START_SECURITY,
        DecoderService.START_NO_SERVICE,
        DecoderService.START_UNEXPECTED
    )
    check(statuses.distinct().size == statuses.size && 0 !in statuses) { "start statuses: $statuses" }
    service.onDestroy()
}

fun main(args: Array<String>) {
    val tests = linkedMapOf<String, () -> Unit>(
        "timeout_dismiss" to ::timeoutThenDismiss,
        "timeout_retry" to ::timeoutThenRetry,
        "timeout_other_source" to ::timeoutThenAnotherSource,
        "retire_during_configure" to ::retireDuringConfigure,
        "retire_before_publication" to ::retireJustBeforeWorkerPublication,
        "stale_idle_snapshot" to ::timeoutFromAnEarlierIdleSnapshot,
        "record" to ::theRecordNamesTheAttempt,
        "earlier_process" to ::anEarlierProcessStartIsRejected,
        "roam_superseded_before_acceptance" to ::roamSupersededBeforeAcceptance,
        "roam_superseded_while_busy" to ::roamSupersededWhileBusy,
        "roam_superseded_before_configure" to ::roamSupersededBeforeConfigure,
        "roam_superseded_during_configure" to ::roamSupersededDuringConfigure,
        "roam_superseded_before_publication" to ::roamSupersededJustBeforePublication,
        "roam_superseded_then_setup_fails" to ::roamSupersededThenSetupFails,
        "roam_cancel_matrix" to ::roamCancelMatrix,
        "roam_cancelled_while_busy" to ::roamCancelledWhileBusy,
        "manual_start_ignores_the_fences" to ::manualStartIgnoresTheFences,
        "configure_failure_waits_for_the_verdict" to ::aConfigureFailureWaitsForTheServiceVerdict,
        "roam_retired_before_commit" to ::roamRetiredBeforeCommit,
        "roam_unchanged_generation_runs" to ::roamWithAnUnchangedGenerationRuns,
        "start_status" to ::theStartStatusSaysWhetherItWasHandedOver
    )
    for ((name, test) in tests) {
        if (args.isNotEmpty() && name !in args) continue
        test()
        println("DecoderServiceAttemptTest: $name OK")
    }
}
