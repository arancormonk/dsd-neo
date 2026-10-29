// SPDX-License-Identifier: GPL-3.0-or-later
package io.github.arancormonk.dsdneo

/** Stands in for an Activity instance; equal by value, so identity is what the gate must go by. */
private data class GateActivity(val name: String)

private val OFF = A11yFacts(enabled = false, touchExploration = false, boundServices = 0)
private val SERVICE_ONLY = A11yFacts(enabled = true, touchExploration = false, boundServices = 2)
private val TALKBACK = A11yFacts(enabled = true, touchExploration = true, boundServices = 1)
private val TALKBACK_AND_SERVICE = A11yFacts(enabled = true, touchExploration = true, boundServices = 3)

private class Rig {
    val records = mutableListOf<String>()
    val gate = AccessibilityGate<GateActivity> { records += it }
    val a = GateActivity("main")

    /** Launches with [facts], attaches [a] and resumes it, as the activity's onCreate and onResume do. */
    fun start(facts: A11yFacts): LaunchPlan {
        val plan = gate.launch(facts)
        gate.attach(a)
        check(gate.resumed(a) == Notice.NONE) { "a launch never owes a notice to its first resume here" }
        return plan
    }

    /** What the platform side does with SHOW: put the dialog up. */
    fun show(notice: Notice, on: GateActivity = a) {
        check(notice == Notice.SHOW) { "expected SHOW, got $notice" }
        gate.noticeShown(on)
        check(gate.showingOn(on))
    }
}

private fun thePredicateIsTouchExplorationOnly() {
    for (facts in listOf(OFF, SERVICE_ONLY, A11yFacts(enabled = true, touchExploration = false, boundServices = 0))) {
        check(!bridgeWanted(facts))
        check(LaunchPlan.of(facts) == LaunchPlan(wanted = false, activated = false, env = "1")) {
            "touch exploration off keeps the bridge off, with or without services: $facts"
        }
    }
    for (facts in listOf(TALKBACK, TALKBACK_AND_SERVICE)) {
        check(bridgeWanted(facts))
        check(LaunchPlan.of(facts) == LaunchPlan(wanted = true, activated = true, env = null)) {
            "touch exploration on leaves the variable unset, so Qt turns the bridge on: $facts"
        }
    }
    // Not seen on Android (touch exploration implies enabled), but decided: wanted, Qt does not activate, so a notice.
    val odd = A11yFacts(enabled = false, touchExploration = true, boundServices = 0)
    check(LaunchPlan.of(odd) == LaunchPlan(wanted = true, activated = false, env = null))
    val records = mutableListOf<String>()
    val gate = AccessibilityGate<GateActivity> { records += it }
    gate.launch(odd)
    check(gate.noticePending) { "wanted but not activated owes the notice" }
    val a = GateActivity("main")
    gate.attach(a)
    check(gate.resumed(a) == Notice.SHOW)
}

private fun theFirstLaunchDecidesForTheProcess() {
    val gate = AccessibilityGate<GateActivity> {}
    val first = gate.launch(SERVICE_ONLY)
    check(gate.launch(TALKBACK) === first && gate.plan === first) { "a later launch returns the stored plan" }
    check(gate.launch(OFF) == LaunchPlan(wanted = false, activated = false, env = "1"))
}

private fun aRisingEdgeWhileOffAsksOnce() {
    val rig = Rig()
    rig.start(SERVICE_ONLY)
    check(rig.gate.onFacts(SERVICE_ONLY) == Notice.NONE) { "no change, no notice" }
    rig.show(rig.gate.onFacts(TALKBACK_AND_SERVICE))
    check(rig.gate.onFacts(TALKBACK_AND_SERVICE) == Notice.NONE) { "repeated facts do not ask again" }
    check(rig.gate.onFacts(TALKBACK) == Notice.NONE) { "a service count change is no edge" }
    check(rig.records.contains("accessibility: notice shown"))
}

private fun aFallingThenRisingEdgeAsksAgain() {
    val rig = Rig()
    rig.start(OFF)
    rig.show(rig.gate.onFacts(TALKBACK))
    rig.gate.noticeAcknowledged()
    check(rig.gate.onFacts(TALKBACK) == Notice.NONE)
    check(rig.gate.onFacts(OFF) == Notice.NONE)
    rig.show(rig.gate.onFacts(TALKBACK))
}

private fun aBridgeOnAtLaunchNeverAsks() {
    val rig = Rig()
    check(rig.start(TALKBACK).activated)
    for (facts in listOf(OFF, TALKBACK, SERVICE_ONLY, TALKBACK_AND_SERVICE, OFF, TALKBACK)) {
        check(rig.gate.onFacts(facts) == Notice.NONE) { "an activated bridge never asks: $facts" }
    }
    rig.gate.paused(rig.a)
    check(rig.gate.resumed(rig.a) == Notice.NONE)
    check(rig.records.none { it.startsWith("accessibility: notice") })
    check(rig.records.contains("accessibility: touch exploration turned off; the bridge stays on until the app restarts"))
}

private fun aPausedOwnerGetsTheNoticeOnResume() {
    val rig = Rig()
    rig.start(OFF)
    rig.gate.paused(rig.a)
    check(rig.gate.onFacts(TALKBACK) == Notice.PENDING) { "never to a paused activity" }
    check(rig.gate.noticePending)
    rig.show(rig.gate.resumed(rig.a))
    rig.gate.paused(rig.a)
    check(rig.gate.resumed(rig.a) == Notice.NONE) { "a notice on screen is not shown twice" }
}

private fun aPendingNoticeOutlivesItsActivity() {
    val rig = Rig()
    rig.start(OFF)
    rig.gate.paused(rig.a)
    check(rig.gate.onFacts(TALKBACK) == Notice.PENDING)
    rig.gate.detach(rig.a)
    val b = GateActivity("main")
    rig.gate.attach(b)
    check(rig.gate.owner === b)
    rig.show(rig.gate.resumed(b), on = b)
}

private fun staleCallbacksAreIgnored() {
    val rig = Rig()
    rig.start(OFF)
    val b = GateActivity("main") // Equal to a, but a different instance.
    rig.gate.attach(b) // B's onCreate before A's onDestroy.
    check(rig.gate.onFacts(TALKBACK) == Notice.PENDING) { "B is not resumed yet" }
    check(rig.gate.resumed(rig.a) == Notice.NONE) { "A's late resume is ignored" }
    rig.gate.noticeShown(rig.a)
    check(!rig.gate.showingOn(rig.a) && rig.gate.noticePending) { "A cannot take the notice" }
    rig.gate.detach(rig.a)
    check(rig.gate.owner === b) { "A's late destroy does not detach B" }
    rig.show(rig.gate.resumed(b), on = b)
    rig.gate.paused(rig.a)
    check(rig.gate.showingOn(b))
}

private fun aDialogClosedWithoutOkComesBack() {
    val rig = Rig()
    rig.start(OFF)
    rig.show(rig.gate.onFacts(TALKBACK))
    // onStop closes the dialog without the user.
    rig.gate.paused(rig.a)
    rig.gate.noticeDropped()
    check(rig.gate.noticePending && !rig.gate.showingOn(rig.a))
    rig.show(rig.gate.resumed(rig.a))
    check(rig.records == listOf(
        "accessibility: touch exploration turned on; the bridge stays off until the app restarts",
        "accessibility: notice shown",
        "accessibility: notice dropped",
        "accessibility: notice shown",
    )) { "records: ${rig.records}" }
}

private fun afterOkNothingUntilTheNextRisingEdge() {
    val rig = Rig()
    rig.start(OFF)
    rig.show(rig.gate.onFacts(TALKBACK))
    rig.gate.noticeAcknowledged()
    check(!rig.gate.noticePending && !rig.gate.showingOn(rig.a))
    rig.gate.noticeDropped()
    check(!rig.gate.noticePending) { "a drop after OK owes nothing" }
    rig.gate.paused(rig.a)
    check(rig.gate.resumed(rig.a) == Notice.NONE)
    check(rig.gate.onFacts(TALKBACK_AND_SERVICE) == Notice.NONE)
    check(rig.records.last() == "accessibility: notice acknowledged")
    // A notice waiting for a resume is moot once touch exploration goes off again.
    rig.gate.paused(rig.a)
    check(rig.gate.onFacts(OFF) == Notice.NONE)
    check(rig.gate.onFacts(TALKBACK) == Notice.PENDING)
    check(rig.gate.onFacts(OFF) == Notice.NONE && !rig.gate.noticePending)
    check(rig.gate.resumed(rig.a) == Notice.NONE)
}

private fun theRecordHoldsFlagsAndCountsOnly() {
    val gate = AccessibilityGate<GateActivity> {}
    check(gate.record(SERVICE_ONLY, LaunchPlan.of(SERVICE_ONLY)) ==
        "accessibility: bridge off at launch (touch exploration off; accessibility on, 2 services bound)")
    check(gate.record(TALKBACK, LaunchPlan.of(TALKBACK)) ==
        "accessibility: bridge on at launch (touch exploration on; accessibility on, 1 service bound)")
    check(gate.record(OFF, LaunchPlan.of(OFF)) ==
        "accessibility: bridge off at launch (touch exploration off; accessibility off, 0 services bound)")
    val odd = A11yFacts(enabled = false, touchExploration = true, boundServices = 0)
    check(gate.record(odd, LaunchPlan.of(odd)) ==
        "accessibility: bridge off at launch (touch exploration on; accessibility off, 0 services bound); " +
        "restart notice pending")
    // The bound services' names are read nowhere: whatever apps are bound, only their number shows.
    val names = listOf("com.anydesk.anydeskandroid", "com.catalinagroup.callrecorder", "AnyDesk", "Cube ACR")
    val records = mutableListOf<String>()
    val rig = AccessibilityGate<GateActivity> { records += it }
    val facts = A11yFacts(enabled = true, touchExploration = false, boundServices = names.size)
    records += rig.record(facts, rig.launch(facts))
    val a = GateActivity("main")
    rig.attach(a)
    rig.resumed(a)
    rig.onFacts(facts.copy(touchExploration = true))
    rig.noticeShown(a)
    rig.noticeAcknowledged()
    check(records.first() ==
        "accessibility: bridge off at launch (touch exploration off; accessibility on, 4 services bound)")
    for (record in records) {
        check(names.none { record.contains(it, ignoreCase = true) }) { "a record names an app: $record" }
        check(record.length <= 512) { "over the host record limit: $record" }
    }
}

fun main() {
    thePredicateIsTouchExplorationOnly()
    theFirstLaunchDecidesForTheProcess()
    aRisingEdgeWhileOffAsksOnce()
    aFallingThenRisingEdgeAsksAgain()
    aBridgeOnAtLaunchNeverAsks()
    aPausedOwnerGetsTheNoticeOnResume()
    aPendingNoticeOutlivesItsActivity()
    staleCallbacksAreIgnored()
    aDialogClosedWithoutOkComesBack()
    afterOkNothingUntilTheNextRisingEdge()
    theRecordHoldsFlagsAndCountsOnly()
    println("PASS: the accessibility gate decides once, and the notice reaches only a resumed activity, never lost")
}
