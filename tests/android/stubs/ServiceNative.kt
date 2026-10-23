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
    var nativeStopCode = STATUS_OK
    val stops = CopyOnWriteArrayList<Boolean>()
    val diagnostics = CopyOnWriteArrayList<String>()
    fun prepareRun() {
        enteredRun = CountDownLatch(1)
        finishRun = CountDownLatch(1)
        configureHook = null
        nativeStopCode = STATUS_OK
        stops.clear()
        diagnostics.clear()
    }
    fun nativeInit(config: String, cache: String) = STATUS_OK
    fun nativeConfigure(args: Array<String>, id: Long): Int { configureHook?.invoke(); return STATUS_OK }
    fun nativeRun(): Int {
        val finish = finishRun
        enteredRun.countDown()
        check(finish.await(5, TimeUnit.SECONDS)) { "test did not release the engine" }
        return STATUS_OK
    }
    fun nativeStop(userRequested: Boolean): Int { stops += userRequested; return nativeStopCode }
    fun nativeDestroy() = STATUS_OK
    fun nativeLifecycleStatus(): LongArray? = null
    fun nativeHostDiagnostic(line: String) { diagnostics += line }
    fun nativeArmStartupDiagnostics() {}
    fun nativeFlushDiagnostics() {}
    fun nativeNotificationStatus(): String? = null
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
    }
}
