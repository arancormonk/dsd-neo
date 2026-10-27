// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

import android.app.NotificationManager
import android.content.Intent
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

fun main(args: Array<String>) {
    val tests = linkedMapOf<String, () -> Unit>(
        "timeout_dismiss" to ::timeoutThenDismiss,
        "timeout_retry" to ::timeoutThenRetry,
        "timeout_other_source" to ::timeoutThenAnotherSource,
        "retire_during_configure" to ::retireDuringConfigure,
        "retire_before_publication" to ::retireJustBeforeWorkerPublication,
        "stale_idle_snapshot" to ::timeoutFromAnEarlierIdleSnapshot,
        "record" to ::theRecordNamesTheAttempt
    )
    for ((name, test) in tests) {
        if (args.isNotEmpty() && name !in args) continue
        test()
        println("DecoderServiceAttemptTest: $name OK")
    }
}
