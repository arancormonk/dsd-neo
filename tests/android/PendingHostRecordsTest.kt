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
    check(delivered.last() == "diagnostics: 10 host records dropped before native diagnostics were ready")
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

/** The app's buffer size, as HostDiagnostics.CAPACITY says; HostDiagnostics itself needs the native library. */
private const val HostDiagnosticsCapacity = 64

fun main() {
    recordsWaitUntilReadyThenGoInOrder()
    aReadyBufferThatWasNotDrainedSendsBeforeTheNextRecord()
    theBufferIsBoundedAndCountsWhatItDropped()
    twoThreadsKeepEveryRecord()
    println("PASS: pending host records wait, bound and drain in order")
}
