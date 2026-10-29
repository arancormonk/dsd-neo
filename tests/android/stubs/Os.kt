// SPDX-License-Identifier: GPL-3.0-or-later
package android.os
import java.util.concurrent.LinkedBlockingQueue
class Bundle
interface IBinder
// Cancelled from one thread and read on another, as the platform's is.
class CancellationSignal {
    @Volatile var cancelled = false
    fun cancel() { cancelled = true }
    val isCanceled: Boolean get() = cancelled
    fun throwIfCanceled() { if (cancelled) throw OperationCanceledException() }
}
class OperationCanceledException(message: String? = null) : RuntimeException(message ?: "The operation has been canceled.")
object Build {
    object VERSION { var SDK_INT = 30 }
    object VERSION_CODES { const val Q = 29 }
}
object SystemClock {
    fun elapsedRealtimeNanos() = Handler.nowMs * 1_000_000L
    fun uptimeMillis() = Handler.nowMs
}
class Looper { companion object { fun getMainLooper() = Looper() } }
class Handler(looper: Looper) {
    companion object {
        // Blocking, so a test can wait for a post from another thread (an engine worker) at an explicit boundary.
        val pending = LinkedBlockingQueue<Runnable>()
        val timeouts = linkedMapOf<Runnable, Long>()
        var nowMs = 1_200_000L
        fun drain() { while (true) (pending.poll() ?: return).run() }
        fun advanceBy(durationMs: Long) {
            val until = nowMs + durationMs
            while (true) {
                val next = timeouts.minByOrNull { it.value } ?: break
                if (next.value > until) break
                nowMs = next.value
                timeouts.remove(next.key)
                next.key.run()
                drain()
            }
            nowMs = until
            drain()
        }
        fun expire() { timeouts.values.minOrNull()?.let { advanceBy(it - nowMs) } }
        /** Moves every task due by now onto the queue without running it, as a looper holds them. */
        fun queueDue() {
            for (task in timeouts.filterValues { it <= nowMs }.keys) {
                timeouts.remove(task)
                pending.add(task)
            }
        }
    }
    fun post(task: Runnable): Boolean { pending.add(task); return true }
    fun postDelayed(task: Runnable, delay: Long): Boolean { timeouts[task] = nowMs + delay; return true }
    fun removeCallbacks(task: Runnable) { timeouts.remove(task); pending.remove(task) }
}
