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
            testContext.setMetric("scanTimingVisible", false);
            testContext.setMetric("scanStayReason", 0);
            testContext.setMetric("scanStayPhrase", "");
            testContext.setMetric("scanTimerLive", false);
            testContext.setMetric("scanTimerRemainingDs", 0);
            testContext.setMetric("scanTimerSpanMs", 0);
            testContext.setMetric("scanDwellMs", 0);
            testContext.setMetric("scanDwellState", 0);
            testContext.setMetric("scanHoldMs", 0);
            testContext.setMetric("scanHangMs", 0);
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

        // Mirrors the terminal's Scan Timing grammar (issue #508): one space before
        // the countdown, two between groups, seconds to one decimal place.
        function test_scan_timing_row() {
            var row = findChild(screen, "scanTimingRow");
            verify(row !== null, "the monitor carries a scan timing row");
            verify(!row.visible, "nothing published: the row stays down");
            testContext.setMetric("scanStayPhrase", "Idle dwell");
            testContext.setMetric("scanTimerLive", true);
            testContext.setMetric("scanTimerRemainingDs", 18);
            testContext.setMetric("scanTimerSpanMs", 3000);
            testContext.setMetric("scanHoldMs", 2000);
            testContext.setMetric("scanTimingVisible", true);
            tryCompare(row, "visible", true);
            tryCompare(row, "text", "Idle dwell 1.8s/3.0s  hold 2.0s");
            // A followed call: no countdown, the dwell disarmed under it, and the -t
            // hangtime. A trunked row never carries a conventional hold.
            testContext.setMetric("scanStayPhrase", "Following call");
            testContext.setMetric("scanTimerLive", false);
            testContext.setMetric("scanTimerRemainingDs", 0);
            testContext.setMetric("scanTimerSpanMs", 0);
            testContext.setMetric("scanHoldMs", 0);
            testContext.setMetric("scanDwellMs", 3000);
            testContext.setMetric("scanDwellState", 2);
            testContext.setMetric("scanHangMs", 2000);
            tryCompare(row, "text", "Following call  dwell 3.0s (suspended)  hang 2.0s");
            // The operator hold pauses the dwell rather than expiring it.
            testContext.setMetric("scanStayPhrase", "Manual hold");
            testContext.setMetric("scanDwellState", 3);
            testContext.setMetric("scanHangMs", 0);
            tryCompare(row, "text", "Manual hold  dwell 3.0s (paused)");
            cleanup();
            tryCompare(row, "visible", false);
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
            // The widest thing the timing row can say, shown: a row that only fits
            // because it is hidden is not a layout that works.
            testContext.setMetric("scanStayPhrase", "Acquiring control");
            testContext.setMetric("scanTimerLive", true);
            testContext.setMetric("scanTimerRemainingDs", 5999);
            testContext.setMetric("scanTimerSpanMs", 600000);
            testContext.setMetric("scanDwellMs", 600000);
            testContext.setMetric("scanDwellState", 2);
            testContext.setMetric("scanHoldMs", 600000);
            testContext.setMetric("scanHangMs", 600000);
            testContext.setMetric("scanTimingVisible", true);
            waitForRendering(screen);
            var names = ["runningSiteChooserButton", "openSpectrumButton", "monitorLiveStatus", "monitorHeaderTitle", "scanTimingRow"];
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
