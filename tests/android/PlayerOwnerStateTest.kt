// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

/** A fake platform player: the seeks and pauses it was handed, a pause that throws when [pauseThrows]. */
private class FakePlatform : PlayerOwnerState.Platform {
    val seeks = mutableListOf<Long>()

    /** What it was handed, in order: "seek <ms>", "pause", "start". */
    val ops = mutableListOf<String>()
    var pauses = 0
    var pauseThrows = false

    override fun seekTo(positionMs: Long) {
        seeks += positionMs
        ops += "seek $positionMs"
    }

    override fun pause() {
        pauses++
        if (pauseThrows) {
            throw IllegalStateException("pause refused")
        }
    }
}

/** The owner's delayed tasks, run when told. */
private class FakeLater : PlayerOwnerState.Later {
    val tasks = mutableListOf<Pair<Long, () -> Unit>>()

    override fun post(delayMs: Long, task: () -> Unit): () -> Unit {
        val entry = delayMs to task
        tasks += entry
        return { tasks.remove(entry) }
    }

    fun runAll() {
        val due = tasks.toList()
        tasks.clear()
        due.forEach { it.second() }
    }
}

private class Owner {
    val platform = FakePlatform()
    val later = FakeLater()
    val landed = mutableListOf<Long>()
    var silences = 0
    val state = PlayerOwnerState(platform, { landed += it }, { silences++ }, later)
}

/** R59: two seeks before the first completes: the first completion lands the first, never the second. */
private fun eachCompletionNamesTheSeekThatLanded() {
    val o = Owner()
    o.state.seek(20_000, 1)
    o.state.seek(40_000, 2)
    check(o.platform.seeks == listOf(20_000L)) { "one platform seek in flight; the second is held" }
    o.state.seekCompleted()
    check(o.landed == listOf(1L)) { "the first completion names the first seek: ${o.landed}" }
    check(o.platform.seeks == listOf(20_000L, 40_000L)) { "then the held one goes to the platform" }
    o.state.seekCompleted()
    check(o.landed == listOf(1L, 2L)) { "the second lands at its own completion" }
    o.state.seekCompleted()
    check(o.landed == listOf(1L, 2L)) { "a completion with none in flight names nothing" }
}

/** R59: five quick seeks: the first and the newest reach the platform, and the newest lands. */
private fun quickSeeksKeepOnlyTheNewest() {
    val o = Owner()
    for (serial in 1L..5L) {
        o.state.seek(serial * 1_000, serial)
    }
    check(o.platform.seeks == listOf(1_000L)) { "the first in flight" }
    o.state.seekCompleted()
    check(o.platform.seeks == listOf(1_000L, 5_000L)) { "then only the newest: ${o.platform.seeks}" }
    o.state.seekCompleted()
    check(o.landed == listOf(1L, 5L) && o.platform.seeks.size == 2) { "at most two platform seeks; the newest lands" }
}

/** A platform seek that throws leaves none in flight, so a later seek is not held forever. */
private fun aSeekThatThrowsHoldsNothing() {
    val platform = object : PlayerOwnerState.Platform {
        var throwing = true
        val seeks = mutableListOf<Long>()

        override fun seekTo(positionMs: Long) {
            if (throwing) throw IllegalStateException("seek refused")
            seeks += positionMs
        }

        override fun pause() = Unit
    }
    val state = PlayerOwnerState(platform, {}, {}, FakeLater())
    try {
        state.seek(1_000, 1)
    } catch (_: IllegalStateException) {
        // onOwner reports it as the player's failure
    }
    platform.throwing = false
    state.seek(2_000, 2)
    check(platform.seeks == listOf(2_000L)) { "the next seek goes to the platform at once" }
}

/** R62: silence only once the audio has stopped: after a pause that returned, else after the release. */
private fun silenceComesOnlyOnceTheAudioStopped() {
    val never = Owner()
    never.state.silence()
    check(never.silences == 1) { "nothing started: silent at once" }

    val paused = Owner()
    paused.state.onStarted()
    paused.state.pause()
    check(!paused.state.started && paused.platform.pauses == 1) { "a pause that returned clears the start" }
    paused.state.silence()
    check(paused.silences == 1) { "silent at once after it" }

    val refused = Owner()
    refused.state.onStarted()
    refused.platform.pauseThrows = true
    try {
        refused.state.pause()
    } catch (_: IllegalStateException) {
        // onOwner reports it under the outgoing token
    }
    check(refused.state.started) { "a pause that threw leaves the start standing" }
    refused.state.silence()
    check(refused.silences == 0) { "so no silence yet" }
    refused.state.released(returned = true)
    check(refused.silences == 1 && !refused.state.started) { "the release, once it returned, confirms it" }

    val stuck = Owner()
    stuck.state.onStarted()
    stuck.state.silence()
    stuck.state.released(returned = false)
    check(stuck.silences == 0) { "a release that threw confirms nothing: the incoming start's watchdog bounds it" }

    val unasked = Owner()
    unasked.state.onStarted()
    unasked.state.released(returned = true)
    check(unasked.silences == 0) { "no silence asked for: none reported" }
}

/**
 * M1: a seek in flight is bounded at 2 s. With no completion by then it is given up, never reported as landed, and the
 * held seek goes to the platform and is matched to the next completion.
 */
private fun aSeekInFlightIsGivenUpAfterItsBound() {
    val o = Owner()
    o.state.seek(10_000, 1)
    o.state.seek(30_000, 2)
    check(o.later.tasks.size == 1 && o.later.tasks.single().first == PlayerOwnerState.SEEK_GIVE_UP_MS) {
        "the seek in flight is bounded at 2 s"
    }
    o.later.runAll() // its completion never came
    check(o.landed.isEmpty()) { "the given-up seek is not reported as landed" }
    check(o.platform.seeks == listOf(10_000L, 30_000L)) { "the held seek goes to the platform" }
    o.state.seekCompleted()
    check(o.landed == listOf(2L)) { "the next completion is the held seek's" }
    check(o.later.tasks.isEmpty()) { "its own bound is cancelled by its completion" }
}

/** M2: a start asked for while a seek is held waits until that seek has gone to the platform: seek, seek, start. */
private fun aStartWaitsForAHeldSeek() {
    val o = Owner()
    o.state.seek(10_000, 1)
    o.state.seek(30_000, 2)
    o.state.start { o.platform.ops += "start" }
    check(o.platform.ops == listOf("seek 10000")) { "the start waits behind the held seek: ${o.platform.ops}" }
    o.state.seekCompleted()
    check(o.platform.ops == listOf("seek 10000", "seek 30000", "start")) { "seek, seek, then start: ${o.platform.ops}" }

    val none = Owner()
    none.state.seek(10_000, 1)
    none.state.start { none.platform.ops += "start" }
    check(none.platform.ops == listOf("seek 10000", "start")) { "with no seek held, a start goes at once" }

    val paused = Owner()
    paused.state.seek(10_000, 1)
    paused.state.seek(30_000, 2)
    paused.state.start { paused.platform.ops += "start" }
    paused.state.pause()
    paused.state.seekCompleted()
    check(paused.platform.ops == listOf("seek 10000", "seek 30000")) { "a pause drops the start that waited" }
}

fun main() {
    aSeekInFlightIsGivenUpAfterItsBound()
    aStartWaitsForAHeldSeek()
    eachCompletionNamesTheSeekThatLanded()
    quickSeeksKeepOnlyTheNewest()
    aSeekThatThrowsHoldsNothing()
    silenceComesOnlyOnceTheAudioStopped()
    println("PlayerOwnerStateTest: OK")
}
