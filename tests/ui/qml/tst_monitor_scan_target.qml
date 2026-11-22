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
            screen.scanListName = "";
            screen.otherListCount = 0;
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
            testContext.setMetric("scanQuietStatus", 0);
            testContext.setMetric("scanQuietReason", 0);
            testContext.setMetric("scanQuietText", "");
            testContext.setMetric("scanRotationActive", false);
            testContext.setMetric("scanKind", 0);
            testContext.setMetric("scanPaused", false);
            testContext.setMetric("scanResumable", false);
            testContext.setMetric("scanEntryCount", 0);
            testContext.setMetric("scanListCount", 0);
            testContext.setMetric("scanActiveList", 0);
            testContext.setMetric("scanRotationOrdinal", 0);
            testContext.setMetric("scanRotationCount", 0);
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

        // Issue #697: with more than one list the line counts the targets of the
        // lists that are on, says "-" while the target on air's own list is off,
        // and closes with the list it comes from; one list reads as before.
        function test_multi_list_header() {
            var header = findChild(screen, "scanTargetHeader");
            screen.scanTargetName = "Dispatch";
            screen.scanListName = "Hospital";
            testContext.setMetric("scanTargetOrdinal", 9);
            testContext.setMetric("scanTargetCount", 20);
            tryCompare(header, "text", "SCANNING · 9/20 · Dispatch");
            testContext.setMetric("scanListCount", 3);
            testContext.setMetric("scanActiveList", 2);
            testContext.setMetric("scanRotationOrdinal", 3);
            testContext.setMetric("scanRotationCount", 14);
            tryCompare(header, "text", "SCANNING · 3/14 · Dispatch · Hospital");
            testContext.setMetric("scanHold", true);
            tryCompare(header, "text", "SCANNING · 3/14 · HOLD · Dispatch · Hospital");
            testContext.setMetric("scanHold", false);
            testContext.setMetric("scanRotationOrdinal", 0);
            tryCompare(header, "text", "SCANNING · -/14 · Dispatch · Hospital");
            // No name for the list: the line ends at the target.
            screen.scanListName = "";
            tryCompare(header, "text", "SCANNING · -/14 · Dispatch");
            cleanup();
            tryCompare(header, "text", "");
        }

        // Issue #697: a session of several lists is titled by its own list and how many more; on a narrow phone at
        // large text the list's name gives way before the count does.
        function test_multi_list_title() {
            var title = findChild(screen, "monitorHeaderTitle");
            screen.system = {name: "Security", sourceType: "usb"};
            tryCompare(title, "text", "Security");
            screen.otherListCount = 2;
            tryCompare(title, "text", "Security + 2 lists");
            compare(screen.systemName, "Security + 2 lists");
            screen.otherListCount = 1;
            tryCompare(title, "text", "Security + 1 list");
            screen.system = {name: "Countywide public safety and municipal services", sourceType: "usb"};
            screen.otherListCount = 7;
            // A phone at 2x text, tall enough for the stacked header, where the
            // title has the row to itself. Checked once the layout has settled:
            // the tier follows the text size a turn of the event loop later.
            root.width = 320;
            root.height = 1300;
            Ui.Theme.fontScale = 2;
            waitForRendering(screen);
            tryVerify(function () { return screen.tier === 0 && screen.headerStacked; }, 1000, "the stacked header");
            tryVerify(function () { return title.text.endsWith(" + 7 lists"); });
            verify(title.text.indexOf("…") > 0, title.text);
            verify(!title.truncated, "the count was elided");
            compare(screen.systemName, "Countywide public safety and municipal services + 7 lists");
            screen.otherListCount = 0;
            Ui.Theme.resetFontScale();
            root.width = 420;
            root.height = 900;
        }

        // Stacked over the actions (tier 0) on a 290 dp window at 2x text, the title's room is the heading's, which
        // keeps a 12 px margin at its right: the form decided with that room keeps a readable name, at least three
        // letters before the ellipsis, and the count whole, on the title's line or its own.
        function test_stacked_multi_list_title_keeps_the_name() {
            var name = "Countywide public safety and municipal services";
            screen.system = {name: name, sourceType: "usb"};
            screen.otherListCount = 7;
            testContext.setHostRunning(true);
            root.width = 290;
            root.height = 1600;
            Ui.Theme.fontScale = 2;
            try {
                waitForRendering(screen);
                tryVerify(function () { return screen.tier === 0 && screen.headerStacked; }, 1000, "tier 0, stacked");
                var title = findChild(screen, "monitorHeaderTitle");
                var count = findChild(screen, "monitorHeaderTitleCount");
                waitForRendering(screen);
                var cut = title.text.indexOf("…");
                verify(cut >= 3 && name.indexOf(title.text.substring(0, cut)) === 0, "the name shown: '" + title.text + "'");
                verify(!title.truncated, "the title line overflows");
                var heading = title.parent;
                verify(title.x + title.width <= heading.width + 0.5, "the title runs past its heading");
                if (count.visible)
                    verify(!count.truncated && count.text === "+ 7 lists", "the count's line: " + count.text);
                else
                    verify(title.text.indexOf(" + 7 lists") > 0, "the count is on the title's line: " + title.text);
                compare(title.Accessible.name.indexOf(" + 7 lists") > 0, true);
                compare(findChild(screen, "monitorHeader").height, screen.headerBarHeight(screen.tier));
            } finally {
                screen.otherListCount = 0;
                Ui.Theme.resetFontScale();
                root.width = 420;
                root.height = 900;
            }
        }

        // Issue #901 B12: beside the header actions on a narrow phone at large
        // text the title has no room for "+ 7 lists" and a readable part of the
        // name, so the count takes a second line of the header's whole width
        // below the actions, and the name the first line, elided. The heading
        // is read as one, and the budget's header is the one drawn.
        function test_multi_list_title_two_lines_data() {
            return [{tag: "320x900@2", width: 320, height: 900, scale: 2},
                    {tag: "320x900@1.6", width: 320, height: 900, scale: 1.6},
                    {tag: "360x690@1.6", width: 360, height: 690, scale: 1.6}];
        }
        function test_multi_list_title_two_lines(data) {
            var name = "Countywide public safety and municipal services";
            screen.system = {name: name, sourceType: "usb"};
            screen.otherListCount = 7;
            testContext.setHostRunning(true);
            root.width = data.width;
            root.height = data.height;
            Ui.Theme.fontScale = data.scale;
            waitForRendering(screen);
            tryVerify(function () { return screen.tier >= 1 && !screen.headerStacked; }, 1000, "the header actions share the title's line");
            var header = findChild(screen, "monitorHeader");
            var title = findChild(screen, "monitorHeaderTitle");
            waitForRendering(screen);
            // A readable part of the name: at least three letters before the ellipsis.
            var cut = title.text.indexOf("…");
            verify(cut >= 3 && name.indexOf(title.text.substring(0, cut)) === 0, "the name shown: '" + title.text + "'");
            verify(!title.truncated, "the title line overflows");
            var count = findChild(screen, "monitorHeaderTitleCount");
            verify(count !== null, "no line for the count");
            tryVerify(function () { return count.visible; }, 1000, "the count's line");
            compare(count.text, "+ 7 lists");
            verify(!count.truncated, "the count was cut");
            compare(title.Accessible.name, title.text + " + 7 lists");
            // Inside the header, and clear of the actions and of each other.
            var parts = [title, count, findChild(screen, "sessionOptionsButton"), findChild(screen, "monitorLiveStatus")];
            var rects = parts.map(function (part) {
                var at = part.mapToItem(header, 0, 0);
                return {name: part.objectName, x: at.x, y: at.y, w: part.width, h: part.height};
            });
            for (var r of rects)
                verify(r.x >= -0.5 && r.y >= -0.5 && r.x + r.w <= header.width + 0.5 && r.y + r.h <= header.height + 0.5,
                       r.name + " inside the header: " + JSON.stringify(r) + " in " + header.width + "x" + header.height);
            for (var i = 0; i < rects.length; ++i) {
                for (var j = i + 1; j < rects.length; ++j) {
                    var a = rects[i], b = rects[j];
                    var clear = Math.min(a.x + a.w, b.x + b.w) - Math.max(a.x, b.x) <= 0.5 || Math.min(a.y + a.h, b.y + b.h) - Math.max(a.y, b.y) <= 0.5;
                    verify(clear, a.name + " overlaps " + b.name);
                }
            }
            // The budget's header is the drawn one.
            compare(header.height, screen.headerBarHeight(screen.tier));
            var heading = title.parent;
            var pill = findChild(screen, "monitorLiveStatus");
            verify(waitForPolish(pill.parent));
            var drawn = Math.max(heading.mapToItem(header, 0, heading.height).y, pill.mapToItem(header, 0, pill.height).y);
            verify(Math.abs(drawn - header.height) < 0.5, "header " + header.height + " vs drawn " + drawn);
            // One list: one line again.
            screen.otherListCount = 0;
            tryVerify(function () { return !count.visible; }, 1000, "the count's line stays for one list");
            compare(header.height, screen.headerBarHeight(screen.tier));
        }

        // Issue #901 r2 B1: every header line is one line in its fixed box. A line break in a session name, a scan
        // target's name or a stay phrase (U+2028, U+2029, a newline) is drawn as a space: the header keeps its height,
        // each line holds one line, and the title is read with every word.
        function test_header_lines_draw_one_line_data() {
            return [{tag: "U+2028", sep: "\u2028"}, {tag: "U+2029", sep: "\u2029"}, {tag: "newline", sep: "\n"}];
        }
        function test_header_lines_draw_one_line(data) {
            var sep = data.sep;
            testContext.setHostRunning(true);
            root.width = 360;
            root.height = 900;
            screen.system = {name: "County Fire", sourceType: "usb"};
            testContext.setMetric("scanRotationActive", true);
            testContext.setMetric("scanTargetOrdinal", 2);
            testContext.setMetric("scanTargetCount", 3);
            testContext.setMetric("scanTimingVisible", true);
            testContext.setMetric("scanStayPhrase", "Following call");
            screen.scanTargetName = "East";
            var header = findChild(screen, "monitorHeader");
            var title = findChild(screen, "monitorHeaderTitle");
            var target = findChild(screen, "scanTargetHeader");
            var timing = findChild(screen, "scanTimingRow");
            tryCompare(target, "text", "SCANNING · 2/3 · East");
            waitForRendering(screen);
            var height = header.height;
            var tier = screen.tier;
            screen.system = {name: "County" + sep + "Fire" + sep + "East", sourceType: "usb"};
            screen.scanTargetName = "East" + sep + "Gate";
            testContext.setMetric("scanStayPhrase", "Following" + sep + "call");
            waitForRendering(screen);
            compare(title.text, "County Fire East");
            compare(target.text, "SCANNING · 2/3 · East Gate");
            compare(timing.text, "Following call");
            for (var line of [title, target, timing]) {
                compare(line.lineCount, 1, line.objectName + " draws one line");
                verify(line.contentHeight <= line.height + 1, line.objectName + " stays in its box: " + line.contentHeight + " in " + line.height);
            }
            compare(header.height, height, "the header's height");
            compare(screen.tier, tier, "the tier");
            compare(title.Accessible.name, "County Fire East");
            // Several lists: the name's line and the count's.
            screen.otherListCount = 2;
            waitForRendering(screen);
            compare(title.lineCount, 1);
            compare(title.Accessible.name, "County Fire East + 2 lists");
        }

        // Issue #901 B9: the header's lines are fixed line boxes, so a scan
        // target or a timing phrase in a script whose line is taller (Burmese)
        // changes neither the header's height nor the tier.
        function test_header_lines_ignore_the_script_data() {
            return [{tag: "420x900@1", width: 420, height: 900, scale: 1},
                    {tag: "320x900@1", width: 320, height: 900, scale: 1},
                    {tag: "360x690@1.6", width: 360, height: 690, scale: 1.6}];
        }
        function test_header_lines_ignore_the_script(data) {
            root.width = data.width;
            root.height = data.height;
            Ui.Theme.fontScale = data.scale;
            testContext.setHostRunning(true);
            testContext.setMetric("scanRotationActive", true);
            testContext.setMetric("scanTargetOrdinal", 2);
            testContext.setMetric("scanTargetCount", 3);
            testContext.setMetric("scanTimingVisible", true);
            testContext.setMetric("scanStayPhrase", "Following call");
            screen.scanTargetName = "East";
            var target = findChild(screen, "scanTargetHeader");
            var header = findChild(screen, "monitorHeader");
            tryCompare(target, "text", "SCANNING · 2/3 · East");
            waitForRendering(screen);
            var height = header.height;
            var tier = screen.tier;
            var steps = [{name: "က", phrase: "Following call"}, {name: "East", phrase: "က"}, {name: "East", phrase: "Following call"}];
            for (var step of steps) {
                screen.scanTargetName = step.name;
                testContext.setMetric("scanStayPhrase", step.phrase);
                tryCompare(target, "text", "SCANNING · 2/3 · " + step.name);
                waitForRendering(screen);
                compare(header.height, height, "the header with " + step.name + " / " + step.phrase);
                compare(screen.tier, tier, "the tier with " + step.name + " / " + step.phrase);
            }
        }

        // Issue #671: a paused scan is named as one, apart from the rotation's
        // HOLD and the timing row's "(paused)" dwell, with how much there is to
        // go back to. It has no target on air, and no timing to show.
        function test_paused_scan_header_data() {
            return [{tag: "trunk scan", kind: 2, count: 3, resumable: true, header: "SCAN PAUSED · 3 targets"},
                    {tag: "one target", kind: 2, count: 1, resumable: true, header: "SCAN PAUSED · 1 target"},
                    {tag: "-Y list", kind: 1, count: 12, resumable: true, header: "SCAN PAUSED · 12 channels"},
                    {tag: "one channel", kind: 1, count: 1, resumable: true, header: "SCAN PAUSED · 1 channel"},
                    {tag: "resume pending", kind: 1, count: 12, resumable: false, header: "RESUMING SCAN · 12 channels"},
                    {tag: "no size", kind: 2, count: 0, resumable: true, header: "SCAN PAUSED"}];
        }
        function test_paused_scan_header(data) {
            var header = findChild(screen, "scanTargetHeader");
            var timing = findChild(screen, "scanTimingRow");
            screen.scanTargetName = "Dispatch";
            testContext.setMetric("scanKind", data.kind);
            testContext.setMetric("scanTargetCount", data.kind === 2 ? data.count : 0);
            testContext.setMetric("scanEntryCount", data.count);
            testContext.setMetric("scanResumable", data.resumable);
            testContext.setMetric("scanPaused", true);
            tryCompare(header, "text", data.header);
            verify(!timing.visible, "a paused scan kept its timing row");
            testContext.setMetric("scanPaused", false);
            testContext.setMetric("scanRotationActive", true);
            testContext.setMetric("scanTargetOrdinal", 2);
            if (data.kind === 2 && data.count > 0)
                tryCompare(header, "text", "SCANNING · 2/" + data.count + " · Dispatch");
        }

        // The count is what the paused line is for: on a narrow phone at large
        // text the noun goes before the number does, and nothing is elided.
        function test_paused_header_keeps_its_count() {
            var header = findChild(screen, "scanTargetHeader");
            testContext.setMetric("scanKind", 1);
            testContext.setMetric("scanEntryCount", 12);
            testContext.setMetric("scanResumable", true);
            testContext.setMetric("scanPaused", true);
            tryCompare(header, "text", "SCAN PAUSED · 12 channels");
            // A 412 dp phone at 1.6x, where the whole line was elided inside its noun.
            root.width = 412;
            Ui.Theme.fontScale = 1.6;
            tryCompare(header, "text", "SCAN PAUSED · 12");
            verify(!header.truncated, "the count was elided");
            // Narrower still, the count goes whole and the state stays.
            root.width = 360;
            tryCompare(header, "text", "SCAN PAUSED");
            verify(!header.truncated, "the state was elided");
            Ui.Theme.resetFontScale();
            root.width = 420;
            tryCompare(header, "text", "SCAN PAUSED · 12 channels");
            verify(!header.truncated);
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
            // The silent-channel skip's status (#834) is the phrase itself, in the same grammar
            // (what a screen reader hears of it is tst_a11y_paced_names.qml's).
            testContext.setMetric("scanStayReason", 8);
            testContext.setMetric("scanStayPhrase", "No check: width");
            testContext.setMetric("scanDwellMs", 0);
            testContext.setMetric("scanDwellState", 0);
            testContext.setMetric("scanTimerLive", true);
            testContext.setMetric("scanTimerRemainingDs", 18);
            testContext.setMetric("scanTimerSpanMs", 3000);
            testContext.setMetric("scanHoldMs", 2000);
            testContext.setMetric("scanQuietStatus", 2);
            testContext.setMetric("scanQuietReason", 5);
            testContext.setMetric("scanQuietText", "Skip silent channels can't check this channel at its channel width. Normal scan timing applies.");
            tryCompare(row, "text", "No check: width 1.8s/3.0s  hold 2.0s");
            testContext.setMetric("scanStayPhrase", "Quiet check");
            testContext.setMetric("scanQuietStatus", 1);
            testContext.setMetric("scanQuietReason", 0);
            testContext.setMetric("scanQuietText", "Checking whether this channel is silent. If it is, the scan moves on early.");
            tryCompare(row, "text", "Quiet check 1.8s/3.0s  hold 2.0s");
            testContext.setMetric("scanQuietStatus", 0);
            testContext.setMetric("scanQuietText", "");
            testContext.setMetric("scanStayPhrase", "Idle dwell");
            tryCompare(row, "text", "Idle dwell 1.8s/3.0s  hold 2.0s");
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
            // Issue #697: the longest scan line, a multi-list one, shown.
            screen.scanTargetName = "Countywide Fire Dispatch Main";
            screen.scanListName = "Municipal services north";
            testContext.setMetric("scanListCount", 8);
            testContext.setMetric("scanActiveList", 8);
            testContext.setMetric("scanRotationOrdinal", 0);
            testContext.setMetric("scanRotationCount", 2424);
            testContext.setMetric("scanHold", true);
            waitForRendering(screen);
            verify(findChild(screen, "scanTargetHeader").text.indexOf("SCANNING · -/2424 · HOLD") === 0);
            var names = ["runningSiteChooserButton", "openSpectrumButton", "monitorLiveStatus", "monitorHeaderTitle", "scanTimingRow", "scanTargetHeader"];
            // Past tier 0 on a phone, Sites and Spectrum are in the session menu (#718)
            // rather than squeezed beside the title: they show exactly when the
            // header has room for them, and the title, LIVE and timing always do.
            for (var name of ["runningSiteChooserButton", "openSpectrumButton"])
                compare(findChild(screen, name).visible, screen.wideHeaderActions, name);
            for (var always of ["monitorLiveStatus", "monitorHeaderTitle", "scanTimingRow", "scanTargetHeader"])
                verify(findChild(screen, always).visible, always);
            var shown = names.filter(function (name) { return findChild(screen, name).visible; });
            compare(shown.length, screen.wideHeaderActions ? 6 : 4);
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
