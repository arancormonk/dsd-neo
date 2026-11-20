// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest

Item {
    width: 420
    height: 900

    Loader {
        id: loader
        anchors.fill: parent
        source: uiDir + "/HistoryDetailSheet.qml"
    }

    Loader {
        id: firstHistory
        width: 420
        height: 900
        visible: false
        source: uiDir + "/HistoryScreen.qml"
    }
    Loader {
        id: secondHistory
        width: 420
        height: 900
        visible: false
        source: uiDir + "/HistoryScreen.qml"
    }

    TestCase {
        name: "HistoryHold"
        when: windowShown

        function init() {
            verify(loader.item !== null)
            testContext.resetCommands()
            testContext.setHostRunning(true)
            testContext.setMetric("heldTg", 0)
            testContext.setMetric("optionsKnown", true)
            setScan(false, false, false)
            callHistory.sessionUid = "test-system"
        }

        // The session's scan, running or paused, and whether the operator holds the tuner (issue #671), as the
        // decoder's scan-state view reports them.
        function setScan(running, paused, operatorTuned) {
            testContext.setMetric("scanKind", running || paused ? 1 : 0)
            testContext.setMetric("scanRotationActive", running)
            testContext.setMetric("scanPaused", paused)
            testContext.setMetric("operatorTuned", operatorTuned)
        }

        function cleanup() {
            loader.item.visible = false
            testContext.setHostRunning(false)
            testContext.setMetric("heldTg", 0)
            testContext.setMetric("optionsKnown", false)
            setScan(false, false, false)
            callHistory.sessionUid = "test-system"
        }

        function test_two_history_search_fields_share_debounced_filter() {
            var first = findChild(firstHistory.item, "historySearch");
            var second = findChild(secondHistory.item, "historySearch");
            verify(first !== null && second !== null);
            historyView.filterText = "initial";
            compare(first.text, "initial");
            compare(second.text, "initial");
            first.input.text = "dispatch";
            wait(100);
            compare(historyView.filterText, "initial", "typing must remain debounced");
            first.input.text = "dispatch 1";
            tryCompare(historyView, "filterText", "dispatch 1");
            compare(second.text, "dispatch 1");
            second.input.text = "fire";
            tryCompare(historyView, "filterText", "fire");
            compare(first.text, "fire");
            // A shared-model update cancels any older local debounce.
            first.input.text = "pending";
            historyView.filterText = "external";
            wait(350);
            compare(historyView.filterText, "external");
            compare(first.text, "external");
            compare(second.text, "external");
            second.input.text = "";
            tryCompare(historyView, "filterText", "");
            compare(first.text, "");
        }

        // Review r2 (issue #800): who a call was to is said as the row knows it: a talkgroup only for a group call,
        // a radio for a private one, the bare number when not known; Hold names what it holds.
        function test_call_kind_words_data() {
            return [{tag: "group", gi: 0, line: "Talkgroup 42", hold: "Hold TG 42", meta: "TG 42"},
                    {tag: "private", gi: 1, line: "Sent to radio 42", hold: "Hold radio 42", meta: "TO RADIO 42"},
                    {tag: "not known", gi: -1, line: "Sent to 42", hold: "Hold 42", meta: "TO 42"}]
        }
        function test_call_kind_words(data) {
            loader.item.open({name: "Unit 7", tg: 42, gi: data.gi, systemUid: "test-system"})
            var lines = loader.item.detailLines
            verify(lines.indexOf(data.line) >= 0, JSON.stringify(lines))
            if (data.gi !== 0)
                verify(lines.indexOf("Talkgroup 42") < 0, "never a talkgroup: " + JSON.stringify(lines))
            compare(findChild(loader.item, "historyHoldButton").text, data.hold)
            loader.item.visible = false
            // The History list's meta line.
            callHistory.clearAll()
            callHistory.pushWithIds(42, 7)
            verify(callHistory.setRowGi(callHistory.newestUid(), data.gi))
            firstHistory.visible = true
            var list = findChild(firstHistory.item, "callLogList")
            tryVerify(function () { return list.itemAtIndex(0) !== null })
            var meta = list.itemAtIndex(0).metaText
            verify(meta.indexOf(data.meta) === 0, meta)
            if (data.gi !== 0)
                verify(meta.indexOf("TG 42") < 0, meta)
            firstHistory.visible = false
            callHistory.clearAll()
        }

        function test_hold_data() {
            return [
                {tag: "same-system", running: true, uid: "test-system", held: 0, enabled: true, send: 1001},
                {tag: "replace-hold", running: true, uid: "test-system", held: 2002, enabled: true, send: 1001},
                {tag: "release", running: true, uid: "test-system", held: 1001, enabled: true, send: 0},
                {tag: "release-other-system", running: true, uid: "different", held: 1001, enabled: true, send: 0},
                {tag: "release-in-explore", running: true, uid: "test-system", sessionUid: "", held: 1001, enabled: true, send: 0},
                {tag: "stopped", running: false, uid: "test-system", held: 0, enabled: false, reason: "Start this system to hold"},
                {tag: "stopped-held", running: false, uid: "test-system", held: 1001, enabled: false, reason: "Start this system to hold"},
                {tag: "another-system", running: true, uid: "different", held: 0, enabled: false, reason: "Heard on another system"},
                {tag: "exploring", running: true, uid: "test-system", sessionUid: "", held: 0, enabled: false, reason: "Hold needs a single saved system"},
                {tag: "scan-from-extra-args", running: true, uid: "test-system", scanning: true, held: 0, enabled: false, reason: "Hold needs a single saved system"},
                {tag: "old-scan-row", running: true, uid: "", held: 0, enabled: false, reason: "Heard on another system"},
                {tag: "legacy", running: true, held: 0, enabled: false, reason: "Heard on another system"},
                {tag: "text-target", running: true, uid: "test-system", held: 0, tg: 0, enabled: false},
                {tag: "release-in-scan", running: true, uid: "", scanning: true, held: 1001, enabled: true, send: 0},
                // Issue #671: the session keeps its UID for the calls heard before the operator took the tuner
                // (Explore under -T, a frequency entry), but the receiver is no longer on that system.
                {tag: "operator-holds-the-tuner", running: true, uid: "test-system", operatorTuned: true, held: 0, enabled: false, reason: "Hold needs a single saved system"},
                {tag: "paused-scan", running: true, uid: "test-system", paused: true, operatorTuned: true, held: 0, enabled: false, reason: "Hold needs a single saved system"},
                {tag: "release-while-the-operator-holds", running: true, uid: "test-system", operatorTuned: true, held: 1001, enabled: true, send: 0}
            ]
        }

        function test_startup_waits_for_effective_options() {
            // Android can report Running during a slow input open, before a
            // redraw has replaced the metrics cleared by Starting.
            testContext.setMetric("optionsKnown", false)
            loader.item.open({name: "Dispatch", tg: 1001, systemUid: "test-system"})
            var button = findChild(loader.item, "historyHoldButton")
            var reason = findChild(loader.item, "historyHoldReason")
            verify(!button.enabled)
            compare(reason.text, "Waiting for session settings")
            waitForRendering(button)
            mouseClick(button, button.width / 2, button.height / 2)
            compare(testContext.holdCalls(), 0)

            setScan(true, false, false)
            testContext.setMetric("optionsKnown", true)
            verify(!button.enabled)
            compare(reason.text, "Hold needs a single saved system")

            // A later single-system start must wait again, then become usable
            // once its effective options arrive, even without a live call.
            testContext.setMetric("optionsKnown", false)
            setScan(false, false, false)
            verify(!button.enabled)
            testContext.setMetric("optionsKnown", true)
            verify(button.enabled)
            mouseClick(button, button.width / 2, button.height / 2)
            compare(testContext.holdCalls(), 1)
            compare(testContext.lastHoldTg(), 1001)
        }

        function test_hold(data) {
            testContext.setHostRunning(data.running)
            testContext.setMetric("heldTg", data.held)
            setScan(data.scanning === true, data.paused === true, data.operatorTuned === true)
            if (data.sessionUid !== undefined)
                callHistory.sessionUid = data.sessionUid
            // A group call's row (gi 0): these cases hold a talkgroup.
            var record = {name: "Dispatch", tg: data.tg === undefined ? 1001 : data.tg, gi: 0}
            if (data.uid !== undefined)
                record.systemUid = data.uid
            loader.item.open(record)
            var button = findChild(loader.item, "historyHoldButton")
            var reason = findChild(loader.item, "historyHoldReason")
            compare(button.visible, record.tg > 0)
            compare(button.enabled, data.enabled)
            compare(button.text, data.held === record.tg && data.held > 0 ? "Release hold" : "Hold TG " + record.tg)
            if (data.reason) {
                verify(reason.visible)
                compare(reason.text, data.reason)
            }
            if (button.visible) {
                waitForRendering(button)
                mouseClick(button, button.width / 2, button.height / 2)
            }
            compare(testContext.holdCalls(), data.enabled ? 1 : 0)
            if (data.enabled) {
                compare(testContext.lastHoldTg(), data.send)
                verify(!loader.item.visible)
                compare(metrics.heldTg, data.held, "The command must not guess the next snapshot")
            }
        }
    }
}
