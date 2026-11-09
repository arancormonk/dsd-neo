// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

/**
 * The RadioReference sign-in form as Android's Autofill Framework sees it, and every decision about what to tell it.
 * [SignInAutofill] feeds it Qt's calls and carries out the [Effect]s each event returns.
 *
 * A form session runs under a token, positive and only growing, from [begin] to [end]; an event for any other token
 * does nothing, and a retired token never comes back. For the live token it keeps the snapshot of the two fields Qt
 * reported (field [USERNAME] and field [PASSWORD]: the rect in Qt window pixels, whether it is visible and focused, and
 * the structure value, always empty for the password), the Qt window's [origin] relative to the host view, and whether
 * a request started a framework session ([sessionStarted]). The rules:
 * - a [request] for a shown field goes out only while the service is enabled and the origin is known, and starts the
 *   framework session; a skipped request waits for the next focus entry;
 * - a field losing focus exits it, in a started session only;
 * - [commit] reports both values, commits, then hides both fields, in a started session only, and ends that session;
 *   a later request under the same token starts a new one;
 * - [end] cancels a started session and clears everything; a newer [begin] does the same for the older token;
 * - [disabled] (the service went away) drops the session flag without effects and keeps the snapshot.
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
    }

    private data class Field(val windowRect: Rect, val visible: Boolean, val focused: Boolean, val value: String)

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

    private var lastToken = 0
    private var origin: Offset? = null
    private val fields = sortedMapOf<Int, Field>()

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
        fields[vid] = Field(windowRect, visible, focused, if (vid == PASSWORD) "" else value)
        return if (sessionStarted && before?.focused == true && !focused) listOf(Effect.Exit(vid)) else NONE
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
        val offset = origin
        if (offset == null || !field.visible) {
            return NONE
        }
        sessionStarted = true
        return listOf(Effect.Request(vid, windowRect.moved(offset)))
    }

    fun commit(token: Int, username: String, password: String): List<Effect> {
        if (!isLive(token) || !sessionStarted) {
            return NONE
        }
        sessionStarted = false
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

    private fun clear() {
        token = 0
        sessionStarted = false
        fields.clear()
    }
}
