// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

/** A fake platform player: the seeks it was handed. */
private class FakePlatform : PlayerOwnerState.Platform {
    val seeks = mutableListOf<Long>()

    override fun seekTo(positionMs: Long) {
        seeks += positionMs
    }
}

private class Owner {
    val platform = FakePlatform()
    val landed = mutableListOf<Long>()
    val state = PlayerOwnerState(platform) { landed += it }
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
    }
    val state = PlayerOwnerState(platform) {}
    try {
        state.seek(1_000, 1)
    } catch (_: IllegalStateException) {
        // onOwner reports it as the player's failure
    }
    platform.throwing = false
    state.seek(2_000, 2)
    check(platform.seeks == listOf(2_000L)) { "the next seek goes to the platform at once" }
}

fun main() {
    eachCompletionNamesTheSeekThatLanded()
    quickSeeksKeepOnlyTheNewest()
    aSeekThatThrowsHoldsNothing()
    println("PlayerOwnerStateTest: OK")
}
