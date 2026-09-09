// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest

Item {
    width: 420
    height: 900
    Loader { id: monitor; anchors.fill: parent; source: uiDir + "/MonitorScreen.qml" }
    Loader { id: row; source: uiDir + "/CallRow.qml"; width: 420 }
    TestCase {
        name: "EmergencyIndication"
        when: windowShown
        function cleanup() {
            testContext.setMetric("leadSlot", 0)
            for (var slot = 1; slot <= 2; ++slot) {
                testContext.setMetric("slot" + slot + "CallState", 0)
                testContext.setMetric("slot" + slot + "CallEmergency", false)
            }
        }
        function test_emergency_tags() {
            compare(monitor.status, Loader.Ready)
            compare(row.status, Loader.Ready)
            testContext.setMetric("leadSlot", 1)
            testContext.setMetric("slot1CallState", 2)
            testContext.setMetric("slot1CallEmergency", true)
            testContext.setMetric("slot2CallState", 2)
            testContext.setMetric("slot2CallEmergency", true)
            var hero = findChild(monitor.item, "heroEmergencyTag")
            var other = findChild(monitor.item, "otherEmergencyTag")
            verify(hero !== null)
            verify(other !== null)
            tryCompare(hero, "visible", true)
            tryCompare(other, "visible", true)
            row.item.emergency = true
            var tag = findChild(row.item, "rowEmergencyTag")
            verify(tag !== null)
            tryCompare(tag, "visible", true)
            row.item.emergency = false
            tryCompare(tag, "visible", false)
            testContext.setMetric("slot1CallEmergency", false)
            testContext.setMetric("slot2CallEmergency", false)
            tryCompare(hero, "visible", false)
            tryCompare(other, "visible", false)
        }
    }
}
