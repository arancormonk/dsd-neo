// SPDX-License-Identifier: GPL-3.0-or-later
package android.os
import java.util.concurrent.ConcurrentLinkedQueue
class Bundle
class CancellationSignal { var cancelled = false; fun cancel() { cancelled = true } }
object Build { object VERSION { var SDK_INT = 30 } }
class Looper { companion object { fun getMainLooper() = Looper() } }
class Handler(looper: Looper) {
    companion object {
        val pending = ConcurrentLinkedQueue<Runnable>()
        val timeouts = mutableListOf<Runnable>()
        fun drain() { while (true) (pending.poll() ?: return).run() }
        fun expire() { val due = timeouts.toList(); timeouts.clear(); due.forEach { it.run() }; drain() }
    }
    fun post(task: Runnable): Boolean { pending.add(task); return true }
    fun postDelayed(task: Runnable, delay: Long): Boolean { timeouts.add(task); return true }
    fun removeCallbacks(task: Runnable) { timeouts.remove(task); pending.remove(task) }
}
