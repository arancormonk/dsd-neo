// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

import java.util.Locale

/**
 * Records each change of the dim, and whether a traversal and a draw of the window followed it (#782). Plain Kotlin, so
 * it runs on the JVM; [ScreenSupport] feeds it from the main thread.
 *
 * A window's brightness reaches the display only through a relayout, which the next traversal of the view tree sends,
 * and only for a window that is drawn. The value read back from the window is the client's copy, so it cannot show
 * either; the traversal and draw that follow can. A change that never drew is told apart from one that drew while the
 * panel stayed dim, which points at the platform.
 *
 * Change records (`screen: change <n>: ...`, [changed]) come at most one per [MIN_INTERVAL_MS]. A change inside that
 * interval waits for its end, and only the latest waiting one is recorded then, with how late and how many changes it
 * replaced. The latest written change is followed: the first [preDraw] after it is its traversal, the first [draw] its
 * draw, and one follow record per recorded change says when they came, that no draw came within [DRAW_WAIT_MS], or
 * that a later change replaced it first. A follow record never comes before its change record.
 *
 * A touch that comes down while the dim is on is recorded too ([touchWhileDimmed]): whether the screen policy took it and
 * what came of it, so "the tap was ignored" reads apart from "no tap arrived", where no record comes. At most one per
 * [MIN_TOUCH_INTERVAL_MS]; the next one counts those left out.
 *
 * [schedule] asks for [tick] at an instant on [clock], replacing any earlier request; null cancels. Main thread only.
 */
class ScreenTrace(
    private val clock: () -> Long,
    private val schedule: (atMs: Long?) -> Unit,
    private val emit: (String) -> Unit,
) {
    private class Follow(val number: Int, val at: Long, var recorded: Boolean) {
        var traversalAt: Long? = null
    }

    private var changes = 0
    private var lastRecordAt: Long? = null

    // The change waiting for the interval to end (its number, 0 for none, and description), when it came, how many
    // waiting changes it replaced, and its follow record if that is ready first.
    private var waiting = 0
    private var waitingText = ""
    private var waitingAt = 0L
    private var replaced = 0
    private var waitingFollow: String? = null

    private var follow: Follow? = null
    private var scheduled: Long? = null

    private var lastTouchRecordAt: Long? = null
    private var touchesLeftOut = 0

    /**
     * The dim changed, or was written again: [description] says which way, what was requested and what the window
     * reads back, and the inputs behind it. Only a [written] change is followed for a traversal and a draw.
     */
    fun changed(description: String, written: Boolean) {
        val now = clock()
        val number = ++changes
        follow?.let { previous ->
            follow = null
            finish(previous, "replaced by change $number after ${now - previous.at} ms; ${seen(previous)}, no draw")
        }
        // A change still waiting is replaced either way: by this one waiting, or by this one recorded now, which a
        // tick held up by a busy main thread can allow.
        val skipped = if (waiting != 0) replaced + 1 else 0
        waiting = 0
        replaced = 0
        waitingFollow = null
        val last = lastRecordAt
        val recorded = if (last == null || now - last >= MIN_INTERVAL_MS) {
            record(number, if (skipped > 0) " (replacing $skipped changes not recorded)" else "", description, now)
            true
        } else {
            waiting = number
            waitingText = description
            waitingAt = now
            replaced = skipped
            false
        }
        follow = if (written) Follow(number, now, recorded) else null
        reschedule()
    }

    /** A traversal of the window's view tree is about to draw; cheap when no change is followed. */
    fun preDraw() {
        val current = follow ?: return
        if (current.traversalAt == null) {
            current.traversalAt = clock()
        }
    }

    /** The view tree of [window] (its generation) drew. Cheap when no change is followed. */
    fun draw(window: Int) {
        val current = follow ?: return
        follow = null
        finish(current, "${seen(current)}, draw after ${clock() - current.at} ms on window $window")
        reschedule()
    }

    /**
     * A touch came down on the activity while the dim was on. [taken]: the screen policy saw it (it reached the
     * activity the policy follows); then [swallowed] is its answer and [dimAfter] whether the dim is still on after it.
     */
    fun touchWhileDimmed(taken: Boolean, swallowed: Boolean, dimAfter: Boolean) {
        val now = clock()
        val last = lastTouchRecordAt
        if (last != null && now - last < MIN_TOUCH_INTERVAL_MS) {
            touchesLeftOut += 1
            return
        }
        val outcome = if (!taken) {
            "not taken: the screen policy follows another activity"
        } else {
            (if (swallowed) "swallowed" else "passed on") + ", dim " + (if (dimAfter) "still on" else "off")
        }
        val leftOut = if (touchesLeftOut > 0) "; $touchesLeftOut more since, not recorded" else ""
        emit(HostRecordText.fit("screen: touch while dimmed: $outcome$leftOut"))
        lastTouchRecordAt = now
        touchesLeftOut = 0
    }

    /** The instant [schedule] asked for. */
    fun tick() {
        scheduled = null
        val now = clock()
        val last = lastRecordAt
        if (waiting != 0 && last != null && now - last >= MIN_INTERVAL_MS) {
            val skipped = if (replaced > 0) ", replacing $replaced changes not recorded" else ""
            record(waiting, " (recorded ${now - waitingAt} ms late$skipped)", waitingText, now)
            waiting = 0
            replaced = 0
            follow?.recorded = true
            waitingFollow?.let(emit)
            waitingFollow = null
        }
        val current = follow
        if (current != null && now - current.at >= DRAW_WAIT_MS) {
            follow = null
            finish(current, "no draw within $DRAW_WAIT_MS ms; ${seen(current)}")
        }
        reschedule()
    }

    private fun record(number: Int, note: String, description: String, now: Long) {
        emit(HostRecordText.fit(HostRecordText.safe("screen: change $number$note: $description")))
        lastRecordAt = now
    }

    private fun seen(follow: Follow): String =
        follow.traversalAt?.let { "traversal after ${it - follow.at} ms" } ?: "no traversal"

    /** The follow record of [follow]'s change; held until that change is recorded, and dropped if it never is. */
    private fun finish(follow: Follow, text: String) {
        val line = HostRecordText.fit("screen: change ${follow.number}: $text")
        if (follow.recorded) {
            emit(line)
        } else {
            waitingFollow = line
        }
    }

    private fun reschedule() {
        val last = lastRecordAt
        val flushAt = if (waiting != 0 && last != null) last + MIN_INTERVAL_MS else null
        val drawBy = follow?.let { it.at + DRAW_WAIT_MS }
        val next = listOfNotNull(flushAt, drawBy).minOrNull()
        if (next != scheduled) {
            scheduled = next
            schedule(next)
        }
    }

    companion object {
        const val MIN_INTERVAL_MS = 1_000L
        const val DRAW_WAIT_MS = 2_000L
        const val MIN_TOUCH_INTERVAL_MS = 5_000L

        /** A window brightness as records show it: "none" for no override (any negative), else the fraction. */
        fun brightness(value: Float): String =
            if (value.isNaN()) "unset" else if (value < 0f) "none" else String.format(Locale.ROOT, "%.3f", value)

        /**
         * The policy inputs behind the dim, at [now]: the mode, the session, where the activity stands, the screen,
         * the outputs, and what engages the policy (each engagement within the delay, and how long ago).
         */
        fun inputs(view: ScreenPolicyView, now: Long): String {
            val delayMs = view.delaySeconds * 1_000L
            val engaged = listOf(
                "audio" to view.lastAudio,
                "touch" to view.lastTouch,
                "start" to view.lastStart,
                "setting" to view.lastConfig,
            ).mapNotNull { (name, at) ->
                at?.takeIf { now < it + delayMs }?.let { "$name ${now - it} ms ago" }
            }
            val outputs = view.outputs
            return listOf(
                "mode ${view.mode.name.lowercase(Locale.ROOT)}, delay ${view.delaySeconds} s",
                "session " + (view.session?.toString() ?: "none"),
                flag("visible", view.visible),
                flag("focused", view.focused),
                flag("top", view.top),
                flag("multi-window", view.multiWindow),
                "screen " + if (view.interactive) "on" else "off",
                "wants " + listOfNotNull(
                    "dim".takeIf { outputs.dimmed },
                    "keep on".takeIf { outputs.keepScreenOn },
                    "lease".takeIf { outputs.holdLease },
                ).ifEmpty { listOf("nothing") }.joinToString(" and "),
                "engaged by " + engaged.ifEmpty { listOf("nothing") }.joinToString(" and "),
            ).joinToString(", ")
        }

        private fun flag(name: String, value: Boolean) = if (value) name else "not $name"
    }
}
