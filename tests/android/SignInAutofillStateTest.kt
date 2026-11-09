// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

import io.github.arancormonk.dsdneo.SignInAutofillState.Companion.PASSWORD
import io.github.arancormonk.dsdneo.SignInAutofillState.Companion.USERNAME
import io.github.arancormonk.dsdneo.SignInAutofillState.Effect
import io.github.arancormonk.dsdneo.SignInAutofillState.Offset
import io.github.arancormonk.dsdneo.SignInAutofillState.Rect

private val USER_RECT = Rect(40, 300, 600, 90)
private val PASS_RECT = Rect(40, 420, 600, 90)
private val ORIGIN = Offset(0, 0)

private fun expect(actual: List<Effect>, vararg wanted: Effect, why: String) {
    check(actual == wanted.toList()) { "$why: expected ${wanted.toList()}, got $actual" }
}

private fun nothing(actual: List<Effect>, why: String) = expect(actual, why = why)

/** A live form under [token] with both fields shown, nothing focused, and the Qt window's origin known. */
private fun shownForm(token: Int = 1, username: String = "alice"): SignInAutofillState {
    val state = SignInAutofillState()
    state.origin(ORIGIN)
    nothing(state.begin(token), "a first begin has nothing to retire")
    nothing(state.field(token, USERNAME, USER_RECT, true, false, username), "an unfocused field sends nothing")
    nothing(state.field(token, PASSWORD, PASS_RECT, true, false, ""), "an unfocused field sends nothing")
    return state
}

/** Focus [vid] and have it requested: the framework session starts. */
private fun started(state: SignInAutofillState, token: Int, vid: Int, rect: Rect) {
    nothing(state.field(token, vid, rect, true, true, if (vid == USERNAME) "alice" else ""), "focus alone sends nothing")
    expect(state.request(token, vid, rect, enabled = true), Effect.HideTextHandle, Effect.Request(vid, rect), why = "a settled focus requests")
    check(state.sessionStarted) { "a request starts the framework session" }
}

private fun aRequestStartsTheSessionOnlyWhenEnabled() {
    val state = shownForm()
    nothing(state.field(1, USERNAME, USER_RECT, true, true, "alice"), "focus alone sends nothing")
    nothing(state.request(1, USERNAME, USER_RECT, enabled = false), "no request while the service is off")
    check(!state.sessionStarted) { "a refused request starts no session" }
    nothing(state.field(1, USERNAME, USER_RECT, true, false, "alice"), "no exit for a session never started")
    started(state, 1, USERNAME, USER_RECT)
}

private fun aRequestWaitsForTheOriginAndAppliesIt() {
    val state = SignInAutofillState()
    nothing(state.begin(4), "begin")
    nothing(state.field(4, PASSWORD, PASS_RECT, true, true, ""), "focus")
    expect(state.request(4, PASSWORD, PASS_RECT, enabled = true), Effect.Held(PASSWORD),
        why = "no request from a guessed origin: it is held")
    check(!state.sessionStarted && state.hasHeldRequest) { "a held request starts no session" }
    check(state.structure().isEmpty()) { "no structure without an origin" }
    nothing(state.retryHeld(enabled = true), "still no origin")
    check(state.hasHeldRequest)
    // The Qt window sits 30 px right of and 120 px below the host (split screen, a caption bar).
    state.origin(Offset(30, 120))
    val local = Rect(PASS_RECT.left + 30, PASS_RECT.top + 120, PASS_RECT.width, PASS_RECT.height)
    expect(state.retryHeld(enabled = true), Effect.HideTextHandle, Effect.Request(PASSWORD, local),
        why = "the held request goes out once the origin is known, in host pixels")
    check(state.sessionStarted && !state.hasHeldRequest)
    nothing(state.retryHeld(enabled = true), "and only once")
    check(state.structure().single().rect == local) { "the structure uses host pixels too" }
    state.origin(Offset(0, 0))
    check(state.structure().single().rect == PASS_RECT) { "a new origin re-applies to the stored window rect" }
    state.origin(null)
    check(state.structure().isEmpty()) { "an origin lost again hides the structure" }
}

private fun aRequestForAFieldNotShownSendsNothing() {
    val state = SignInAutofillState()
    state.origin(ORIGIN)
    state.begin(1)
    nothing(state.request(1, USERNAME, USER_RECT, enabled = true), "a field never described is not requested")
    nothing(state.field(1, USERNAME, Rect(0, 0, 0, 0), false, true, "alice"), "a hidden field")
    nothing(state.request(1, USERNAME, USER_RECT, enabled = true), "a hidden field is not requested")
    nothing(state.request(1, 3, USER_RECT, enabled = true), "only fields 1 and 2 exist")
    check(!state.sessionStarted)
}

private fun focusLossExitsOnlyAStartedSession() {
    val state = shownForm()
    started(state, 1, USERNAME, USER_RECT)
    expect(state.field(1, USERNAME, USER_RECT, true, false, "alice"), Effect.Exit(USERNAME), why = "focus left the field")
    nothing(state.field(1, USERNAME, USER_RECT, true, false, "alice"), "no second exit without a focus change")
    nothing(state.field(1, PASSWORD, PASS_RECT, true, true, ""), "the next field's focus waits for its request")
    expect(state.request(1, PASSWORD, PASS_RECT, enabled = true), Effect.HideTextHandle, Effect.Request(PASSWORD, PASS_RECT),
        why = "every focus entry gets its own request")
    expect(state.field(1, PASSWORD, PASS_RECT, false, false, ""), Effect.Exit(PASSWORD), why = "focus lost as it hides")
}

private fun commitReportsTheValuesThenCommitsThenHides() {
    val state = shownForm()
    started(state, 1, PASSWORD, PASS_RECT)
    expect(
        state.commit(1, "alice", "s3cret"),
        Effect.Value(USERNAME, "alice"),
        Effect.Value(PASSWORD, "s3cret"),
        Effect.Commit,
        Effect.Visibility(USERNAME, false),
        Effect.Visibility(PASSWORD, false),
        why = "commit's effects, in order",
    )
    check(!state.sessionStarted) { "commit ends the framework session" }
    nothing(state.commit(1, "alice", "s3cret"), "a second commit has no session to commit")
    nothing(state.end(1), "no cancel after a commit")
}

private fun aRequestAfterCommitStartsAgain() {
    val state = shownForm()
    started(state, 1, PASSWORD, PASS_RECT)
    check(state.commit(1, "alice", "s3cret").contains(Effect.Commit))
    // "Account: alice · Change" reopens the card under the same screen token.
    nothing(state.field(1, PASSWORD, PASS_RECT, true, false, ""), "no exit: the committed session is over")
    started(state, 1, USERNAME, USER_RECT)
    expect(state.end(1), Effect.Cancel, why = "the new session is cancelled when the form leaves")
}

private fun noCommitOrCancelWithoutASession() {
    val state = shownForm()
    nothing(state.commit(1, "alice", "s3cret"), "no commit when the framework never saw the form")
    nothing(state.end(1), "no cancel when the framework never saw the form")
}

private fun endCancelsAndClears() {
    val state = shownForm()
    started(state, 1, USERNAME, USER_RECT)
    expect(state.end(1), Effect.Cancel, why = "a form left without a commit cancels")
    check(!state.sessionStarted && state.token == 0) { "end clears the session and the token" }
    check(state.structure().isEmpty()) { "end clears the snapshot" }
    check(!state.isVisible(USERNAME) && !state.isVisible(PASSWORD)) { "nothing is visible after end" }
    nothing(state.end(1), "a second end is stale")
    nothing(state.field(1, USERNAME, USER_RECT, true, true, "alice"), "a retired token describes nothing")
    check(state.structure().isEmpty()) { "a retired token's field is not kept" }
}

private fun staleTokensProduceNothing() {
    val state = shownForm(token = 5)
    started(state, 5, USERNAME, USER_RECT)
    nothing(state.field(4, USERNAME, USER_RECT, true, false, "mallory"), "an older token's field")
    nothing(state.request(4, PASSWORD, PASS_RECT, enabled = true), "an older token's request")
    nothing(state.commit(4, "mallory", "x"), "an older token's commit")
    nothing(state.end(4), "an older token's end")
    nothing(state.field(6, USERNAME, USER_RECT, true, false, "mallory"), "a token not begun yet")
    nothing(state.end(0), "token 0 is never live")
    check(state.token == 5 && state.sessionStarted) { "stale calls change nothing" }
    check(state.structure().first { it.vid == USERNAME }.value == "alice") { "the snapshot stays the live token's" }
    // Tokens only grow: a retired one is never revived, and begin never goes back.
    nothing(state.begin(3), "begin with an older token")
    check(state.token == 5)
    expect(state.end(5), Effect.Cancel, why = "the live session ends")
    nothing(state.begin(5), "a retired token cannot be begun again")
    check(state.token == 0)
    nothing(state.begin(-1), "tokens are positive")
    nothing(state.begin(6), "a newer token begins")
    check(state.token == 6 && state.structure().isEmpty()) { "a new token starts with an empty snapshot" }
}

private fun aNewerBeginRetiresAStartedSession() {
    val state = shownForm(token = 2)
    started(state, 2, USERNAME, USER_RECT)
    expect(state.begin(3), Effect.Cancel, why = "the older form's session is over")
    check(state.token == 3 && !state.sessionStarted && state.structure().isEmpty())
}

private fun disabledDropsTheSessionAndKeepsTheSnapshot() {
    val state = shownForm()
    started(state, 1, USERNAME, USER_RECT)
    nothing(state.disabled(), "disabled has no effects")
    check(!state.sessionStarted) { "disabled drops the framework session" }
    check(state.token == 1) { "the token stays live" }
    val fields = state.structure()
    check(fields.map { it.vid } == listOf(USERNAME, PASSWORD)) { "the snapshot is kept: $fields" }
    check(fields[0].focused && fields[0].value == "alice")
    nothing(state.field(1, USERNAME, USER_RECT, true, false, "alice"), "no exit after the service went away")
    nothing(state.commit(1, "alice", "s3cret"), "no commit after the service went away")
    nothing(state.end(1), "no cancel after the service went away")
}

private fun resumedReadsTheServiceAgain() {
    val state = shownForm()
    started(state, 1, USERNAME, USER_RECT)
    nothing(state.resumed(enabled = true), "a resume with the service on changes nothing")
    check(state.sessionStarted)
    nothing(state.resumed(enabled = false), "a resume with the service off")
    check(!state.sessionStarted && state.token == 1 && state.structure().size == 2) { "as disabled: snapshot kept" }
    // A service turned on while the form is open works from the next focus.
    nothing(state.field(1, PASSWORD, PASS_RECT, true, true, ""), "focus")
    expect(state.request(1, PASSWORD, PASS_RECT, enabled = true), Effect.HideTextHandle, Effect.Request(PASSWORD, PASS_RECT), why = "next focus")
}

private fun theStructureDescribesTheSnapshot() {
    val state = shownForm(username = "alice")
    nothing(state.field(1, PASSWORD, PASS_RECT, true, true, "typed-by-mistake"), "focus on the password")
    val fields = state.structure()
    check(fields.size == 2)
    val user = fields[0]
    val pass = fields[1]
    check(user.vid == USERNAME && user.rect == USER_RECT && user.visible && !user.focused && user.value == "alice")
    check(pass.vid == PASSWORD && pass.rect == PASS_RECT && pass.visible && pass.focused)
    check(pass.value.isEmpty()) { "the password's structure value is always empty" }
    check(state.isVisible(USERNAME) && state.isVisible(PASSWORD) && !state.isVisible(3))
    nothing(state.field(1, USERNAME, Rect(0, 0, 0, 0), false, false, "alice"), "the username scrolled away")
    check(!state.isVisible(USERNAME) && !state.structure()[0].visible)
    check(SignInAutofillState().structure().isEmpty()) { "no form, no children" }
}

private fun aValueNeverPrintsItsText() {
    val text = Effect.Value(PASSWORD, "s3cret").toString()
    check(!text.contains("s3cret")) { "an effect's text must not reach a log line: $text" }
}

private fun aHeldRequestIsDroppedWhenItNoLongerApplies() {
    fun held(): SignInAutofillState {
        val state = SignInAutofillState()
        state.begin(1)
        state.field(1, USERNAME, USER_RECT, true, true, "alice")
        expect(state.request(1, USERNAME, USER_RECT, enabled = true), Effect.Held(USERNAME), why = "held")
        return state
    }
    var state = held()
    nothing(state.field(1, USERNAME, USER_RECT, true, false, "alice"), "blur before the origin: no exit, no session")
    state.origin(ORIGIN)
    nothing(state.retryHeld(enabled = true), "a blurred field's held request is dropped")

    state = held()
    nothing(state.field(1, USERNAME, Rect(0, 0, 0, 0), false, true, "alice"), "hidden before the origin")
    state.origin(ORIGIN)
    nothing(state.retryHeld(enabled = true), "a hidden field's held request is dropped")

    state = held()
    state.origin(ORIGIN)
    nothing(state.retryHeld(enabled = false), "the service went away")
    check(!state.hasHeldRequest) { "and the held request with it" }
    nothing(state.retryHeld(enabled = true), "it does not come back")

    state = held()
    nothing(state.end(1), "no session to cancel")
    state.origin(ORIGIN)
    check(!state.hasHeldRequest)
    nothing(state.retryHeld(enabled = true), "an ended form's request is dropped")

    state = held()
    nothing(state.begin(2), "a newer form")
    state.origin(ORIGIN)
    nothing(state.retryHeld(enabled = true), "an older form's request is dropped")
}

private fun aFieldHiddenWhileFocusedIsExited() {
    val state = shownForm()
    started(state, 1, PASSWORD, PASS_RECT)
    expect(state.field(1, PASSWORD, Rect(0, 0, 0, 0), false, true, ""), Effect.Exit(PASSWORD),
        why = "the focused field was hidden (card hidden, clipped away)")
    nothing(state.field(1, PASSWORD, Rect(0, 0, 0, 0), false, true, ""), "no second exit while it stays hidden")
    nothing(state.field(1, PASSWORD, Rect(0, 0, 0, 0), false, false, ""), "nor when it then loses focus")
    nothing(state.field(1, PASSWORD, PASS_RECT, true, true, ""), "shown again: Qt's next focus entry requests")
}

private val MOVED = Rect(PASS_RECT.left, PASS_RECT.top - 200, PASS_RECT.width, PASS_RECT.height)
private val MOVED_AGAIN = Rect(PASS_RECT.left + 50, PASS_RECT.top - 150, PASS_RECT.width - 100, PASS_RECT.height)

/** A form whose password field was requested at [PASS_RECT], with the fill UI shown for it if [shown]. */
private fun requestedPassword(shown: Boolean): SignInAutofillState {
    val state = shownForm()
    started(state, 1, PASSWORD, PASS_RECT)
    if (shown) {
        nothing(state.fillUi(PASSWORD, shown = true), "the framework showed its UI")
    }
    return state
}

private fun aShownFillUiFollowsASettledMove() {
    val state = requestedPassword(shown = true)
    nothing(state.field(1, PASSWORD, PASS_RECT, true, true, ""), "no move, no settle")
    expect(state.field(1, PASSWORD, MOVED, true, true, ""), Effect.Settle(PASSWORD), why = "a move arms the settle")
    expect(state.settled(), Effect.Exit(PASSWORD), Effect.HideTextHandle, Effect.Enter(PASSWORD, MOVED), why = "one re-anchor at the new place")
    nothing(state.settled(), "the settle is used once")
    nothing(state.field(1, PASSWORD, MOVED, true, true, ""), "anchored there now")
    // The exit hides the UI and the enter shows it again, as the framework reports.
    nothing(state.fillUi(PASSWORD, shown = false), "hidden by the exit")
    nothing(state.fillUi(PASSWORD, shown = true), "shown by the enter")
    expect(state.field(1, PASSWORD, MOVED_AGAIN, true, true, ""), Effect.Settle(PASSWORD), why = "another move")
    expect(state.settled(), Effect.Exit(PASSWORD), Effect.HideTextHandle, Effect.Enter(PASSWORD, MOVED_AGAIN), why = "re-anchored again")
}

private fun noReAnchorWithoutAShownFillUi() {
    var state = requestedPassword(shown = false)
    nothing(state.field(1, PASSWORD, MOVED, true, true, ""), "no fill UI shown: a move is not followed")
    nothing(state.settled(), "nothing armed")

    state = requestedPassword(shown = true)
    nothing(state.fillUi(PASSWORD, shown = false), "the UI went (a pick, a dismissal)")
    nothing(state.field(1, PASSWORD, MOVED, true, true, ""), "a hidden UI is not followed")

    state = requestedPassword(shown = true)
    expect(state.field(1, PASSWORD, MOVED, true, true, ""), Effect.Settle(PASSWORD), why = "armed")
    nothing(state.fillUi(PASSWORD, shown = false), "hidden before the settle ran out")
    nothing(state.settled(), "no re-anchor for a hidden UI")

    state = requestedPassword(shown = true)
    expect(state.field(1, PASSWORD, MOVED, true, true, ""), Effect.Settle(PASSWORD), why = "armed")
    expect(state.field(1, PASSWORD, MOVED, true, false, ""), Effect.Exit(PASSWORD), why = "blur")
    nothing(state.settled(), "no re-anchor after a blur")

    state = requestedPassword(shown = true)
    expect(state.field(1, PASSWORD, MOVED, true, true, ""), Effect.Settle(PASSWORD), why = "armed")
    nothing(state.field(1, PASSWORD, PASS_RECT, true, true, ""), "moved back before the settle ran out")
    nothing(state.settled(), "back where it was anchored: nothing to do")

    state = requestedPassword(shown = true)
    expect(state.field(1, PASSWORD, MOVED, true, true, ""), Effect.Settle(PASSWORD), why = "armed")
    nothing(state.disabled(), "the service went away")
    nothing(state.settled(), "no re-anchor without the service")

    state = requestedPassword(shown = true)
    expect(state.field(1, PASSWORD, MOVED, true, true, ""), Effect.Settle(PASSWORD), why = "armed")
    expect(state.end(1), Effect.Cancel, why = "the form left")
    nothing(state.settled(), "no re-anchor for an ended form")
    nothing(state.fillUi(PASSWORD, shown = true), "no live form: fill UI events are ignored")

    state = requestedPassword(shown = true)
    nothing(state.fillUi(USERNAME, shown = true), "the UI moved to the other field")
    nothing(state.field(1, PASSWORD, MOVED, true, true, ""), "the password's move is not followed")
    nothing(state.fillUi(7, shown = true), "only fields 1 and 2 exist")
}

private fun aMoveBeforeTheFillUiShowedIsFollowedOnceShown() {
    val state = requestedPassword(shown = false)
    nothing(state.field(1, PASSWORD, MOVED, true, true, ""), "moved while the service was still answering")
    expect(state.fillUi(PASSWORD, shown = true), Effect.Settle(PASSWORD),
        why = "shown at the old place: it waits for the field to settle")
    expect(state.settled(), Effect.Exit(PASSWORD), Effect.HideTextHandle, Effect.Enter(PASSWORD, MOVED),
        why = "then follows it once")
    nothing(state.fillUi(PASSWORD, shown = false), "hidden by the exit")
    nothing(state.fillUi(PASSWORD, shown = true), "shown again where the field is: nothing more")
    nothing(state.settled(), "nothing armed")

    val still = requestedPassword(shown = false)
    nothing(still.fillUi(PASSWORD, shown = true), "shown where the field still is: nothing to follow")
    nothing(still.settled(), "nothing armed")
}

private fun everyRequestAndEnterHidesTheTextHandleFirst() {
    val state = shownForm()
    nothing(state.field(1, USERNAME, USER_RECT, true, true, "alice"), "focus")
    val request = state.request(1, USERNAME, USER_RECT, enabled = true)
    check(request.indexOf(Effect.HideTextHandle) == request.indexOfFirst { it is Effect.Request } - 1) {
        "the handle is hidden right before the request: $request"
    }
    nothing(state.fillUi(USERNAME, shown = true), "shown")
    expect(state.field(1, USERNAME, MOVED, true, true, "alice"), Effect.Settle(USERNAME), why = "moved")
    val reanchor = state.settled()
    check(reanchor.indexOf(Effect.HideTextHandle) == reanchor.indexOfFirst { it is Effect.Enter } - 1) {
        "the handle is hidden right before the enter, which may start a request: $reanchor"
    }
}

private fun aCommitEndsTheFillUiState() {
    val state = requestedPassword(shown = true)
    check(state.commit(1, "alice", "s3cret").contains(Effect.Commit))
    nothing(state.field(1, PASSWORD, MOVED, true, true, ""), "after a commit a move is not followed")
}

fun main() {
    aRequestStartsTheSessionOnlyWhenEnabled()
    aRequestWaitsForTheOriginAndAppliesIt()
    aRequestForAFieldNotShownSendsNothing()
    focusLossExitsOnlyAStartedSession()
    commitReportsTheValuesThenCommitsThenHides()
    aRequestAfterCommitStartsAgain()
    noCommitOrCancelWithoutASession()
    endCancelsAndClears()
    staleTokensProduceNothing()
    aNewerBeginRetiresAStartedSession()
    disabledDropsTheSessionAndKeepsTheSnapshot()
    resumedReadsTheServiceAgain()
    theStructureDescribesTheSnapshot()
    aValueNeverPrintsItsText()
    aHeldRequestIsDroppedWhenItNoLongerApplies()
    aFieldHiddenWhileFocusedIsExited()
    aShownFillUiFollowsASettledMove()
    noReAnchorWithoutAShownFillUi()
    aCommitEndsTheFillUiState()
    aMoveBeforeTheFillUiShowedIsFollowedOnceShown()
    everyRequestAndEnterHidesTheTextHandleFirst()
    println("PASS: sign-in autofill state follows tokens, focus, commit and the service")
}
