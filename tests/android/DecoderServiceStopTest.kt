// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

import android.app.NotificationManager
import android.content.Context
import android.content.Intent
import android.os.Handler
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import org.json.JSONObject

private var nextStartId = 1
private const val STOP_SESSION = "io.github.arancormonk.dsdneo.extra.STOP_SESSION"
private const val NOTIFICATION_ID = 1
private const val STATUS_POLL_MS = 1000L
private val shade get() = NotificationManager.device

private fun await(latch: CountDownLatch) {
    check(latch.await(5, TimeUnit.SECONDS)) { "service test boundary timed out" }
}

private fun start(service: DecoderService): Int {
    val id = nextStartId++
    service.deliver(Intent().setAction(DecoderService.ACTION_START), id)
    await(DsdNative.enteredRun)
    check(DecoderService.stateName() == "RUNNING")
    return id
}

private fun deliver(service: DecoderService, intent: Intent): Int {
    val id = nextStartId++
    service.deliver(intent, id)
    return id
}

/** Runs the main queue, as its looper would, until [done]; a run's end reaches the service through it. */
private fun pumpUntil(what: String, done: () -> Boolean) {
    val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5)
    while (!done()) {
        check(System.nanoTime() < deadline) { "service test boundary timed out: $what" }
        Handler.pending.poll(10, TimeUnit.MILLISECONDS)?.run()
    }
}

/** The engine worker's last act, taken off the main queue unrun; nothing else may be queued. */
private fun awaitSessionEnd(): Runnable =
    checkNotNull(Handler.pending.poll(5, TimeUnit.SECONDS)) { "the engine worker never handed over its run's end" }

/** Waits for the engine worker to finish its tail, without running anything on the main queue. */
private fun joinEngineWorker() {
    for (worker in Thread.getAllStackTraces().keys.filter { it.name == "dsd-neo-engine" }) {
        worker.join(5000)
        check(!worker.isAlive) { "the engine worker did not finish" }
    }
}

private fun newService(): DecoderService {
    DsdNative.prepareRun()
    shade.clear()
    return DecoderService().also { it.onCreate() }
}

private fun settle(service: DecoderService) {
    check(DecoderService.stateName() == "IDLE")
    check(service.power.created.none { it.isHeld }) { "a completed run must release its wake lock" }
    Handler.pending.clear()
    Handler.timeouts.clear()
}

private fun stopIntent(cause: StopCause? = StopCause.USER): Intent =
    Intent().setAction(DecoderService.ACTION_STOP).also {
        cause?.let { cause -> it.putExtra(DecoderService.EXTRA_STOP_CAUSE, cause.wire) }
    }

private fun finish(service: DecoderService) {
    service.beforeForeground = null
    service.beforeStopSelf = null
    val stopped = service.stoppedIds.size
    DsdNative.finishRun.countDown()
    pumpUntil("the run's end") { service.stoppedIds.size > stopped }
    service.onDestroy()
    settle(service)
}

private fun withRunningService(test: (DecoderService) -> Unit) {
    val service = newService()
    start(service)
    try {
        test(service)
    } finally {
        finish(service)
    }
}

private fun stopDoesNotWaitForIntentDelivery() = withRunningService { service ->
    DecoderService.stopDecoder(service)
    check(DsdNative.stops == listOf(true)) { "Stop must reach native code before any service callback" }
    check(DecoderService.stateName() == "STOPPING")
    check(service.intents.size == 1) { "notification bookkeeping is still queued" }
    val queued = service.intents.peek()
    check(queued.getLongExtra(STOP_SESSION, 0) == JSONObject(DecoderService.lifecycleStatus()).optLong("sessionId"))
}

private fun rejectedDispatchStillCancelsTheRun() {
    for (failure in listOf(IllegalStateException("background restriction"), SecurityException("refused"))) {
        withRunningService { service ->
            service.dispatchFailure = failure
            DecoderService.stopDecoder(service)
            check(DsdNative.stops == listOf(true))
            check(DecoderService.stateName() == "STOPPING")
            check(DsdNative.diagnostics.any { it.contains("dispatch failed") && it.contains(failure.javaClass.simpleName) })
        }
    }
    withRunningService { service ->
        service.dispatchReturnsNull = true
        DecoderService.stopDecoder(service)
        check(DsdNative.stops == listOf(true))
        check(DsdNative.diagnostics.any { it.contains("no component") })
    }
}

private fun aMissingActivityStillAllowsCancellation() = withRunningService { _ ->
    // Reflection keeps this case runnable against the previous non-null signature as well.
    DecoderService::class.java.getMethod("stopDecoder", Context::class.java).invoke(null, null)
    check(DsdNative.stops == listOf(true))
    check(DecoderService.stateName() == "STOPPING")
    check(DsdNative.diagnostics.any { it.contains("no Android context") })
}

private fun notificationStopPrecedesNotificationWork() = withRunningService { service ->
    service.beforeForeground = {
        check(DsdNative.stops == listOf(true)) { "notification binder work must not precede nativeStop" }
    }
    deliver(service, stopIntent())
    check(DecoderService.stateName() == "STOPPING")
}

private fun removingTheTaskStopsWithoutActivityDestruction() = withRunningService { service ->
    service.onTaskRemoved(Intent())
    check(DsdNative.stops == listOf(true)) { "removing Recents must stop even without Activity.onDestroy" }
    check(DecoderService.stateName() == "STOPPING")
}

private fun aUserStopEscalatesAnExistingDrain() = withRunningService { service ->
    deliver(service, stopIntent(null))
    check(DsdNative.stops == listOf(false)) { "an unspecified lifecycle stop still drains" }
    check(DecoderService.stateName() == "STOPPING")
    deliver(service, stopIntent())
    check(DsdNative.stops == listOf(false, true)) { "the user's Stop must abandon the ongoing drain" }
    // #647: each stop names its cause; the native side keeps the session's first.
    check(DsdNative.stopCauses == listOf("lifecycle", "user")) { "each stop must pass its own cause" }
}

// #647: the session's ending and its receiver ride the lifecycle record, for its own session only.
private fun theLifecycleRecordCarriesTheReceiverEnding() = withRunningService { _ ->
    val session = JSONObject(DecoderService.lifecycleStatus()).optLong("sessionId")
    try {
        DsdNative.lifecycle = longArrayOf(session, 1, DsdNative.RUN_PENDING.toLong(), 0, 0, 0, 0, 2, 1)
        var record = JSONObject(DecoderService.lifecycleStatus())
        check(record.optLong("endCause") == 2L && record.optLong("receiverSource") == 1L) {
            "the native ending and receiver must reach the record: $record"
        }
        DsdNative.lifecycle = longArrayOf(session + 1, 1, DsdNative.RUN_PENDING.toLong(), 0, 0, 0, 0, 2, 1)
        record = JSONObject(DecoderService.lifecycleStatus())
        check(record.optLong("endCause") == 0L && record.optLong("receiverSource") == 0L) {
            "another session's ending must not reach this record"
        }
        DsdNative.lifecycle = longArrayOf(session, 1, DsdNative.RUN_PENDING.toLong(), 0, 0, 0, 0)
        record = JSONObject(DecoderService.lifecycleStatus())
        check(record.optLong("endCause") == 0L) { "a record without the fields has no ending" }
        // The UI's acknowledgement of an ending outlives its host; only a newer session moves it.
        val before = record.optLong("endingAcknowledged")
        DecoderService.acknowledgeReceiverEnding(session)
        DecoderService.acknowledgeReceiverEnding(session - 1)
        record = JSONObject(DecoderService.lifecycleStatus())
        check(record.optLong("endingAcknowledged") == maxOf(before, session)) { "the acknowledgement is kept: $record" }
    } finally {
        DsdNative.lifecycle = null
    }
}

private fun aRefusedNativeStopRemainsRetryable() = withRunningService { service ->
    DsdNative.nativeStopCode = -2
    DecoderService.stopDecoder(service)
    check(DecoderService.stateName() == "RUNNING") { "a refused cancellation must not latch STOPPING" }
    check(DsdNative.diagnostics.any { it.contains("nativeStop=-2") })
    DsdNative.nativeStopCode = DsdNative.STATUS_OK
    deliver(service, service.intents.remove())
    check(DecoderService.stateName() == "STOPPING")
}

private fun aLateStopCannotCancelTheNextRun() {
    val first = newService()
    start(first)
    DecoderService.stopDecoder(first)
    val delayed = first.intents.remove()
    finish(first)
    // The platform creates a new instance for the next start once the last one is destroyed.
    DsdNative.prepareRun()
    val service = DecoderService()
    service.onCreate()
    start(service)
    try {
        deliver(service, delayed)
        check(DsdNative.stops.isEmpty()) { "old session's deferred Stop must not cancel its replacement" }
        check(DecoderService.stateName() == "RUNNING")
        check(DsdNative.diagnostics.any { it.contains("stale stop") })
    } finally {
        finish(service)
    }
}

private fun stopDuringConfigureUsesOrderedDelivery() {
    DsdNative.prepareRun()
    val service = DecoderService()
    service.onCreate()
    DsdNative.configureHook = {
        DecoderService.stopDecoder(service)
        check(DsdNative.stops.isEmpty()) { "configure can still reset the native stop flags" }
        check(DecoderService.stateName() == "STARTING")
    }
    start(service)
    try {
        deliver(service, service.intents.remove())
        check(DsdNative.stops == listOf(true))
        check(DecoderService.stateName() == "STOPPING")
    } finally {
        finish(service)
    }
}

private fun stopBeforeStartDeliveryStillStopsThatStart() {
    DsdNative.prepareRun()
    val service = DecoderService()
    service.onCreate()
    DecoderService.startDecoder(service, emptyArray())
    DecoderService.stopDecoder(service)
    check(DsdNative.stops.isEmpty())
    deliver(service, service.intents.remove())
    await(DsdNative.enteredRun)
    try {
        deliver(service, service.intents.remove())
        check(DsdNative.stops == listOf(true))
        check(DecoderService.stateName() == "STOPPING")
    } finally {
        finish(service)
    }
}

// The old run's end waits on the main queue behind a START, which the main thread handles first.
private fun completedRunKeepsItsOwnStopId() {
    val service = newService()
    val oldStart = start(service)
    Handler.drain()
    DsdNative.finishRun.countDown()
    val oldEnd = awaitSessionEnd()
    DsdNative.prepareRun()
    val newStart = start(service)
    service.beforeStopSelf = { id -> check(id == oldStart) }
    try {
        oldEnd.run()
        check(service.stoppedIds.toList() == listOf(oldStart)) { "the old run's end must stop with its own id" }
        check(newStart > oldStart)
        check(DsdNative.stops.isEmpty())
        check(DecoderService.stateName() == "RUNNING")
        // The newer run owns the service again: its notification and polling stay.
        check(service.foreground && shade.shown.containsKey(NOTIFICATION_ID)) {
            "the old run's end took the new run's notification"
        }
        check(NOTIFICATION_ID !in shade.cancels)
        check(Handler.pending.isNotEmpty() || Handler.timeouts.isNotEmpty()) { "the new run's polling was removed" }
    } finally {
        finish(service)
    }
}

private fun aStartInNotificationWorkCannotLendItsIdToTheOldRun() {
    val service = newService()
    val oldStart = start(service)
    Handler.drain()
    var oldEnd: Runnable? = null
    service.beforeForeground = {
        // A new START has entered onStartCommand, but not reached its start decision.
        // Let the previous engine finish while notification promotion is in progress.
        DsdNative.finishRun.countDown()
        oldEnd = awaitSessionEnd()
        DsdNative.prepareRun()
    }
    start(service)
    try {
        // The main thread reaches the old run's end once the START has returned.
        checkNotNull(oldEnd).run()
        check(service.stoppedIds.toList() == listOf(oldStart)) { "completion must not borrow the arriving start's id" }
        check(DecoderService.stateName() == "RUNNING")
    } finally {
        finish(service)
    }
}

// #782 (F9): a run ends on a slow phone. The engine returns while a status tick is already due on the main queue;
// stopSelf() takes the foreground notification down at once; whatever is queued runs; onDestroy() comes later. A tick
// that posted in that window left "Stopping..." in the shade with nothing to remove it.
private fun endTheRunBehindAQueuedTick(service: DecoderService) {
    DsdNative.notificationStatus = "status"
    Handler.drain()
    check(Handler.timeouts.isNotEmpty()) { "the first tick re-arms the poll" }
    Handler.nowMs += STATUS_POLL_MS
    Handler.queueDue()
    val posts = shade.posts.size
    DsdNative.finishRun.countDown()
    joinEngineWorker()
    Handler.drain()
    check(service.stoppedIds.isNotEmpty()) { "the run's end must stop the service" }
    check(!service.foreground)
    service.onDestroy()
    Handler.drain()
    check(shade.shown.isEmpty()) { "a notification outlived the session" }
    check(shade.posts.size == posts) { "nothing may post once the run has ended" }
}

private fun aRunThatEndsLeavesNoNotification() {
    val service = newService()
    start(service)
    endTheRunBehindAQueuedTick(service)
    settle(service)
}

private fun aFailedOpenLeavesNoNotification() {
    val service = newService()
    // The receiver opens inside nativeRun, so a device that fails to open ends the run with a failure.
    DsdNative.runCode = -1
    start(service)
    endTheRunBehindAQueuedTick(service)
    check(JSONObject(DecoderService.lifecycleStatus()).optLong("reason") == DsdNative.RUN_FAILED.toLong())
    settle(service)
}

private fun aFailedOrRefusedStartLeavesNoNotification() {
    val refusals = listOf(
        "configure failure" to { service: DecoderService ->
            DsdNative.configureCode = -3
            deliver(service, Intent().setAction(DecoderService.ACTION_START))
        },
        // A one-shot configuration (help and the like) that ends without a session.
        "configuration exit" to { service: DecoderService ->
            DsdNative.configureCode = DsdNative.STATUS_CONFIG_EXIT
            deliver(service, Intent().setAction(DecoderService.ACTION_START))
        },
        "nativeInit failure" to { service: DecoderService ->
            DsdNative.initCode = -2
            deliver(service, Intent().setAction(DecoderService.ACTION_START))
        },
        "stop with nothing running" to { service: DecoderService ->
            deliver(service, Intent().setAction(DecoderService.ACTION_STOP))
        },
        "unrecognised action" to { service: DecoderService -> deliver(service, Intent().setAction("unknown")) },
    )
    for ((what, refuse) in refusals) {
        val service = newService()
        val id = refuse(service)
        Handler.drain()
        check(service.stoppedIds.toList() == listOf(id)) { "$what: the service must stop against its start" }
        check(!service.foreground) { "$what: the service stayed in the foreground" }
        service.onDestroy()
        Handler.drain()
        check(shade.shown.isEmpty()) { "$what: a notification outlived the start" }
        settle(service)
    }
}

// The Activity's stopService() destroys the service while it runs. The platform drops the foreground first, so the
// "Stopping..." that onDestroy() posts on its way out must not stay behind it.
private fun aServiceDestroyedWhileRunningLeavesNoNotification() {
    val service = newService()
    start(service)
    DsdNative.stopHook = { DsdNative.finishRun.countDown() }
    service.bringDown()
    service.onDestroy()
    // The released run can end before onDestroy() looks for its thread, which then has nothing to join: its session
    // end still reaches the main queue, and must not land in the next case's.
    joinEngineWorker()
    Handler.drain()
    check(DsdNative.stops == listOf(false)) { "a destroyed service drains its run" }
    check(shade.shown.isEmpty()) { "onDestroy() left a notification behind" }
    settle(service)
}

// A join that timed out destroyed the instance that started the run. A START then created a replacement, which the
// still-STOPPING run left in the foreground with no poll. Only its own stopSelf() can stop it.
private fun aReplacementInstanceGetsTheEndOfTheRunItFronts() {
    val old = newService()
    start(old)
    DecoderService.stopDecoder(old)
    deliver(old, old.intents.remove())
    old.bringDown()
    old.onDestroy()
    check(DecoderService.stateName() == "STOPPING") { "the engine is still draining" }
    val replacement = DecoderService()
    replacement.onCreate()
    val id = deliver(replacement, Intent().setAction(DecoderService.ACTION_START))
    check(replacement.foreground && Handler.pending.isEmpty() && Handler.timeouts.isEmpty())
    DsdNative.finishRun.countDown()
    joinEngineWorker()
    Handler.drain()
    check(replacement.stoppedIds.toList() == listOf(id)) { "the run's end must stop the instance that fronts it" }
    check(old.stoppedIds.isEmpty()) { "the destroyed instance has nothing left to stop" }
    check(!replacement.foreground && shade.shown.isEmpty())
    replacement.onDestroy()
    check(shade.shown.isEmpty())
    settle(replacement)
}

// A START refused because a run is RUNNING is not that run's end.
private fun aDuplicateStartKeepsTheRunningNotification() = withRunningService { service ->
    DsdNative.notificationStatus = "status"
    Handler.drain()
    val cancels = shade.cancels.size
    deliver(service, Intent().setAction(DecoderService.ACTION_START))
    check(DecoderService.stateName() == "RUNNING")
    check(service.foreground && shade.shown.containsKey(NOTIFICATION_ID)) {
        "the running session lost its notification"
    }
    check(shade.cancels.size == cancels && service.stoppedIds.isEmpty())
    check(JSONObject(DecoderService.lifecycleStatus()).optLong("reason") == DsdNative.RUN_PENDING.toLong())
    Handler.drain()
    check(Handler.timeouts.isNotEmpty()) { "the running session's polling must go on" }
}

// Each intent is logged as it arrives, with how long it took when its sender stamped it.
private fun receiptsLogTheDelayOfDirectSenders() {
    val service = newService()
    DecoderService.startDecoder(service, emptyArray())
    Handler.nowMs += 40
    val startId = deliver(service, service.intents.remove())
    await(DsdNative.enteredRun)
    val log = DsdNative.diagnostics
    check("service: received START id=$startId state=IDLE delay=40 ms" in log) { log }
    DecoderService.stopDecoder(service)
    Handler.nowMs += 250
    val stopId = deliver(service, service.intents.remove())
    check("service: received STOP id=$stopId state=STOPPING delay=250 ms" in log) { log }
    finish(service)
}

// The notification's Stop is an immutable PendingIntent built once: it has no send time, however old it is.
private fun anAgedNotificationStopLogsNoDelay() = withRunningService { service ->
    val stop = checkNotNull(shade.shown[NOTIFICATION_ID]).actions.single().intent.intent
    Handler.nowMs += 60_000
    DsdNative.diagnostics.clear()
    val id = deliver(service, stop)
    val log = DsdNative.diagnostics
    check("service: received STOP id=$id state=RUNNING delay unavailable" in log) { log }
    check(log.none { "delay=" in it }) { "a PendingIntent has no send time: $log" }
    check(DecoderService.stateName() == "STOPPING")
}

private fun aStopThatFindsNothingRunningIsLogged() {
    val service = newService()
    DecoderService.stopDecoder(service)
    val log = DsdNative.diagnostics
    check(log.any { it.startsWith("Stop session ") && it.endsWith("cause=user state=IDLE: no run to cancel") }) { log }
    val id = deliver(service, service.intents.remove())
    check(log.any { it.startsWith("service: received STOP id=$id state=IDLE delay=") }) { log }
    // The service's own teardown at IDLE, which follows every session end, is no stop anybody asked for.
    service.onDestroy()
    check(log.none { "cause=lifecycle" in it }) { "an idle teardown logged a stop: $log" }
    settle(service)
}

// The acquire is a binder call; the Qt thread's lifecycle reads and stops must not wait behind it.
private fun aSlowWakeLockAcquireBlocksNoLockReader() {
    val service = newService()
    val acquiring = CountDownLatch(1)
    val acquired = CountDownLatch(1)
    service.power.acquireHook = {
        acquiring.countDown()
        await(acquired)
    }
    val startId = nextStartId++
    val starter = Thread { service.deliver(Intent().setAction(DecoderService.ACTION_START), startId) }
    starter.start()
    try {
        await(acquiring)
        val read = CountDownLatch(1)
        Thread {
            DecoderService.lifecycleStatus()
            DecoderService.stopDecoder(service)
            read.countDown()
        }.start()
        check(read.await(2, TimeUnit.SECONDS)) { "a wake-lock acquire blocked lifecycleStatus() or stopDecoder()" }
        // A stop while STARTING is not lost: the intent it sends cancels the run once the start has published it.
        check(DsdNative.diagnostics.any { it.endsWith("cause=user state=STARTING: deferred to the stop intent ordered behind its start") }) {
            DsdNative.diagnostics
        }
    } finally {
        acquired.countDown()
        starter.join(5000)
        service.power.acquireHook = null
    }
    await(DsdNative.enteredRun)
    check(DecoderService.stateName() == "RUNNING")
    check(service.power.created.single().isHeld) { "the run holds its wake lock" }
    deliver(service, service.intents.remove())
    check(DecoderService.stateName() == "STOPPING") { "the stop sent during the acquire still stops the run" }
    finish(service)
}

fun main(args: Array<String>) {
    val tests = linkedMapOf<String, () -> Unit>(
        "direct" to ::stopDoesNotWaitForIntentDelivery,
        "dispatch_failure" to ::rejectedDispatchStillCancelsTheRun,
        "no_activity" to ::aMissingActivityStillAllowsCancellation,
        "notification" to ::notificationStopPrecedesNotificationWork,
        "task_removed" to ::removingTheTaskStopsWithoutActivityDestruction,
        "drain" to ::aUserStopEscalatesAnExistingDrain,
        "receiver_ending" to ::theLifecycleRecordCarriesTheReceiverEnding,
        "native_failure" to ::aRefusedNativeStopRemainsRetryable,
        "stale" to ::aLateStopCannotCancelTheNextRun,
        "configure" to ::stopDuringConfigureUsesOrderedDelivery,
        "pending_start" to ::stopBeforeStartDeliveryStillStopsThatStart,
        "completion" to ::completedRunKeepsItsOwnStopId,
        "arriving_start" to ::aStartInNotificationWorkCannotLendItsIdToTheOldRun,
        "end_notification" to ::aRunThatEndsLeavesNoNotification,
        "failed_open_notification" to ::aFailedOpenLeavesNoNotification,
        "refused_start_notification" to ::aFailedOrRefusedStartLeavesNoNotification,
        "destroyed_notification" to ::aServiceDestroyedWhileRunningLeavesNoNotification,
        "replacement" to ::aReplacementInstanceGetsTheEndOfTheRunItFronts,
        "duplicate_start" to ::aDuplicateStartKeepsTheRunningNotification,
        "receipt_delay" to ::receiptsLogTheDelayOfDirectSenders,
        "receipt_pending_intent" to ::anAgedNotificationStopLogsNoDelay,
        "receipt_idle_stop" to ::aStopThatFindsNothingRunningIsLogged,
        "wake_lock" to ::aSlowWakeLockAcquireBlocksNoLockReader
    )
    for ((name, test) in tests) {
        if (args.isNotEmpty() && name !in args) continue
        test()
        println("DecoderServiceStopTest: $name OK")
    }
}
