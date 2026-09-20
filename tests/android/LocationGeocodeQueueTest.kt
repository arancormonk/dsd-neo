// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.CountDownLatch
import java.util.concurrent.ThreadPoolExecutor
import java.util.concurrent.TimeUnit

private fun await(latch: CountDownLatch) {
    check(latch.await(5, TimeUnit.SECONDS)) { "executor regression timed out" }
}

/** Deliver even removed tasks to simulate a worker that dequeued just before cancellation. */
private class DelayedExecutor : ThreadPoolExecutor(1, 1, 0, TimeUnit.MILLISECONDS, ArrayBlockingQueue(1)) {
    val delivered = mutableListOf<Runnable>()
    override fun execute(command: Runnable) {
        delivered.add(command)
    }
}

private fun cancelledBeforeInvocation() {
    val executor = DelayedExecutor()
    val worker = LocationGeocodeQueue(executor)
    val calls = mutableListOf<Int>()
    val old = worker.submit { calls.add(1) }
    val current = worker.submit { calls.add(2) }
    worker.cancel(old) // Retiring an older request must not retire its replacement.
    executor.delivered.forEach { it.run() }
    check(calls == listOf(2)) { "a retired job invoked the geocoder" }
    worker.cancel(current)
    executor.shutdownNow()
}

/** A native call can ignore interruption; stale queued jobs must still be removed promptly. */
private fun blockedFirstJob() {
    val executor = ThreadPoolExecutor(1, 1, 0, TimeUnit.MILLISECONDS, ArrayBlockingQueue<Runnable>(1))
    val worker = LocationGeocodeQueue(executor)
    val started = CountDownLatch(1)
    val unblock = CountDownLatch(1)
    val finished = CountDownLatch(1)
    val calls = CopyOnWriteArrayList<Int>()
    try {
        worker.submit {
            calls.add(1)
            started.countDown()
            var released = false
            while (!released) {
                try {
                    unblock.await()
                    released = true
                } catch (_: InterruptedException) { /* Simulate an uninterruptible platform geocoder. */ }
            }
        }
        await(started)
        for (id in 2..20) {
            val queued = worker.submit { calls.add(id) }
            check(executor.queue.size == 1) { "pending work must be bounded to one queued job" }
            // Exercises the common retirement used by timeout, replacement and teardown.
            if (id % 2 == 0) {
                worker.cancel(queued)
                check(queued.isCancelled) { "retirement must cancel its future" }
                check(executor.queue.isEmpty()) { "retirement left geocoding queued" }
            }
        }
        worker.submit {
            calls.add(99)
            finished.countDown()
        }
        check(executor.queue.size == 1)
        unblock.countDown()
        await(finished)
        check(calls == listOf(1, 99)) { "cancelled queued jobs delayed the current request" }
    } finally {
        unblock.countDown()
        executor.shutdownNow()
        check(executor.awaitTermination(5, TimeUnit.SECONDS))
    }
}

fun main() {
    cancelledBeforeInvocation()
    blockedFirstJob()
    println("PASS: retired jobs are removed; only the current geocoder runs after a blocked job")
}
