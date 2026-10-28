// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

/**
 * Notices when the main looper stops running ordinary messages, and records what the main thread was doing (#782).
 * Plain Kotlin, so it runs on the JVM; [MainLooperWatchdog] runs the app's one instance on a thread of its own.
 *
 * It keeps exactly one probe outstanding: a synchronous message posted to the main looper, the kind a sync barrier
 * holds back while input and frames still run. The next probe is posted only after the previous one has run, at the
 * first check at least [PERIOD_MS] after it was posted, so an idle process costs one main-thread message and one
 * watchdog message per period. A probe that has waited more than [STALL_MS] is a stall, and the first stall in any
 * [MIN_INCIDENT_INTERVAL_MS] becomes an incident: several correlated records, each with the incident id and a sequence
 * number and each within [HostRecordText.MAX_BYTES], that say how long the main thread has been blocked, where the
 * activity stood, what the screen policy last saw, how many touch events Android delivered since the probe was posted
 * (touches during a stall mean input ran while ordinary messages did not), and the main thread's stack, top frame
 * first. Nothing is read from the message queue: no `Looper.dump()`, no message contents. When the probe runs, one
 * more record says `main thread resumed after N ms` and how many touches the stall saw. Stalls the interval keeps out
 * are counted in the next incident.
 *
 * A check that itself runs more than [SUSPENDED_MS] late means this thread did not run either (the process was frozen
 * or starved), so the probe's wait leaves that time out rather than blaming the main thread for it, and a resume says
 * how much was left out.
 *
 * Every field but the probe's two is touched only on the watchdog's thread. Time is [clock], in milliseconds:
 * `SystemClock.uptimeMillis()`, the Handler time base, in the app.
 */
class LooperWatchdog(
    private val clock: () -> Long,
    private val timer: Timer,
    private val postToMain: (Runnable) -> Unit,
    private val touches: () -> Long,
    private val capture: () -> Capture,
    private val emit: (String) -> Unit,
) {
    /** Runs a task on the watchdog's thread at an instant on [clock]. */
    fun interface Timer {
        fun at(timeMs: Long, task: Runnable)
    }

    /** What an incident records besides the wait, read on the watchdog's thread at the capture. */
    class Capture(val stack: List<String>, val activity: String, val screen: String)

    private var checkAt = NONE

    // The outstanding probe: when it was posted (NONE when there is none) and the touch count then.
    private var sentAt = NONE
    private var touchesAtSend = 0L

    // Written by the probe on the main thread, read here; reset before each post.
    @Volatile
    private var ranAt = NONE

    @Volatile
    private var touchesAtRun = 0L

    // Whether this probe's stall was judged, and the incident it opened (0 for none).
    private var judged = false
    private var incident = 0
    private var incidentRecords = 0
    private var touchesAtCapture = 0L
    private var heldMs = 0L

    private var nextIncident = 1
    private var lastIncidentAt = NONE
    private var unrecorded = 0

    private val probe = Runnable {
        touchesAtRun = touches()
        ranAt = clock()
    }
    private val check = Runnable { check() }

    /** Starts the checks; call once. Thread-safe as far as [Timer.at] is. */
    fun start() {
        arm(clock())
    }

    private fun arm(atMs: Long) {
        checkAt = atMs
        timer.at(atMs, check)
    }

    private fun check() {
        val now = clock()
        val late = now - checkAt
        if (sentAt == NONE) {
            send(now)
            arm(now + PERIOD_MS)
            return
        }
        val ran = ranAt
        if (ran != NONE) {
            if (incident != 0) {
                resumed(ran)
            }
            send(now)
            arm(now + PERIOD_MS)
            return
        }
        if (late > SUSPENDED_MS) {
            // This thread was held too, for as long as the check is late; that time is not the main thread's wait.
            heldMs += late
            sentAt += late
            arm(now + PERIOD_MS)
            return
        }
        val waited = now - sentAt
        if (!judged && waited > STALL_MS) {
            judged = true
            if (lastIncidentAt != NONE && now - lastIncidentAt < MIN_INCIDENT_INTERVAL_MS) {
                unrecorded += 1
            } else {
                record(now, waited)
            }
        }
        arm(if (judged) now + PERIOD_MS else minOf(now + PERIOD_MS, sentAt + STALL_MS + 1))
    }

    private fun send(now: Long) {
        sentAt = now
        touchesAtSend = touches()
        judged = false
        incident = 0
        heldMs = 0L
        ranAt = NONE
        postToMain(probe)
    }

    private fun record(now: Long, waited: Long) {
        val id = nextIncident++
        lastIncidentAt = now
        val facts = capture()
        touchesAtCapture = touches()
        val records = incident(id, now, waited, touchesAtCapture - touchesAtSend, facts, unrecorded)
        unrecorded = 0
        incident = id
        incidentRecords = records.size
        records.forEach(emit)
    }

    private fun resumed(ran: Long) {
        val during = touchesAtRun - touchesAtSend
        val byCapture = touchesAtCapture - touchesAtSend
        val held = if (heldMs > 0) "; the watchdog itself was held for $heldMs ms, not counted" else ""
        emit(
            HostRecordText.fit(
                "watchdog: stall $incident.${incidentRecords + 1}: main thread resumed after ${ran - sentAt} ms; " +
                    "$during touches delivered while it waited, $byCapture of them by the capture$held",
            ),
        )
        incident = 0
    }

    companion object {
        const val PERIOD_MS = 500L
        const val STALL_MS = 1_500L
        const val MIN_INCIDENT_INTERVAL_MS = 30_000L
        const val SUSPENDED_MS = 1_000L

        /** Stack records per incident; frames that do not fit are counted in the first record. */
        const val MAX_STACK_RECORDS = 6

        /** The most any one frame takes in a record. */
        const val MAX_FRAME_BYTES = 200

        private const val NONE = Long.MIN_VALUE
        private const val FRAME_SEPARATOR = " | "

        /**
         * An incident's records, in order: what stalled and where the activity stood, then the screen policy's inputs,
         * then the main thread's stack from the top frame down, as many frames as [MAX_STACK_RECORDS] records hold.
         * Each is `watchdog: stall <id>.<n> of <total>: ...` and within [HostRecordText.MAX_BYTES].
         */
        fun incident(
            id: Int,
            now: Long,
            waited: Long,
            touches: Long,
            facts: Capture,
            unrecorded: Int,
        ): List<String> {
            val frames = facts.stack.map { HostRecordText.fit(HostRecordText.safe(it), MAX_FRAME_BYTES) }
            // Totals stay single digits (at most 2 + MAX_STACK_RECORDS records), so "of 9" sizes every prefix.
            val chunks = mutableListOf<List<String>>()
            var next = 0
            while (next < frames.size && chunks.size < MAX_STACK_RECORDS) {
                val prefix = stackPrefix(id, 9, 9, next + 1, frames.size)
                val budget = HostRecordText.MAX_BYTES - HostRecordText.bytes(prefix)
                val chunk = mutableListOf(frames[next])
                var used = HostRecordText.bytes(frames[next])
                next += 1
                while (next < frames.size) {
                    val size = HostRecordText.bytes(FRAME_SEPARATOR) + HostRecordText.bytes(frames[next])
                    if (used + size > budget) {
                        break
                    }
                    chunk += frames[next]
                    used += size
                    next += 1
                }
                chunks += chunk
            }
            val total = 2 + chunks.size
            val omitted = frames.size - next
            val head = buildString {
                append("watchdog: stall $id.1 of $total at uptime ${HostRecordText.uptime(now)}: ")
                append("main thread blocked $waited ms; $touches touches delivered since the probe; ")
                append(HostRecordText.safe(facts.activity))
                append("; stack of ${frames.size} frames")
                if (omitted > 0) {
                    append(", top ${frames.size - omitted} recorded")
                }
                if (unrecorded > 0) {
                    append("; $unrecorded earlier stalls not recorded, within ${MIN_INCIDENT_INTERVAL_MS / 1_000} s")
                }
            }
            val records = mutableListOf(
                HostRecordText.fit(head),
                HostRecordText.fit("watchdog: stall $id.2 of $total: screen ${HostRecordText.safe(facts.screen)}"),
            )
            var first = 1
            chunks.forEachIndexed { index, chunk ->
                val prefix = stackPrefix(id, 3 + index, total, first, first + chunk.size - 1)
                records += HostRecordText.fit(prefix + chunk.joinToString(FRAME_SEPARATOR))
                first += chunk.size
            }
            return records
        }

        // The ranges are frame numbers, so the widest prefix for a chunk starting at [from] ends at the stack's size.
        private fun stackPrefix(id: Int, sequence: Int, total: Int, from: Int, to: Int): String =
            "watchdog: stall $id.$sequence of $total: main stack $from-$to: "
    }
}
