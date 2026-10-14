// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream

/** [size] bytes counting up from 0, so a copy's content shows where it came from. */
private fun bytes(size: Int) = ByteArray(size) { it.toByte() }

/** Sizes on both sides of the chunk boundaries, where an off-by-one in the cap would hide. */
private val SIZES = intArrayOf(1, DocumentTransfer.CHUNK_BYTES - 1, DocumentTransfer.CHUNK_BYTES,
    DocumentTransfer.CHUNK_BYTES + 1, 3 * DocumentTransfer.CHUNK_BYTES + 7)

/** Counts reads, so a test can tell whether a copy touched its input at all. */
private class CountingInput(private val inner: InputStream) : InputStream() {
    var reads = 0
        private set

    override fun read(): Int {
        reads++
        return inner.read()
    }

    override fun read(b: ByteArray, off: Int, len: Int): Int {
        reads++
        return inner.read(b, off, len)
    }
}

/** Gives [first] bytes, then fails every read, as a provider that drops the connection mid-download does. */
private class FailingInput(first: Int) : InputStream() {
    private val inner = ByteArrayInputStream(bytes(first))

    override fun read(): Int = throw IOException("the provider went away")

    override fun read(b: ByteArray, off: Int, len: Int): Int {
        val n = inner.read(b, off, len)
        if (n < 0) throw IOException("the provider went away")
        return n
    }
}

/** Takes [room] bytes, then fails every write, as a full disk does. */
private class FailingOutput(private var room: Int) : OutputStream() {
    override fun write(b: Int) = write(byteArrayOf(b.toByte()), 0, 1)

    override fun write(b: ByteArray, off: Int, len: Int) {
        if (len > room) throw IOException("no space left")
        room -= len
    }
}

private fun copy(size: Int, maxBytes: Long): Pair<Long, ByteArray> {
    val output = ByteArrayOutputStream()
    val copied = DocumentTransfer.copyCapped(ByteArrayInputStream(bytes(size)), output, maxBytes)
    return copied to output.toByteArray()
}

private fun failsWithIOException(block: () -> Unit): Boolean = try {
    block()
    false
} catch (e: IOException) {
    true
}

private fun aSourceExactlyAtTheCapCopiesWhole() {
    for (size in SIZES) {
        val (copied, written) = copy(size, size.toLong())
        check(copied == size.toLong()) { "$size bytes under a $size-byte cap copy whole, not $copied" }
        check(written.contentEquals(bytes(size))) { "$size bytes under a $size-byte cap arrive unchanged" }
        check(copy(size, Long.MAX_VALUE).first == size.toLong()) { "$size bytes copy whole with no real cap" }
    }
}

private fun oneByteOverTheCapFails() {
    for (size in SIZES) {
        val (copied, written) = copy(size + 1, size.toLong())
        check(copied == -1L) { "${size + 1} bytes under a $size-byte cap are too large, not $copied" }
        check(written.size <= size) { "nothing past a $size-byte cap is written (${written.size} were)" }
    }
    check(copy(1, 0).first == -1L) { "a single byte is over a zero cap" }
}

private fun anEmptyStreamCopiesNothing() {
    val (copied, written) = copy(0, 0)
    check(copied == 0L && written.isEmpty()) { "an empty stream fits a zero cap and writes nothing" }
    check(copy(0, 1_000).first == 0L) { "an empty stream copies zero bytes" }
}

private fun aReadOrWriteErrorFails() {
    check(failsWithIOException { DocumentTransfer.copyCapped(FailingInput(0), ByteArrayOutputStream(), 1_000) }) {
        "a read error before any data fails the copy"
    }
    check(failsWithIOException {
        DocumentTransfer.copyCapped(FailingInput(DocumentTransfer.CHUNK_BYTES + 5), ByteArrayOutputStream(), 1L shl 30)
    }) { "a read error after some data fails the copy instead of passing it off as the whole document" }
    check(failsWithIOException {
        DocumentTransfer.copyCapped(ByteArrayInputStream(bytes(3 * DocumentTransfer.CHUNK_BYTES)),
            FailingOutput(DocumentTransfer.CHUNK_BYTES), 1L shl 30)
    }) { "a write error fails the copy" }
}

/** Last: the abort is sticky for the rest of the process. */
private fun anAbortStopsACopyAtTheNextChunkAndRefusesNewOnes() {
    check(!DocumentTransfer.isAborted) { "nothing is aborted at first" }
    val source = bytes(3 * DocumentTransfer.CHUNK_BYTES)
    // Teardown lands while the first chunk is being read.
    val input = object : InputStream() {
        private val inner = ByteArrayInputStream(source)

        override fun read(): Int = inner.read()

        override fun read(b: ByteArray, off: Int, len: Int): Int {
            val n = inner.read(b, off, len)
            DocumentTransfer.abort()
            return n
        }
    }
    val output = ByteArrayOutputStream()
    check(failsWithIOException { DocumentTransfer.copyCapped(input, output, Long.MAX_VALUE) }) {
        "an abort fails the copy in progress"
    }
    check(output.size() == DocumentTransfer.CHUNK_BYTES) {
        "the copy stops at the next chunk (${output.size()} of ${source.size} bytes written)"
    }
    check(DocumentTransfer.isAborted) { "the abort is visible" }

    val fresh = CountingInput(ByteArrayInputStream(bytes(10)))
    check(failsWithIOException { DocumentTransfer.copyCapped(fresh, ByteArrayOutputStream(), 100) }) {
        "a copy started after the abort fails"
    }
    check(fresh.reads == 0) { "a copy started after the abort reads nothing" }
}

fun main() {
    aSourceExactlyAtTheCapCopiesWhole()
    oneByteOverTheCapFails()
    anEmptyStreamCopiesNothing()
    aReadOrWriteErrorFails()
    anAbortStopsACopyAtTheNextChunkAndRefusesNewOnes()
    println("PASS: document transfers copy within the cap, fail on errors and stop on abort")
}
