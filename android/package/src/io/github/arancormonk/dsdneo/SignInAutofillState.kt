// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

/**
 * The RadioReference sign-in form as Android's Autofill Framework sees it, and every decision about what to tell it.
 * [SignInAutofill] feeds it Qt's calls and the framework's fill UI events, and carries out the [Effect]s each event
 * returns.
 *
 * A form session runs under a token, positive and only growing, from [begin] to [end]; an event for any other token
 * does nothing, and a retired token never comes back. For the live token it keeps the snapshot of the two fields Qt
 * reported (field [USERNAME] and field [PASSWORD]: the rect in Qt window pixels, whether it is visible and focused, and
 * the structure value, always empty for the password), the Qt window's [origin] relative to the host view, and whether
 * a request started a framework session ([sessionStarted]). A field is entered while it is both focused and visible.
 * The rules:
 * - a [request] for a shown field goes out only while the service is enabled, and starts the framework session; with
 *   the origin not known yet it is held ([Effect.Held]) and goes out from [retryHeld] once the origin is known, while
 *   the field is still entered; leaving the field, the service going away or the token ending drops it;
 * - a field that stops being entered (focus lost, or hidden while focused) is exited, in a started session only;
 * - while the framework reports its fill UI shown for the entered field ([fillUi]), a move of that field arms the
 *   settle wait ([Effect.Settle]); [settled] then re-anchors the UI once at the field's new place (an exit, then an
 *   enter with the new bounds), if the field is still entered, still moved and the UI still shown;
 * - [commit] reports both values, commits, then hides both fields, in a started session only, and ends that session;
 *   a later request under the same token starts a new one;
 * - [end] cancels a started session and clears everything; a newer [begin] does the same for the older token;
 * - [disabled] (the service went away) drops the session flag, a held request and the fill UI state without effects,
 *   and keeps the snapshot.
 *
 * Plain Kotlin with no Android imports, so the JVM tests run it as is. Not thread-safe: [SignInAutofill] calls it on
 * the main thread only.
 */
class SignInAutofillState {
    /** A rect in pixels: Qt window pixels as reported, host pixels in effects and the structure. */
    data class Rect(val left: Int, val top: Int, val width: Int, val height: Int) {
        fun moved(by: Offset) = Rect(left + by.dx, top + by.dy, width, height)
    }

    /** Where the Qt window's top left corner is, in the host view's pixels. */
    data class Offset(val dx: Int, val dy: Int)

    /** One field of the structure: its id, host-pixel rect, visibility, focus and structure value. */
    data class Child(val vid: Int, val rect: Rect, val visible: Boolean, val focused: Boolean, val value: String)

    /** What the framework is told. A [Value] carries a credential, so its text stays out of [toString]. */
    sealed class Effect {
        data class Request(val vid: Int, val rect: Rect) : Effect()

        data class Exit(val vid: Int) : Effect()

        data class Value(val vid: Int, val text: String) : Effect() {
            override fun toString() = "Value(vid=$vid)"
        }

        data object Commit : Effect()

        data object Cancel : Effect()

        data class Visibility(val vid: Int, val visible: Boolean) : Effect()

        /** Field [vid]'s request waits for the Qt window's origin: a diagnostic only, no framework call. */
        data class Held(val vid: Int) : Effect()

        /** Arm (or restart) the settle wait for field [vid]; [settled] answers it. No framework call. */
        data class Settle(val vid: Int) : Effect()

        /** Enter field [vid] again at [rect], host pixels: with the response the UI already has, no new request. */
        data class Enter(val vid: Int, val rect: Rect) : Effect()
    }

    private data class Field(val windowRect: Rect, val visible: Boolean, val focused: Boolean, val value: String) {
        val entered get() = focused && visible
    }

    companion object {
        const val USERNAME = 1
        const val PASSWORD = 2

        private val NONE = emptyList<Effect>()
    }

    /** The live token, 0 when no form is shown. */
    var token = 0
        private set

    /** Whether a request started a framework session that is not committed, cancelled or dropped yet. */
    var sessionStarted = false
        private set

    /** Whether a request waits for the Qt window's origin. */
    val hasHeldRequest get() = held != 0

    private var lastToken = 0
    private var origin: Offset? = null
    private val fields = sortedMapOf<Int, Field>()

    /** The field whose request waits for the origin, 0 for none. */
    private var held = 0

    /** The field the framework reports its fill UI shown for, 0 for none. */
    private var shownFor = 0

    /** The field whose settle wait is armed, 0 for none. */
    private var settling = 0

    /** Where each field's request or last re-anchor put the fill UI, in Qt window pixels. */
    private val anchors = HashMap<Int, Rect>()

    /** The Qt window's origin in host pixels, or null while it is not known. Kept across tokens. */
    fun origin(offset: Offset?) {
        origin = offset
    }

    fun begin(token: Int): List<Effect> {
        if (token <= lastToken) {
            return NONE
        }
        val effects = if (sessionStarted) listOf(Effect.Cancel) else NONE
        clear()
        lastToken = token
        this.token = token
        return effects
    }

    fun field(token: Int, vid: Int, windowRect: Rect, visible: Boolean, focused: Boolean, value: String): List<Effect> {
        if (!isLive(token) || !isField(vid)) {
            return NONE
        }
        val before = fields[vid]
        val now = Field(windowRect, visible, focused, if (vid == PASSWORD) "" else value)
        fields[vid] = now
        if (!now.entered) {
            if (held == vid) {
                held = 0
            }
            if (shownFor == vid) {
                shownFor = 0
            }
            return if (sessionStarted && before?.entered == true) listOf(Effect.Exit(vid)) else NONE
        }
        if (sessionStarted && shownFor == vid && windowRect != anchors[vid]) {
            settling = vid
            return listOf(Effect.Settle(vid))
        }
        return NONE
    }

    fun request(token: Int, vid: Int, windowRect: Rect, enabled: Boolean): List<Effect> {
        if (!isLive(token)) {
            return NONE
        }
        val field = fields[vid] ?: return NONE
        fields[vid] = field.copy(windowRect = windowRect)
        if (!enabled) {
            disabled()
            return NONE
        }
        if (!field.visible) {
            return NONE
        }
        if (origin == null) {
            held = vid
            return listOf(Effect.Held(vid))
        }
        return send(vid)
    }

    /** The origin may be known now: a held request goes out if its field is still entered and the service on. */
    fun retryHeld(enabled: Boolean): List<Effect> {
        val vid = held
        if (vid == 0 || token == 0) {
            return NONE
        }
        if (!enabled) {
            disabled()
            return NONE
        }
        if (origin == null) {
            return NONE
        }
        held = 0
        return if (fields[vid]?.entered == true) send(vid) else NONE
    }

    /** The framework showed ([shown]) or hid its fill UI for field [vid] of the live form. */
    fun fillUi(vid: Int, shown: Boolean): List<Effect> {
        if (token == 0 || !isField(vid)) {
            return NONE
        }
        if (shown) {
            shownFor = vid
        } else if (shownFor == vid) {
            shownFor = 0
        }
        return NONE
    }

    /** The settle wait armed by [Effect.Settle] ran out: re-anchor the shown fill UI once, if it still applies. */
    fun settled(): List<Effect> {
        val vid = settling
        settling = 0
        val field = fields[vid]
        val offset = origin
        if (vid == 0 || token == 0 || !sessionStarted || shownFor != vid || offset == null || field == null ||
            !field.entered || field.windowRect == anchors[vid]
        ) {
            return NONE
        }
        anchors[vid] = field.windowRect
        return listOf(Effect.Exit(vid), Effect.Enter(vid, field.windowRect.moved(offset)))
    }

    fun commit(token: Int, username: String, password: String): List<Effect> {
        if (!isLive(token) || !sessionStarted) {
            return NONE
        }
        sessionStarted = false
        dropFramework()
        return listOf(
            Effect.Value(USERNAME, username),
            Effect.Value(PASSWORD, password),
            Effect.Commit,
            Effect.Visibility(USERNAME, false),
            Effect.Visibility(PASSWORD, false),
        )
    }

    fun end(token: Int): List<Effect> {
        if (!isLive(token)) {
            return NONE
        }
        val effects = if (sessionStarted) listOf(Effect.Cancel) else NONE
        clear()
        return effects
    }

    /** The service is off or gone: no framework session is running any more. */
    fun disabled(): List<Effect> {
        sessionStarted = false
        dropFramework()
        return NONE
    }

    /** The activity resumed with the service [enabled] or not. One turned on works from the next focus entry. */
    fun resumed(enabled: Boolean): List<Effect> = if (enabled) NONE else disabled()

    /** The structure's children, by field id: none without a live token or a known origin. */
    fun structure(): List<Child> {
        val offset = origin
        if (token == 0 || offset == null) {
            return emptyList()
        }
        return fields.map { (vid, field) -> Child(vid, field.windowRect.moved(offset), field.visible, field.focused, field.value) }
    }

    /** Whether field [vid] of the live form is visible. */
    fun isVisible(vid: Int): Boolean = token != 0 && fields[vid]?.visible == true

    private fun isLive(token: Int) = token > 0 && token == this.token

    private fun isField(vid: Int) = vid == USERNAME || vid == PASSWORD

    /** Field [vid]'s request goes out, at its rect in host pixels, and starts the framework session. */
    private fun send(vid: Int): List<Effect> {
        val field = fields.getValue(vid)
        val offset = origin ?: return NONE
        held = 0
        sessionStarted = true
        anchors[vid] = field.windowRect
        return listOf(Effect.Request(vid, field.windowRect.moved(offset)))
    }

    /** What only a running framework session keeps: a held request, the fill UI state and the settle wait. */
    private fun dropFramework() {
        held = 0
        shownFor = 0
        settling = 0
        anchors.clear()
    }

    private fun clear() {
        token = 0
        sessionStarted = false
        fields.clear()
        dropFramework()
    }
}
