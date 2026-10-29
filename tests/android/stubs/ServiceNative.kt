// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

import android.content.Context
import java.util.concurrent.CountDownLatch
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.TimeUnit

// The real service runs its real worker. Native work is held at explicit boundaries, never by timing sleeps.
object DsdNative {
    const val STATUS_OK = 0
    const val STATUS_CONFIG_EXIT = 1
    const val RUN_PENDING = 0
    const val RUN_COMPLETED = 1
    const val RUN_CANCELLED = 2
    const val RUN_FAILED = 3
    var enteredRun = CountDownLatch(1)
    var finishRun = CountDownLatch(1)
    var configureHook: (() -> Unit)? = null
    var configureCode = STATUS_OK
    /** What nativeInit returns. */
    var initCode = STATUS_OK
    var nativeStopCode = STATUS_OK
    /** Runs after every nativeStop, as a run that honours the stop at once would. */
    var stopHook: (() -> Unit)? = null
    /** What nativeRun returns once released. */
    @Volatile var runCode = STATUS_OK
    /** The notification status record the service polls. */
    @Volatile var notificationStatus: String? = null
    val stops = CopyOnWriteArrayList<Boolean>()
    val stopCauses = CopyOnWriteArrayList<String>()
    val diagnostics = CopyOnWriteArrayList<String>()
    /** The argv of every configure, in order. */
    val configured = CopyOnWriteArrayList<List<String>>()
    /** Configured sessions the service gave up (nativeDiscardConfigured). */
    @Volatile var discards = 0
    /** Runs on every diagnostics line as it is logged, on the thread logging it: a boundary inside a start. */
    @Volatile var diagnosticHook: ((String) -> Unit)? = null
    fun prepareRun() {
        enteredRun = CountDownLatch(1)
        finishRun = CountDownLatch(1)
        configureHook = null
        configureCode = STATUS_OK
        initCode = STATUS_OK
        nativeStopCode = STATUS_OK
        stopHook = null
        runCode = STATUS_OK
        notificationStatus = null
        stops.clear()
        stopCauses.clear()
        lifecycle = null
        diagnostics.clear()
        configured.clear()
        discards = 0
        diagnosticHook = null
        UsbSourceManager.reset()
    }
    fun nativeInit(config: String) = initCode
    fun nativeConfigure(args: Array<String>, id: Long): Int {
        configured += args.toList()
        configureHook?.invoke()
        return configureCode
    }
    fun nativeDiscardConfigured(): Int { discards++; return STATUS_OK }
    fun nativeRun(): Int {
        val finish = finishRun
        enteredRun.countDown()
        check(finish.await(5, TimeUnit.SECONDS)) { "test did not release the engine" }
        return runCode
    }
    fun nativeStop(userRequested: Boolean, cause: String): Int {
        stops += userRequested
        stopCauses += cause
        stopHook?.invoke()
        return nativeStopCode
    }
    fun nativeDestroy() = STATUS_OK
    @Volatile var lifecycle: LongArray? = null
    fun nativeLifecycleStatus(): LongArray? = lifecycle
    fun nativeHostDiagnostic(line: String) {
        diagnostics += line
        diagnosticHook?.invoke(line)
    }
    fun nativeArmStartupDiagnostics() {}
    fun nativeFlushDiagnostics() {}
    fun nativeNotificationStatus(): String? = notificationStatus
}

// The USB broker as the service's start sees it (#782): each take of a local receiver is recorded and answered as the
// test scripted, on the thread that asked.
object UsbSourceManager {
    /** The receiver kind of every take, in order. */
    val takes = CopyOnWriteArrayList<String>()
    /** Sessions whose end the worker reported, so a reset the receiver owes is kept (#782). */
    @Volatile var sessionEnds = 0
    @Volatile internal var answer: (String) -> UsbSessionTake = { UsbSessionTake() }
    fun reset() {
        takes.clear()
        sessionEnds = 0
        answer = { UsbSessionTake() }
    }
    fun noteSessionEnded() { sessionEnds++ }
    internal fun takeForSession(context: Context, source: String): UsbSessionTake {
        takes += source
        return answer(source)
    }
}

object ProcessFacts {
    fun attach(context: Context) {}
}

object ScreenSupport {
    fun statusStopped() {}
    fun statusTick(context: Context, session: Long, status: DecoderStatus?) {}
}

object R {
    object drawable { const val ic_stat_dsdneo = 1 }
    object string {
        const val action_stop = 1
        const val app_name = 2
        const val channel_name = 3
        const val decoder_emergency_prefix = 4
        const val decoder_listening = 5
        const val decoder_running = 6
        const val decoder_starting = 7
        const val decoder_stopping = 8
        const val error_busy = 9
        const val error_configure_failed = 10
        const val error_init_failed = 11
        const val notif_alg = 12
        const val notif_control_freq = 13
        const val notif_encrypted = 14
        const val notif_freq_mhz = 15
        const val notif_kid = 16
        const val notif_separator = 17
        const val notif_slot = 18
        const val notif_unit = 19
        const val error_usb_gone = 20
        const val error_usb_permission = 21
        const val error_usb_open_failed = 22
        const val notif_private = 23
        const val notif_radio_id = 24
        const val notif_private_call = 25
        const val notif_protocol_call = 26
        const val notif_identity_not_decoded = 27
    }
}
