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
    expect(state.request(token, vid, rect, enabled = true), Effect.Request(vid, rect), why = "a settled focus requests")
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
    nothing(state.request(4, PASSWORD, PASS_RECT, enabled = true), "no request from a guessed origin")
    check(!state.sessionStarted) { "a skipped request starts no session" }
    check(state.structure().isEmpty()) { "no structure without an origin" }
    // The Qt window sits 30 px right of and 120 px below the host (split screen, a caption bar).
    state.origin(Offset(30, 120))
    val local = Rect(PASS_RECT.left + 30, PASS_RECT.top + 120, PASS_RECT.width, PASS_RECT.height)
    expect(state.request(4, PASSWORD, PASS_RECT, enabled = true), Effect.Request(PASSWORD, local),
        why = "the next request goes out, in host pixels")
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
    expect(state.request(1, PASSWORD, PASS_RECT, enabled = true), Effect.Request(PASSWORD, PASS_RECT),
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
    expect(state.request(1, PASSWORD, PASS_RECT, enabled = true), Effect.Request(PASSWORD, PASS_RECT), why = "next focus")
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
    println("PASS: sign-in autofill state follows tokens, focus, commit and the service")
}
