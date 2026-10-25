// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

import android.content.Context
import android.content.Intent
import android.os.Handler
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import org.json.JSONObject

private var nextStartId = 1
private const val STOP_SESSION = "io.github.arancormonk.dsdneo.extra.STOP_SESSION"

private fun await(latch: CountDownLatch) {
    check(latch.await(5, TimeUnit.SECONDS)) { "service test boundary timed out" }
}

private fun start(service: DecoderService): Int {
    val id = nextStartId++
    service.onStartCommand(Intent().setAction(DecoderService.ACTION_START), 0, id)
    await(DsdNative.enteredRun)
    check(DecoderService.stateName() == "RUNNING")
    return id
}

private fun deliver(service: DecoderService, intent: Intent) {
    service.onStartCommand(intent, 0, nextStartId++)
}

private fun stopIntent(cause: StopCause? = StopCause.USER): Intent =
    Intent().setAction(DecoderService.ACTION_STOP).also {
        cause?.let { cause -> it.putExtra(DecoderService.EXTRA_STOP_CAUSE, cause.wire) }
    }

private fun finish(service: DecoderService) {
    service.beforeForeground = null
    val completed = CountDownLatch(1)
    service.beforeStopSelf = { completed.countDown() }
    DsdNative.finishRun.countDown()
    await(completed)
    service.onDestroy()
    check(DecoderService.stateName() == "IDLE")
    check(service.power.created.none { it.isHeld }) { "a completed run must release its wake lock" }
    Handler.pending.clear()
    Handler.timeouts.clear()
}

private fun withRunningService(test: (DecoderService) -> Unit) {
    DsdNative.prepareRun()
    val service = DecoderService()
    service.onCreate()
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
    DsdNative.prepareRun()
    val service = DecoderService()
    service.onCreate()
    start(service)
    DecoderService.stopDecoder(service)
    val delayed = service.intents.remove()
    finish(service)
    DsdNative.prepareRun()
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

private fun completedRunKeepsItsOwnStopId() {
    DsdNative.prepareRun()
    val service = DecoderService()
    service.onCreate()
    val oldStart = start(service)
    val atStopSelf = CountDownLatch(1)
    val continueStopSelf = CountDownLatch(1)
    val oldStopped = CountDownLatch(1)
    service.beforeStopSelf = { id ->
        check(id == oldStart)
        atStopSelf.countDown()
        await(continueStopSelf)
        oldStopped.countDown()
    }
    DsdNative.finishRun.countDown()
    await(atStopSelf)
    DsdNative.prepareRun()
    val newStart = start(service)
    try {
        continueStopSelf.countDown()
        await(oldStopped)
        check(newStart > oldStart)
        check(DsdNative.stops.isEmpty())
        check(DecoderService.stateName() == "RUNNING")
    } finally {
        continueStopSelf.countDown()
        finish(service)
    }
}

private fun aStartInNotificationWorkCannotLendItsIdToTheOldRun() {
    DsdNative.prepareRun()
    val service = DecoderService()
    service.onCreate()
    val oldStart = start(service)
    val completed = CountDownLatch(1)
    service.beforeStopSelf = { completed.countDown() }
    service.beforeForeground = {
        // A new START has entered onStartCommand, but not reached its start decision.
        // Let the previous engine finish while notification promotion is in progress.
        DsdNative.finishRun.countDown()
        await(completed)
        DsdNative.prepareRun()
    }
    start(service)
    try {
        check(service.stoppedIds.toList() == listOf(oldStart)) { "completion must not borrow the arriving start's id" }
        check(DecoderService.stateName() == "RUNNING")
    } finally {
        finish(service)
    }
}

// #647: the native record names a session's receiver only once configure returns, so until then the service says whether
// the session it is configuring takes a local RTL-SDR or Airspy. A USB detach in that window belongs to it, and is
// stopped again once configure returns: configure's flag reset can swallow the detach's own stop.
private fun configuringALocalReceiverSessionOwnsItsDetach() {
    val cases = listOf(
        arrayOf("-f", "a", "-i", "rtl:0:851.0125M:22:-2:24:0:2") to true,
        arrayOf("-i", "rtl") to true,
        arrayOf("-i", "airspy:26A464DC:851.0125M:12:0:0") to true,
        arrayOf("-i", "rtltcp:10.0.2.2:1234:851.0125M") to false,
        arrayOf("-i", "udp:0.0.0.0:7355") to false,
        arrayOf("-i", "/sdcard/capture.wav") to false,
        arrayOf("-i", "rtltcp:10.0.2.2:1234", "-i", "rtl:0") to true,
        emptyArray<String>() to false,
    )
    for ((args, local) in cases) {
        for (detach in listOf(true, false)) {
            DsdNative.prepareRun()
            val service = DecoderService()
            service.onCreate()
            var during: Boolean? = null
            DsdNative.configureHook = {
                if (detach) {
                    during = DecoderService.detachWhileConfiguring()
                    check(DsdNative.stopCauses.isEmpty()) { "the service stops nothing while configure runs" }
                }
            }
            check(!DecoderService.detachWhileConfiguring()) { "nothing is configuring before the start" }
            DecoderService.startDecoder(service, args)
            deliver(service, service.intents.remove())
            await(DsdNative.enteredRun)
            val what = args.joinToString(" ")
            try {
                if (detach) check(during == local) { "configuring $what: expected $local, got $during" }
                val stopped = if (detach && local) listOf("detached") else emptyList()
                check(DsdNative.stopCauses == stopped) { "configuring $what, detach $detach: stops ${DsdNative.stopCauses}" }
                check(DsdNative.stops == stopped.map { false }) { "an unplug drains the uploads" }
                check(!DecoderService.detachWhileConfiguring()) { "once configure returns, the native record answers" }
            } finally {
                finish(service)
            }
        }
    }
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
        "configure_receiver" to ::configuringALocalReceiverSessionOwnsItsDetach,
        "pending_start" to ::stopBeforeStartDeliveryStillStopsThatStart,
        "completion" to ::completedRunKeepsItsOwnStopId,
        "arriving_start" to ::aStartInNotificationWorkCannotLendItsIdToTheOldRun
    )
    for ((name, test) in tests) {
        if (args.isNotEmpty() && name !in args) continue
        test()
        println("DecoderServiceStopTest: $name OK")
    }
}
