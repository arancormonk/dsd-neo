// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// Issue #901: the shared call row's height comes from its fonts and the view's
// width, never from what a call says. A name in another script came from a
// fallback face and made its row 59 or 65 px high beside a 64-px Latin row
// (B5), and a recorded emergency call in Home's 280-px Recent pane left its
// name 6 px wide at 2x text (B7): there the name keeps its place and the
// extras at the right give way, in priority order, while the row keeps its
// height. Compact view (D5) tightens the padding; the row keeps its two lines
// and its right side.
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
        Ui.CallRow {
            id: tightAll
            width: pane.width
            name: "Fire Dispatch North"
            metaText: "TG 4001 · SRC 7001"
            rightText: "09:41"
            emergency: true
            enc: true
            hasRecording: true
        }
        Ui.CallRow {
            id: tightShort
            width: pane.width
            name: "Fire"
            metaText: "TG 4001"
            rightText: "09:41"
            emergency: true
            hasRecording: true
        }
    }

    TextMetrics {
        id: naturalName
    }

    // A Latin line alone in the row's two fonts: what the base face draws,
    // without the taller fallback faces the line boxes also hold.
    Text {
        id: latinNameLine
        visible: false
        text: "Ag"
        font: Ui.LineBoxes.callRowNameFont
    }
    Text {
        id: latinMetaLine
        visible: false
        text: "Ag"
        font: Ui.LineBoxes.callRowMetaFont
    }

    // A Monitor row, whose time is an age that ticks (Util.shortAge).
    Ui.CallRow {
        id: ageRow
        x: 740
        width: 322
        name: "Fire Dispatch North"
        metaText: "TG 4001 · SRC 7001"
        rightText: "now"
        emergency: true
        hasRecording: true
    }

    // Words in a caller's own text that look like the row's fields (r2 C2):
    // an age "now" inside a name, "encrypted" inside a system's name, and a
    // History-like row whose caller says its meta line and name already carry
    // EMERGENCY, the encryption and the time.
    Column {
        id: speech

        x: 740
        y: 100
        width: 420

        Ui.CallRow {
            id: snowPatrol
            width: speech.width
            name: "Snow Patrol"
            metaText: "TG 4001 · SRC 7001 · encrypted"
            rightText: "now"
            enc: true
            metaSaysEnc: true
        }
        Ui.CallRow {
            id: unencrypted
            width: speech.width
            name: "Fire Dispatch"
            metaText: "Unencrypted Ops"
            rightText: "09:41"
            enc: true
            emergency: true
        }
        Ui.CallRow {
            id: emergencyName
            width: speech.width
            name: "EMERGENCY Net"
            metaText: "TG 4001"
            rightText: "09:41"
            emergency: true
        }
        Ui.CallRow {
            id: historyLike
            width: speech.width
            name: "Fire Dispatch"
            accessibleName: "Fire Dispatch, 09:41"
            metaText: "EMERGENCY · TG 4001 · encrypted"
            rightText: "09:41"
            enc: true
            emergency: true
            metaSaysEmergency: true
            metaSaysEnc: true
            nameSaysTime: true
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
        readonly property var tightView: [tight, tightPlain, tightEnc, tightAll, tightShort]

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
            pane.width = 280;
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
            compare(latin.height, Ui.LineBoxes.callRowHeight(data.compact), "the row and the shared measure disagree");
            for (row of tc.sameView) {
                compare(findChild(row, "callRowName").height, Ui.LineBoxes.callRowNameBox(), "the name's box: " + row.name);
                compare(findChild(row, "callRowMeta").height, Ui.LineBoxes.callRowMetaBox(), "the meta line's box: " + row.name);
            }
        }

        // D5: a compact row halves the default row's padding (6 px above and
        // below its lines against 12) and is never under a touch target; a
        // default row is never under 58. Both are their two line boxes, as
        // drawn, plus that padding (issue #901 r3 S2: never one host's pixels).
        // Where the boxes are the base face's lines (within 2 px; a fallback
        // face for the boxes' CJK and emoji probe draws no taller), that is
        // about 52 px at 1x against about 64; a taller fallback face (Noto
        // CJK: 68 and 56) makes those two numbers impossible, never the rule.
        function test_compact_height_data() {
            return [{tag: "1.0", scale: 1.0}, {tag: "1.3", scale: 1.3}, {tag: "1.6", scale: 1.6}, {tag: "2.0", scale: 2.0}];
        }
        function test_compact_height(data) {
            Ui.Theme.fontScale = data.scale;
            waitForRendering(scripts);
            var normal = latin.height;
            var lines = findChild(latin, "callRowName").height + 3 + findChild(latin, "callRowMeta").height;
            verify(normal >= Ui.Theme.rowHeight, "a default row under " + Ui.Theme.rowHeight + ": " + normal);
            compare(normal, Math.max(Ui.Theme.rowHeight, lines + 2 * 12), "a default row: its lines and 12 px each side");
            setCompact(true);
            waitForRendering(scripts);
            var lines2 = findChild(latin, "callRowName").height + 3 + findChild(latin, "callRowMeta").height;
            compare(lines2, lines, "compact keeps the line boxes");
            verify(latin.height >= Ui.Theme.minimumTouchSize, "a compact row under a touch target: " + latin.height);
            verify(latin.height < normal, "compact is no smaller: " + latin.height + " vs " + normal);
            compare(latin.height, Math.max(Ui.Theme.minimumTouchSize, lines + 2 * 6), "a compact row: its lines and 6 px each side");
            if (data.scale === 1.0) {
                compare(normal - latin.height, 12, "compact is 12 px shorter at 1x");
                var latinLines = latinNameLine.implicitHeight + 3 + latinMetaLine.implicitHeight;
                if (lines - latinLines <= 2) {
                    verify(Math.abs(latin.height - 52) <= 2, "a compact row at 1x: " + latin.height);
                    verify(Math.abs(normal - 64) <= 2, "a default row at 1x: " + normal);
                } else {
                    console.log("a fallback face makes the line boxes " + (lines - latinLines) + " px taller than the base face's lines: rows "
                        + normal + " and " + latin.height + " px");
                }
            }
            // Same lines in either density, and the right side stays.
            var name = findChild(latin, "callRowName");
            var meta = findChild(latin, "callRowMeta");
            var time = findChild(latin, "callRowTime");
            verify(name.visible && meta.visible && time.visible);
            verify(inside(rectOf(name, latin), latin) && inside(rectOf(meta, latin), latin) && inside(rectOf(time, latin), latin));
        }

        // A box holds the Latin line a Text draws in that font at any text
        // size. A Text rounds a fractional pixel size (17.6 draws at 18) and
        // Qt.font() truncates it (17), so a box measured with the truncated
        // size was a pixel short at 1.6, and the row's lines were drawn a
        // size smaller than every other Text at that size.
        function test_box_holds_the_drawn_line_data() {
            return [{tag: "1.0", scale: 1.0}, {tag: "1.3", scale: 1.3}, {tag: "1.6", scale: 1.6}, {tag: "2.0", scale: 2.0}];
        }
        function test_box_holds_the_drawn_line(data) {
            Ui.Theme.fontScale = data.scale;
            var line = Qt.createQmlObject("import QtQuick; Text { visible: false; textFormat: Text.PlainText; text: \"Ag\" }", root);
            try {
                for (var size of [11, 12, 14, 16, 20]) {
                    line.font.family = Ui.Theme.mono;
                    line.font.weight = Font.Normal;
                    line.font.pixelSize = Ui.Theme.fontSize(size);
                    var box = Ui.LineBoxes.lineHeight(Ui.Theme.mono, Ui.Theme.fontSize(size), Font.Normal);
                    verify(box >= Math.ceil(line.implicitHeight), size + " px mono: box " + box + " under the line's " + line.implicitHeight);
                    line.font.family = Ui.Theme.sans;
                    line.font.weight = Font.DemiBold;
                    box = Ui.LineBoxes.lineHeight(Ui.Theme.sans, Ui.Theme.fontSize(size), Font.DemiBold);
                    verify(box >= Math.ceil(line.implicitHeight), size + " px sans: box " + box + " under the line's " + line.implicitHeight);
                }
                line.font.pixelSize = Ui.Theme.fontSize(16);
                compare(Ui.LineBoxes.callRowNameFont.pixelSize, line.font.pixelSize, "the row's name is drawn at another size");
                line.font.pixelSize = Ui.Theme.fontSize(12);
                compare(Ui.LineBoxes.callRowMetaFont.pixelSize, line.font.pixelSize, "the row's meta line is drawn at another size");
            } finally {
                line.destroy();
            }
        }

        // The probe cache keeps only the current font revision's probes: the app bumps the revision on every font
        // refresh, also back to a setting it had, so a cache keyed by revision alone grew with each refresh.
        function test_probe_cache_keeps_one_revision() {
            var start = decoderHost.fontRevision;
            var read = function () {
                Ui.LineBoxes.callRowNameBox();
                Ui.LineBoxes.callRowMetaBox();
                Ui.LineBoxes.callRowTimeWidth("00:00");
                Ui.LineBoxes.lineHeight(Ui.Theme.mono, Ui.Theme.fontSize(11), Font.Normal);
            };
            var probeCount = function () {
                var made = 0;
                for (var child of Ui.LineBoxes.children)
                    made += child instanceof Text ? 1 : 0;
                return made;
            };
            try {
                decoderHost.fontRevision = start + 1;
                read();
                var once = Object.keys(Ui.LineBoxes.probes).length;
                verify(once >= 4, "the reads made their probes: " + once);
                for (var i = 2; i <= 21; ++i) {
                    decoderHost.fontRevision = start + i;
                    read();
                }
                wait(0);
                var keys = Object.keys(Ui.LineBoxes.probes);
                compare(keys.length, once, "the cache after 20 revisions: " + keys.length);
                for (var key of keys)
                    compare(key.split("|")[0], String(start + 21), "a probe of an older revision is kept: " + key);
                verify(probeCount() <= once, "older probes are not destroyed: " + probeCount() + " probes");
                compare(Ui.LineBoxes.callRowNameBox(), findChild(latin, "callRowName").height, "the row reads the current box");
            } finally {
                decoderHost.fontRevision = start;
            }
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

        // What a row's right side can carry, in the order it is drawn in
        // (issue #901 B7): each with whether the call has it.
        function rightParts(row) {
            return [{name: "rowEmergencyTag", on: row.emergency},
                    {name: "rowEncTag", on: row.enc},
                    {name: "callRowTime", on: !row.enc && row.rightText.length > 0},
                    {name: "rowRecordingGlyph", on: row.hasRecording}];
        }
        // The right side's width with the items in @p drawn (objectNames), by
        // the row's spacing: ENC or the time at its right, the glyph 8 px
        // before that, EMERGENCY 8 px before the glyph or 6 before ENC or the
        // time.
        function rightWidthOf(row, drawn) {
            // The time counts as its slot, the view's widest time.
            var w = function (n) {
                var item = findChild(row, n);
                return n === "callRowTime" ? item.width : item.implicitWidth;
            };
            var has = function (n) {
                return drawn.indexOf(n) >= 0;
            };
            var endDrawn = has("rowEncTag") || has("callRowTime");
            var end = has("rowEncTag") ? w("rowEncTag") : has("callRowTime") ? w("callRowTime") : 0;
            var glyph = has("rowRecordingGlyph") ? w("rowRecordingGlyph") + (endDrawn ? 8 : 0) : 0;
            var tag = has("rowEmergencyTag") ? w("rowEmergencyTag") + (has("rowRecordingGlyph") ? 8 : endDrawn ? 6 : 0) : 0;
            return end + glyph + tag;
        }
        // B7 on one row: the name keeps min(its natural width, 96); the
        // right side's items are drawn in priority order, each only while it
        // fits beside that minimum, the first that does not ending the run;
        // what is drawn is inside the row, clear of the lines and of each
        // other, in its place left to right; the height is the shared one; and
        // the accessible text keeps every word, drawn or not.
        function checkPlan(row, at) {
            var name = findChild(row, "callRowName");
            var meta = findChild(row, "callRowMeta");
            naturalName.font = name.font;
            naturalName.text = name.text;
            var least = Math.min(Math.ceil(naturalName.advanceWidth), 96);
            verify(name.width >= least - 0.5, at + ": the name is " + name.width.toFixed(1) + " px wide, under " + least);
            compare(meta.width, name.width, at + ": the meta line is as wide as the name");
            compare(row.height, Ui.LineBoxes.callRowHeight(row.compact), at + ": the row's height");
            var nameRect = rectOf(name, row);
            var metaRect = rectOf(meta, row);
            verify(inside(nameRect, row) && inside(metaRect, row), at + ": the lines leave the row");
            var room = Math.max(0, row.width - 18 - 12 - 18);
            var drawn = [];
            var dropped = null;
            for (var part of rightParts(row)) {
                var item = findChild(row, part.name);
                verify(item !== null, part.name);
                if (!part.on) {
                    verify(!item.visible, at + ": " + part.name + " drawn for a call without it");
                    continue;
                }
                if (dropped !== null) {
                    verify(!item.visible, at + ": " + part.name + " drawn while " + dropped + " is not");
                    continue;
                }
                if (!item.visible) {
                    dropped = part.name;
                    // It could not have been drawn beside the name's minimum.
                    var need = least + rightWidthOf(row, drawn.concat([part.name]));
                    verify(need > room, at + ": " + part.name + " left out, though it needs " + need.toFixed(1) + " of " + room);
                    continue;
                }
                drawn.push(part.name);
                var r = rectOf(item, row);
                verify(item.width > 0, at + ": " + part.name + " drawn with no width");
                verify(inside(r, row), at + ": " + part.name + " leaves the row: " + JSON.stringify(r) + " in " + row.width + "x" + row.height);
                verify(!overlaps(r, nameRect), at + ": " + part.name + " covers the name");
                verify(!overlaps(r, metaRect), at + ": " + part.name + " covers the meta line");
                verify(r.x >= nameRect.x + nameRect.width + 12 - 0.5, at + ": " + part.name + " within 12 px of the lines");
                verify(r.x + r.width <= row.width - 18 + 0.5, at + ": " + part.name + " past the row's margin");
            }
            // The lines take exactly the room the drawn items leave.
            verify(Math.abs(name.width - Math.max(0, room - rightWidthOf(row, drawn))) < 0.5,
                   at + ": the name is " + name.width.toFixed(1) + " px, the room left " + (room - rightWidthOf(row, drawn)).toFixed(1));
            // Left to right: EMERGENCY, the glyph, then ENC or the time, apart.
            var order = ["rowEmergencyTag", "rowRecordingGlyph", "rowEncTag", "callRowTime"].filter(function (n) {
                return drawn.indexOf(n) >= 0;
            });
            for (var i = 0; i + 1 < order.length; ++i) {
                var a = rectOf(findChild(row, order[i]), row);
                var b = rectOf(findChild(row, order[i + 1]), row);
                verify(a.x + a.width + 6 - 0.5 <= b.x, at + ": " + order[i] + " runs into " + order[i + 1]);
            }
            // Every word, drawn or not.
            var spoken = row.node.Accessible.name + " | " + row.node.Accessible.description;
            verify(spoken.indexOf(row.name) >= 0, at + ": " + spoken);
            verify(spoken.indexOf(row.metaText) >= 0, at + ": " + spoken);
            if (row.emergency)
                verify(spoken.indexOf("EMERGENCY") >= 0, at + ": EMERGENCY unsaid: " + spoken);
            if (row.enc)
                verify(spoken.indexOf("ENC") >= 0, at + ": ENC unsaid: " + spoken);
            // The time is said on every row, ENC's included, drawn or not.
            verify(spoken.indexOf(row.rightText) >= 0, at + ": the time unsaid: " + spoken);
            if (row.hasRecording)
                verify(row.node.Accessible.name.indexOf("has a recording") >= 0, at + ": the recording unsaid: " + spoken);
            return {drawn: drawn, dropped: dropped};
        }

        function test_b7_extras_give_way_data() {
            var cases = [];
            for (var scale of [1.0, 1.6, 2.0]) {
                cases.push({tag: scale.toFixed(1) + " default", scale: scale, compact: false});
                cases.push({tag: scale.toFixed(1) + " compact", scale: scale, compact: true});
            }
            return cases;
        }
        // B7: in Home's 280-px pane a recorded emergency call keeps its name at
        // least min(its natural width, 96) wide; the tags, the time and the
        // glyph give way in priority order instead, and the row keeps the
        // height every row of the view has. Encrypted, emergency and short
        // names follow the same rule.
        function test_b7_extras_give_way(data) {
            Ui.Theme.fontScale = data.scale;
            setCompact(data.compact);
            waitForRendering(pane);
            for (var row of tc.tightView) {
                var plan = checkPlan(row, data.tag + " " + row.name + (row.enc ? " ENC" : "") + (row.emergency ? " EMERGENCY" : ""));
                console.log("B7", data.tag, row.name, row.emergency, row.enc, row.hasRecording, "drawn", plan.drawn.join(","), "dropped", plan.dropped);
            }
            // EMERGENCY leads: at 280 px it is drawn at every text size.
            verify(findChild(tight, "rowEmergencyTag").visible, "EMERGENCY gave way");
            // A short name leaves the extras more room than a long one.
            var count = function (row) {
                return checkPlan(row, "count").drawn.length;
            };
            verify(count(tightShort) >= count(tight), "a short name drew less");
        }

        // A Monitor row's age ticks ("now", "9m", "10m", ..., "999d", "3y", "99y"): what the row draws never follows it. The time
        // has the slot of the view's widest age, so the plan and the drawn set are the same for every age at every
        // width, here 220 to 460 px (322 px is a 360 dp phone's Monitor row), where the age's own width once made the
        // time and the glyph come and go in a band 30 to 46 px wide.
        function test_age_ticks_never_change_the_plan_data() {
            return [{tag: "1.0", scale: 1.0}, {tag: "1.6", scale: 1.6}, {tag: "2.0", scale: 2.0}];
        }
        function test_age_ticks_never_change_the_plan(data) {
            Ui.Theme.fontScale = data.scale;
            // The Monitor's ages, one per line (MonitorScreen.rowTimeProbe).
            if (ageRow.timeWidthProbe !== undefined)
                ageRow.timeWidthProbe = [qsTr("now"), "59m", "23h", "999d", "99y"].join("\n");
            var ages = ["now", "1m", "9m", "10m", "59m", "1h", "23h", "1d", "999d", "3y", "99y"];
            var drawnNow = function () {
                return JSON.stringify({plan: ageRow.plan, tag: findChild(ageRow, "rowEmergencyTag").visible,
                    time: findChild(ageRow, "callRowTime").visible, glyph: findChild(ageRow, "rowRecordingGlyph").visible,
                    name: Math.round(findChild(ageRow, "callRowName").width)});
            };
            var changed = [];
            var timeDrawn = 0;
            try {
                for (var w = 220; w <= 460; w += 2) {
                    ageRow.width = w;
                    ageRow.rightText = ages[0];
                    var first = drawnNow();
                    if (findChild(ageRow, "callRowTime").visible)
                        ++timeDrawn;
                    for (var age of ages.slice(1)) {
                        ageRow.rightText = age;
                        var now = drawnNow();
                        if (now !== first)
                            changed.push(w + " px \"" + age + "\": " + now + " vs \"now\": " + first);
                    }
                }
                compare(changed.length, 0, "the drawn set follows the age: " + changed.slice(0, 3).join("; "));
                verify(timeDrawn > 0, "the time is never drawn");
                // Where it is drawn, the age sits right-aligned in its slot, ending at the row's margin.
                ageRow.width = 460;
                ageRow.rightText = "9m";
                waitForRendering(ageRow);
                var time = findChild(ageRow, "callRowTime");
                verify(time.visible);
                compare(rectOf(time, ageRow).x + time.width, ageRow.width - 18);
                verify(time.contentWidth <= time.width + 0.5, "the age is wider than its slot");
                checkPlan(ageRow, "460 px \"9m\"");
                // The widest forms ("999d", "99y") are drawn whole inside the slot, clear of EMERGENCY.
                for (var widest of ["999d", "99y"]) {
                    ageRow.rightText = widest;
                    waitForRendering(ageRow);
                    verify(time.visible, widest + " is drawn at 460 px");
                    var slot = rectOf(time, ageRow);
                    verify(time.contentWidth <= time.width + 0.5, widest + " is " + time.contentWidth + " px in a " + time.width + " px slot");
                    var tag = findChild(ageRow, "rowEmergencyTag");
                    verify(tag.visible);
                    var tagRect = rectOf(tag, ageRow);
                    verify(tagRect.x + tagRect.width + 6 - 0.5 <= slot.x, "EMERGENCY runs into the age's slot");
                    verify(tagRect.x + tagRect.width < slot.x + slot.width - time.contentWidth, "EMERGENCY covers " + widest);
                    checkPlan(ageRow, "460 px " + widest);
                }
            } finally {
                ageRow.width = 322;
                ageRow.rightText = "now";
            }
        }

        // Row heights are the density's and the fonts', never the width's or
        // the call's: at every width, every row in either density is the
        // shared height, and a row that drops its extras keeps it.
        function test_height_follows_neither_width_nor_call_data() {
            return test_b7_extras_give_way_data();
        }
        function test_height_follows_neither_width_nor_call(data) {
            Ui.Theme.fontScale = data.scale;
            setCompact(data.compact);
            var expected = Ui.LineBoxes.callRowHeight(data.compact);
            try {
                for (var w of [200, 240, 280, 322, 384, 420, 640]) {
                    pane.width = w;
                    waitForRendering(pane);
                    for (var row of tc.tightView) {
                        compare(row.height, expected, w + " px: " + row.name);
                        checkPlan(row, w + " px " + row.name);
                    }
                }
            } finally {
                pane.width = 280;
            }
        }

        // The ENC tag keeps the place of the time, as on main, with the glyph
        // 8 px before it where both are drawn; the time it stands in for is
        // still said (issue #901 G3: an encrypted Monitor row's age was neither
        // drawn nor said).
        function test_enc_keeps_the_place_of_the_time() {
            Ui.Theme.fontScale = 1.0;
            pane.width = 420;
            try {
                waitForRendering(pane);
                var enc = findChild(tightEnc, "rowEncTag");
                var glyph = findChild(tightEnc, "rowRecordingGlyph");
                verify(enc.visible && glyph.visible && !findChild(tightEnc, "callRowTime").visible);
                var encRect = rectOf(enc, tightEnc);
                compare(encRect.x + encRect.width, tightEnc.width - 18, "ENC ends at the row's margin");
                compare(rectOf(glyph, tightEnc).x + glyph.width + 8, encRect.x, "the glyph sits just before ENC");
                compare(tightEnc.node.Accessible.name, "Fire Dispatch North, has a recording");
                compare(tightEnc.node.Accessible.description, "TG 4001 · SRC 7001, ENC, 09:41");
            } finally {
                pane.width = 280;
            }
        }

        // Issue #901 r2 C2: which fields the row's name and meta line already say comes from the caller's flags,
        // never from searching their text. An encrypted call named "Snow Patrol" aged "now" said no age ("now" is in
        // "Snow"); a Home system named "Unencrypted Ops" kept an encrypted row from saying ENC; a name with EMERGENCY
        // in it kept the tag unsaid. A caller that does put a field in its text says so, and it is said once.
        function test_speech_follows_the_callers_flags() {
            waitForRendering(speech);
            compare(snowPatrol.node.Accessible.name, "Snow Patrol");
            compare(snowPatrol.node.Accessible.description, "TG 4001 · SRC 7001 · encrypted, now");
            compare(unencrypted.node.Accessible.description, "Unencrypted Ops, EMERGENCY, ENC, 09:41");
            compare(emergencyName.node.Accessible.description, "TG 4001, EMERGENCY, 09:41");
            compare(historyLike.node.Accessible.name, "Fire Dispatch, 09:41");
            compare(historyLike.node.Accessible.description, "EMERGENCY · TG 4001 · encrypted");
        }

        // A wide row is unchanged: the right side beside the lines.
        function test_wide_form_keeps_the_right_side_beside_the_lines() {
            Ui.Theme.fontScale = 1.0;
            waitForRendering(scripts);
            for (var part of ["rowEmergencyTag", "rowRecordingGlyph", "callRowTime"])
                verify(findChild(tagged, part).visible, part + " gave way at 420 px");
            compare(tagged.height, latin.height);
            var name = rectOf(findChild(tagged, "callRowName"), tagged);
            var tag = rectOf(findChild(tagged, "rowEmergencyTag"), tagged);
            var time = rectOf(findChild(tagged, "callRowTime"), tagged);
            verify(name.x + name.width <= tag.x, "the name runs into the tags");
            compare(time.x + time.width, tagged.width - 18, "the time ends at the row's margin");
            verify(Math.abs(tag.y + tag.height / 2 - tagged.height / 2) <= 1, "the tags are centred on the row");
        }

        // What a screen reader hears is the same whatever the row draws: at
        // 420 px everything is drawn, at 280 px at 2x text the extras give way.
        function test_accessible_text_is_the_same_whatever_is_drawn() {
            Ui.Theme.fontScale = 1.0;
            scripts.width = 420;
            waitForRendering(scripts);
            verify(findChild(tagged, "rowRecordingGlyph").visible && findChild(tagged, "callRowTime").visible);
            var name = tagged.node.Accessible.name;
            var description = tagged.node.Accessible.description;
            verify(name.length > 0 && description.length > 0);
            compare(name, "Fire Dispatch North, has a recording");
            compare(description, "TG 4001 · SRC 7001, EMERGENCY, 09:41");
            scripts.width = 280;
            Ui.Theme.fontScale = 2.0;
            waitForRendering(scripts);
            verify(!findChild(tagged, "rowRecordingGlyph").visible, "the glyph is drawn at 280 px at 2x");
            compare(tagged.node.Accessible.name, name);
            compare(tagged.node.Accessible.description, description);
        }

        // Issue #901 r2 C2: Home's Recent rows show the system's name as their meta line, which the row never
        // searches: a system named "Unencrypted Ops" no longer keeps an encrypted call from saying ENC.
        function test_home_recent_says_enc_beside_any_system_name() {
            callHistory.push("TODAY");
            verify(callHistory.nameNewest("Fire Dispatch", "Unencrypted Ops", true, false));
            home.active = true;
            home.item.prefsStore = homeStore;
            waitForRendering(home.item);
            var list = findChild(home.item, "homeRecentList");
            var row = null;
            tryVerify(function () { row = list.itemAtIndex(0); return row !== null && row.name === "Fire Dispatch"; });
            compare(row.metaText, "Unencrypted Ops");
            compare(row.node.Accessible.description, "Unencrypted Ops, ENC, " + row.rightText);
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
            compare(row.height, Ui.LineBoxes.callRowHeight(true));
            homeStore.compactView = false;
            waitForRendering(list);
            row = list.itemAtIndex(0);
            verify(!row.compact);
            compare(row.height, Ui.LineBoxes.callRowHeight(false));

            // The list at its start as drawn, not only as its contentY says:
            // the newest row at the top of the viewport. A view placed before
            // its rows took their new height stops at the old layout's start.
            var drawnAtStart = function (at) {
                verify(Math.abs(list.contentY - list.originY) < 1, at + ": contentY " + list.contentY + " vs " + list.originY);
                var first = list.itemAtIndex(0);
                verify(first !== null, at + ": row 0 is not built");
                var y = first.mapToItem(list, 0, 0).y;
                verify(Math.abs(y) < 1, at + ": row 0 starts at " + y);
            };

            // Shown: a change puts the list back at its start, from near the top
            // and from deep, where the first rows are no longer built; the focus
            // stays where it was.
            var focusHolder = Qt.createQmlObject("import QtQuick; Item { activeFocusOnTab: true }", root);
            try {
                for (var deep of [false, true, true]) {
                    if (deep)
                        list.positionViewAtIndex(39, ListView.Beginning);
                    else
                        list.contentY = list.originY + 400;
                    waitForRendering(list);
                    verify(Math.abs(list.contentY - list.originY) > 100, "scrolled down");
                    focusHolder.forceActiveFocus();
                    var focused = root.Window.window.activeFocusItem;
                    verify(focused === focusHolder);
                    homeStore.compactView = !homeStore.compactView;
                    tryVerify(function () { return Math.abs(list.contentY - list.originY) < 1; }, 1000, "back at the newest call");
                    waitForRendering(list);
                    drawnAtStart(deep ? "from deep" : "from near");
                    compare(root.Window.window.activeFocusItem, focused, "the change moved the focus");
                }
            } finally {
                focusHolder.destroy();
            }
            // Hidden: it comes back at its start, not at the place it held.
            list.positionViewAtIndex(39, ListView.Beginning);
            waitForRendering(list);
            home.item.visible = false;
            homeStore.compactView = !homeStore.compactView;
            waitForRendering(home);
            home.item.visible = true;
            waitForRendering(list);
            tryVerify(function () { return Math.abs(list.contentY - list.originY) < 1; }, 1000, "shown again at the newest call");
            drawnAtStart("hidden");
        }
    }
}
