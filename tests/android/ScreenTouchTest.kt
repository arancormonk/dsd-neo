// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

import io.github.arancormonk.dsdneo.ScreenPolicy.Companion.ACTION_CANCEL
import io.github.arancormonk.dsdneo.ScreenPolicy.Companion.ACTION_DOWN
import io.github.arancormonk.dsdneo.ScreenPolicy.Companion.ACTION_MOVE
import io.github.arancormonk.dsdneo.ScreenPolicy.Companion.ACTION_POINTER_DOWN
import io.github.arancormonk.dsdneo.ScreenPolicy.Companion.ACTION_POINTER_UP
import io.github.arancormonk.dsdneo.ScreenPolicy.Companion.ACTION_UP

// MotionEvent.ACTION_OUTSIDE: not part of a touch gesture the gate knows by name.
private const val ACTION_OUTSIDE = 4

/** A sample with a fixed verdict, which the policy asks for as it would ask Android. */
private fun ScreenController.sample(stamp: ULong?, ageMs: Long, wakeAllowed: Boolean) =
    sample(stamp, ageMs) { wakeAllowed }

/** The activity's dispatchTouchEvent decision, through the controller, on a Dim-mode screen. */
private class TouchRig {
    var now = 3_000_000L
    var dimmed = false
    val controller = ScreenController(
        { now },
        object : ScreenController.Effects {
            override fun setKeepScreenOn(on: Boolean) {}

            override fun setDimmed(dimmed: Boolean) {
                this@TouchRig.dimmed = dimmed
            }

            override fun renewLease() {}

            override fun releaseLease() {}

            override fun pulseWake(): Boolean = true

            override fun schedule(atMs: Long?) {}
        },
    )

    init {
        controller.configure(2, 30)
        controller.started()
        controller.topResumedChanged(true, true)
        controller.focusChanged(true, true)
        controller.sessionStarted(1L)
        controller.sample(0uL, -1, true)
    }

    fun dim(): TouchRig {
        now += 30_000
        controller.tick()
        check(dimmed)
        return this
    }

    /** Delivers [actions] as one gesture, 16 ms apart, and returns what each was answered. */
    fun gesture(vararg actions: Int, exploring: Boolean = false): List<Boolean> = actions.map {
        now += 16
        controller.touch(it, exploring)
    }
}

private fun theWholeGestureOnADimmedScreenIsSwallowed() {
    val rig = TouchRig().dim()
    val answers = rig.gesture(
        ACTION_DOWN,
        ACTION_MOVE,
        ACTION_POINTER_DOWN,
        ACTION_MOVE,
        ACTION_POINTER_UP,
        ACTION_MOVE,
        ACTION_UP,
    )
    check(answers.all { it }) { "every event of the brightening gesture is swallowed: $answers" }
    check(!rig.dimmed) { "the DOWN brightened the screen" }
    check(rig.gesture(ACTION_DOWN, ACTION_MOVE, ACTION_UP).none { it }) { "the next gesture reaches the app" }
}

private fun aCancelledGestureIsSwallowedWhole() {
    val rig = TouchRig().dim()
    check(rig.gesture(ACTION_DOWN, ACTION_MOVE, ACTION_POINTER_DOWN, ACTION_CANCEL).all { it })
    check(!rig.dimmed)
    check(rig.gesture(ACTION_DOWN, ACTION_UP).none { it }) { "the CANCEL ended the latch" }
}

private fun touchExplorationBrightensButNeverSwallows() {
    val rig = TouchRig().dim()
    check(rig.gesture(ACTION_DOWN, ACTION_MOVE, ACTION_UP, exploring = true).none { it })
    check(!rig.dimmed) { "a TalkBack touch still brightens" }
    rig.dim()
    check(rig.gesture(ACTION_DOWN, ACTION_POINTER_DOWN, ACTION_POINTER_UP, ACTION_CANCEL, exploring = true).none { it })
}

private fun aBrightScreenSwallowsNothing() {
    val rig = TouchRig()
    check(!rig.dimmed)
    check(rig.gesture(ACTION_DOWN, ACTION_MOVE, ACTION_POINTER_DOWN, ACTION_POINTER_UP, ACTION_UP).none { it })
    check(rig.gesture(ACTION_DOWN, ACTION_CANCEL).none { it })
}

private fun otherActionsFollowTheLatch() {
    val rig = TouchRig().dim()
    check(rig.gesture(ACTION_DOWN, ACTION_OUTSIDE, ACTION_UP).all { it }) { "inside a swallowed gesture" }
    check(rig.gesture(ACTION_OUTSIDE).none { it }) { "outside any gesture" }
}

private fun theDecisionFollowsWhatIsOnScreen() {
    // The dim deadline has passed but its tick has not run: the screen still shows bright, so the tap goes through.
    val rig = TouchRig()
    rig.now += 30_000
    check(!rig.dimmed)
    check(rig.gesture(ACTION_DOWN, ACTION_UP).none { it })
    check(!rig.dimmed)
}

private fun keysBrightenWithoutAVeto() {
    val rig = TouchRig().dim()
    rig.controller.userInteraction()
    check(!rig.dimmed)
}

fun main() {
    theWholeGestureOnADimmedScreenIsSwallowed()
    aCancelledGestureIsSwallowedWhole()
    touchExplorationBrightensButNeverSwallows()
    aBrightScreenSwallowsNothing()
    otherActionsFollowTheLatch()
    theDecisionFollowsWhatIsOnScreen()
    keysBrightenWithoutAVeto()
    println("PASS: touch gate swallows whole brightening gestures, never under touch exploration")
}
