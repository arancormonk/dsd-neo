// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

import io.github.arancormonk.dsdneo.PlaybackServicePhases.Phase
import io.github.arancormonk.dsdneo.PlaybackServicePhases.Start

private const val TOKEN = 0x5eedL
private const val OTHER_PROCESS = 0x0beefL

/**
 * The playback service as ActivityManager runs it, around the phases RecordingPlayback and RecordingPlaybackService
 * consult: starts in flight (each delivered once, in order), one instance at a time with its start ids, and
 * stopSelfResult, which stops only when no newer start is queued.
 */
private class ServiceRig {
    val phases = PlaybackServicePhases(TOKEN)

    /** Starts handed to the platform and not delivered yet: (token, generation). */
    val inFlight = ArrayDeque<Pair<Long, Int>>()

    /** Transport intents queued for delivery, which count as starts too. */
    var queuedTransports = 0
    val starts = mutableListOf<Start>()
    val promoted = mutableListOf<Int>()
    val refused = mutableListOf<Int>()
    val applied = mutableListOf<Int>()

    /** What each delivery's startForeground() showed: the owner's notification, or 0 for the instance's own. */
    val notices = mutableListOf<Int>()
    var instance = 0
    var startId = 0
    var foreground = false
    var retired = false
    var stopServiceCalls = 0
    var refuseNextStart = false
    val looper = mutableListOf<String>()

    fun play(playlist: Boolean = true): Int = phases.accept(playlist) { generation, start ->
        starts += start
        looper += "play $generation"
        if (start == Start.ISSUE) {
            inFlight.addLast(TOKEN to generation)
        }
    }.also { generation ->
        if (generation != 0 && starts.last() == Start.ISSUE && refuseNextStart) {
            refuseNextStart = false
            inFlight.removeLast()
            refused += phases.startFailed(generation).toList()
        }
    }

    private fun ensureInstance() {
        if (instance == 0) {
            instance = phases.created()
            startId = 0
            retired = false
        }
    }

    /** The next start in flight reaches the service, as onStartCommand runs it. */
    fun deliverStart(allowed: Boolean = true) {
        val (token, generation) = inFlight.removeFirst()
        deliverPromote(token, generation, allowed)
    }

    fun deliverPromote(token: Long, generation: Int, allowed: Boolean = true) {
        ensureInstance()
        startId++
        notices += phases.foregroundOwner()
        foreground = allowed
        val promotion = phases.promote(instance, token, generation, allowed)
        if (promotion.promoted != 0) promoted += promotion.promoted
        if (promotion.refused != 0) refused += promotion.refused
        retireCheck()
    }

    fun deliverTransport(token: Long, generation: Int) {
        ensureInstance()
        startId++
        if (queuedTransports > 0) queuedTransports--
        val target = phases.transport(token, generation)
        if (target != 0) applied += target
        retireCheck()
    }

    /** An asynchronous end posted to the service's thread: the phases settle it, then the instance checks. */
    fun ended(generation: Int) {
        phases.ended(generation)
        if (instance != 0) retireCheck()
    }

    fun retireCheck(): Boolean = phases.retire(instance) {
        // stopSelfResult(newest seen): refused while a start the instance has not seen is queued.
        inFlight.isEmpty() && queuedTransports == 0
    }.also { if (it) retired = true }

    /** onDestroy, once a retired (or stopped) instance goes: what it served. */
    fun destroy(): Int {
        val served = phases.destroyed(instance)
        instance = 0
        foreground = false
        return served
    }

    fun release(): Boolean = phases.release { looper += "stop" }.also { if (it) stopServiceCalls++ }
}

/** The player machine behind the phases, as RecordingPlayback runs it: its ends reach the phases and the retire. */
private class Playing(val rig: ServiceRig) {
    var nowMs = 0L
    val tasks = mutableListOf<Pair<Long, () -> Unit>>()
    val tokens = mutableListOf<Long>()
    val seeks = mutableListOf<Pair<Long, Long>>()
    val ends = mutableListOf<Pair<Int, String>>()
    val machine = RecordingPlayer(
        { token ->
            tokens += token
            object : RecordingPlayer.Player {
                override fun prepare(source: RecordingPlayer.Source) = Unit
                override fun start() = Unit
                override fun pause() = Unit
                override fun seekTo(positionMs: Long) {
                    seeks += token to positionMs
                }
                override val durationMs: Long get() = 10_000
                override fun release() = Unit
            }
        },
        object : RecordingPlayer.Opener {
            override fun setup(
                work: () -> RecordingPlayer.SetupResult,
                deliver: (RecordingPlayer.SetupResult) -> Unit,
            ) = deliver(work())

            override fun abandonSetup() = Unit

            override fun open(item: RecordingPlayer.Item) =
                RecordingPlayer.OpenResult.Opened(
                    object : RecordingPlayer.Source {
                        override fun close() = Unit
                    },
                )
        },
        object : RecordingPlayer.Focus {
            override fun request(mode: RecordingPlayer.FocusMode, epoch: Int) = true
            override fun abandon() = Unit
        },
        { nowMs },
        { delayMs, task ->
            val entry = (nowMs + delayMs) to task
            tasks += entry
            RecordingPlayer.Cancellable { tasks.remove(entry) }
        },
        object : RecordingPlayer.ServiceLink {
            override fun changed(status: RecordingPlayer.Status) = Unit
            override fun ended(generation: Int, reason: String) {
                ends += generation to reason
                rig.ended(generation)
            }
        },
        RecordingRecheck(),
    )

    val items = listOf(RecordingPlayer.Item("/r/a.wav", 10, 1, 0), RecordingPlayer.Item("/r/b.wav", 10, 1, 0, call = 1))

    /** A History playlist accepted and posted as RecordingPlayback does: the play, then a hand-over's promotion. */
    fun playlist(): Int {
        var start = Start.NONE
        val generation = rig.phases.accept(true) { g, s ->
            start = s
            rig.starts += s
            if (s == Start.ISSUE) rig.inFlight.addLast(TOKEN to g)
        }
        machine.play(generation, RecordingPlayer.MODE_PLAYLIST, items)
        if (start == Start.HAND_OVER) machine.onPromoted(generation)
        return generation
    }

    fun single(): Int {
        val generation = rig.phases.accept(false) { _, s -> rig.starts += s }
        machine.play(generation, "/r/c.wav")
        return generation
    }

    /** The next start's delivery, with its promotion posted to the player. */
    fun deliver() {
        val before = rig.promoted.size
        rig.deliverStart()
        rig.promoted.drop(before).forEach(machine::onPromoted)
    }

    fun elapse(ms: Long) {
        nowMs += ms
        for (task in tasks.filter { it.first <= nowMs }) {
            if (tasks.remove(task)) task.second()
        }
    }
}

fun aPromoteForTheCurrentRequestMakesItTheOwner() {
    val rig = ServiceRig()
    val a = rig.play()
    check(a > 0 && rig.starts == listOf(Start.ISSUE)) { "a playlist with no service issues a start: ${rig.starts}" }
    check(rig.phases.currentPhase == Phase.ISSUED && rig.phases.currentRequest == a) { "the start is outstanding" }
    check(rig.looper == listOf("play $a")) { "the play reaches the player under the lock, before the start" }
    rig.deliverStart()
    check(rig.notices == listOf(0)) { "its first notification is the instance's own: nothing owned it yet" }
    check(rig.promoted == listOf(a) && rig.phases.currentOwner == a) { "the current request is promoted and owns it" }
    check(rig.phases.currentPhase == Phase.PROMOTED && !rig.retired) { "an owned service does not retire" }
    check(rig.phases.isOwner(a) && !rig.phases.isOwner(a + 1)) { "commands apply for the owner only" }
}

fun aPromoteForASettledRequestOnlyRetires() {
    // Stopped before its delivery, and the promotion timeout: the late start promotes nothing and lets it retire.
    for (settle in listOf("stop", "timeout")) {
        val rig = ServiceRig()
        val a = rig.play()
        rig.ended(a)
        check(rig.phases.currentRequest == 0) { "$settle: the request is settled" }
        check(rig.phases.currentPhase == Phase.ISSUED) { "$settle: the start is still outstanding: no stopService" }
        rig.deliverStart()
        check(rig.foreground && rig.promoted.isEmpty() && rig.refused.isEmpty()) {
            "$settle: the late start still enters the foreground and promotes nothing"
        }
        check(rig.retired && rig.phases.currentPhase == Phase.RETIRING) { "$settle: and the service retires" }
        check(rig.destroy() == 0 && rig.phases.currentPhase == Phase.NONE) { "$settle: nothing to settle at its end" }
    }
}

fun aPromoteFromAnotherProcessServesNothing() {
    // A start an earlier process issued, redelivered into this one after it died.
    val rig = ServiceRig()
    rig.deliverPromote(OTHER_PROCESS, 1)
    check(rig.foreground && rig.notices == listOf(0)) { "it enters the foreground, with its own notification" }
    check(rig.promoted.isEmpty() && rig.phases.currentOwner == 0) { "a foreign token promotes nothing" }
    check(rig.retired) { "with nothing of this process outstanding it retires at once" }
    check(rig.destroy() == 0)

    // The same redelivery while this process's own start is outstanding: kept alive for it.
    val busy = ServiceRig()
    val a = busy.play()
    busy.deliverPromote(OTHER_PROCESS, a)
    check(busy.promoted.isEmpty() && !busy.retired) { "a foreign start never takes the request, nor retires it" }
    check(busy.phases.currentPhase == Phase.ISSUED && busy.phases.currentRequest == a) { "the own start still stands" }
    busy.deliverStart()
    check(busy.promoted == listOf(a) && busy.phases.currentOwner == a) { "the own start promotes its request" }
}

fun atMostOneStartIsOutstanding() {
    val rig = ServiceRig()
    val a = rig.play()
    val b = rig.play()
    check(rig.starts == listOf(Start.ISSUE, Start.JOIN) && rig.inFlight.size == 1) {
        "a play while a start is outstanding joins it, with no second start: ${rig.starts}"
    }
    check(rig.phases.currentRequest == b) { "and becomes its request" }
    rig.ended(a) // the player tells the service the replaced playlist ended
    check(rig.phases.currentRequest == b) { "the replaced play's end leaves the new request" }
    rig.deliverStart()
    check(rig.promoted == listOf(b) && rig.phases.currentOwner == b) { "A's start promotes B, the request now" }
    val c = rig.play()
    check(rig.starts.last() == Start.HAND_OVER && rig.inFlight.isEmpty()) { "a play while promoted is handed over" }
    check(rig.phases.currentOwner == c && !rig.phases.isOwner(b)) { "in the process, at once" }
    rig.ended(b)
    check(rig.phases.currentOwner == c && !rig.retired) { "B's late end never touches C's service" }
    val single = rig.play(playlist = false)
    check(single > c && rig.starts.last() == Start.NONE) { "a sheet's play takes no service" }
    check(rig.phases.currentOwner == 0) { "and ends the playlist's ownership as it is accepted" }
    rig.ended(c)
    check(rig.retired) { "the replaced playlist's end retires the service" }
}

fun aStalePromoteKeepsTheOwnersNotification() {
    val rig = ServiceRig()
    val b = rig.play()
    rig.deliverStart()
    rig.deliverPromote(OTHER_PROCESS, 7)
    rig.deliverPromote(TOKEN, b) // the same start delivered again: not outstanding now
    check(rig.notices == listOf(0, b, b)) { "a stale start shows B's notification: ${rig.notices}" }
    check(rig.promoted == listOf(b) && rig.phases.currentOwner == b) { "and changes nothing" }
    check(!rig.retired) { "B still owns the service" }
}

fun aStaleTransportNeverActsOnALaterPlaylist() {
    val rig = ServiceRig()
    val a = rig.play()
    rig.deliverStart()
    val b = rig.play()
    rig.deliverTransport(TOKEN, a)
    check(rig.applied.isEmpty() && rig.phases.currentOwner == b && !rig.retired) {
        "A's notification Pause after B started is dropped, and B keeps its service"
    }
    rig.deliverTransport(OTHER_PROCESS, b)
    check(rig.applied.isEmpty()) { "a button from another process is dropped" }
    rig.deliverTransport(TOKEN, b)
    check(rig.applied == listOf(b)) { "B's own button reaches B" }
    check(!rig.phases.isOwner(a) && rig.phases.isOwner(b)) { "and the looper checks the owner again as it runs" }

    // The playlist ended and the service went; a button left on screen creates a fresh instance, which retires.
    rig.ended(b)
    check(rig.retired) { "the end retires the service" }
    check(rig.destroy() == 0) { "its end settles nothing: B already ended" }
    rig.deliverTransport(TOKEN, b)
    check(rig.instance != 0 && rig.applied == listOf(b)) { "a fresh instance applies nothing" }
    check(rig.retired && rig.phases.currentPhase == Phase.RETIRING) { "and retires on that delivery" }
}

fun theRetireCheckNeedsNoOwnerNoRequestAndNoStart() {
    val rig = ServiceRig()
    val a = rig.play()
    rig.deliverStart()
    check(!rig.retireCheck()) { "an owner keeps it" }
    rig.ended(a)
    check(rig.retired) { "with no owner, request or start, it retires" }

    // A start the instance has not seen yet (a notification button pressed as it ended): stopSelfResult refuses.
    val queued = ServiceRig()
    val g = queued.play()
    queued.deliverStart()
    queued.queuedTransports = 1
    queued.ended(g)
    check(!queued.retired && queued.phases.currentPhase == Phase.PROMOTED) { "a queued start keeps the instance" }
    queued.deliverTransport(TOKEN, g)
    check(queued.applied.isEmpty() && queued.retired) { "and its delivery, applying nothing, retires it" }

    // A play accepted before a posted end runs: the end's check finds the new owner.
    val handed = ServiceRig()
    val x = handed.play()
    handed.deliverStart()
    val y = handed.play()
    check(handed.starts.last() == Start.HAND_OVER && handed.phases.currentOwner == y) { "still promoted: handed over" }
    handed.ended(x)
    check(!handed.retired && handed.phases.currentOwner == y) { "the late end of the replaced playlist keeps it" }

    // A request outstanding keeps an instance with no owner (a foreign start got there first).
    val waiting = ServiceRig()
    waiting.play()
    waiting.deliverPromote(OTHER_PROCESS, 1)
    check(!waiting.retired && !waiting.retireCheck()) { "an issued start keeps the instance" }

    // A start issued and not handed to the platform yet (startForegroundService runs after the lock), its request
    // settled: stopSelfResult would stop, so the outstanding start alone must keep the instance.
    val early = ServiceRig()
    val e = early.play()
    val start = early.inFlight.removeFirst()
    early.deliverTransport(TOKEN, e) // a stale button makes an instance first
    early.ended(e)
    check(!early.retired && early.phases.currentPhase == Phase.ISSUED) { "the start not yet handed over keeps it" }
    early.inFlight.addLast(start)
    early.deliverStart()
    check(early.promoted.isEmpty() && early.retired) { "its delivery promotes nothing and retires it" }
}

fun theDestroyIsFencedToWhatTheInstanceServed() {
    val rig = ServiceRig()
    val a = rig.play()
    rig.deliverStart()
    rig.ended(a)
    check(rig.retired) { "A's end retires the instance" }
    val b = rig.play()
    check(rig.starts.last() == Start.ISSUE && rig.phases.currentRequest == b) { "past stopSelfResult, a play issues" }
    check(rig.destroy() == 0) { "the retired instance's end settles nothing" }
    check(rig.phases.currentPhase == Phase.ISSUED && rig.phases.currentRequest == b) {
        "and leaves B's start, the next instance's, outstanding"
    }
    rig.deliverStart()
    check(rig.promoted == listOf(a, b) && rig.phases.currentOwner == b) { "the next instance promotes B" }
    check(rig.destroy() == b && rig.phases.currentOwner == 0) { "an instance destroyed while it serves hands B back" }
    check(rig.phases.currentPhase == Phase.NONE) { "and the next play issues a start" }
    rig.play()
    check(rig.starts.last() == Start.ISSUE)
}

fun releaseStopsAPromotedServiceAndLeavesAnIssuedStart() {
    val promoted = ServiceRig()
    promoted.play()
    promoted.deliverStart()
    check(promoted.release() && promoted.stopServiceCalls == 1) { "a promoted service is stopped" }
    check(promoted.looper.last() == "stop") { "the player's stop is posted under the lock" }
    check(promoted.phases.currentOwner == 0 && promoted.phases.currentPhase == Phase.RETIRING) { "nothing owns it" }
    check(promoted.play() == 0 && promoted.looper.last() == "stop") { "admission is closed: a later play is refused" }
    check(promoted.destroy() == 0) { "its end has nothing left to settle" }

    // The start issued but not promoted, its delivery held back until after the release.
    val issued = ServiceRig()
    issued.play()
    check(!issued.release() && issued.stopServiceCalls == 0) { "a start not promoted is never stopped" }
    check(issued.phases.currentPhase == Phase.ISSUED && issued.phases.currentRequest == 0) { "its request is settled" }
    issued.deliverStart()
    check(issued.foreground && issued.promoted.isEmpty()) { "the delivery promotes the instance, serving nothing" }
    check(issued.retired) { "and it retires" }
}

fun aRefusedStartSettlesItsOwnRequest() {
    // startForegroundService throws as it is asked (the app went to the background): the play ends "couldn't start".
    val rig = ServiceRig()
    rig.refuseNextStart = true
    val a = rig.play()
    check(a > 0 && rig.phases.currentPhase == Phase.NONE && rig.phases.currentRequest == 0) {
        "the generation stands and nothing is outstanding"
    }
    check(rig.refused == listOf(a)) { "A is told it could not start" }
    val b = rig.play()
    check(b > a && rig.starts.last() == Start.ISSUE) { "the next playlist issues its own start" }

    // startForeground() refused on delivery (API 31+): its request is told, and the instance retires.
    rig.deliverStart(allowed = false)
    check(rig.refused == listOf(a, b) && rig.promoted.isEmpty() && rig.phases.currentOwner == 0) { "B is refused" }
    check(rig.retired) { "and the instance retires" }
}

fun aRefusedStartSettlesWhateverWaitedForIt() {
    // Plays come from one thread, so none joins between an ISSUE and its refusal; the phases settle one anyway.
    val phases = PlaybackServicePhases(TOKEN)
    val a = phases.accept(true) { _, _ -> }
    val b = phases.accept(true) { _, start -> check(start == Start.JOIN) }
    check(phases.startFailed(a).toList() == listOf(a, b)) { "both A and the request that joined its start are told" }
    check(phases.currentRequest == 0 && phases.currentPhase == Phase.NONE) { "nothing waits on a start that never ran" }
    check(phases.startFailed(a).toList() == listOf(a)) { "a repeated refusal settles nothing more" }
    val c = phases.accept(true) { _, start -> check(start == Start.ISSUE) { "the next playlist issues: $start" } }
    check(phases.currentRequest == c)
}

fun aNewInstanceEndsTheRetiringPhase() {
    // The retired instance's destroy is reported only after the next instance was created.
    val rig = ServiceRig()
    val a = rig.play()
    rig.deliverStart()
    rig.ended(a)
    check(rig.retired && rig.phases.currentPhase == Phase.RETIRING)
    val old = rig.instance
    val fresh = rig.phases.created()
    check(rig.phases.currentPhase == Phase.NONE) { "a new instance means the old one is gone" }
    check(rig.phases.destroyed(old) == 0 && rig.phases.currentPhase == Phase.NONE) { "its late destroy is a no-op" }
    rig.instance = fresh
    rig.play()
    check(rig.starts.last() == Start.ISSUE) { "the next playlist issues a start, never waits on a phase left stuck" }
}

fun theTaskRemovedEndsWhatTheServiceServesOrWaitsFor() {
    val rig = ServiceRig()
    check(rig.phases.current() == 0)
    val a = rig.play()
    check(rig.phases.current() == a) { "a request waiting for its start" }
    rig.deliverStart()
    check(rig.phases.current() == a) { "the owner" }
}

fun thePlayersEndsDriveTheServiceOrder() {
    val playing = Playing(ServiceRig())
    val rig = playing.rig
    val a = playing.playlist()
    val b = playing.playlist()
    check(playing.ends == listOf(a to RecordingPlayer.ENDED_REPLACED)) { "B replaces A in the player: ${playing.ends}" }
    check(rig.phases.currentRequest == b && rig.inFlight.size == 1) { "A's end leaves B the request of A's start" }
    playing.deliver()
    check(rig.phases.currentOwner == b && playing.machine.status.generation == b) { "the start promotes B" }
    check(playing.machine.status.sessionState == RecordingPlayer.SESSION_PLAYING) { "and B's audio may start" }
    val c = playing.playlist()
    check(rig.starts.last() == Start.HAND_OVER && rig.phases.currentOwner == c) { "C is handed the running service" }
    check(playing.ends.last() == b to RecordingPlayer.ENDED_REPLACED && !rig.retired) { "B's end keeps it for C" }
    check(playing.machine.status.sessionState == RecordingPlayer.SESSION_PLAYING) { "C is promoted at once" }
    playing.machine.stop(c)
    check(playing.ends.last() == c to RecordingPlayer.ENDED_STOPPED && rig.retired) { "C's Stop retires the service" }
    check(rig.destroy() == 0)

    // A start not promoted in time: the player ends it "start", and the late delivery only retires the new instance.
    val d = playing.playlist()
    playing.elapse(RecordingPlayer.PROMOTION_MS)
    check(playing.ends.last() == d to RecordingPlayer.ENDED_START) { "D could not start: ${playing.ends}" }
    check(rig.phases.currentRequest == 0 && rig.phases.currentPhase == Phase.ISSUED) { "its start still stands" }
    playing.deliver()
    check(rig.promoted == listOf(b) && playing.machine.status.endedReason == RecordingPlayer.ENDED_START) {
        "the late delivery promotes nothing, and D stays ended"
    }
    check(rig.retired) { "it retires" }
    check(rig.destroy() == 0)

    // A sheet's play replaces a playlist the service serves: it ends the ownership, and the playlist's end retires it.
    playing.playlist()
    playing.deliver()
    playing.single()
    check(rig.starts.last() == Start.NONE && rig.retired) { "a sheet's play takes no service and the old one retires" }
}

/** A media session command queued for one target never acts on the next (#909): each callback names its own. */
fun aSessionCommandActsOnTheTargetItWasQueuedFor() {
    val playing = Playing(ServiceRig())
    val machine = playing.machine
    val a = playing.playlist()
    playing.deliver()
    machine.onPrepared(playing.tokens.last())
    val onA = SessionTarget(a, machine.status.playToken)
    check(onA.shows(machine.status) && machine.status.sessionState == RecordingPlayer.SESSION_PLAYING) { "A plays" }
    val b = playing.playlist()
    machine.onPrepared(playing.tokens.last())
    check(!onA.shows(machine.status) && machine.status.generation == b) { "B took the session: another target" }
    onA.pause(machine)
    onA.next(machine)
    onA.seek(machine, 5_000)
    onA.stop(machine)
    check(machine.status.sessionState == RecordingPlayer.SESSION_PLAYING && machine.status.call == 0) {
        "A's queued Pause, Next and Stop leave B playing where it was"
    }
    check(playing.seeks.isEmpty()) { "and A's seek is dropped" }
    val onB = SessionTarget(b, machine.status.playToken)
    onB.seek(machine, 5_000)
    check(playing.seeks == listOf(machine.status.playToken to 5_000L)) { "B's own seek applies" }
    machine.next(b)
    machine.onPrepared(playing.tokens.last())
    check(!onB.shows(machine.status) && machine.status.call == 1) { "B's next call is another target" }
    onB.seek(machine, 1_000)
    check(playing.seeks.size == 1) { "a seek queued for B's first call is dropped on its second" }
    SessionTarget.NONE.stop(machine)
    SessionTarget.NONE.pause(machine)
    check(machine.status.sessionState == RecordingPlayer.SESSION_PLAYING) { "no target, no command" }
    SessionTarget(b, machine.status.playToken).pause(machine)
    check(machine.status.sessionState == RecordingPlayer.SESSION_PAUSED) { "the current target's Pause pauses" }
}

/** The session's play/pause taps against the player, as the service counts them on the looper (#909). */
private class Taps(val playing: Playing) {
    val toggles = mutableListOf<Int>()
    val nexts = mutableListOf<Int>()

    /** The taps' own timeouts, apart from the player's tasks: (due, task). */
    val timeouts = mutableListOf<Pair<Long, () -> Unit>>()
    val taps = PlayPauseTaps(
        { delayMs, task ->
            val entry = (playing.nowMs + delayMs) to task
            timeouts += entry
            RecordingPlayer.Cancellable { timeouts.remove(entry) }
        },
        TAP_TIMEOUT_MS,
        { g ->
            toggles += g
            if (playing.rig.phases.isOwner(g)) PlayPauseTaps.toggle(playing.machine, g)
        },
        { g ->
            nexts += g
            if (playing.rig.phases.isOwner(g)) PlayPauseTaps.next(playing.machine, g)
        },
    )

    fun elapse(ms: Long) {
        playing.elapse(ms)
        for (entry in timeouts.filter { it.first <= playing.nowMs }) {
            if (timeouts.remove(entry)) entry.second()
        }
    }

    companion object {
        const val TAP_TIMEOUT_MS = 300L
    }
}

/**
 * A headset tap waits for a double tap on the playlist, not on its part: a part change keeps it and its timeout toggles
 * play and pause; a second tap is Next; another playlist, the end or the service going drops it.
 */
fun headsetTapsBelongToThePlaylistNotThePart() {
    val playing = Playing(ServiceRig())
    val machine = playing.machine
    val t = Taps(playing)
    val g = playing.playlist()
    playing.deliver()
    machine.onPrepared(playing.tokens.last())
    check(machine.status.sessionState == RecordingPlayer.SESSION_PLAYING) { "it plays call 1" }

    // A tap on call 1's part, the playlist on to the next part before the timeout: the tap still pauses.
    t.taps.keyDown(g, 0)
    machine.next(g)
    machine.onPrepared(playing.tokens.last())
    t.taps.follow(g)
    check(machine.status.call == 1 && t.toggles.isEmpty()) { "the part changed, the tap still waits" }
    t.elapse(Taps.TAP_TIMEOUT_MS - 1)
    check(t.toggles.isEmpty()) { "not before the double-tap timeout" }
    t.elapse(1)
    check(t.toggles == listOf(g) && machine.status.sessionState == RecordingPlayer.SESSION_PAUSED) {
        "the timeout toggles: paused"
    }
    t.taps.keyDown(g, 0)
    t.elapse(Taps.TAP_TIMEOUT_MS)
    check(machine.status.sessionState == RecordingPlayer.SESSION_PLAYING) { "a single tap again: playing" }

    // Two taps within the timeout: Next, and no toggle after.
    machine.previous(g)
    machine.onPrepared(playing.tokens.last())
    check(machine.status.call == 0) { "back on call 1" }
    t.taps.keyDown(g, 0)
    t.elapse(Taps.TAP_TIMEOUT_MS / 2)
    t.taps.keyDown(g, 0)
    check(t.nexts == listOf(g) && machine.status.call == 1) { "a double tap is Next" }
    t.elapse(Taps.TAP_TIMEOUT_MS)
    check(t.toggles.size == 2 && t.timeouts.isEmpty()) { "and its timeout is gone" }

    // A long press, or another media key, counts the waiting tap as single at once.
    t.taps.keyDown(g, 0)
    t.taps.keyDown(g, 1)
    check(t.toggles.size == 3 && t.timeouts.isEmpty()) { "a repeat settles the tap now" }
    t.taps.keyDown(g, 1)
    check(t.toggles.size == 3) { "a repeat with no tap waiting does nothing" }
    t.taps.keyDown(g, 0)
    t.taps.otherKey()
    check(t.toggles.size == 4 && t.timeouts.isEmpty()) { "another key settles it first" }

    // Another playlist takes the session: the tap goes, with its timeout.
    t.taps.keyDown(g, 0)
    val b = playing.playlist()
    t.taps.follow(b)
    check(t.timeouts.isEmpty()) { "the timeout is removed" }
    t.elapse(Taps.TAP_TIMEOUT_MS)
    check(t.toggles.size == 4 && t.nexts.size == 1) { "a tap for the old playlist is dropped" }
    t.taps.keyDown(g, 0)
    t.taps.keyDown(b, 0)
    t.elapse(Taps.TAP_TIMEOUT_MS)
    check(t.nexts.size == 1 && t.toggles == listOf(g, g, g, g, b)) { "a tap of each playlist is no double tap" }

    // The playlist's end, and the service going, drop it too.
    t.taps.keyDown(b, 0)
    t.taps.ended(g)
    check(t.timeouts.size == 1) { "another playlist's end leaves it" }
    t.taps.ended(b)
    t.taps.keyDown(b, 0)
    t.taps.drop()
    t.elapse(Taps.TAP_TIMEOUT_MS)
    check(t.toggles.size == 5 && t.timeouts.isEmpty()) { "ended or torn down, nothing toggles" }
    t.taps.keyDown(0, 0)
    check(t.timeouts.isEmpty()) { "no playlist, no tap" }
}

/**
 * The transport keys are the session's own queue commands against the player's state on the looper (#909): paused, a
 * headset tap and then a dedicated Pause within the double-tap wait: the tap plays first, then the Pause pauses.
 */
fun transportKeysFollowAWaitingTapOnThePlayersState() {
    val playing = Playing(ServiceRig())
    val machine = playing.machine
    val t = Taps(playing)
    val seen = mutableListOf<Int>()
    val keys = SessionKeys(t.taps) { g, command ->
        seen += machine.status.sessionState
        if (playing.rig.phases.isOwner(g)) command(machine, g)
    }
    val g = playing.playlist()
    playing.deliver()
    machine.onPrepared(playing.tokens.last())
    machine.pause(g)
    check(machine.status.sessionState == RecordingPlayer.SESSION_PAUSED) { "paused" }

    check(keys.onKey(g, MediaKeys.HEADSETHOOK, down = true, repeat = 0)) { "the tap is the session's" }
    check(keys.onKey(g, MediaKeys.HEADSETHOOK, down = false, repeat = 0) && t.toggles.isEmpty()) { "it waits" }
    check(keys.onKey(g, MediaKeys.MEDIA_PAUSE, down = true, repeat = 0)) { "Pause is the session's too" }
    check(t.toggles == listOf(g) && seen == listOf(RecordingPlayer.SESSION_PLAYING)) {
        "the waiting tap counts first, as a single tap: it plays, and the Pause finds it playing"
    }
    check(machine.status.sessionState == RecordingPlayer.SESSION_PAUSED) { "then the Pause pauses: paused at the end" }
    t.elapse(Taps.TAP_TIMEOUT_MS)
    check(t.toggles.size == 1 && machine.status.sessionState == RecordingPlayer.SESSION_PAUSED) {
        "nothing left waiting"
    }

    // Their ups and repeats are taken and ignored.
    check(keys.onKey(g, MediaKeys.MEDIA_PAUSE, down = false, repeat = 0) && seen.size == 1) { "an up does nothing" }
    check(keys.onKey(g, MediaKeys.MEDIA_PLAY, down = true, repeat = 1) && seen.size == 1) { "a repeat does nothing" }

    // Each key is its command, on whatever state the player is in; one the state does not allow does nothing.
    check(keys.onKey(g, MediaKeys.MEDIA_PAUSE, down = true, repeat = 0)) { "taken" }
    check(machine.status.sessionState == RecordingPlayer.SESSION_PAUSED) { "Pause while paused: still paused" }
    keys.onKey(g, MediaKeys.MEDIA_PLAY, down = true, repeat = 0)
    check(machine.status.sessionState == RecordingPlayer.SESSION_PLAYING) { "Play plays" }
    keys.onKey(g, MediaKeys.MEDIA_PLAY, down = true, repeat = 0)
    check(machine.status.sessionState == RecordingPlayer.SESSION_PLAYING) { "Play while playing: still playing" }
    keys.onKey(g, MediaKeys.MEDIA_NEXT, down = true, repeat = 0)
    machine.onPrepared(playing.tokens.last())
    check(machine.status.call == 1) { "Next" }
    keys.onKey(g, MediaKeys.MEDIA_PREVIOUS, down = true, repeat = 0)
    machine.onPrepared(playing.tokens.last())
    check(machine.status.call == 0) { "Previous" }
    keys.onKey(g, MediaKeys.MEDIA_STOP, down = true, repeat = 0)
    check(machine.status.endedReason == RecordingPlayer.ENDED_STOPPED) { "Stop" }

    // Any other key is the framework's, after a waiting tap counts as single.
    val h = playing.playlist()
    playing.deliver()
    machine.onPrepared(playing.tokens.last())
    keys.onKey(h, MediaKeys.HEADSETHOOK, down = true, repeat = 0)
    check(!keys.onKey(h, MediaKeys.MEDIA_REWIND, down = true, repeat = 0)) { "Rewind is left to the framework" }
    check(t.toggles.last() == h && t.timeouts.isEmpty()) { "after the waiting tap counted" }
}

fun main() {
    aPromoteForTheCurrentRequestMakesItTheOwner()
    aPromoteForASettledRequestOnlyRetires()
    aPromoteFromAnotherProcessServesNothing()
    atMostOneStartIsOutstanding()
    aStalePromoteKeepsTheOwnersNotification()
    aStaleTransportNeverActsOnALaterPlaylist()
    theRetireCheckNeedsNoOwnerNoRequestAndNoStart()
    theDestroyIsFencedToWhatTheInstanceServed()
    releaseStopsAPromotedServiceAndLeavesAnIssuedStart()
    aRefusedStartSettlesItsOwnRequest()
    theTaskRemovedEndsWhatTheServiceServesOrWaitsFor()
    aRefusedStartSettlesWhateverWaitedForIt()
    aNewInstanceEndsTheRetiringPhase()
    thePlayersEndsDriveTheServiceOrder()
    aSessionCommandActsOnTheTargetItWasQueuedFor()
    headsetTapsBelongToThePlaylistNotThePart()
    transportKeysFollowAWaitingTapOnThePlayersState()
    println("PASS: the playback service promotes, hands over, drops stale deliveries, retires and tears down in order")
}
