import "../../../src/ui/qt/qml" as Ui
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest

Item {
    id: root
    width: 420
    height: 900

    Ui.MonitorScreen {
        id: screen

        anchors.fill: parent
    }

    TestCase {
        function cleanup() {
            screen.sitesAvailable = false;
            screen.scanTargetName = "";
            Ui.Theme.resetFontScale();
            root.width = 420; root.height = 900;
            testContext.setMetric("radioInput", false);
            testContext.setMetric("scanTargetId", "");
            testContext.setMetric("scanTargetOrdinal", 0);
            testContext.setMetric("scanTargetCount", 0);
            testContext.setMetric("scanHold", false);
        }

        function test_target_rotation_and_hold() {
            screen.scanTargetName = "Dispatch";
            testContext.setMetric("scanTargetId", "internal-entry-uid");
            testContext.setMetric("scanTargetOrdinal", 2);
            testContext.setMetric("scanTargetCount", 3);
            var header = findChild(screen, "scanTargetHeader");
            tryCompare(header, "text", "SCANNING · 2/3 · Dispatch");
            testContext.setMetric("scanHold", true);
            tryCompare(header, "text", "SCANNING · 2/3 · HOLD · Dispatch");
            cleanup();
            tryCompare(header, "text", "");
        }

        function test_header_controls_do_not_overlap_data() {
            return [{tag: "phone", w: 411, h: 900, scale: 1},
                    {tag: "large text", w: 411, h: 900, scale: 1.6},
                    {tag: "landscape", w: 900, h: 411, scale: 1.6}];
        }
        function test_header_controls_do_not_overlap(data) {
            root.width = data.w; root.height = data.h;
            Ui.Theme.fontScale = data.scale;
            screen.sitesAvailable = true;
            testContext.setMetric("radioInput", true);
            waitForRendering(screen);
            var names = ["runningSiteChooserButton", "openSpectrumButton", "monitorLiveStatus", "monitorHeaderTitle"];
            for (var i = 0; i < names.length; ++i) {
                var a = findChild(screen, names[i]);
                var ap = a.mapToItem(screen, 0, 0);
                verify(ap.x >= 0 && ap.x + a.width <= screen.width + 1, names[i] + " fits width");
                var hero = findChild(screen, "monitorHero");
                verify(ap.y + a.height <= hero.y, names[i] + " stays above hero");
                for (var j = i + 1; j < names.length; ++j) {
                    var b = findChild(screen, names[j]);
                    var bp = b.mapToItem(screen, 0, 0);
                    verify(ap.x + a.width <= bp.x || bp.x + b.width <= ap.x
                           || ap.y + a.height <= bp.y || bp.y + b.height <= ap.y,
                           names[i] + " overlaps " + names[j]);
                }
            }
        }

        name: "MonitorScanTarget"
        when: windowShown
    }

}
