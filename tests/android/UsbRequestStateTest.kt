// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

private const val RTL_A = "/dev/bus/usb/001/002"
private const val RTL_A_REPLUGGED = "/dev/bus/usb/001/003"
private const val AIRSPY_A = "/dev/bus/usb/001/004"
private const val AIRSPY_B = "/dev/bus/usb/001/005"
private const val SERIAL = "26A464DC"

private class Clock {
    var ms = 1_000L
}

private fun fresh(clock: Clock = Clock()) = UsbRequestState { clock.ms }

private fun aLateReplyIsIgnored() {
    val clock = Clock()
    val state = fresh(clock)
    val first = state.begin("usb", "", RTL_A)
    clock.ms += UsbRequestState.TIMEOUT_MS
    check(state.expireIfStale()) { "the first request expires" }
    val second = state.begin("usb", "", RTL_A)
    check(second != first) { "each request gets its own generation" }
    check(state.onReply(first, RTL_A, true) == UsbRequestState.Reply.IGNORED) { "a reply to the old request is ignored" }
    check(state.pending) { "the old reply leaves the new request outstanding" }
    check(state.onReply(second, AIRSPY_A, true) == UsbRequestState.Reply.IGNORED) { "a reply for another device is ignored" }
    check(state.onReply(second, RTL_A, false) == UsbRequestState.Reply.DENIED) { "the request's own reply counts" }
    check(!state.pending) { "an answered request is no longer pending" }
    check(state.onReply(second, RTL_A, true) == UsbRequestState.Reply.IGNORED) { "a replayed reply is ignored" }
}

private fun aSerialSearchHopKeepsTheGeneration() {
    val clock = Clock()
    val state = fresh(clock)
    val generation = state.begin("airspy", SERIAL, AIRSPY_A)
    check(state.onReply(generation, AIRSPY_A, true) == UsbRequestState.Reply.GRANTED)
    // AIRSPY_A turned out to carry another serial: the same request moves on to the next Airspy.
    clock.ms += UsbRequestState.TIMEOUT_MS - 1
    check(state.hop(AIRSPY_B) == generation) { "a hop is the same request" }
    check(state.pending && state.device == AIRSPY_B) { "the hop prompts for the next device" }
    check(state.requestedSource == "airspy" && state.requestedSerial == SERIAL) { "the hop keeps the request's target" }
    clock.ms += UsbRequestState.TIMEOUT_MS - 1
    check(!state.expireIfStale()) { "the hop's prompt gets its own wait" }
    check(state.onReply(generation, AIRSPY_A, true) == UsbRequestState.Reply.IGNORED) { "a duplicate for the first device is ignored" }
    check(state.onReply(generation, AIRSPY_B, true) == UsbRequestState.Reply.GRANTED) { "the hopped device's reply counts" }
}

private fun aDetachWhilePendingRetiresTheRequest() {
    val state = fresh()
    val generation = state.begin("usb", "", RTL_A)
    check(!state.onDetach(AIRSPY_A)) { "another device's detach leaves the request alone" }
    check(state.pending && state.device == RTL_A)
    check(state.onDetach(RTL_A)) { "the requested device's detach retires the request" }
    check(!state.pending && state.device == null)
    check(state.requestedSource == "usb") { "the request's target stays for Connect" }
    check(state.onReply(generation, RTL_A, true) == UsbRequestState.Reply.IGNORED) { "a reply after the detach is ignored" }
    check(!state.onDetach(RTL_A)) { "nothing is left to retire" }
}

private fun aReplugWithinTheTimeoutCanAskAgain() {
    val clock = Clock()
    val state = fresh(clock)
    val first = state.begin("usb", "", RTL_A)
    clock.ms += 5_000
    check(state.onDetach(RTL_A))
    clock.ms += 5_000
    // Connect right after the re-plug must prompt, not wait out the old request's 60 s.
    val second = state.begin("usb", "", RTL_A_REPLUGGED)
    check(second != first && state.pending) { "the re-plugged device gets a request of its own" }
    check(state.onReply(first, RTL_A_REPLUGGED, true) == UsbRequestState.Reply.IGNORED) { "the old generation stays retired" }
    check(state.onReply(second, RTL_A_REPLUGGED, true) == UsbRequestState.Reply.GRANTED)
}

private fun anUnansweredRequestExpiresOnItsOwn() {
    val clock = Clock()
    val state = fresh(clock)
    val generation = state.begin("airspy", "", AIRSPY_A)
    clock.ms += UsbRequestState.TIMEOUT_MS - 1
    check(!state.expireIfStale() && state.pending) { "a request inside its wait stays pending" }
    clock.ms += 1
    // Nothing asks again: only the poll's clock moves.
    check(state.expireIfStale()) { "a request with no answer for 60 s expires" }
    check(!state.pending) { "an expired request is not pending" }
    check(!state.expireIfStale()) { "expiry is reported once" }
    check(state.generation == generation) { "expiry never prompts again" }
    check(state.onReply(generation, AIRSPY_A, true) == UsbRequestState.Reply.IGNORED) { "a reply after expiry is ignored" }
}

private fun noRequestBeforeTheFirstOne() {
    val state = fresh()
    check(state.requestedSource.isEmpty() && state.requestedSerial.isEmpty() && !state.pending) {
        "nothing is requested before the first request"
    }
    check(state.generation == 0 && state.device == null)
    check(!state.satisfiedBy("usb", "")) { "no request is satisfied by anything" }
    check(state.attachReplacesDiagnostic("usb", "", connected = false)) { "with no request and no connection any attach reports" }
}

private fun anotherKindNeverSatisfiesTheRequest() {
    val state = fresh()
    state.select("airspy", "")
    check(!state.satisfiedBy("usb", "")) { "an RTL-SDR does not satisfy an Airspy request" }
    check(!state.attachReplacesDiagnostic("usb", "", connected = false)) { "an RTL attach keeps the Airspy diagnostic" }
    check(state.satisfiedBy("airspy", "")) { "any Airspy satisfies a request with no serial" }
    check(state.attachReplacesDiagnostic("airspy", "", connected = false))
}

private fun aWrongSerialNeverSatisfiesTheRequest() {
    val state = fresh()
    state.select("airspy", SERIAL)
    check(!state.satisfiedBy("airspy", "0000000000000001")) { "another serial does not satisfy it" }
    check(!state.satisfiedBy("airspy", "")) { "an unreadable serial does not satisfy it" }
    check(!state.attachReplacesDiagnostic("airspy", "", connected = false)) {
        "an Airspy of unknown serial keeps the missing-serial diagnostic"
    }
    check(state.satisfiedBy("airspy", SERIAL.lowercase())) { "serials compare without case" }
}

private fun anAttachWhileConnectedKeepsTheConnectionsText() {
    val state = fresh()
    state.select("usb", "")
    check(state.satisfiedBy("usb", ""))
    check(!state.attachReplacesDiagnostic("usb", "", connected = true)) { "a live connection's text is not overwritten" }
}

private fun aNewTargetRetiresTheOldPrompt() {
    val state = fresh()
    val generation = state.begin("usb", "", RTL_A)
    state.select("usb", "")
    check(state.pending && state.device == RTL_A) { "the same target keeps its prompt" }
    state.select("airspy", "")
    check(!state.pending && state.device == null) { "another target retires the prompt" }
    check(state.requestedSource == "airspy")
    check(state.onReply(generation, RTL_A, true) == UsbRequestState.Reply.IGNORED)
}

private fun aDeviceThatNeedsNoPromptRetiresThePrompt() {
    val state = fresh()
    val generation = state.begin("usb", "", RTL_A)
    // Permission arrived while the broadcast was lost: the open goes ahead without one.
    state.aim(RTL_A)
    check(!state.pending && state.device == RTL_A) { "a direct open leaves nothing outstanding" }
    check(state.onReply(generation, RTL_A, true) == UsbRequestState.Reply.IGNORED)
}

private fun switchingWhileRunningIsRefused() {
    val running = true
    check(UsbRequestState.switchFor("usb", "", "airspy", "", running) == UsbRequestState.Switch.REFUSE) {
        "another kind while running is refused"
    }
    check(UsbRequestState.switchFor("airspy", "0000", "airspy", SERIAL, running) == UsbRequestState.Switch.REFUSE) {
        "another serial while running is refused"
    }
    check(UsbRequestState.switchFor("usb", "", "airspy", "", !running) == UsbRequestState.Switch.RELEASE) {
        "a stopped session releases for the switch"
    }
    check(UsbRequestState.switchFor("airspy", SERIAL, "airspy", SERIAL.lowercase(), running) == UsbRequestState.Switch.KEEP)
    check(UsbRequestState.switchFor("airspy", SERIAL, "airspy", "", running) == UsbRequestState.Switch.KEEP) {
        "a request with no serial takes the connected Airspy"
    }
    check(UsbRequestState.switchFor("", "", "airspy", "", running) == UsbRequestState.Switch.KEEP) { "no connection, nothing to switch" }
    // Request feedback, not a device failure: the connection works (B11).
    check(UsbRequestState.REFUSED == "refused")
}

private fun theDefaultTargetFollowsTheRequest() {
    val state = fresh()
    check(state.accessTarget("", listOf("airspy")) == ("airspy" to "")) { "a fresh install with only an Airspy asks for it" }
    check(state.accessTarget("", listOf("usb", "airspy")) == ("usb" to "")) { "with both, the first on the bus" }
    check(state.accessTarget("", listOf("airspy", "usb")) == ("airspy" to ""))
    check(state.accessTarget("", emptyList()) == ("usb" to "")) { "an empty bus asks for an RTL-SDR" }
    check(state.accessTarget("airspy", listOf("usb")) == ("airspy" to "")) { "the live connection's kind before the bus" }
    state.select("airspy", SERIAL)
    check(state.accessTarget("", listOf("usb")) == ("airspy" to SERIAL)) { "the outstanding request comes first" }
    check(state.accessTarget("usb", listOf("usb")) == ("airspy" to SERIAL))
}

private fun anAttachmentNamesItsKind() {
    val airspy = UsbAttachmentTracker.Identity(AIRSPY_A, 0x1d50, 0x60a1)
    val rtl = UsbAttachmentTracker.Identity(RTL_A, 0x0bda, 0x2838)
    check(UsbAttachmentTracker.take(setOf(airspy, rtl)) == "") { "nothing reported, nothing taken" }
    check(UsbAttachmentTracker.report(airspy, setOf(airspy, rtl)))
    check(UsbAttachmentTracker.take(setOf(airspy, rtl)) == "airspy:$AIRSPY_A") { "an Airspy attaches as airspy" }
    check(UsbAttachmentTracker.take(setOf(airspy, rtl)) == "") { "an attachment is taken once" }
    check(UsbAttachmentTracker.report(rtl, setOf(airspy, rtl)))
    check(UsbAttachmentTracker.take(setOf(airspy, rtl)) == "usb:$RTL_A") { "an RTL-SDR attaches as usb" }
}

fun main() {
    aLateReplyIsIgnored()
    aSerialSearchHopKeepsTheGeneration()
    aDetachWhilePendingRetiresTheRequest()
    aReplugWithinTheTimeoutCanAskAgain()
    anUnansweredRequestExpiresOnItsOwn()
    noRequestBeforeTheFirstOne()
    anotherKindNeverSatisfiesTheRequest()
    aWrongSerialNeverSatisfiesTheRequest()
    anAttachWhileConnectedKeepsTheConnectionsText()
    aNewTargetRetiresTheOldPrompt()
    aDeviceThatNeedsNoPromptRetiresThePrompt()
    switchingWhileRunningIsRefused()
    theDefaultTargetFollowsTheRequest()
    anAttachmentNamesItsKind()
    println("UsbRequestStateTest: OK")
}
