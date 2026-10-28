// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

private fun recordsWaitUntilReadyThenGoInOrder() {
    val delivered = mutableListOf<String>()
    val pending = PendingHostRecords(64) { delivered += it }
    pending.submit("first")
    pending.submit("second")
    check(delivered.isEmpty()) { "nothing reaches the native side before it is ready" }
    check(pending.heldCount == 2)
    pending.markReady()
    check(delivered.isEmpty()) { "markReady only marks; the drain sends" }
    pending.drain()
    check(delivered == listOf("first", "second"))
    pending.submit("third")
    check(delivered == listOf("first", "second", "third")) { "once ready, a record goes straight out" }
    pending.drain()
    check(delivered.size == 3)
}

private fun aReadyBufferThatWasNotDrainedSendsBeforeTheNextRecord() {
    val delivered = mutableListOf<String>()
    val pending = PendingHostRecords(64) { delivered += it }
    pending.submit("held")
    pending.markReady()
    pending.submit("live")
    check(delivered == listOf("held", "live"))
}

private fun theBufferIsBoundedAndCountsWhatItDropped() {
    val delivered = mutableListOf<String>()
    val pending = PendingHostRecords(HostDiagnosticsCapacity) { delivered += it }
    repeat(HostDiagnosticsCapacity + 10) { pending.submit("record $it") }
    check(pending.heldCount == HostDiagnosticsCapacity)
    pending.markReady()
    pending.drain()
    check(delivered.size == HostDiagnosticsCapacity + 1)
    check(delivered.first() == "record 0" && delivered[HostDiagnosticsCapacity - 1] == "record 63") {
        "the first records are kept: an incident's header and stack come first"
    }
    check(delivered.last() == "diagnostics: 10 host records dropped while waiting for native diagnostics")
    pending.submit("after")
    check(delivered.last() == "after") { "the count is reported once" }
}

private fun twoThreadsKeepEveryRecord() {
    val delivered = java.util.Collections.synchronizedList(mutableListOf<String>())
    val pending = PendingHostRecords(64) { delivered += it }
    repeat(32) { pending.submit("early $it") }
    val threads = (0 until 2).map { thread ->
        Thread {
            repeat(500) { pending.submit("t$thread $it") }
        }
    }
    threads.forEach { it.start() }
    pending.markReady()
    pending.drain()
    threads.forEach { it.join() }
    pending.drain()
    val early = delivered.filter { it.startsWith("early ") }
    check(early == (0 until 32).map { "early $it" }) { "held records go out first, in order" }
    // Every record is delivered or counted as dropped, and each thread's keep their order.
    val dropped = delivered.filter { it.startsWith("diagnostics: ") }.sumOf { it.split(' ')[1].toInt() }
    check(delivered.count { !it.startsWith("diagnostics: ") } + dropped == 32 + 2 * 500)
    for (thread in 0 until 2) {
        val mine = delivered.filter { it.startsWith("t$thread ") }.map { it.substringAfter(' ').toInt() }
        check(mine == mine.sorted()) { "thread $thread's records out of order" }
    }
}

// The class-initialization cycle the app could hit (#782 review): in the app, delivery is a call into DsdNative,
// which waits for DsdNative's class initialization if another thread is running it, and that initialization marks
// these records ready. StubNative plays DsdNative with a real JVM class initializer.
private lateinit var cyclePending: PendingHostRecords
private val initStarted = java.util.concurrent.CountDownLatch(1)
private val deliverEntered = java.util.concurrent.CountDownLatch(1)
private val cycleDelivered = java.util.Collections.synchronizedList(mutableListOf<String>())

private object StubNative {
    init {
        initStarted.countDown()
        // Another thread is now inside deliver and about to touch this class, which waits for this initializer.
        deliverEntered.await(5, java.util.concurrent.TimeUnit.SECONDS)
        // As DsdNative's init does through HostDiagnostics.nativeReady().
        if (!cyclePending.isReady) {
            cyclePending.markReady()
        }
    }

    fun take(line: String) {
        cycleDelivered += line
    }
}

private fun deliveryNeverHoldsTheLockAClassInitializerNeeds() {
    cyclePending = PendingHostRecords(64) { line ->
        deliverEntered.countDown()
        StubNative.take(line)
    }
    cyclePending.markReady() // As ProcessFacts.processFacts() does, before DsdNative is first used.
    val initializer = Thread { StubNative.take("initializer") }.apply { isDaemon = true }
    val watchdog = Thread {
        initStarted.await()
        cyclePending.submit("incident")
    }.apply { isDaemon = true }
    initializer.start()
    watchdog.start()
    initializer.join(3_000)
    watchdog.join(3_000)
    check(!initializer.isAlive && !watchdog.isAlive) {
        "deadlock: delivery held the lock while waiting for a class initializer that waits for the lock"
    }
    check(cycleDelivered.toSet() == setOf("incident", "initializer")) { cycleDelivered.toString() }
}

/** The app's buffer size, as HostDiagnostics.CAPACITY says; HostDiagnostics itself needs the native library. */
private const val HostDiagnosticsCapacity = 64

fun main() {
    recordsWaitUntilReadyThenGoInOrder()
    aReadyBufferThatWasNotDrainedSendsBeforeTheNextRecord()
    theBufferIsBoundedAndCountsWhatItDropped()
    twoThreadsKeepEveryRecord()
    deliveryNeverHoldsTheLockAClassInitializerNeeds()
    println("PASS: pending host records wait, bound and drain in order")
}
