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

    TestCase {
        name: "HistoryHold"
        when: windowShown

        function init() {
            verify(loader.item !== null)
            testContext.resetCommands()
            testContext.setHostRunning(true)
            testContext.setMetric("heldTg", 0)
            testContext.setMetric("scanRotationActive", false)
            callHistory.sessionUid = "test-system"
        }

        function cleanup() {
            loader.item.visible = false
            testContext.setHostRunning(false)
            testContext.setMetric("heldTg", 0)
            testContext.setMetric("scanRotationActive", false)
            callHistory.sessionUid = "test-system"
        }

        function test_hold_data() {
            return [
                {tag: "same-system", running: true, uid: "test-system", held: 0, enabled: true, send: 1001},
                {tag: "replace-hold", running: true, uid: "test-system", held: 2002, enabled: true, send: 1001},
                {tag: "release", running: true, uid: "test-system", held: 1001, enabled: true, send: 0},
                {tag: "stopped", running: false, uid: "test-system", held: 0, enabled: false, reason: "Start this system to hold"},
                {tag: "stopped-held", running: false, uid: "test-system", held: 1001, enabled: false, reason: "Start this system to hold"},
                {tag: "another-system", running: true, uid: "different", held: 0, enabled: false, reason: "Heard on another system"},
                {tag: "exploring", running: true, uid: "test-system", sessionUid: "", held: 0, enabled: false, reason: "Hold needs a single saved system"},
                {tag: "scan-from-extra-args", running: true, uid: "test-system", scanning: true, held: 0, enabled: false, reason: "Hold needs a single saved system"},
                {tag: "old-scan-row", running: true, uid: "", held: 0, enabled: false, reason: "Heard on another system"},
                {tag: "legacy", running: true, held: 0, enabled: false, reason: "Heard on another system"},
                {tag: "text-target", running: true, uid: "test-system", held: 0, tg: 0, enabled: false},
                {tag: "release-in-scan", running: true, uid: "", scanning: true, held: 1001, enabled: true, send: 0}
            ]
        }

        function test_hold(data) {
            testContext.setHostRunning(data.running)
            testContext.setMetric("heldTg", data.held)
            testContext.setMetric("scanRotationActive", data.scanning === true)
            if (data.sessionUid !== undefined)
                callHistory.sessionUid = data.sessionUid
            var record = {name: "Dispatch", tg: data.tg === undefined ? 1001 : data.tg}
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
