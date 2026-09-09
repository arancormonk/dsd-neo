import "../../../src/ui/qt/qml" as Ui
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest

Item {
    width: 420
    height: 900

    Ui.MonitorScreen {
        id: screen

        anchors.fill: parent
    }

    TestCase {
        function cleanup() {
            testContext.setMetric("scanTargetId", "");
            testContext.setMetric("scanTargetOrdinal", 0);
            testContext.setMetric("scanTargetCount", 0);
            testContext.setMetric("scanHold", false);
        }

        function test_target_rotation_and_hold() {
            testContext.setMetric("scanTargetId", "dispatch");
            testContext.setMetric("scanTargetOrdinal", 2);
            testContext.setMetric("scanTargetCount", 3);
            var header = findChild(screen, "scanTargetHeader");
            tryCompare(header, "text", "SCANNING · dispatch (2/3)");
            testContext.setMetric("scanHold", true);
            tryCompare(header, "text", "SCANNING · dispatch (2/3) · HOLD");
            cleanup();
            tryCompare(header, "text", "");
        }

        name: "MonitorScanTarget"
        when: windowShown
    }

}
