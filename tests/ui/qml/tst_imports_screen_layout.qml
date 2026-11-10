// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// A ListView positions its own delegates: for a vertical list it writes the
// item's x on every layout pass, which silently overwrites an `x` the delegate
// declared. A card that subtracts the screen padding from its width but loses
// the matching x still measures right — it just sits flush against the left
// edge with the whole inset piled up on the right. Nothing in a C++ model test
// sees that, and on a phone it reads as the whole library being off-centre.
//
// So the assertion is on the rendered geometry: each card's gap to the left
// edge equals its gap to the right, and both equal the inset the Import file
// button below the list already uses.
Item {
    id: root

    width: 420
    height: 900

    // A wrapping pill on its own, for words longer than any kind's.
    Item {
        id: pillBox
        y: 860
        width: 200
        height: 40
        Ui.FilterPill {
            id: longPill
            width: Math.min(implicitWidth, pillBox.width)
            wrapLabel: true
            caret: false
            visible: false
        }
    }

    Loader {
        id: screenLoader

        anchors.fill: parent
        source: uiDir + "/ImportsScreen.qml"
    }

    TestCase {
        id: tc

        name: "ImportsScreenLayout"
        when: windowShown

        property var screen: null
        property var list: null
        property var importButton: null

        function initTestCase() {
            tc.screen = screenLoader.item;
            verify(tc.screen !== null, "ImportsScreen.qml failed to load");
            tc.list = findChild(tc.screen, "importedFilesList");
            verify(tc.list !== null, "the imported-files list is missing");
            tc.importButton = findChild(tc.screen, "importFileButton");
            verify(tc.importButton !== null, "the import button is missing");

            // Two rows, because one card centred by accident on an empty model
            // would prove nothing; the library copies and parses a real file, so
            // the fixture has to be one.
            var groups = testContext.writeFixtureCsv("groups.csv", "id,mode,name\n1001,A,Dispatch\n1002,A,Fireground\n");
            verify(groups.length > 0, "could not write the talkgroup fixture");
            var accepted = testContext.importNow(groups, "group");
            verify(accepted.ok, "the talkgroup fixture did not import");

            var more = testContext.writeFixtureCsv("extra.csv", "id,mode,name\n2001,A,Public Works\n");
            verify(more.length > 0, "could not write the second fixture");
            verify(testContext.importNow(more, "group").ok, "the second fixture did not import");

            tryCompare(tc.list, "count", 2);
            tc.waitForRendering(tc.list);
        }

        function cleanupTestCase() {
            // The library is one model shared by every case in this suite and it
            // persists; leaving rows behind would hand the next file a fixture
            // it never asked for.
            while (importedFiles.count > 0) {
                importedFiles.remove(0);
            }
        }

        // Issue #521: the channel-map review reports each row's own --squelch-db, and a row
        // without one says it inherits.
        function test_channel_review_shows_row_squelch() {
            var map = testContext.writeFixtureCsv("squelch-map.csv",
                "channel,frequency,mode,options\n"
                + "1,851012500,p25,--squelch-db -60\n"
                + "2,852012500,dmr,--squelch-db 0\n"
                + "3,853012500,dmr,\n");
            var result = testContext.importNow(map, "chan");
            verify(result.ok, result.detail || "the channel map did not import");
            var row = importedFiles.rowForPath(result.path);
            try {
                var profiles = importedFiles.channelProfiles(row);
                verify(profiles.ok);
                var sheet = findChild(tc.screen, "channelReviewSheet");
                verify(sheet !== null, "the channel review is missing");
                sheet.rows = profiles.rows;
                sheet.valid = profiles.ok;
                sheet.visible = true;
                var rows = findChild(tc.screen, "channelReviewRows");
                verify(rows !== null);
                tryVerify(function() { return rows.count === 3 && rows.itemAtIndex(2) !== null; });
                verify(rows.itemAtIndex(0).text.indexOf("Squelch: -60 dB") >= 0, rows.itemAtIndex(0).text);
                verify(rows.itemAtIndex(1).text.indexOf("Squelch: off") >= 0, rows.itemAtIndex(1).text);
                verify(rows.itemAtIndex(2).text.indexOf("Squelch: inherit") >= 0, rows.itemAtIndex(2).text);
                sheet.visible = false;
            } finally {
                importedFiles.remove(importedFiles.rowForPath(result.path));
                tryCompare(tc.list, "count", 2);
            }
        }

        // Issue #526: the channel-map review reports an analog row's own width by its kind (an nfm
        // row's --nfm-bandwidth-hz, an am row's --am-bandwidth-hz), an analog row without one says
        // it inherits, and a digital row says nothing about it. Only a digital row lists keys and
        // identifiers: the parser refuses them on an analog row.
        function test_channel_review_shows_row_bandwidth() {
            var map = testContext.writeFixtureCsv("bandwidth-map.csv",
                "channel,frequency,mode,options\n"
                + "1,154430000,nfm,--nfm-bandwidth-hz 12500\n"
                + "2,155100000,nfm,\n"
                + "3,853012500,dmr,\n"
                + "4,118300000,am,--am-bandwidth-hz 8333\n"
                + "5,121500000,am,\n");
            var result = testContext.importNow(map, "chan");
            verify(result.ok, result.detail || "the channel map did not import");
            var row = importedFiles.rowForPath(result.path);
            try {
                var profiles = importedFiles.channelProfiles(row);
                verify(profiles.ok);
                var sheet = findChild(tc.screen, "channelReviewSheet");
                verify(sheet !== null, "the channel review is missing");
                sheet.rows = profiles.rows;
                sheet.valid = profiles.ok;
                sheet.visible = true;
                var rows = findChild(tc.screen, "channelReviewRows");
                verify(rows !== null);
                tryVerify(function() { return rows.count === 5 && rows.itemAtIndex(4) !== null; });
                verify(rows.itemAtIndex(0).text.indexOf("NFM bandwidth: 12.5 kHz") >= 0, rows.itemAtIndex(0).text);
                verify(rows.itemAtIndex(1).text.indexOf("NFM bandwidth: inherit") >= 0, rows.itemAtIndex(1).text);
                verify(rows.itemAtIndex(2).text.indexOf("NFM bandwidth") < 0, rows.itemAtIndex(2).text);
                verify(rows.itemAtIndex(3).text.indexOf("AM bandwidth: 8.333 kHz") >= 0, rows.itemAtIndex(3).text);
                verify(rows.itemAtIndex(4).text.indexOf("AM bandwidth: inherit") >= 0, rows.itemAtIndex(4).text);
                for (var i = 0; i < 5; ++i) {
                    var text = rows.itemAtIndex(i).text;
                    var analog = i !== 2;
                    verify((text.indexOf("Keys:") >= 0) === !analog, text);
                    verify((text.indexOf("Identifier") >= 0) === !analog, text);
                    if (i >= 3)
                        verify(text.indexOf("NFM") < 0, text);
                }
                sheet.visible = false;
            } finally {
                importedFiles.remove(importedFiles.rowForPath(result.path));
                tryCompare(tc.list, "count", 2);
            }
        }

        // Issue #527: the channel-map review reports an nfm row's own tone policy with its list, an
        // nfm row without one says it runs the configured policy, and an am or digital row, which
        // cannot set one, says nothing about it.
        function test_channel_review_shows_row_tone_filter() {
            var map = testContext.writeFixtureCsv("tone-map.csv",
                "channel,frequency,mode,options\n"
                + "1,154430000,nfm,--tone-allow 100.0/D023N\n"
                + "2,155475000,nfm,--tone-block 67.0\n"
                + "3,155520000,nfm,--no-tone-filter\n"
                + "4,155535000,nfm,\n"
                + "5,118300000,am,\n"
                + "6,853012500,dmr,\n");
            var result = testContext.importNow(map, "chan");
            verify(result.ok, result.detail || "the channel map did not import");
            var row = importedFiles.rowForPath(result.path);
            try {
                var profiles = importedFiles.channelProfiles(row);
                verify(profiles.ok);
                var sheet = findChild(tc.screen, "channelReviewSheet");
                verify(sheet !== null, "the channel review is missing");
                sheet.rows = profiles.rows;
                sheet.valid = profiles.ok;
                sheet.visible = true;
                var rows = findChild(tc.screen, "channelReviewRows");
                verify(rows !== null);
                tryVerify(function() { return rows.count === 6 && rows.itemAtIndex(5) !== null; });
                var want = ["Tone filter: allow 100.0 Hz/D023N", "Tone filter: block 67.0 Hz", "Tone filter: off",
                            "Tone filter: inherit"];
                for (var i = 0; i < 4; ++i)
                    verify(rows.itemAtIndex(i).text.indexOf(want[i]) >= 0, rows.itemAtIndex(i).text);
                verify(rows.itemAtIndex(4).text.indexOf("Tone filter") < 0, rows.itemAtIndex(4).text);
                verify(rows.itemAtIndex(5).text.indexOf("Tone filter") < 0, rows.itemAtIndex(5).text);
                sheet.visible = false;
            } finally {
                importedFiles.remove(importedFiles.rowForPath(result.path));
                tryCompare(tc.list, "count", 2);
            }
        }

        function test_cancel_import_cleanup_data() {
            return [{tag: "primary"}, {tag: "sheet"}];
        }
        function test_cancel_import_cleanup(data) {
            tc.screen.pendingRow = 1;
            tc.screen.notice = "Old notice";
            tc.screen.noticeIsProblem = true;
            var flow = findChild(tc.screen, "importsCsvImport");
            verify(flow !== null);
            if (data.tag === "primary") {
                flow.pick("chan", tc.screen.importContext());
                testContext.answerDocumentCancelled(testContext.lastDocumentRequest().id);
            } else {
                var source = testContext.writeFixtureCsv("cancel-consumer.csv",
                    "channel,frequency,mode,keys_dec_csv\n1,851012500,p25,missing.csv\n");
                // A new import, as the kind sheet starts: pendingRow is the stale state under test.
                flow.begin(source, "Map.csv", "chan", {generation: tc.screen.generation});
                tryVerify(function () { return flow.requiredFiles.length > 0; }, 5000, "the companions are asked for");
                findChild(flow, "csvCompanionSheet").requestDismiss();
            }
            tryCompare(flow, "active", false);
            compare(tc.screen.pendingRow, -1);
            compare(tc.screen.notice, "");
            verify(!tc.screen.noticeIsProblem);
        }

        // ---- issue #829: an update replaces the file it was asked for ----

        function groupFile(name, rows) {
            var path = testContext.writeFixtureCsv(name, "id,mode,name\n" + rows);
            verify(path.length > 0);
            return path;
        }
        function addGroup(name) {
            var result = testContext.importNamed(groupFile(name, "3001,A," + name + "\n"), name, "group");
            verify(result.ok, "the fixture " + name + " did not import");
            return result.path;
        }
        function readRows(path) {
            return importedFiles.get(importedFiles.rowForPath(path)).accepted;
        }
        // "Update" from a row's action sheet, as the screen runs it.
        function startUpdate(path) {
            tc.screen.notice = "";
            tc.screen.noticeIsProblem = false;
            tc.screen.pendingRow = importedFiles.rowForPath(path);
            tc.screen.importPickedFile();
            var request = testContext.lastDocumentRequest();
            compare(request.kind, "open");
            return request.id;
        }
        // A row above the one being updated goes while the picker is out:
        // the update still lands on its own file, never the one that moved
        // into its place.
        function test_update_follows_its_file_when_an_earlier_row_goes() {
            var first = addGroup("first.csv");
            var second = addGroup("second.csv");
            var third = addGroup("third.csv");
            try {
                var id = startUpdate(second);
                importedFiles.remove(importedFiles.rowForPath(first));
                testContext.answerDocumentPicked(id, "file://" + groupFile("second-new.csv", "4001,A,One\n4002,A,Two\n"), "second-new.csv", false);
                // #832: the copy answers later, from the import worker.
                tryCompare(findChild(tc.screen, "importsCsvImport"), "active", false);
                compare(readRows(second), 2, "the asked-for file was replaced");
                compare(readRows(third), 1, "the file that moved up is untouched");
                verify(!tc.screen.noticeIsProblem, tc.screen.notice);
            } finally {
                for (var path of [first, second, third]) {
                    if (importedFiles.rowForPath(path) >= 0)
                        importedFiles.remove(importedFiles.rowForPath(path));
                }
                tryCompare(tc.list, "count", 2);
            }
        }
        // The row being updated goes while the picker is out: nothing is
        // replaced, and the notice says so, read out once as an alert.
        function test_update_of_a_removed_file_is_refused() {
            var second = addGroup("second.csv");
            var third = addGroup("third.csv");
            try {
                var said = tc.screen.announcements;
                var id = startUpdate(second);
                importedFiles.remove(importedFiles.rowForPath(second));
                var count = importedFiles.count;
                testContext.answerDocumentPicked(id, "file://" + groupFile("second-new.csv", "4001,A,One\n4002,A,Two\n"), "second-new.csv", false);
                tryCompare(findChild(tc.screen, "importsCsvImport"), "active", false);
                compare(importedFiles.count, count, "nothing was added");
                compare(readRows(third), 1, "the file now in its place is untouched");
                compare(tc.screen.notice, "That file changed while you were picking. Try again.");
                verify(tc.screen.noticeIsProblem);
                compare(tc.screen.pendingRow, -1);
                var line = findChild(tc.screen, "importsNotice");
                verify(line.visible);
                compare(line.Accessible.role, Accessible.AlertMessage);
                compare(tc.screen.announcements, said + 1);
                compare(tc.screen.announced, tc.screen.notice);
            } finally {
                for (var path of [second, third]) {
                    if (importedFiles.rowForPath(path) >= 0)
                        importedFiles.remove(importedFiles.rowForPath(path));
                }
                tryCompare(tc.list, "count", 2);
            }
        }
        // A picker failure shows its reason, read out once per failure.
        function test_picker_failure_is_shown_and_read_out() {
            var said = tc.screen.announcements;
            tc.screen.pendingRow = -1;
            tc.screen.pendingType = "group";
            for (var i = 1; i <= 2; ++i) {
                tc.screen.importPickedFile();
                testContext.answerDocumentUnavailable(testContext.lastDocumentRequest().id, "noResult");
                compare(tc.screen.notice, "No file came back from the picker. Try again.");
                verify(tc.screen.noticeIsProblem);
                compare(tc.screen.announcements, said + i);
            }
            compare(importedFiles.count, 2);
            tc.screen.notice = "";
            compare(findChild(tc.screen, "importsNotice").Accessible.role, Accessible.AlertMessage);
            tc.screen.noticeIsProblem = false;
            compare(findChild(tc.screen, "importsNotice").Accessible.role, Accessible.StaticText);
        }
        // Leaving the screen ends a pick still out; its late answer imports nothing.
        function test_a_pick_ends_when_the_screen_closes() {
            var flow = findChild(tc.screen, "importsCsvImport");
            tc.screen.pendingRow = -1;
            tc.screen.pendingType = "group";
            tc.screen.importPickedFile();
            var id = testContext.lastDocumentRequest().id;
            screenLoader.visible = false;
            try {
                verify(!flow.active);
                testContext.answerDocumentPicked(id, "file://" + groupFile("late-group.csv", "5001,A,Late\n"), "late.csv", false);
                compare(importedFiles.count, 2, "nothing was imported");
            } finally {
                screenLoader.visible = true;
            }
        }

        // The text item under @p item that shows @p text.
        function textItem(item, text) {
            if (item.text === text)
                return item;
            for (var i = 0; i < item.children.length; ++i) {
                var found = textItem(item.children[i], text);
                if (found !== null)
                    return found;
            }
            return null;
        }

        // #830: at the largest text on a narrow phone the title stays inside
        // the header, which grows to hold it, and the back chevron stays
        // level with its first line.
        function test_title_fits_at_large_font_data() {
            return [{tag: "320", width: 320}, {tag: "360", width: 360}];
        }
        function test_title_fits_at_large_font(data) {
            root.width = data.width;
            Ui.Theme.fontScale = 1.6;
            try {
                var title = textItem(tc.screen, "Imported files");
                verify(title !== null, "the title is missing");
                var header = title.parent;
                tc.waitForRendering(header);
                var titleRight = title.mapToItem(tc.screen, title.width, 0).x;
                var headerRight = header.mapToItem(tc.screen, header.width, 0).x;
                verify(titleRight <= headerRight, "the title runs " + (titleRight - headerRight) + " px past the header");
                verify(title.contentWidth <= title.width, "the title's text is wider than the title");
                verify(header.height >= title.height, "the header is " + header.height + " px for a " + title.height + " px title");
                var back = null;
                for (var i = 0; i < header.children.length; ++i) {
                    if (header.children[i].icon === "back")
                        back = header.children[i];
                }
                verify(back !== null, "the back chevron is missing");
                var lineCentre = title.y + title.height / title.lineCount / 2;
                verify(Math.abs(back.y + back.height / 2 - lineCentre) < 1, "the back chevron is not level with the title's first line");
                var count = textItem(tc.screen, "2 files");
                verify(count !== null, "the file count is missing");
                verify(count.mapToItem(tc.screen, 0, 0).y >= title.mapToItem(tc.screen, 0, title.height).y,
                       "the file count overlaps the title");
            } finally {
                Ui.Theme.fontScale = 1;
                root.width = 420;
            }
        }

        // #830: an imported file's row is a button a screen reader can press,
        // and so is Return: each opens the row's actions, and neither does
        // while a sheet covers the screen.
        function test_row_press_action() {
            var actions = findChild(tc.screen, "importsActionSheet");
            verify(actions !== null, "the action sheet is missing");
            var card = tc.list.itemAtIndex(1);
            verify(card !== null, "the second row has no delegate");
            compare(card.Accessible.role, Accessible.Button);
            // Named by its file, described by its kind and summary line.
            compare(card.Accessible.name, importedFiles.get(1).name);
            compare(card.Accessible.description, "TALKGROUPS · 1 talkgroup · " + Qt.formatDate(new Date(Number(importedFiles.get(1).importedAt) * 1000), "MMM d"));
            // The badge, the name and the summary take no stop of their own:
            // the card reads them.
            var badge = findChild(card, "importsKindBadge");
            verify(badge !== null, "the kind badge is missing");
            var badgeText = findChild(badge, "csvTypeBadgeText");
            compare(badgeText.text, "TALKGROUPS");
            verify(badgeText.Accessible.ignored, "the badge is read by the card, not on its own");
            compare(badgeText.Accessible.name, "");
            verify(findChild(card, "importsFileName").Accessible.ignored, "the name is read as the card's name");
            verify(findChild(card, "importsFileSummary").Accessible.ignored, "the summary is read in its description");

            card.Accessible.pressAction();
            verify(actions.visible, "the press opened no actions");
            compare(tc.screen.actionRow, 1);
            actions.visible = false;
            tc.screen.actionRow = -1;

            card.forceActiveFocus();
            keyClick(Qt.Key_Return);
            verify(actions.visible, "Return opened no actions");
            compare(tc.screen.actionRow, 1);
            actions.visible = false;
            tc.screen.actionRow = -1;

            // Under a sheet the row is covered: neither way reaches it.
            card.forceActiveFocus();
            tc.importButton.clicked();
            var kinds = findChild(tc.screen, "importKindPicker");
            var sheet = kinds;
            while (sheet && typeof sheet.requestDismiss !== "function")
                sheet = sheet.parent;
            verify(sheet !== null, "the Import kind sheet is missing");
            try {
                tryVerify(function () { return kinds.visible; });
                verify(!Ui.Navigation.allows(card), "the row is still allowed under the sheet");
                card.Accessible.pressAction();
                // A key still reaching the row, as from focus it kept.
                card.forceActiveFocus();
                keyClick(Qt.Key_Return);
                verify(!actions.visible, "a covered row opened its actions");
                compare(tc.screen.actionRow, -1);
            } finally {
                // Left open it would cover every later case.
                sheet.visible = false;
                actions.visible = false;
                tc.screen.actionRow = -1;
            }
        }

        // #830: Tab reaches the file cards in order: from the screen's last
        // control above the list (the back chevron) to the first card, then
        // to the next one.
        function test_tab_reaches_the_cards_in_order() {
            var first = tc.list.itemAtIndex(0);
            var second = tc.list.itemAtIndex(1);
            verify(first !== null && second !== null, "the rows have no delegates");
            var back = null;
            for (var i = 0; i < tc.screen.children.length && back === null; ++i) {
                var kids = tc.screen.children[i].children;
                for (var k = 0; k < kids.length; ++k) {
                    if (kids[k].icon === "back")
                        back = kids[k];
                }
            }
            verify(back !== null, "the back chevron is missing");
            back.forceActiveFocus();
            verify(back.activeFocus);
            keyClick(Qt.Key_Tab);
            verify(first.activeFocus, "Tab from the back chevron did not reach the first card");
            keyClick(Qt.Key_Tab);
            verify(second.activeFocus, "Tab from the first card did not reach the next one");
        }

        function test_the_cards_sit_centred_between_the_screen_edges() {
            var inset = tc.importButton.mapToItem(tc.screen, 0, 0).x;
            verify(inset > 0, "the import button is not inset from the edge");

            for (var i = 0; i < tc.list.count; i++) {
                var card = tc.list.itemAtIndex(i);
                verify(card !== null, "row " + i + " has no delegate");

                var left = card.mapToItem(tc.screen, 0, 0).x;
                var right = tc.screen.width - (left + card.width);
                compare(left, inset, "row " + i + " does not start where the import button does");
                compare(right, left, "row " + i + " is not centred between the screen edges");
            }
        }

        // The empty-state sentence sits one step further in than the cards, and
        // it measures itself against the view rather than the screen — so it
        // moved with the inset and had to give up a step of its own.
        function test_the_empty_message_stays_a_step_in_from_the_cards() {
            var message = findChild(tc.screen, "importsEmptyMessage");
            verify(message !== null, "the empty-state message is missing");

            var inset = tc.importButton.mapToItem(tc.screen, 0, 0).x;
            var card = tc.list.itemAtIndex(0);
            verify(card !== null, "the first row has no delegate");
            compare(message.width, card.width - 2 * inset);
        }
    }

    // Kind selection needs no imported fixture; keep its geometry check independent
    // of the library-card cases and their persistent test data.
    TestCase {
        id: kindTc
        name: "ImportsKindPickerLayout"
        when: windowShown
        readonly property var screen: screenLoader.item
        readonly property var importButton: findChild(screen, "importFileButton")

        function test_kind_labels_fit_at_phone_width() {
            root.width = 360;
            kindTc.importButton.clicked();
            var picker = findChild(kindTc.screen, "importKindPicker");
            verify(picker !== null);
            tryVerify(function () {
                return picker.visible;
            });
            wait(100);
            var kinds = ["chan", "group", "keys", "p25Bandplan", "src"];
            var labels = ["Channel map", "Talkgroups", "Keys", "P25 band plan", "Radio IDs"];
            for (var i = 0; i < kinds.length; i++) {
                var pill = findChild(picker, "importKind_" + kinds[i]);
                verify(pill !== null && pill.visible);
                compare(pill.text, labels[i]);
                verify(!pill.caret);
                var label = findChild(pill, "filterPillLabel");
                compare(label.text, labels[i]);
                verify(!label.truncated && label.width >= label.contentWidth);
                var inPill = label.mapToItem(pill, 0, 0);
                verify(inPill.x >= 0 && inPill.x + label.width <= pill.width);
                verify(inPill.y >= 0 && inPill.y + label.height <= pill.height);
                var onScreen = pill.mapToItem(kindTc.screen, 0, 0);
                verify(onScreen.x >= 0 && onScreen.x + pill.width <= kindTc.screen.width);
                verify(onScreen.y >= 0 && onScreen.y + pill.height <= kindTc.screen.height);
                pill.clicked();
                compare(kindTc.screen.pendingType, kinds[i]);
            }
            root.width = 420;
            // Close the sheet again: left open it would be the screen's top
            // modal for the cases after this one.
            var sheet = picker;
            while (sheet && typeof sheet.requestDismiss !== "function")
                sheet = sheet.parent;
            verify(sheet !== null);
            sheet.visible = false;
        }

        function sheetOf(item) {
            var sheet = item;
            while (sheet && typeof sheet.requestDismiss !== "function")
                sheet = sheet.parent;
            verify(sheet !== null);
            return sheet;
        }

        // #831 B8: on the narrowest phone with the largest text, the longer
        // kinds were one-line pills wider than the sheet, which cut them off.
        // Each pill now fits inside the sheet, its label wraps between words
        // inside the pill, and the pill grows to hold it.
        function test_kind_pills_fit_a_narrow_sheet_at_large_text() {
            var width = root.width;
            try {
                root.width = 320;
                Ui.Theme.fontScale = 2;
                kindTc.importButton.clicked();
                var picker = findChild(kindTc.screen, "importKindPicker");
                verify(picker !== null);
                var sheet = sheetOf(picker);
                tryCompare(sheet, "visible", true);
                waitForRendering(kindTc.screen);
                var scroll = findChild(sheet, "modalSheetScroll");
                verify(scroll !== null);
                var kinds = ["chan", "group", "keys", "p25Bandplan", "src", "dmrTgKeys", "vertexKeys", "trunkTargets"];
                var labels = ["Channel map", "Talkgroups", "Keys", "P25 band plan", "Radio IDs", "DMR key mappings", "Vertex keystreams", "Target CSV"];
                var wrapped = 0;
                for (var i = 0; i < kinds.length; i++) {
                    var pill = findChild(picker, "importKind_" + kinds[i]);
                    verify(pill !== null && pill.visible);
                    var where = labels[i] + ": ";
                    verify(pill.width <= picker.width + 0.5, where + "the pill is wider than the sheet: " + pill.width + " > " + picker.width);
                    var inSheet = pill.mapToItem(scroll, 0, 0);
                    verify(inSheet.x >= -0.5 && inSheet.x + pill.width <= scroll.width + 0.5, where + "the pill leaves the sheet: " + [inSheet.x, pill.width, scroll.width]);
                    verify(pill.height >= Ui.Theme.minimumTouchSize);
                    var label = findChild(pill, "filterPillLabel");
                    compare(label.text, labels[i]);
                    verify(!label.truncated);
                    verify(label.contentWidth <= label.width + 0.5, where + "the label is wider than its line: " + label.contentWidth + " > " + label.width);
                    // Between words only (issue #646): never more lines than words.
                    verify(label.lineCount <= labels[i].split(" ").length, where + label.lineCount + " lines");
                    if (label.lineCount > 1)
                        wrapped++;
                    var inPill = label.mapToItem(pill, 0, 0);
                    verify(inPill.x >= 0 && inPill.x + label.width <= pill.width, where + "the label leaves the pill horizontally");
                    verify(inPill.y >= 0 && inPill.y + label.height <= pill.height, where + "the label leaves the pill vertically");
                }
                verify(wrapped > 0, "a long kind wraps at this width and text size");
                // The key format follows Keys, inside the sheet as well.
                findChild(picker, "importKind_keys").clicked();
                var format = findChild(kindTc.screen, "importKeyFormat");
                verify(format !== null);
                tryCompare(format, "visible", true);
                var formatAt = format.mapToItem(scroll, 0, 0);
                verify(formatAt.x >= -0.5 && formatAt.x + format.width <= scroll.width + 0.5);
                sheet.visible = false;
            } finally {
                Ui.Theme.resetFontScale();
                root.width = width;
                kindTc.screen.pendingType = "chan";
                kindTc.screen.pendingKeyHex = false;
            }
        }

        // The kind and key format the sheet holds decide what is imported.
        function test_kind_and_key_format_choose_the_type() {
            kindTc.importButton.clicked();
            var picker = findChild(kindTc.screen, "importKindPicker");
            var sheet = sheetOf(picker);
            tryCompare(sheet, "visible", true);
            try {
                findChild(picker, "importKind_keys").clicked();
                var format = findChild(kindTc.screen, "importKeyFormat");
                verify(format.visible);
                compare(format.currentIndex, 0);
                var types = [];
                for (var hex of [false, true]) {
                    format.selected(hex ? 1 : 0);
                    compare(kindTc.screen.pendingKeyHex, hex);
                    kindTc.screen.importPickedFile();
                    types.push(findChild(kindTc.screen, "importsCsvImport").type);
                    var request = testContext.lastDocumentRequest();
                    compare(request.kind, "open");
                    testContext.answerDocumentCancelled(request.id);
                }
                compare(types, ["keysDec", "keysHex"]);
                findChild(picker, "importKind_src").clicked();
                compare(kindTc.screen.pendingType, "src");
                verify(!format.visible);
            } finally {
                sheet.visible = false;
                kindTc.screen.pendingType = "chan";
                kindTc.screen.pendingKeyHex = false;
            }
        }

        // #831 B3: a screen reader names the Imports sheets by what they are
        // for, not by the "Dialog" every unnamed sheet reads as.
        function test_sheets_have_accessible_names() {
            kindTc.importButton.clicked();
            var typeSheet = sheetOf(findChild(kindTc.screen, "importKindPicker"));
            tryCompare(typeSheet, "visible", true);
            compare(typeSheet.Accessible.role, Accessible.Dialog);
            compare(typeSheet.Accessible.name, "File contents");
            // Not its first line again, which is read right after the name.
            var first = typeSheet.content[0];
            compare(first.text, "Import file");
            verify(typeSheet.Accessible.name !== first.text);
            typeSheet.visible = false;

            var groups = testContext.writeFixtureCsv("named-sheet.csv", "id,mode,name\n1001,A,Dispatch\n");
            var result = testContext.importNamed(groups, "Dispatch groups.csv", "group");
            verify(result.ok, "the fixture did not import");
            try {
                kindTc.screen.actionRow = importedFiles.rowForPath(result.path);
                var name = importedFiles.get(kindTc.screen.actionRow).name;
                verify(name.length > 0);
                var actionSheet = findChild(kindTc.screen, "importsActionSheet");
                actionSheet.visible = true;
                compare(actionSheet.Accessible.name, name);
                actionSheet.visible = false;
                var removeSheet = findChild(kindTc.screen, "importsRemoveSheet");
                removeSheet.visible = true;
                compare(removeSheet.Accessible.name, "Remove " + name + "?");
                removeSheet.visible = false;
            } finally {
                kindTc.screen.actionRow = -1;
                importedFiles.remove(importedFiles.rowForPath(result.path));
            }
        }
    }
    // #831 B8, issue #646: a wrapping pill keeps whole words. A word wider
    // than the pill is drawn smaller, down to its normal-text size, then
    // elided; with room again it is drawn full size on one line.
    TestCase {
        name: "FilterPillWrap"
        when: windowShown

        function labelOf(pill) {
            var label = findChild(pill, "filterPillLabel");
            verify(label !== null);
            return label;
        }
        function test_a_long_word_never_leaves_the_pill_data() {
            return [
                {tag: "shrinks", box: 200, scale: 2, text: "Ultrakeystreams list", elided: false},
                {tag: "elides", box: 120, scale: 1, text: "Supercalifragilistic keys", elided: true}
            ];
        }
        function test_a_long_word_never_leaves_the_pill(data) {
            var box = pillBox.width;
            try {
                pillBox.width = data.box;
                Ui.Theme.fontScale = data.scale;
                longPill.text = data.text;
                longPill.visible = true;
                waitForRendering(longPill);
                var label = labelOf(longPill);
                var full = Math.round(Ui.Theme.fontSize(13));
                verify(longPill.width <= pillBox.width + 0.5);
                verify(label.contentWidth <= label.width + 0.5, "the label is wider than its line: " + label.contentWidth + " > " + label.width);
                var inPill = label.mapToItem(longPill, 0, 0);
                verify(inPill.x >= 0 && inPill.x + label.width <= longPill.width + 0.5);
                verify(inPill.y >= 0 && inPill.y + label.height <= longPill.height + 0.5);
                verify(label.lineCount <= data.text.split(" ").length, label.lineCount + " lines");
                compare(label.elided, data.elided);
                compare(label.truncated, data.elided);
                if (!data.elided)
                    verify(label.fitPixelSize < full && label.fitPixelSize >= 13, "drawn smaller: " + label.fitPixelSize);
                compare(longPill.Accessible.name, data.text, "the pill still reads the whole label");

                pillBox.width = 800;
                waitForRendering(longPill);
                compare(label.fitPixelSize, full, "full size again once there is room");
                verify(!label.truncated);
                compare(label.lineCount, 1);
                compare(longPill.width, longPill.implicitWidth);
            } finally {
                longPill.visible = false;
                pillBox.width = box;
                Ui.Theme.resetFontScale();
            }
        }
    }

    // #832: an import answers later, from the import worker.
    TestCase {
        id: importTc
        name: "ImportsScreenImports"
        when: windowShown
        readonly property var screen: screenLoader.item
        readonly property var flow: findChild(screenLoader.item, "importsCsvImport")

        function groups(name, rows) {
            var text = "id,mode,name\n";
            for (var i = 0; i < rows; ++i)
                text += (1001 + i) + ",A,Group " + i + "\n";
            return testContext.writeFixtureCsv(name, text);
        }
        function init() {
            importTc.screen.visible = true;
            importTc.screen.notice = "";
            importTc.screen.pendingRow = -1;
        }
        function cleanup() {
            testContext.holdImports(false);
            importTc.screen.visible = true;
            tryCompare(importTc.flow, "active", false);
            while (importedFiles.count > 0)
                importedFiles.remove(0);
        }
        // The host's picker hands back @p path (issue #829).
        function accept(path) {
            testContext.answerDocumentPicked(testContext.lastDocumentRequest().id, "file://" + path, path.substring(path.lastIndexOf('/') + 1), false);
        }

        // An update names its row by path: a row removed above it while the
        // copy runs moves it, and the update still lands on it.
        function test_update_lands_on_its_row_after_rows_move() {
            var above = testContext.importNow(groups("imports-above.csv", 1), "group");
            var target = testContext.importNow(groups("imports-target.csv", 1), "group");
            verify(above.ok && target.ok);
            importTc.screen.pendingRow = importedFiles.rowForPath(target.path);
            compare(importTc.screen.pendingRow, 1);
            testContext.holdImports(true);
            importTc.screen.importPickedFile();
            accept(groups("imports-update.csv", 3));
            verify(importTc.flow.copying);
            verify(importedFiles.remove(importedFiles.rowForPath(above.path)));
            compare(importedFiles.rowForPath(target.path), 0, "the row moved up");
            testContext.holdImports(false);
            tryCompare(importTc.flow, "active", false);
            compare(importedFiles.count, 1);
            compare(importedFiles.get(0).path, target.path);
            compare(importedFiles.get(0).accepted, 3, "the update replaced its own row's file");
            compare(importTc.screen.notice, "Updated · 3 talkgroups");
            verify(!importTc.screen.noticeIsProblem);
        }

        // The screen is kept while closed: closing cancels the copy, and the
        // answer is not reported to the next visit.
        function test_closing_cancels_the_copy() {
            testContext.holdImports(true);
            importTc.screen.importPickedFile();
            accept(groups("imports-closed.csv", 2));
            verify(importTc.flow.copying);
            importTc.screen.visible = false;
            tryCompare(importTc.flow, "active", false);
            compare(importedFiles.count, 0, "the cancelled copy added nothing");
            compare(importTc.screen.notice, "");
        }

        // #831: a file another app opened, its kind already chosen, imports as
        // a new file whatever row was last updated, and says how it went.
        function test_import_incoming_imports_and_says_so() {
            importTc.screen.pendingRow = 3;
            importTc.screen.notice = "Old notice";
            testContext.holdImports(true);
            verify(importTc.screen.importIncoming(groups("incoming-groups.csv", 3), "incoming-groups.csv", "group"));
            compare(importTc.screen.pendingRow, -1);
            compare(importTc.screen.notice, "");
            verify(importTc.flow.copying);
            verify(importTc.screen.importActive, "a running import is reported");
            testContext.holdImports(false);
            tryCompare(importTc.flow, "active", false);
            verify(!importTc.screen.importActive);
            compare(importedFiles.count, 1);
            compare(importedFiles.get(0).type, "group");
            compare(importTc.screen.notice, "Imported · 3 talkgroups");
            verify(!importTc.screen.noticeIsProblem);
        }

        // #831: the screen cancels its import when hidden, so a file handed to
        // it hidden is refused rather than imported with no one to see it.
        function test_import_incoming_refused_while_hidden() {
            importTc.screen.visible = false;
            verify(!importTc.screen.importIncoming(groups("incoming-hidden.csv", 1), "incoming-hidden.csv", "group"));
            verify(!importTc.flow.active);
            verify(!importTc.screen.importActive);
            importTc.screen.visible = true;
            wait(50);
            compare(importedFiles.count, 0, "nothing was imported");
        }

        // #831: begin() would replace an import still copying or waiting on
        // its companion sheet; importIncoming() refuses instead and leaves it.
        function test_import_incoming_refused_while_an_import_runs() {
            importTc.screen.pendingType = "group";
            testContext.holdImports(true);
            importTc.screen.importPickedFile();
            accept(groups("incoming-first.csv", 2));
            verify(importTc.flow.copying);
            var reference = importTc.flow.reference;
            verify(!importTc.screen.importIncoming(groups("incoming-second.csv", 1), "incoming-second.csv", "group"));
            verify(importTc.flow.copying);
            compare(importTc.flow.reference, reference, "the running import is untouched");
            testContext.holdImports(false);
            tryCompare(importTc.flow, "active", false);
            compare(importedFiles.count, 1);
            compare(importedFiles.get(0).accepted, 2, "the first import landed");

            var map = testContext.writeFixtureCsv("incoming-map.csv", "channel,frequency,mode,keys_dec_csv\n1,851012500,p25,missing.csv\n");
            verify(importTc.screen.importIncoming(map, "incoming-map.csv", "chan"));
            tryVerify(function () { return importTc.flow.requiredFiles.length > 0; }, 5000, "the companions are asked for");
            verify(importTc.flow.active && !importTc.flow.copying);
            verify(!importTc.screen.importIncoming(groups("incoming-third.csv", 1), "incoming-third.csv", "group"));
            verify(importTc.flow.requiredFiles.length > 0, "the companion sheet still waits");
            compare(importTc.flow.fileName, "incoming-map.csv");
            findChild(importTc.flow, "csvCompanionSheet").requestDismiss();
            tryCompare(importTc.flow, "active", false);
            compare(importedFiles.count, 1, "nothing else was imported");
        }

        // #831: a key mapping apply outlives the action sheet that started it.
        function test_map_apply_reports_while_pending() {
            var apply = findChild(importTc.screen, "importsMapApply");
            verify(apply !== null);
            verify(!importTc.screen.mapApplyActive);
            apply.requestId = "incoming-test";
            verify(importTc.screen.mapApplyActive);
            apply.requestId = "";
            verify(!importTc.screen.mapApplyActive);
        }

        // Each failure says what went wrong.
        function test_failures_say_why() {
            var gone = testContext.storePath("fixtures/imports-gone.csv");
            importTc.flow.begin(gone, "imports-gone.csv", "group", importTc.screen.importContext());
            tryCompare(importTc.flow, "active", false);
            compare(importTc.screen.notice, "Could not read imports-gone.csv. Check that it is still available, then try again.");
            verify(importTc.screen.noticeIsProblem);
            var read = importTc.screen.notice;

            var bad = testContext.writeFixtureCsv("imports-bad-map.csv", "not a talkgroup row\n");
            importTc.flow.begin(bad, "imports-bad-map.csv", "dmrTgKeys", importTc.screen.importContext());
            tryCompare(importTc.flow, "active", false);
            verify(importTc.screen.noticeIsProblem);
            // The parser's own reason, not only "is not a valid ...".
            compare(importTc.screen.notice, "DMR TG key ID map CSV 'imports-bad-map.csv' contains no mappings.");

            // The row is removed while its update copies.
            var target = testContext.importNow(groups("imports-stale.csv", 1), "group");
            verify(target.ok);
            importTc.screen.pendingRow = 0;
            testContext.holdImports(true);
            importTc.screen.importPickedFile();
            accept(groups("imports-stale-update.csv", 2));
            verify(importedFiles.remove(0));
            testContext.holdImports(false);
            tryCompare(importTc.flow, "active", false);
            compare(importTc.screen.notice, "The imported file changed while the new copy was being made. Nothing was replaced.");
            verify(importTc.screen.noticeIsProblem);
            compare(importedFiles.count, 0);
        }
    }
}
