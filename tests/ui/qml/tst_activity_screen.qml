// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// The Activity screen over the production model (testContext.pushUnitActivity()
// feeds it as a redraw tick does): History's row grammar, the filter pills with
// their checked state, the empty states, a merge updating its row in place, the
// one gap row, names drawn as written, the hidden screen holding no rows, the
// reader's place kept as rows arrive, and the layout at a large text size.
// IDs and names are made up.
Item {
    id: root

    width: 411
    height: 800

    Loader {
        id: screenLoader
        anchors.fill: parent
        source: uiDir + "/ActivityScreen.qml"
    }

    TestCase {
        id: tc
        name: "ActivityScreen"
        when: windowShown

        readonly property var screen: screenLoader.item

        function item(name, within) {
            var found = findChild(within || screen, name);
            verify(found !== null, name + " is missing");
            return found;
        }
        function list() {
            return item("activityList");
        }
        // The delegate showing the row whose seq is @p seq, or null.
        function rowItem(seq) {
            var view = list();
            var at = activityView.rowForSeq(seq);
            return at < 0 ? null : view.itemAtIndex(at);
        }
        function shown(row, name) {
            return findChild(row, name);
        }

        function init() {
            testContext.clearUnitActivity();
            root.width = 411;
            Ui.Theme.fontScale = 1.0;
            screen.visible = true;
            waitForRendering(screen);
        }
        function cleanup() {
            testContext.clearUnitActivity();
            Ui.Theme.fontScale = 1.0;
            root.width = 411;
            screen.visible = true;
        }

        // Nothing heard yet: the screen says what shows here and what a listener
        // never hears; "All" is the checked pill.
        function test_empty_state_explains() {
            compare(unitActivity.count, 0);
            var empty = item("activityEmptyState");
            verify(empty.visible);
            compare(item("activityEmptyTitle").text, "No activity yet");
            verify(item("activityEmptyDetail").text.indexOf("control channel") >= 0);
            var limits = item("activityEmptyLimits");
            verify(limits.visible);
            verify(limits.text.indexOf("voice channel") >= 0);
            verify(limits.text.indexOf("logging off") >= 0);
            verify(limits.text.indexOf("carried out a command") >= 0);
            verify(!item("activityShowAll").visible);
            var labels = ["All", "Logins", "Talkgroups", "Refused & queued", "Radio commands", "Other"];
            for (var i = 0; i < labels.length; ++i) {
                var pill = item("activityFilter" + i);
                compare(pill.text, labels[i]);
                verify(pill.Accessible.checkable, labels[i] + " says it can be checked");
                compare(pill.Accessible.checked, i === 0, labels[i] + " checked state");
            }
        }

        // Rows arrive newest first in History's grammar: who (the name with its
        // ID), what (the headline with its target, a deny's reason in it), where
        // (another reason, the protocol and system), and the time. Only a kill
        // or inhibit command is in the warning colour.
        function test_rows_read_like_history() {
            verify(testContext.pushUnitActivity({kind: "affiliation", subject: 1234567, group: 100, name: "Alice"}));
            verify(testContext.pushUnitActivity({kind: "deny", subject: 1234568, reason: 0x20}));
            verify(testContext.pushUnitActivity({kind: "kill", subject: 1234569}));
            tryCompare(list(), "count", 3);
            verify(!item("activityEmptyState").visible);
            waitForRendering(list());

            var kill = rowItem(3);
            verify(kill !== null);
            compare(shown(kill, "activityRowWho").text, "1234569");
            compare(shown(kill, "activityRowWhat").text, "Kill sent");
            compare(shown(kill, "activityRowWhat").color, Ui.Theme.alert);
            verify(shown(kill, "activityRowTime").text.length === 8, "HH:mm:ss");
            verify(!shown(kill, "activityRowRepeat").visible);

            var joined = rowItem(1);
            compare(shown(joined, "activityRowWho").text, "Alice (1234567)");
            verify(shown(joined, "activityRowWhat").text.indexOf("TG 100") >= 0, shown(joined, "activityRowWhat").text);
            verify(shown(joined, "activityRowWhat").color !== Ui.Theme.alert);
            compare(shown(joined, "activityRowWhere").text, "P25 Phase 1 · WACN BEE00 SYS 123");
            compare(joined.Accessible.name, "Alice (1234567), " + shown(joined, "activityRowTime").text);
            verify(joined.Accessible.description.indexOf(shown(joined, "activityRowWhat").text) >= 0);

            var denied = rowItem(2);
            var what = shown(denied, "activityRowWhat").text;
            verify(what.indexOf("Denied: ") === 0 && what.indexOf("(0x20)") > 0, what);
            // The reason is said once: in the headline, not again on the third line.
            compare(shown(denied, "activityRowWhere").text.indexOf("0x20"), -1);
            // Rows top to bottom: newest first.
            verify(kill.y < denied.y && denied.y < joined.y);
        }

        // A message heard again merges into its row: the same delegate shows the
        // count, nothing is added, and the row stays where it is.
        function test_merge_updates_its_row_in_place() {
            verify(testContext.pushUnitActivity({kind: "registration", subject: 1234567}));
            verify(testContext.pushUnitActivity({kind: "registration", subject: 7654321}));
            tryCompare(list(), "count", 2);
            tryVerify(function () { return rowItem(1) !== null; });
            var older = rowItem(1);
            var y = older.y;
            verify(testContext.pushUnitActivity({kind: "registration", subject: 1234567}));
            compare(unitActivity.count, 2);
            compare(rowItem(1), older, "the same delegate");
            compare(older.y, y);
            var repeat = shown(older, "activityRowRepeat");
            verify(repeat.visible);
            compare(repeat.text, "×2");
            verify(older.Accessible.description.indexOf("heard 2 times") >= 0, older.Accessible.description);
        }

        // One pill at a time filters the rows and says it is checked; a filter
        // with no rows says they are hidden, and Show all brings them back.
        function test_filters_and_their_empty_state() {
            verify(testContext.pushUnitActivity({kind: "registration", subject: 1}));
            verify(testContext.pushUnitActivity({kind: "affiliation", subject: 2, group: 100}));
            verify(testContext.pushUnitActivity({kind: "inhibit", subject: 3}));
            tryCompare(list(), "count", 3);
            var logins = item("activityFilter1");
            mouseClick(logins);
            compare(activityView.filter, 1);
            verify(logins.Accessible.checked && !item("activityFilter0").Accessible.checked);
            tryCompare(list(), "count", 1);
            mouseClick(item("activityFilter4"));
            compare(activityView.filter, 8);
            tryCompare(list(), "count", 1);
            tryVerify(function () { return list().itemAtIndex(0) !== null; });
            compare(shown(list().itemAtIndex(0), "activityRowWhat").text, "Inhibit sent");
            mouseClick(item("activityFilter3"));
            tryCompare(list(), "count", 0);
            var empty = item("activityEmptyState");
            verify(empty.visible);
            compare(item("activityEmptyTitle").text, "No matching activity");
            compare(item("activityEmptyDetail").text, "3 events are hidden by the filter.");
            verify(!item("activityEmptyLimits").visible);
            var showAll = item("activityShowAll");
            verify(showAll.visible);
            waitForRendering(showAll);
            mouseClick(showAll);
            compare(activityView.filter, 0);
            tryCompare(list(), "count", 3);
            verify(item("activityFilter0").Accessible.checked);
        }

        // Review focus 3: back from the background on a busy site, the list shows
        // the newest rows and one row saying earlier activity was not kept, at
        // the bottom; a name change that reads every row again adds no second one.
        function test_gap_row_once() {
            for (var i = 0; i < 4; ++i)
                verify(testContext.pushUnitActivity({kind: "registration", subject: 10 + i}));
            for (i = 0; i < 1029; ++i)
                verify(testContext.pushUnitActivity({kind: "registration", subject: 100000 + i}, false));
            testContext.readUnitActivity();
            compare(unitActivity.count, 1024);
            var gapAt = activityView.rowForSeq(0);
            compare(gapAt, 1024, "below every row read after it");
            compare(list().count, 1025);
            verify(testContext.learnUnitActivityName(1234567, "Alice"));
            compare(list().count, 1025);
            compare(activityView.rowForSeq(0), gapAt);
            list().positionViewAtEnd();
            waitForRendering(list());
            var gap = list().itemAtIndex(gapAt);
            verify(gap !== null);
            compare(item("activityGapTitle", gap).text, "Earlier activity was not kept");
            compare(gap.Accessible.name, "Earlier activity was not kept");
            compare(gap.Accessible.description, "The log keeps the newest 1,024 events.");
            verify(!item("activityEmptyState").visible);
        }

        // Names come from the air or a list: a tag in one is drawn as written,
        // and a line break as a space.
        function test_names_are_plain_text() {
            verify(testContext.pushUnitActivity({kind: "registration", subject: 42, name: "<b>Car</b>\n7"}));
            tryCompare(list(), "count", 1);
            waitForRendering(list());
            var who = shown(rowItem(1), "activityRowWho");
            compare(who.textFormat, Text.PlainText);
            compare(who.text, "<b>Car</b> 7 (42)");
        }

        // Issue #782: hidden, the list holds no rows while messages land; shown
        // again it lists them all, and a reader at the top is on the newest.
        function test_hidden_screen_holds_no_rows() {
            verify(testContext.pushUnitActivity({kind: "registration", subject: 1}));
            tryCompare(list(), "count", 1);
            screen.visible = false;
            compare(list().count, 0, "a hidden list holds no rows");
            for (var i = 0; i < 5; ++i)
                verify(testContext.pushUnitActivity({kind: "registration", subject: 100 + i}));
            compare(list().count, 0);
            screen.visible = true;
            compare(list().count, 6);
            waitForRendering(list());
            verify(Math.abs(list().contentY - list().originY) < 1, "on the newest row");
            compare(list().indexAt(list().width / 2, list().contentY + 2), 0);
        }

        // A reader further down keeps the row they read as rows arrive above it;
        // one at the top keeps seeing the newest.
        function test_reader_keeps_place_on_arrivals() {
            for (var i = 0; i < 30; ++i)
                verify(testContext.pushUnitActivity({kind: "registration", subject: 1000 + i}));
            tryCompare(list(), "count", 30);
            var view = list();
            view.contentY = view.originY + 600;
            waitForRendering(view);
            var top = view.indexAt(view.width / 2, view.contentY + 2);
            verify(top > 0);
            var seq = view.itemAtIndex(top).rowSeq;
            var offset = view.itemAtIndex(top).y - view.contentY;
            for (i = 0; i < 5; ++i)
                verify(testContext.pushUnitActivity({kind: "registration", subject: 2000 + i}));
            wait(20);
            waitForRendering(view);
            var row = rowItem(seq);
            verify(row !== null);
            verify(Math.abs(row.y - view.contentY - offset) <= 1, "the row read stays put: " + (row.y - view.contentY) + " vs " + offset);
            view.positionViewAtBeginning();
            waitForRendering(view);
            verify(testContext.pushUnitActivity({kind: "registration", subject: 3000}));
            wait(20);
            waitForRendering(view);
            compare(view.indexAt(view.width / 2, view.contentY + 2), 0);
            compare(view.itemAtIndex(0).rowSeq, unitActivity.count);
        }

        // The empty state stays in the list's own area at any width and text
        // size: below the filter pills, never over them, and every line of it
        // reachable, scrolling when it is taller than that area.
        function test_empty_state_fits_data() {
            return [{tag: "320-x1", width: 320, scale: 1.0}, {tag: "320-x2", width: 320, scale: 2.0},
                {tag: "411-x2", width: 411, scale: 2.0}, {tag: "320-x2-filtered", width: 320, scale: 2.0, filtered: true},
                {tag: "411-x1-gap-filtered", width: 411, scale: 1.0, filtered: true, gap: true},
                {tag: "320-x2-gap-filtered", width: 320, scale: 2.0, filtered: true, gap: true}];
        }
        function test_empty_state_fits(data) {
            root.width = data.width;
            Ui.Theme.fontScale = data.scale;
            if (data.filtered) {
                verify(testContext.pushUnitActivity({kind: "registration", subject: 1}));
                // Back from the background on a busy site: the gap row, then a
                // filter none of the kept rows is in. The gap row alone is no
                // activity: the filter's message shows below it.
                if (data.gap) {
                    for (var i = 0; i < 1029; ++i)
                        verify(testContext.pushUnitActivity({kind: "registration", subject: 100000 + i}, false));
                    testContext.readUnitActivity();
                }
                activityView.filter = 8;
            }
            tryCompare(activityView, "count", data.gap ? 1 : 0);
            waitForRendering(screen);
            var scroll = item("activityEmptyScroll");
            verify(scroll.visible);
            if (data.filtered) {
                compare(item("activityEmptyTitle").text, "No matching activity");
                compare(item("activityEmptyDetail").text, data.gap ? "1,024 events are hidden by the filter." : "1 event is hidden by the filter.");
            }
            var header = item("activityHeaderScroll");
            var headerBottom = header.mapToItem(screen, 0, header.height).y;
            if (data.gap) {
                tryVerify(function () { return list().itemAtIndex(0) !== null; });
                var gapRow = list().itemAtIndex(0);
                verify(gapRow.gap);
                headerBottom = gapRow.mapToItem(screen, 0, gapRow.height).y;
            }
            var top = scroll.mapToItem(screen, 0, 0).y;
            verify(top >= headerBottom - 0.5, "the empty state starts below the pills and the gap row: " + top + " vs " + headerBottom);
            verify(top + scroll.height <= screen.height + 0.5, "and ends inside the screen");
            var lines = ["activityEmptyTitle", "activityEmptyDetail"].concat(data.filtered ? ["activityShowAll"] : ["activityEmptyLimits"]);
            for (var name of lines) {
                var line = item(name);
                verify(line.visible, name);
                var p = line.mapToItem(scroll.contentItem, 0, 0);
                verify(p.x >= -0.5 && p.x + line.width <= scroll.width + 0.5, name + " inside the width");
                verify(p.y >= -0.5 && p.y + line.height <= scroll.contentHeight + 0.5, name + " inside the scrolled area");
            }
            if (scroll.contentHeight > scroll.height + 0.5)
                verify(scroll.interactive, "a taller empty state scrolls");
            else
                verify(!scroll.interactive);
            activityView.filter = 0;
        }

        // At 320 px and twice the text size the pills wrap inside the screen and
        // every row's lines stay inside it; the header scrolls rather than
        // pushing the list off the screen.
        function test_large_text_fits() {
            root.width = 320;
            Ui.Theme.fontScale = 2.0;
            verify(testContext.pushUnitActivity({kind: "affiliation", subject: 1234567, group: 100, name: "Engine 7 Driver"}));
            verify(testContext.pushUnitActivity({kind: "deny", subject: 1234568, reason: 0x20}));
            tryCompare(list(), "count", 2);
            waitForRendering(screen);
            for (var i = 0; i < 6; ++i) {
                var pill = item("activityFilter" + i);
                var at = pill.mapToItem(screen, 0, 0);
                verify(at.x >= 0 && at.x + pill.width <= screen.width + 0.5, pill.text + " inside the screen");
            }
            verify(list().height >= screen.height * 0.4 - 1, "the list keeps its share");
            for (var seq of [1, 2]) {
                var row = rowItem(seq);
                verify(row !== null);
                for (var name of ["activityRowWho", "activityRowWhat", "activityRowWhere", "activityRowTime"]) {
                    var line = shown(row, name);
                    var p = line.mapToItem(row, 0, 0);
                    verify(p.x >= 0 && p.x + line.width <= row.width + 0.5, name + " inside the row");
                    verify(p.y >= 0 && p.y + line.height <= row.height + 0.5, name + " inside the row's height");
                }
            }
        }
    }
}
