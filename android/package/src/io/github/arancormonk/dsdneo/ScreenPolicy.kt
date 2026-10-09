// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

/** A [ScreenPolicy]'s state, read-only: what [ScreenController.state] hands out, so nothing bypasses the effects. */
interface ScreenPolicyView {
    val mode: ScreenPolicy.Mode
    val delaySeconds: Int

    /** The running decoder session's id, or null with none. */
    val session: Long?

    // Where DSD-neo's activity stands.
    val visible: Boolean
    val focused: Boolean
    val top: Boolean
    val multiWindow: Boolean

    /** Whether the screen is on, as the screen-on and screen-off broadcasts (and a refused wake) last said. */
    val interactive: Boolean

    val armed: Boolean
    val snoozed: Boolean

    /**
     * Whether a sample's new audio could wake the screen as things stand: Off between calls, a session, the activity
     * not visible, armed, and the screen off. The rest of the wake rule is the sample's own (the audio's age, a snooze
     * that audio may end) and Android's verdict, which [ScreenPolicy.sample] asks for last, only for a sample that
     * would otherwise wake. Every wake is judged through this same property.
     */
    val wakeVerdictNeeded: Boolean

    /** Whether a foreground-loss sequence has started and not yet ended by DSD-neo being in front again. */
    val lossSequenceOpen: Boolean

    /**
     * When DSD-neo's own wake turned the screen on with an arming to carry, and the phone's timeout then; null and 0
     * otherwise. Both go at the next screen-off, and as soon as that arming ends or is set afresh, so the own-wake
     * window judges only an arming the wake carried.
     */
    val wakeAt: Long?
    val wakeTimeoutMs: Long

    /** When the lease output last went from held to released. */
    val leaseReleasedAt: Long?

    /** The last audible-audio stamp this session has seen; the policy is its only keeper. */
    val lastStamp: ULong?
    val lastAudio: Long?
    val lastTouch: Long?
    val lastStart: Long?
    val lastConfig: Long?

    /** Whether the touch gesture in progress began on a dimmed screen, so every event of it is swallowed. */
    val swallowing: Boolean

    /** Advances once per wake the glue should carry out. */
    val wakeSerial: Long

    val outputs: ScreenPolicy.Outputs

    /** The next instant an output changes by time alone, or null. */
    val deadline: Long?
}

/**
 * What the screen should do between calls: keep it on, dim it, hold it on with DSD-neo's own lease, or wake it for a
 * new call. Plain Kotlin with no Android types, so it runs on the JVM; [ScreenController] applies its outputs.
 *
 * Qt for Android stops its event loop while the activity is paused or stopped, so this lives here rather than in
 * QML, fed by the decoder service's once-a-second status poll and by the activity's lifecycle and input callbacks. Not
 * thread-safe: every call comes from the main thread. Time is whatever [clock] says, in milliseconds; nothing here
 * reads a system clock.
 *
 * Outputs, recomputed after every input:
 * - [Outputs.keepScreenOn]: visible, and Always on, or Dim with a session (the window's keep-on flag).
 * - [Outputs.dimmed]: visible, focused, outside multi-window (the brightness override is panel-wide), in a
 *   between-calls mode with a session, and not engaged.
 * - [Outputs.holdLease]: Off between calls with a session, engaged, not snoozed, and either visible or armed with the
 *   screen on (after DSD-neo's own wake).
 * - [deadline]: the next instant an output changes by time alone.
 *
 * Engagement is audible audio, a touch, a session or foreground start, or a setting change within the last
 * [delaySeconds]. New audible audio is a changed stamp with an age of zero or more; the first readable sample of a
 * session only records its stamp, a changed stamp without an age (-1: none, expired or reset) records the stamp and
 * nothing else, and a sample without a stamp (no readable record) changes nothing. The policy is the only keeper of the
 * last stamp. A wake ([wakeSerial] advancing) needs that new audio to be at most [WAKE_MAX_AGE_MS] old, Off mode, a
 * session, the activity not visible, armed, not snoozed, the screen off, and the caller's [wakeAllowed] verdict, which
 * is asked for last and only then, since it costs the glue two binder calls; the conditions that do not depend on the
 * sample are [wakeVerdictNeeded]. A blocked wake still counts as audio. Audio that would wake but for the activity
 * still being visible (the display went off in front, and Android has not stopped DSD-neo yet) is held for [stopped],
 * which judges it the same way once the activity is gone, its age then included.
 *
 * Arming means DSD-neo was the app in front when the display went off. Only the first foreground-loss callback of a
 * sequence decides it (focus, top-resumed, paused or stopped, whichever Android delivers first), from whether DSD-neo
 * was top-resumed and focused just before and whether the screen was already off. Regaining the foreground ends the
 * sequence and clears the arming and any snooze. Focus coming back while DSD-neo is still top-resumed and visible (a
 * closed notification shade or dialog) also ends the sequence, so the next sleep in front can arm; it clears nothing.
 * The screen counts as on or off only by the screen broadcasts ([screenOn], [screenOff]) and [wakeRefused], so the
 * glue's sample just before [screenOff] cannot wake a screen the user has just turned off. Any screen-on DSD-neo did
 * not cause (the user's, or another app's or a notification's), or an unlock, disarms at once. After DSD-neo's own
 * wake the arming it carried survives the next screen-off only if that off comes no later than
 * `max(leaseReleasedAt, wakeAt + screen-off timeout) + OWN_WAKE_GRACE_MS` (a lease still held at that off counts as
 * released by it); a later off means someone used the phone. That window judges only an arming the wake carried. An
 * own wake keeps its time only while armed, and whatever ends that arming or sets one afresh drops it: an unlock,
 * DSD-neo back in front (on a phone with no lock screen, before or after the wake's screen-on broadcast), or the first
 * loss of a new sequence. So an arming from a later sleep in front is judged like any other.
 *
 * A screen-off (or arming loss) while audio is engaged snoozes: no wakes until a full delay passes without audio,
 * judged by when audio was heard rather than when a poll reported it.
 * Touches and starts never snooze. The glue takes a synchronous status sample just before those callbacks so audio
 * that began between polls is known.
 *
 * Two platform limits are accepted rather than solved:
 * - Lifecycle callbacks are delivered asynchronously. A Home press followed quickly by Power, processed late on the
 *   main thread, reaches this policy as a loss with the screen already off and looks exactly like a sleep in front,
 *   so it arms.
 * - Quick use of the secure camera from the lock screen, inside the window an own wake's screen is expected to stay
 *   on, gives DSD-neo no callback at all; arming survives it, and the next call wakes the screen again.
 */
class ScreenPolicy(private val clock: () -> Long) : ScreenPolicyView {
    /** The screen modes. The codes are a contract with Qt's `AppPrefs::ScreenMode`; never renumber them. */
    enum class Mode(val code: Int) {
        SYSTEM(0),
        ALWAYS_ON(1),
        DIM_BETWEEN_CALLS(2),
        OFF_BETWEEN_CALLS(3),
        ;

        companion object {
            /** The mode for [code]; anything unknown reads as [SYSTEM]. */
            fun fromCode(code: Int): Mode = entries.firstOrNull { it.code == code } ?: SYSTEM
        }
    }

    data class Outputs(val keepScreenOn: Boolean, val dimmed: Boolean, val holdLease: Boolean)

    override var mode = Mode.SYSTEM
        private set
    override var delaySeconds = DEFAULT_DELAY_SECONDS
        private set
    private val delayMs: Long get() = delaySeconds * 1_000L
    override var session: Long? = null
        private set
    override var visible = false
        private set
    override var focused = false
        private set
    override var top = false
        private set
    override var multiWindow = false
        private set
    override var interactive = true
        private set
    override var armed = false
        private set
    override var snoozed = false
        private set
    override var lossSequenceOpen = false
        private set
    override var wakeAt: Long? = null
        private set
    override var wakeTimeoutMs = 0L
        private set
    override var leaseReleasedAt: Long? = null
        private set
    override var lastStamp: ULong? = null
        private set
    override var lastAudio: Long? = null
        private set
    override var lastTouch: Long? = null
        private set
    override var lastStart: Long? = null
        private set
    override var lastConfig: Long? = null
        private set

    /** Whether this session's first readable sample has been taken (it only records the stamp). */
    private var primed = false

    /**
     * When the newest audio was heard that would have woken the screen but for DSD-neo still being visible: armed and
     * asleep in Off between calls, before Android stopped the activity. [stopped] judges it again; null for none. A
     * change of arming, the screen coming on, and a session starting or ending drop it.
     */
    private var heldAudio: Long? = null

    override var swallowing = false
        private set
    override var wakeSerial = 0L
        private set
    override var outputs = Outputs(keepScreenOn = false, dimmed = false, holdLease = false)
        private set
    override var deadline: Long? = null
        private set

    private val between: Boolean
        get() = session != null && (mode == Mode.DIM_BETWEEN_CALLS || mode == Mode.OFF_BETWEEN_CALLS)

    /** Off between calls with a session, armed and the screen off: a wake waits only on the activity being gone. */
    private val armedAsleep: Boolean
        get() = session != null && mode == Mode.OFF_BETWEEN_CALLS && armed && !interactive

    override val wakeVerdictNeeded: Boolean
        get() = armedAsleep && !visible

    fun configure(modeCode: Int, delaySeconds: Int) = step { now ->
        val newMode = Mode.fromCode(modeCode)
        val newDelay = sanitizeDelaySeconds(delaySeconds)
        if (newMode != mode || newDelay != this.delaySeconds) {
            mode = newMode
            this.delaySeconds = newDelay
            lastConfig = now
        }
    }

    /** A decoder session is running. Idempotent for the running [id], which duplicate START intents repeat. */
    fun sessionStarted(id: Long) = step { now ->
        if (id != session) {
            session = id
            primed = false
            snoozed = false
            heldAudio = null
            lastStart = now
        }
    }

    /**
     * The session ended: brightness, keep-on, lease and deadline all follow from there being none. Keeps the arming.
     */
    fun sessionEnded() = step {
        session = null
        snoozed = false
        heldAudio = null
    }

    /**
     * One status poll (or the glue's synchronous sample): [stamp] identifies the last audible audio and [ageMs] is how
     * long ago it was heard, or -1 for none. A null [stamp] means no readable record, and changes nothing. Ignored
     * outside a session. [wakeAllowed] is Android's verdict (D5), asked at most once and only for audio that would
     * otherwise wake the screen.
     */
    fun sample(stamp: ULong?, ageMs: Long, wakeAllowed: () -> Boolean) = step { now ->
        if (session == null || stamp == null) {
            return@step
        }
        if (!primed) {
            // Whatever the stamp says, it was there before this session's first look.
            primed = true
            lastStamp = stamp
            return@step
        }
        if (stamp == lastStamp) {
            return@step
        }
        lastStamp = stamp
        if (ageMs < 0) {
            return@step
        }
        val heardAt = now - ageMs
        val previous = lastAudio
        // A snooze ends only if this audio was heard a full delay after the last: judged by when it was heard, not
        // when the poll ran, so audio a poll reports late still continues the run of calls.
        if (snoozed && (previous == null || heardAt >= previous + delayMs)) {
            snoozed = false
        }
        lastAudio = if (previous == null) heardAt else maxOf(previous, heardAt)
        if (ageMs > WAKE_MAX_AGE_MS) {
            return@step
        }
        if (wakeVerdictNeeded) {
            wakeUnlessBlocked(wakeAllowed)
        } else if (armedAsleep) {
            // The display went off in front, but Android has not stopped DSD-neo yet: the stop judges this audio.
            heldAudio = heldAudio?.let { maxOf(it, heardAt) } ?: heardAt
        }
    }

    fun focusChanged(focused: Boolean, interactive: Boolean) = step { now ->
        if (focused) {
            this.focused = true
            lastTouch = now
            // Focus back while still top-resumed and visible (a closed shade or dialog): DSD-neo is in front again,
            // so the next loss starts a new sequence.
            if (top && visible) {
                lossSequenceOpen = false
            }
        } else {
            loss(now, interactive)
            this.focused = false
        }
    }

    fun topResumedChanged(top: Boolean, interactive: Boolean) = step { now ->
        if (top) {
            regain(now)
        } else {
            loss(now, interactive)
            this.top = false
        }
    }

    fun paused(interactive: Boolean) = step { now ->
        loss(now, interactive)
        top = false
    }

    /**
     * The activity stopped. Audio heard while the display went off in front but the activity was still visible (see
     * [sample]) is judged now as any wake: at most [WAKE_MAX_AGE_MS] old by now, not snoozed, and [wakeAllowed], asked
     * only then. Read after the loss, so a stop that arms afresh judges nothing heard before it.
     */
    fun stopped(interactive: Boolean, wakeAllowed: () -> Boolean) = step { now ->
        loss(now, interactive)
        visible = false
        top = false
        val held = heldAudio
        heldAudio = null
        if (held != null && now - held <= WAKE_MAX_AGE_MS && wakeVerdictNeeded) {
            wakeUnlessBlocked(wakeAllowed)
        }
    }

    fun started() = step { now -> regain(now) }

    fun destroyed() = step {
        visible = false
        top = false
        // A destroyed window has no focus either; a recreated one gets its own focus callback.
        focused = false
        setArmed(false)
        swallowing = false
    }

    fun multiWindowChanged(multiWindow: Boolean) = step {
        this.multiWindow = multiWindow
    }

    /** A key, a generic motion event or Back: counts as a touch while the activity is visible. Never vetoed. */
    fun userInteraction() = step { now -> interact(now) }

    /**
     * One touch event, by its masked action. Returns whether the activity should swallow it: every event of a
     * gesture that began on a dimmed screen, so the tap that brightens does nothing else. Under touch exploration
     * ([exploring], TalkBack) a touch brightens but is never swallowed.
     */
    fun touch(actionMasked: Int, exploring: Boolean): Boolean = step { now ->
        when (actionMasked) {
            ACTION_DOWN -> {
                // The dimming on screen now, before this touch brightens it.
                swallowing = outputs.dimmed && !exploring
                interact(now)
                swallowing
            }
            ACTION_UP, ACTION_CANCEL -> {
                val swallow = swallowing
                swallowing = false
                interact(now)
                swallow
            }
            // MOVE, POINTER_DOWN, POINTER_UP and anything else belong to the gesture in progress.
            else -> {
                interact(now)
                swallowing
            }
        }
    }

    /**
     * The screen came on: by DSD-neo's own wake ([ours]), with the phone's [screenOffTimeoutMs], or by anything else,
     * which disarms. An own wake keeps its time for the [screenOff] window only if it carried an arming: on a phone
     * with no lock screen DSD-neo can be back in front, with nothing armed, before the broadcast lands.
     */
    fun screenOn(ours: Boolean, screenOffTimeoutMs: Long) = step { now ->
        interactive = true
        heldAudio = null
        if (!ours) {
            setArmed(false)
        } else if (armed) {
            wakeAt = now
            wakeTimeoutMs = screenOffTimeoutMs.coerceAtLeast(0L)
        }
    }

    /** The screen went off. The glue samples the decoder status first. */
    fun screenOff() = step { now ->
        // A lease still held at this off was held up to it.
        val leaseEnd = if (outputs.holdLease) now else leaseReleasedAt
        interactive = false
        val woke = wakeAt
        if (armed && woke != null) {
            val expectedOff = maxOf(leaseEnd ?: Long.MIN_VALUE, woke + wakeTimeoutMs)
            if (now > expectedOff + OWN_WAKE_GRACE_MS) {
                armed = false
            }
        }
        if (between && audioEngagedAt(now)) {
            snoozed = true
        }
        forgetWake()
    }

    /** The user unlocked the phone. */
    fun userPresent() = step {
        setArmed(false)
    }

    /** The wake the glue tried left the screen off. */
    fun wakeRefused() = step {
        interactive = false
        snoozed = true
    }

    /** The scheduled [deadline] came (or any moment): recompute by time. */
    fun tick() = step { }

    private fun regain(now: Long) {
        top = true
        visible = true
        setArmed(false)
        snoozed = false
        lastStart = now
        lossSequenceOpen = false
    }

    /** A foreground-loss callback; only the first of a sequence judges arming, from the state just before it. */
    private fun loss(now: Long, interactive: Boolean) {
        if (lossSequenceOpen) {
            return
        }
        lossSequenceOpen = true
        if (top && focused && !interactive) {
            setArmed(true)
            snoozed = between && audioEngagedAt(now)
        } else {
            setArmed(false)
        }
    }

    /**
     * Arms afresh, or disarms. Either way no arming an own wake carried is left, so that wake's window goes as well:
     * [screenOff] judges by it only an arming the wake carried, never one set after it. Audio held for the stop goes
     * too: it was heard under the old arming.
     */
    private fun setArmed(armed: Boolean) {
        this.armed = armed
        heldAudio = null
        forgetWake()
    }

    /** A wake, unless snoozed or Android refuses: the verdict is asked last, and only then. */
    private fun wakeUnlessBlocked(wakeAllowed: () -> Boolean) {
        if (!snoozed && wakeAllowed()) {
            wakeSerial++
        }
    }

    private fun forgetWake() {
        wakeAt = null
        wakeTimeoutMs = 0L
    }

    private fun interact(now: Long) {
        if (visible) {
            lastTouch = now
        }
    }

    /**
     * Runs one input: the change, then housekeeping, then the outputs. Housekeeping ends a snooze once audio is no
     * longer engaged. No change reads the snooze before it except [sample], which judges it by when its new audio was
     * heard before it judges a wake, so a poll that arrives after the delay cannot end a snooze its own audio extends.
     */
    private fun <T> step(change: (now: Long) -> T): T {
        val now = clock()
        val result = change(now)
        settleSnooze(now)
        val next = outputsAt(now)
        if (outputs.holdLease && !next.holdLease) {
            leaseReleasedAt = now
        }
        outputs = next
        deadline = nextChange(now)
        return result
    }

    private fun settleSnooze(now: Long) {
        if (snoozed && !audioEngagedAt(now)) {
            snoozed = false
        }
    }

    private fun audioEngagedAt(t: Long): Boolean = lastAudio?.let { t < it + delayMs } ?: false

    private fun engagedAt(t: Long): Boolean =
        audioEngagedAt(t) || listOfNotNull(lastTouch, lastStart, lastConfig).any { t < it + delayMs }

    private fun outputsAt(t: Long): Outputs {
        val engaged = engagedAt(t)
        // A snooze lasts while audio is engaged; housekeeping clears it at the first input after.
        val snoozedAt = snoozed && audioEngagedAt(t)
        return Outputs(
            keepScreenOn = visible && (mode == Mode.ALWAYS_ON || (mode == Mode.DIM_BETWEEN_CALLS && session != null)),
            dimmed = visible && focused && !multiWindow && between && !engaged,
            holdLease = session != null && mode == Mode.OFF_BETWEEN_CALLS && engaged && !snoozedAt &&
                (visible || (armed && interactive)),
        )
    }

    /** The earliest instant after [now] at which an output changes with no further input, or null for none. */
    private fun nextChange(now: Long): Long? {
        // Outputs depend on time only through the engagement windows, so they can change only where one ends.
        return listOfNotNull(lastAudio, lastTouch, lastStart, lastConfig)
            .map { it + delayMs }
            .filter { it > now }
            .sorted()
            .firstOrNull { outputsAt(it) != outputs }
    }

    companion object {
        /** The delay choices, in seconds, that the settings offer; anything else reads as [DEFAULT_DELAY_SECONDS]. */
        val DELAY_CHOICES_SECONDS = listOf(10, 30, 60, 120, 300)
        const val DEFAULT_DELAY_SECONDS = 30

        /** The oldest audio, in ms, that may still wake the screen. */
        const val WAKE_MAX_AGE_MS = 2_000L

        /** How late, in ms, an own wake's screen-off may come and still count as nobody having used the phone. */
        const val OWN_WAKE_GRACE_MS = 5_000L

        // AudioManager.getMode() values (android.media.AudioManager.MODE_*).
        const val AUDIO_MODE_NORMAL = 0
        const val AUDIO_MODE_RINGTONE = 1
        const val AUDIO_MODE_IN_CALL = 2
        const val AUDIO_MODE_IN_COMMUNICATION = 3
        const val AUDIO_MODE_CALL_SCREENING = 4
        const val AUDIO_MODE_CALL_REDIRECT = 5
        const val AUDIO_MODE_COMMUNICATION_REDIRECT = 6

        // NotificationManager.getCurrentInterruptionFilter() values
        // (android.app.NotificationManager.INTERRUPTION_FILTER_*).
        const val INTERRUPTION_FILTER_UNKNOWN = 0
        const val INTERRUPTION_FILTER_ALL = 1
        const val INTERRUPTION_FILTER_PRIORITY = 2
        const val INTERRUPTION_FILTER_NONE = 3
        const val INTERRUPTION_FILTER_ALARMS = 4

        // MotionEvent.getActionMasked() values (android.view.MotionEvent.ACTION_*).
        const val ACTION_DOWN = 0
        const val ACTION_UP = 1
        const val ACTION_MOVE = 2
        const val ACTION_CANCEL = 3
        const val ACTION_POINTER_DOWN = 5
        const val ACTION_POINTER_UP = 6

        /**
         * Whether a call may wake the screen: only in the normal audio mode (no phone or VoIP call, no ringing, no
         * call screening or redirection) and with no Do Not Disturb filter. Every other mode, including ones newer
         * than this list, blocks.
         */
        fun wakeAllowed(audioMode: Int, interruptionFilter: Int): Boolean =
            audioMode == AUDIO_MODE_NORMAL &&
                (interruptionFilter == INTERRUPTION_FILTER_ALL || interruptionFilter == INTERRUPTION_FILTER_UNKNOWN)

        /** [seconds] if it is one of [DELAY_CHOICES_SECONDS], else [DEFAULT_DELAY_SECONDS]. */
        fun sanitizeDelaySeconds(seconds: Int): Int =
            if (seconds in DELAY_CHOICES_SECONDS) seconds else DEFAULT_DELAY_SECONDS
    }
}
