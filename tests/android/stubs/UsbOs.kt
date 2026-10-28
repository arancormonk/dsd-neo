// SPDX-License-Identifier: GPL-3.0-or-later
package android.os

import java.util.concurrent.LinkedBlockingQueue

object Build {
    object VERSION { var SDK_INT = 30 }
    object VERSION_CODES { const val TIRAMISU = 33 }
}

// The broker's clock, moved by the test only.
object SystemClock {
    @Volatile var nowMs = 1_000_000L
    fun elapsedRealtime() = nowMs
}

// The main thread is the test's own (the thread that set [mainThread]); any other thread is the Qt thread or a worker.
class Looper private constructor() {
    companion object {
        private val main = Looper()
        @Volatile var mainThread: Thread? = null
        fun getMainLooper() = main
        fun myLooper(): Looper? = if (Thread.currentThread() === mainThread) main else null
    }
}

// Posts from other threads wait here until the test runs them on the main thread, as its looper would.
class Handler(looper: Looper) {
    companion object {
        val pending = LinkedBlockingQueue<Runnable>()
        fun drain() { while (true) (pending.poll() ?: return).run() }
    }
    fun post(task: Runnable): Boolean { pending.add(task); return true }
}
