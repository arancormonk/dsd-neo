import "../../../src/ui/qt/qml" as Ui
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest

Item {
    id: root
    width: 420
    height: 900

    // The FREQ reading lives in the "Signal & site" disclosure (#718), open here.
    Ui.MonitorScreen {
        id: screen

        anchors.fill: parent
        detailsExpanded: true
    }

    TestCase {
        function cleanup() {
            screen.sitesAvailable = false;
            screen.scanTargetName = "";
            screen.system = null;
            decoderHost.localDeviceBrokered = false;
            decoderHost.receiverHealth = {state: "ok", source: "", session: 0};
            decoderHost.statusText = "idle";
            testContext.setHostRunning(false);
            Ui.Theme.resetFontScale();
            root.width = 420; root.height = 900;
            testContext.setMetric("radioInput", false);
            testContext.setMetric("centerFreqHz", 851000000);
            testContext.setMetric("snrValid", false);
            testContext.setMetric("snrDb", 0.0);
            testContext.setMetric("cfoHz", 0.0);
            testContext.setMetric("tunerGainText", "auto");
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
            testContext.setMetric("scanVisitMs", 0);
            testContext.setMetric("scanVisitLive", false);
            testContext.setMetric("scanVisitRemainingDs", 0);
        }

        // Readings that round to zero carry no minus sign (issue #669): toFixed keeps it.
        function test_signal_readings_have_no_negative_zero() {
            testContext.setMetric("radioInput", true);
            testContext.setMetric("snrValid", true);
            testContext.setMetric("snrDb", -0.04);
            testContext.setMetric("cfoHz", -0.4);
            tryCompare(findChild(screen, "monitorSnrValue"), "text", "0.0 dB");
            tryCompare(findChild(screen, "monitorCfo"), "text", "CFO 0 Hz");
            verify(screen.detailsSummary.plain.indexOf("SNR 0.0 dB") >= 0, screen.detailsSummary.plain);
            testContext.setMetric("snrDb", -10.04);
            testContext.setMetric("cfoHz", -1234.4);
            tryCompare(findChild(screen, "monitorSnrValue"), "text", "-10.0 dB");
            tryCompare(findChild(screen, "monitorCfo"), "text", "CFO -1234 Hz");
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

        // Issue #575: the tuned frequency is the signal strip's first reading in
        // every mode, plain, scanning and holding, while there is one. The header
        // is not where it goes, so the header reads exactly as it did.
        function test_freq_reading_data() {
            return [{tag: "plain", count: 0, hold: false, header: ""},
                    {tag: "scanning", count: 3, hold: false, header: "SCANNING · 2/3 · Dispatch"},
                    {tag: "holding", count: 3, hold: true, header: "SCANNING · 2/3 · HOLD · Dispatch"}];
        }
        function test_freq_reading(data) {
            var strip = findChild(screen, "signalStrip");
            var freq = findChild(screen, "monitorFreq");
            var value = findChild(screen, "monitorFreqValue");
            verify(strip !== null, "the signal strip is named");
            verify(freq !== null && value !== null, "the strip carries a FREQ reading");
            compare(strip.children[0], freq, "FREQ is the strip's first reading");
            testContext.setMetric("radioInput", true);
            testContext.setMetric("centerFreqHz", 851012500);
            if (data.count > 0) {
                screen.scanTargetName = "Dispatch";
                testContext.setMetric("scanTargetId", "internal-entry-uid");
                testContext.setMetric("scanTargetOrdinal", 2);
                testContext.setMetric("scanTargetCount", data.count);
            }
            testContext.setMetric("scanHold", data.hold);
            var header = findChild(screen, "scanTargetHeader");
            tryCompare(header, "text", data.header);
            tryCompare(freq, "visible", true);
            compare(value.text, "851.0125 MHz");
            // No frequency known: no reading, rather than "0.0 MHz".
            testContext.setMetric("centerFreqHz", 0);
            tryCompare(freq, "visible", false);
            testContext.setMetric("centerFreqHz", 851012500);
            tryCompare(freq, "visible", true);
            // No tuner under the session: the strip, and the reading with it, go.
            testContext.setMetric("radioInput", false);
            tryCompare(strip, "visible", false);
            verify(!freq.visible, "the reading outlived its strip");
            compare(header.text, data.header, "the header is unchanged");
        }

        // With FREQ first and every reading at its widest, the strip wraps whole
        // readings onto the next line and nothing runs off a phone.
        function test_freq_reading_fits_a_phone_data() {
            return [{tag: "phone", scale: 1}, {tag: "large text", scale: 1.6}];
        }
        function test_freq_reading_fits_a_phone(data) {
            root.width = 411; root.height = 900;
            Ui.Theme.fontScale = data.scale;
            testContext.setMetric("radioInput", true);
            testContext.setMetric("centerFreqHz", 1296987500);
            testContext.setMetric("snrValid", true);
            testContext.setMetric("snrDb", -10.0);
            testContext.setMetric("cfoHz", -1234.0);
            testContext.setMetric("tunerGainText", "49.6 dB");
            var strip = findChild(screen, "signalStrip");
            var value = findChild(screen, "monitorFreqValue");
            tryCompare(value, "text", "1296.9875 MHz");
            waitForRendering(screen);
            verify(findChild(screen, "monitorFreq").visible, "the reading shows");
            var shown = 0;
            for (var i = 0; i < strip.children.length; ++i) {
                var reading = strip.children[i];
                if (!reading.visible)
                    continue;
                ++shown;
                var at = reading.mapToItem(screen, 0, 0);
                verify(at.x >= 0 && at.x + reading.width <= screen.width + 1,
                       "strip reading " + i + " runs off the screen");
            }
            verify(shown >= 6, "the strip shows all its readings");
        }

        // Mirrors the terminal's Scan Timing grammar (issue #508): one space before
        // the countdown, two between groups, seconds to one decimal place.
        function test_scan_timing_row() {
            var row = findChild(screen, "scanTimingRow");
            verify(row !== null, "the monitor carries a scan timing row");
            verify(!row.visible, "nothing published: the row stays down");
            // A rotation keeps the line, blank, so the stays coming and going never
            // move the screen (#718).
            testContext.setMetric("scanRotationActive", true);
            tryCompare(row, "visible", true);
            compare(row.text, "");
            verify(row.height > 0);
            testContext.setMetric("scanRotationActive", false);
            tryCompare(row, "visible", false);
            testContext.setMetric("scanStayPhrase", "Idle dwell");
            testContext.setMetric("scanTimerLive", true);
            testContext.setMetric("scanTimerRemainingDs", 18);
            testContext.setMetric("scanTimerSpanMs", 3000);
            testContext.setMetric("scanHoldMs", 2000);
            testContext.setMetric("scanTimingVisible", true);
            tryCompare(row, "visible", true);
            tryCompare(row, "text", "Idle dwell 1.8s/3.0s  hold 2.0s");
            // Large accepted -t values retain the publisher's unsigned total.
            testContext.setMetric("scanTimerSpanMs", 4294967295);
            tryCompare(row, "text", "Idle dwell 1.8s/4294967.3s  hold 2.0s");
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
            // The per-visit cap (issue #507) closes the row, in the same grammar: two
            // spaces before it, one decimal place, and the word `paused` where a hold has
            // suspended the cap rather than a countdown that would read as expiry.
            testContext.setMetric("scanVisitMs", 20000);
            testContext.setMetric("scanVisitLive", false);
            tryCompare(row, "text", "Manual hold  dwell 3.0s (paused)  Visit: paused");
            testContext.setMetric("scanVisitLive", true);
            testContext.setMetric("scanVisitRemainingDs", 123);
            tryCompare(row, "text", "Manual hold  dwell 3.0s (paused)  Visit: 12.3s/20.0s");
            // No cap in force: the view withholds the width, so the row closes as before.
            testContext.setMetric("scanVisitMs", 0);
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
            testContext.setMetric("scanVisitMs", 3600000);
            testContext.setMetric("scanVisitLive", true);
            testContext.setMetric("scanVisitRemainingDs", 35999);
            testContext.setMetric("scanTimingVisible", true);
            waitForRendering(screen);
            var names = ["runningSiteChooserButton", "openSpectrumButton", "monitorLiveStatus", "monitorHeaderTitle", "scanTimingRow"];
            // Past tier 0 on a phone, Sites and Spectrum are in the session menu (#718)
            // rather than squeezed beside the title: they show exactly when the
            // header has room for them, and the title, LIVE and timing always do.
            for (var name of ["runningSiteChooserButton", "openSpectrumButton"])
                compare(findChild(screen, name).visible, screen.wideHeaderActions, name);
            for (var always of ["monitorLiveStatus", "monitorHeaderTitle", "scanTimingRow"])
                verify(findChild(screen, always).visible, always);
            var shown = names.filter(function (name) { return findChild(screen, name).visible; });
            compare(shown.length, screen.wideHeaderActions ? 5 : 3);
            var hero = findChild(screen, "monitorHero");
            var heroY = hero.mapToItem(screen, 0, 0).y;
            for (var i = 0; i < shown.length; ++i) {
                var a = findChild(screen, shown[i]);
                var ap = a.mapToItem(screen, 0, 0);
                verify(ap.x >= 0 && ap.x + a.width <= screen.width + 1, shown[i] + " fits width");
                verify(ap.y + a.height <= heroY, shown[i] + " stays above hero");
                for (var j = i + 1; j < shown.length; ++j) {
                    var b = findChild(screen, shown[j]);
                    var bp = b.mapToItem(screen, 0, 0);
                    verify(ap.x + a.width <= bp.x || bp.x + b.width <= ap.x
                           || ap.y + a.height <= bp.y || bp.y + b.height <= ap.y,
                           shown[i] + " overlaps " + shown[j]);
                }
            }
        }

        // ---- #647: the header actions' width (B16) ----

        readonly property var actionNames: ["runningSiteChooserButton", "sessionOptionsButton", "openSpectrumButton", "monitorLiveStatus"]
        // What the pill can say in a USB session: live, its receiver lost (the
        // engine still running), and the host's transition words once it is not.
        function setPillLabel(label) {
            decoderHost.receiverHealth = label === "LOST" ? {state: "lost", source: "usb", session: 1} : {state: "ok", source: "", session: 0};
            decoderHost.statusText = label === "STARTING…" ? "Starting…" : label === "STOPPING…" ? "Stopping…" : "idle";
            testContext.setHostRunning(label === "LIVE" || label === "LOST");
            tryCompare(findChild(screen, "monitorLiveLabel"), "text", label);
            settleActions();
        }
        // Rows place their items on polish: the pill's own row sizes the pill,
        // then the actions' row places it.
        function settleActions() {
            verify(waitForPolish(findChild(screen, "monitorLiveLabel").parent));
            verify(waitForPolish(findChild(screen, "monitorLiveStatus").parent));
        }
        function visibleActions() {
            return actionNames.filter(function (name) { return findChild(screen, name).visible; });
        }
        function actionRects() {
            return visibleActions().map(function (name) {
                var action = findChild(screen, name);
                var at = action.mapToItem(screen, 0, 0);
                return {name: name, x: at.x, y: at.y, width: action.width, height: action.height};
            });
        }
        function useTallPhone(width, scale) {
            root.width = width; root.height = 1400;
            Ui.Theme.fontScale = scale;
            screen.system = {name: "County P25", sourceType: "usb"};
            screen.sitesAvailable = true;
            testContext.setMetric("radioInput", true);
        }

        // Brokered hosts: the actions are measured with the pill at the widest
        // label the session can show, so every action stays on the screen at any
        // label, and which actions show never changes with the label. When they
        // do not fit, Sites and Spectrum are left to the session menu, as past
        // tier 0.
        function test_header_actions_fit_a_tall_phone_data() {
            var rows = [];
            for (var width of [320, 360, 411])
                for (var scale of [1, 1.6, 2])
                    rows.push({tag: width + "@" + scale, width: width, scale: scale});
            return rows;
        }
        function test_header_actions_fit_a_tall_phone(data) {
            failOnWarning(/.*/);
            decoderHost.localDeviceBrokered = true;
            useTallPhone(data.width, data.scale);
            var shown = null;
            for (var label of ["LIVE", "LOST", "STARTING…", "STOPPING…"]) {
                setPillLabel(label);
                compare(screen.tier, 0, "a tall phone is tier 0");
                var rects = actionRects();
                var names = rects.map(function (rect) { return rect.name; }).join();
                if (shown === null)
                    shown = names;
                compare(names, shown, label + " changed which actions show");
                for (var rect of rects)
                    verify(rect.x >= Ui.Theme.screenPadding - 0.5 && rect.x + rect.width <= screen.width - Ui.Theme.screenPadding + 0.5, rect.name + " leaves the row at " + label);
            }
            compare(findChild(screen, "runningSiteChooserButton").visible, screen.wideHeaderActions);
            compare(findChild(screen, "openSpectrumButton").visible, screen.wideHeaderActions);
            // Measured with Sites and Spectrum in, whether they show or not.
            verify(screen.headerActionsWidth > findChild(screen, "monitorLiveStatus").width + 48 + 2 * 8);
        }

        // The desktop's header is exactly as it was: Sites and Spectrum follow the
        // tier and the width alone, and the actions sit right-aligned, 8 px apart.
        // Where the measured actions fit, Android lays them out identically.
        function test_header_geometry_unchanged_off_android_data() {
            var rows = [];
            for (var width of [320, 360, 411, 540])
                for (var scale of [1, 1.6, 2])
                    rows.push({tag: width + "@" + scale, width: width, scale: scale});
            return rows;
        }
        function test_header_geometry_unchanged_off_android(data) {
            failOnWarning(/.*/);
            useTallPhone(data.width, data.scale);
            var header = findChild(screen, "monitorHeader");
            for (var label of ["LIVE", "STARTING…", "STOPPING…"]) {
                decoderHost.localDeviceBrokered = false;
                setPillLabel(label);
                compare(screen.wideHeaderActions, screen.tier === 0 || screen.width >= 500);
                compare(screen.headerStacked, screen.tier === 0 && screen.width < 500);
                var rects = actionRects();
                compare(rects.length, screen.wideHeaderActions ? 4 : 2, label);
                var right = header.mapToItem(screen, header.width, 0).x;
                compare(rects[rects.length - 1].x + rects[rects.length - 1].width, right, "the pill ends the row");
                for (var i = 0; i + 1 < rects.length; ++i) {
                    compare(rects[i].x + rects[i].width + 8, rects[i + 1].x, rects[i].name + " to " + rects[i + 1].name);
                    compare(rects[i].y, rects[i + 1].y);
                }
                decoderHost.localDeviceBrokered = true;
                settleActions();
                if (visibleActions().length !== rects.length)
                    continue;
                var brokered = actionRects();
                for (var j = 0; j < rects.length; ++j) {
                    compare(brokered[j].x, rects[j].x, rects[j].name + " x at " + label);
                    compare(brokered[j].width, rects[j].width, rects[j].name + " width at " + label);
                    compare(brokered[j].y, rects[j].y, rects[j].name + " y at " + label);
                }
            }
        }

        name: "MonitorScanTarget"
        when: windowShown
    }

}
