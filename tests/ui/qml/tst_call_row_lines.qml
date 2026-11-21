// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// Issue #901: the shared call row's height comes from its fonts and the view's
// width, never from what a call says. A name in another script came from a
// fallback face and made its row 59 or 65 px high beside a 64-px Latin row
// (B5), and a recorded emergency call in Home's 280-px Recent pane left its
// name 6 px wide at 2x text (B7). Compact view (D5) tightens the padding; the
// row keeps its two lines and its right side.
Item {
    id: root

    width: 1000
    height: 900

    // Rows a view lays out side by side: the same width, different calls.
    Column {
        id: scripts

        width: 420

        Ui.CallRow {
            id: latin
            width: scripts.width
            name: "Ag"
            metaText: "TG 4001 · SRC 7001"
            rightText: "09:41"
        }
        Ui.CallRow {
            id: emoji
            width: scripts.width
            name: "🚒🚒"
            metaText: "TG 4001 · SRC 7001"
            rightText: "09:41"
        }
        Ui.CallRow {
            id: cjk
            width: scripts.width
            name: "消防指令"
            metaText: "TG 4001 · SRC 7001"
            rightText: "09:41"
        }
        Ui.CallRow {
            id: cjkMeta
            width: scripts.width
            name: "Ag"
            metaText: "消防 · 東区 4001"
            rightText: "09:41"
        }
        Ui.CallRow {
            id: tagged
            width: scripts.width
            name: "Fire Dispatch North"
            metaText: "TG 4001 · SRC 7001"
            rightText: "09:41"
            emergency: true
            hasRecording: true
        }
        Ui.CallRow {
            id: tall
            width: scripts.width
            // A script whose lines stand taller than the box (D10's limit).
            name: "བོད་སྐད་ཀྱི་རྒྱུད"
            metaText: "TG 4001 · SRC 7001"
            rightText: "09:41"
        }
    }

    // Home's Recent pane is 280 px wide.
    Column {
        id: pane

        x: 440
        width: 280

        Ui.CallRow {
            id: tight
            width: pane.width
            name: "Fire Dispatch North"
            metaText: "TG 4001 · SRC 7001 · 851.0125 MHz"
            rightText: "09:41"
            emergency: true
            hasRecording: true
        }
        Ui.CallRow {
            id: tightPlain
            width: pane.width
            name: "Fire Dispatch North"
            metaText: "TG 4001 · SRC 7001"
            rightText: "09:41"
        }
        Ui.CallRow {
            id: tightEnc
            width: pane.width
            name: "Fire Dispatch North"
            metaText: "TG 4001 · SRC 7001"
            rightText: "09:41"
            enc: true
            hasRecording: true
        }
    }

    // A store a screen is handed in place of the app's preferences.
    QtObject {
        id: homeStore

        property bool compactView: true
        property string listenSection: "systems"
        property bool metricUnits: false
    }

    Loader {
        id: home

        y: 0
        width: 1000
        height: 900
        active: false
        visible: active
        source: uiDir + "/HomeScreen.qml"
    }

    TestCase {
        id: tc

        name: "CallRowLines"
        when: windowShown

        readonly property var sameView: [latin, emoji, cjk, cjkMeta, tagged, tall]
        readonly property var tightView: [tight, tightPlain, tightEnc]

        function rectOf(item, under) {
            var at = item.mapToItem(under, 0, 0);
            return {x: at.x, y: at.y, width: item.width, height: item.height};
        }
        function overlaps(a, b) {
            return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
        }
        function inside(a, row) {
            return a.x >= 0 && a.y >= 0 && a.x + a.width <= row.width && a.y + a.height <= row.height;
        }
        function setCompact(on) {
            for (var row of tc.sameView.concat(tc.tightView))
                row.compact = on;
        }

        function cleanup() {
            Ui.Theme.resetFontScale();
            setCompact(false);
            scripts.width = 420;
            home.active = false;
            wait(0);
            homeStore.compactView = true;
            callHistory.clearAll();
        }

        function test_b5_rows_share_one_height_data() {
            var cases = [];
            for (var scale of [1.0, 1.6, 2.0]) {
                cases.push({tag: scale.toFixed(1) + " default", scale: scale, compact: false});
                cases.push({tag: scale.toFixed(1) + " compact", scale: scale, compact: true});
            }
            return cases;
        }
        // B5: Latin, emoji and CJK names, and a CJK meta line, give the same row
        // height, which is the shared function's.
        function test_b5_rows_share_one_height(data) {
            Ui.Theme.fontScale = data.scale;
            setCompact(data.compact);
            waitForRendering(scripts);
            for (var row of tc.sameView)
                compare(row.height, latin.height, "\"" + row.name + "\" / \"" + row.metaText + "\" moved its row");
            compare(latin.height, Ui.LineBoxes.callRowHeight(data.compact, latin.narrow), "the row and the shared measure disagree");
            for (row of tc.sameView) {
                compare(findChild(row, "callRowName").height, Ui.LineBoxes.callRowNameBox(), "the name's box: " + row.name);
                compare(findChild(row, "callRowMeta").height, Ui.LineBoxes.callRowMetaBox(), "the meta line's box: " + row.name);
            }
        }

        // D5: a compact row is about 52 px at 1x against about 64 by default,
        // and never under a touch target; a default row never under 58.
        function test_compact_height_data() {
            return [{tag: "1.0", scale: 1.0}, {tag: "1.3", scale: 1.3}, {tag: "1.6", scale: 1.6}, {tag: "2.0", scale: 2.0}];
        }
        function test_compact_height(data) {
            Ui.Theme.fontScale = data.scale;
            waitForRendering(scripts);
            var normal = latin.height;
            verify(normal >= Ui.Theme.rowHeight, "a default row under " + Ui.Theme.rowHeight + ": " + normal);
            setCompact(true);
            waitForRendering(scripts);
            verify(latin.height >= Ui.Theme.minimumTouchSize, "a compact row under a touch target: " + latin.height);
            verify(latin.height < normal, "compact is no smaller: " + latin.height + " vs " + normal);
            if (data.scale === 1.0) {
                verify(Math.abs(latin.height - 52) <= 2, "a compact row at 1x: " + latin.height);
                verify(Math.abs(normal - 64) <= 2, "a default row at 1x: " + normal);
            }
            // Same lines in either density, and the right side stays.
            var name = findChild(latin, "callRowName");
            var meta = findChild(latin, "callRowMeta");
            var time = findChild(latin, "callRowTime");
            verify(name.visible && meta.visible && time.visible);
            verify(inside(rectOf(name, latin), latin) && inside(rectOf(meta, latin), latin) && inside(rectOf(time, latin), latin));
        }

        // D10's limit: a line in a taller script keeps its box and is still
        // drawn, centred and unclipped, not dropped or elided away.
        function test_tall_script_is_drawn_in_its_box() {
            waitForRendering(scripts);
            var name = findChild(tall, "callRowName");
            compare(name.height, Ui.LineBoxes.callRowNameBox());
            compare(name.verticalAlignment, Text.AlignVCenter);
            verify(!name.clip);
            compare(name.lineCount, 1);
            verify(name.contentWidth > 0, "the taller line was not drawn");
            verify(!name.truncated, "the taller line was elided");
        }

        function test_b7_narrow_form_data() {
            return [{tag: "1.0", scale: 1.0}, {tag: "1.6", scale: 1.6}, {tag: "2.0", scale: 2.0}];
        }
        // B7: in Home's 280-px pane, a recorded emergency call keeps its name
        // at least max(96, 35% of the row) wide; where the tags would not
        // leave that, they move to a line of their own, inside the row and
        // clear of the name and the meta line.
        function test_b7_narrow_form(data) {
            Ui.Theme.fontScale = data.scale;
            waitForRendering(pane);
            var least = Math.max(96, 0.35 * tight.width);
            var name = findChild(tight, "callRowName");
            var meta = findChild(tight, "callRowMeta");
            verify(name.width >= least, "the name is " + name.width.toFixed(1) + " px wide, under " + least);
            verify(meta.width >= least, "the meta line is " + meta.width.toFixed(1) + " px wide, under " + least);
            var nameRect = rectOf(name, tight);
            var metaRect = rectOf(meta, tight);
            verify(inside(nameRect, tight) && inside(metaRect, tight));
            var tag = findChild(tight, "rowEmergencyTag");
            var glyph = findChild(tight, "rowRecordingGlyph");
            var time = findChild(tight, "callRowTime");
            for (var part of [tag, glyph, time]) {
                verify(part.visible, part.objectName);
                var r = rectOf(part, tight);
                verify(inside(r, tight), part.objectName + " leaves the row: " + JSON.stringify(r) + " in " + tight.width + "x" + tight.height);
                verify(!overlaps(r, nameRect), part.objectName + " covers the name");
                verify(!overlaps(r, metaRect), part.objectName + " covers the meta line");
            }
            // EMERGENCY, then the glyph, then the time, in either form.
            var tagRect = rectOf(tag, tight);
            var glyphRect = rectOf(glyph, tight);
            var timeRect = rectOf(time, tight);
            verify(tagRect.x + tagRect.width <= glyphRect.x, "EMERGENCY before the glyph");
            compare(glyphRect.x + glyphRect.width + 8, timeRect.x, "the glyph sits just before the time");
            if (tight.narrow) {
                // Below the meta line, starting where the labels start.
                compare(tagRect.x, nameRect.x, "the third line starts with the labels");
                verify(tagRect.y >= metaRect.y + metaRect.height, "the third line is below the meta line");
                compare(name.width, tight.width - 2 * 18, "the name takes the full width");
            }
        }

        // The form is the view's, not the call's: every row at a width takes
        // the same form and so the same height, whatever tags it carries, and
        // the right side's line is reserved even on a row with no tag.
        function test_narrow_form_is_the_views_data() {
            var cases = [];
            for (var scale of [1.0, 1.6, 2.0]) {
                cases.push({tag: scale.toFixed(1) + " default", scale: scale, compact: false});
                cases.push({tag: scale.toFixed(1) + " compact", scale: scale, compact: true});
            }
            return cases;
        }
        function test_narrow_form_is_the_views(data) {
            Ui.Theme.fontScale = data.scale;
            setCompact(data.compact);
            waitForRendering(pane);
            compare(tight.narrow, Ui.LineBoxes.callRowNarrow(tight.width, tight.timeProbe));
            for (var row of tc.tightView) {
                compare(row.narrow, tight.narrow, row.name + " took another form");
                compare(row.height, tight.height, "a row with other tags has another height");
                compare(row.height, Ui.LineBoxes.callRowHeight(data.compact, tight.narrow));
            }
            // The encrypted row's ENC tag keeps the place of the time.
            var enc = findChild(tightEnc, "rowEncTag");
            var glyph = findChild(tightEnc, "rowRecordingGlyph");
            var encRect = rectOf(enc, tightEnc);
            verify(inside(encRect, tightEnc));
            compare(rectOf(glyph, tightEnc).x + glyph.width + 8, encRect.x, "the glyph sits just before ENC");
            for (var line of ["callRowName", "callRowMeta"])
                verify(!overlaps(encRect, rectOf(findChild(tightEnc, line), tightEnc)), "ENC covers " + line);
        }

        // A wide row is unchanged: the right side beside the lines.
        function test_wide_form_keeps_the_right_side_beside_the_lines() {
            Ui.Theme.fontScale = 1.0;
            waitForRendering(scripts);
            verify(!tagged.narrow);
            compare(tagged.height, latin.height);
            var name = rectOf(findChild(tagged, "callRowName"), tagged);
            var tag = rectOf(findChild(tagged, "rowEmergencyTag"), tagged);
            var time = rectOf(findChild(tagged, "callRowTime"), tagged);
            verify(name.x + name.width <= tag.x, "the name runs into the tags");
            compare(time.x + time.width, tagged.width - 18, "the time ends at the row's margin");
            verify(Math.abs(tag.y + tag.height / 2 - tagged.height / 2) <= 1, "the tags are centred on the row");
        }

        // What a screen reader hears is the same in either form.
        function test_accessible_text_is_the_same_in_either_form() {
            Ui.Theme.fontScale = 1.0;
            scripts.width = 420;
            waitForRendering(scripts);
            verify(!tagged.narrow);
            var name = tagged.node.Accessible.name;
            var description = tagged.node.Accessible.description;
            verify(name.length > 0 && description.length > 0);
            scripts.width = 280;
            waitForRendering(scripts);
            verify(tagged.narrow, "280 px at 1x is the narrow form");
            compare(tagged.node.Accessible.name, name);
            compare(tagged.node.Accessible.description, description);
        }

        // D6 and D11 on Home's Recent pane: its rows follow the store the
        // screen was handed, and a change of density puts the list back at
        // the newest call, shown or hidden.
        function test_home_recent_compact_rows() {
            callHistory.pushMany(40, "TODAY");
            home.active = true;
            home.item.prefsStore = homeStore;
            waitForRendering(home.item);
            var list = findChild(home.item, "homeRecentList");
            verify(list !== null && list.visible);
            verify(prefs.compactView === false, "the app's own preference is off");
            tryVerify(function () { return list.itemAtIndex(0) !== null; });
            var row = list.itemAtIndex(0);
            verify(row.compact, "the row follows the store handed in");
            compare(row.height, Ui.LineBoxes.callRowHeight(true, row.narrow));
            compare(row.narrow, Ui.LineBoxes.callRowNarrow(list.width, "00:00"));
            homeStore.compactView = false;
            waitForRendering(list);
            row = list.itemAtIndex(0);
            verify(!row.compact);
            compare(row.height, Ui.LineBoxes.callRowHeight(false, row.narrow));

            // Shown: a change puts the list back at its start.
            list.contentY = list.originY + 400;
            waitForRendering(list);
            homeStore.compactView = true;
            tryVerify(function () { return Math.abs(list.contentY - list.originY) < 1; }, 1000, "back at the newest call");
            // Hidden: it comes back at its start, not at the place it held.
            list.contentY = list.originY + 400;
            waitForRendering(list);
            home.item.visible = false;
            homeStore.compactView = false;
            waitForRendering(home);
            home.item.visible = true;
            waitForRendering(list);
            tryVerify(function () { return Math.abs(list.contentY - list.originY) < 1; }, 1000, "shown again at the newest call");
        }
    }
}
