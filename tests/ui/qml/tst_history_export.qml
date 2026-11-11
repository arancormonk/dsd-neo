// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// History's Export…: the rows the list shows, as a CSV file. Both History
// screens (the idle tab, and the session tools' one without a title) share
// historyView and the one `historyExport`, a scripted stand-in here
// (qml_test_context.h): each screen keeps its own export and result line, and
// a write running from either holds both buttons.
Item {
    id: root

    width: 360
    height: 1280

    Loader {
        id: idleLoader

        width: root.width
        height: 640
        source: uiDir + "/HistoryScreen.qml"
        onLoaded: item.systems = clockStub
    }

    Loader {
        id: sessionLoader

        y: 640
        width: root.width
        height: 640
        source: uiDir + "/HistoryScreen.qml"
        onLoaded: {
            item.showTitle = false;
            item.systems = clockStub;
        }
    }

    // A fixed clock: local noon on 2026-11-09, so an export's suggested name
    // is known whenever the test runs.
    QtObject {
        id: clockStub
        function realtimeNowMs() { return new Date(2026, 10, 9, 12, 0, 0).getTime(); }
    }

    TestCase {
        name: "HistoryExport"
        when: windowShown

        readonly property string failure: "The export couldn't be written. Try another location."

        function screens() {
            return [idleLoader.item, sessionLoader.item];
        }
        function exportButton(screen) {
            return findChild(screen, "historyExportButton");
        }
        function clearButton(screen) {
            return findChild(screen, "clearHistoryButton");
        }
        function exportLine(screen) {
            return findChild(screen, "historyExportMessage");
        }
        function exportPicker(screen) {
            return findChild(screen, "historyExportPicker");
        }
        function calls(name) {
            return JSON.stringify(historyExport.calls.filter(function (call) {
                return call[0] === name;
            }).map(function (call) {
                // The view itself does not stringify: its search text and row count say which rows it showed.
                return call[0] === "prepare" ? [call[0], call[2], call[3]] : call;
            }));
        }
        function noRequest() {
            return JSON.stringify(testContext.lastDocumentRequest()) === "{}";
        }
        // Press @p screen's Export… over the log: the id of the save request it opens.
        function beginExport(screen) {
            var button = exportButton(screen);
            verify(button && button.visible && button.enabled, "History can be exported");
            button.activate();
            var request = testContext.lastDocumentRequest();
            compare(request.kind, "save");
            verify(exportPicker(screen).visible, "the picker waits");
            return request.id;
        }
        function rectIn(item, ancestor) {
            var p = item.mapToItem(ancestor, 0, 0);
            return {x: p.x, y: p.y, width: item.width, height: item.height};
        }
        function overlaps(a, b) {
            return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
        }
        function describe(r) {
            return "(" + r.x.toFixed(1) + ", " + r.y.toFixed(1) + " " + r.width.toFixed(1) + "x" + r.height.toFixed(1) + ")";
        }
        function title(screen) {
            return findChild(screen, "historyTitle");
        }

        function init() {
            verify(idleLoader.item !== null && sessionLoader.item !== null, "both History screens load");
            historyExport.reset();
            testContext.resetHostPlayback();
            historyView.filterText = "";
            historyView.filterSystem = "";
            historyView.filterKind = 0;
            callHistory.clearAll();
            callHistory.pushMany(2, "TODAY");
            for (var screen of screens())
                screen.visible = true;
            wait(0);
        }
        function cleanup() {
            // No picker, export, result line, sheet or text size outlives its case, even one that failed part way.
            for (var screen of screens()) {
                screen.visible = true;
                var picker = exportPicker(screen);
                if (picker && picker.visible)
                    picker.cancel();
                if (screen.exportToken > 0)
                    historyExport.finish(screen.exportToken, false);
                screen.exportMessage = {text: "", error: false, heard: false};
                for (var name of ["historyDetailSheet", "clearHistoryConfirm", "historyFilterChoices"]) {
                    var sheet = findChild(screen, name);
                    if (sheet)
                        sheet.visible = false;
                }
            }
            historyExport.setBusy(false);
            Ui.Theme.resetFontScale();
            root.width = 360;
        }

        // Export… sits left of Clear in both screens, on Clear's rule: shown while History holds a row.
        function test_export_button_beside_clear() {
            for (var screen of screens()) {
                var button = exportButton(screen);
                verify(button !== null, "the button is there");
                compare(button.text, "Export…");
                compare(button.accessibleName, "Export history as a CSV file");
                compare(button.Accessible.name, "Export history as a CSV file");
                verify(button.visible && clearButton(screen).visible);
                waitForRendering(screen);
                var b = rectIn(button, screen), c = rectIn(clearButton(screen), screen);
                verify(b.x + b.width <= c.x, "left of Clear: " + describe(b) + " " + describe(c));
                compare(b.y, c.y, "on Clear's line");
            }
            callHistory.clearAll();
            for (screen of screens()) {
                verify(!exportButton(screen).visible, "an empty log has nothing to export");
                verify(!clearButton(screen).visible);
            }
        }

        // Held while the list shows no row, while this screen's picker is out, and while either screen's file is
        // being written.
        function test_export_disabled_rules() {
            var idle = idleLoader.item, session = sessionLoader.item;
            historyView.filterText = "no such call";
            tryCompare(historyView, "count", 0);
            for (var screen of screens()) {
                verify(exportButton(screen).visible, "a filtered log still offers it");
                verify(!exportButton(screen).enabled, "filtered to no row, nothing to export");
            }
            historyView.filterText = "";
            verify(exportButton(idle).enabled && exportButton(session).enabled);

            var id = beginExport(idle);
            verify(!exportButton(idle).enabled, "waits while its picker asks");
            verify(exportButton(session).enabled, "the other screen holds no export of its own");
            testContext.answerDocumentPicked(id, "content://docs/a", "a.csv", false);
            verify(historyExport.busy);
            verify(!exportButton(idle).enabled && !exportButton(session).enabled, "both wait while the file is written");
            historyExport.finish(7, true);
            verify(exportButton(idle).enabled && exportButton(session).enabled, "free for the next export");

            historyExport.setBusy(true);
            verify(!exportButton(idle).enabled && !exportButton(session).enabled, "a write from either holds both");
            historyExport.setBusy(false);
            verify(exportButton(idle).enabled && exportButton(session).enabled);
        }

        // Clear's confirmation covers the header: the button is held under it with the rest.
        function test_clear_confirmation_holds_the_button() {
            var screen = idleLoader.item;
            findChild(screen, "clearHistoryConfirm").open();
            verify(!exportButton(screen).enabled, "held under the confirmation");
            exportButton(screen).activate();
            verify(noRequest(), "a press under the sheet does nothing");
            compare(calls("prepare"), "[]");
            findChild(screen, "clearHistoryConfirm").requestDismiss();
            verify(exportButton(screen).enabled);
        }

        // Text typed less than the search's debounce before the press is in the file.
        function test_typed_search_exports_at_once() {
            var screen = idleLoader.item;
            callHistory.pushWithIds(4004, 7004);
            findChild(screen, "historySearch").input.text = "4004";
            compare(historyView.filterText, "", "still debounced");
            beginExport(screen);
            compare(historyView.filterText, "4004", "the press applied the typed search");
            compare(calls("prepare"), JSON.stringify([["prepare", "4004", 1]]));
            verify(historyExport.calls[0][1] === historyView, "the screen's own view");
        }

        // Typing on one screen and leaving it before the debounce hands the search to the other screen, whose
        // export then holds it.
        function test_instance_switch_during_debounce() {
            var idle = idleLoader.item, session = sessionLoader.item;
            callHistory.pushWithIds(4004, 7004);
            findChild(idle, "historySearch").input.text = "4004";
            idle.visible = false;
            compare(historyView.filterText, "4004", "flushed as the screen hid");
            compare(findChild(session, "historySearch").text, "4004");
            beginExport(session);
            compare(calls("prepare"), JSON.stringify([["prepare", "4004", 1]]));
            wait(300);
            compare(historyView.filterText, "4004", "no older debounce overwrites it");
        }

        // One CSV save picker, named for the day.
        function test_export_opens_a_csv_save_picker() {
            var screen = sessionLoader.item;
            beginExport(screen);
            var request = testContext.lastDocumentRequest();
            compare(request.name, "Call history 2026-11-09.csv");
            compare(request.mime, "text/csv");
            var picker = exportPicker(screen);
            compare(picker.mode, "save");
            compare(picker.mimeType, "text/csv");
            compare(JSON.stringify(picker.nameFilters), JSON.stringify(["CSV files (*.csv)"]));
            compare(picker.defaultSuffix, "csv");
            compare(screen.exportToken, 7);
        }

        // No picker for a file the exporter would not build: no row any more, or a view it refuses.
        function test_export_without_a_file_data() {
            return [{tag: "no rows", prepared: {token: 0, count: 0}, text: "No entries to export.", error: false},
                    {tag: "refused", prepared: {token: 0, count: -1}, text: "The export couldn't be written. Try another location.", error: true}];
        }
        function test_export_without_a_file(data) {
            for (var screen of screens()) {
                historyExport.prepared = data.prepared;
                exportButton(screen).activate();
                verify(noRequest(), "no picker");
                verify(!exportPicker(screen).visible);
                var line = exportLine(screen);
                compare(line.text, data.text);
                verify(line.visible);
                verify(Qt.colorEqual(line.color, data.error ? Ui.Theme.alert : Ui.Theme.textSecondary));
                verify(exportButton(screen).enabled, "nothing pending");
                compare(screen.exportToken, 0);
            }
        }

        // Backing out of the picker drops the file and says nothing.
        function test_picker_cancel_discards_quietly() {
            var screen = idleLoader.item;
            var id = beginExport(screen);
            testContext.answerDocumentCancelled(id);
            compare(calls("discard"), JSON.stringify([["discard", 7]]));
            compare(calls("save"), "[]");
            verify(!exportLine(screen).visible, "nothing to say");
            compare(screen.exportToken, 0);
            verify(exportButton(screen).enabled);
        }

        function test_picker_unavailable() {
            var screen = idleLoader.item;
            var id = beginExport(screen);
            testContext.answerDocumentUnavailable(id, "noSavePicker");
            compare(calls("discard"), JSON.stringify([["discard", 7]]));
            compare(exportLine(screen).text, "This device has no save picker, and DSD-neo could not save to the Download folder.");
            verify(exportLine(screen).visible);
            verify(Qt.colorEqual(exportLine(screen).color, Ui.Theme.alert));
            verify(exportButton(screen).enabled);
        }

        // An accepted pick is saved; the line under the buttons says how it went, read out once.
        function test_export_says_how_it_went_data() {
            return [{tag: "all", count: 2, total: 2, ok: true, text: "Exported 2 entries."},
                    {tag: "one", count: 1, total: 1, ok: true, text: "Exported 1 entry."},
                    {tag: "filtered", count: 37, total: 214, ok: true,
                     text: "Exported 37 of 214 entries. The search and filters left out the rest."},
                    {tag: "Download", count: 214, total: 214, ok: true, saved: "Call history 2026-11-10 (1).csv",
                     text: "Exported 214 entries to Download/DSD-neo as Call history 2026-11-10 (1).csv."},
                    {tag: "one to Download", count: 1, total: 1, ok: true, saved: "Call history 2026-11-10.csv",
                     text: "Exported 1 entry to Download/DSD-neo as Call history 2026-11-10.csv."},
                    {tag: "filtered to Download", count: 37, total: 214, ok: true, saved: "Call history 100%1.csv",
                     text: "Exported 37 of 214 entries to Download/DSD-neo as Call history 100%1.csv."},
                    {tag: "failure", count: 2, total: 2, ok: false, text: "The export couldn't be written. Try another location."}];
        }
        function test_export_says_how_it_went(data) {
            for (var screen of screens()) {
                historyExport.reset();
                historyExport.prepared = {token: 7, count: data.count, total: data.total};
                var id = beginExport(screen);
                var reference = "content://docs/history";
                if (data.saved)
                    testContext.setSavedDocumentName(reference, data.saved);
                testContext.answerDocumentPicked(id, reference, "export.csv", !!data.saved);
                compare(calls("save"), JSON.stringify([["save", 7, reference]]));
                verify(!exportLine(screen).visible, "nothing to say yet");
                var before = screen.announcements;
                historyExport.finish(7, data.ok);
                var line = exportLine(screen);
                compare(line.text, data.text);
                verify(line.visible);
                verify(Qt.colorEqual(line.color, data.ok ? Ui.Theme.textSecondary : Ui.Theme.alert), "error tone for a failure only");
                compare(line.Accessible.role, Accessible.AlertMessage);
                compare(line.Accessible.name, data.text);
                tryCompare(screen, "announcements", before + 1);
                compare(screen.announced, data.text);
                verify(exportButton(screen).enabled, "free for the next export");
                compare(calls("discard"), "[]");
                wait(20);
                compare(screen.announcements, before + 1, "read once");
                screen.exportMessage = {text: "", error: false, heard: false};
            }
        }

        // One screen's result is its own: the other's export neither clears nor replaces it.
        function test_results_stay_on_their_own_screen() {
            var idle = idleLoader.item, session = sessionLoader.item;
            var id = beginExport(idle);
            testContext.answerDocumentPicked(id, "content://docs/a", "a.csv", false);
            historyExport.finish(7, true);
            compare(exportLine(idle).text, "Exported 2 entries.");
            verify(!exportLine(session).visible);
            historyExport.prepared = {token: 0, count: 0};
            exportButton(session).activate();
            compare(exportLine(session).text, "No entries to export.");
            compare(exportLine(idle).text, "Exported 2 entries.", "untouched by the other screen's press");
            historyExport.prepared = {token: 9, count: 2, total: 2};
            id = beginExport(session);
            testContext.answerDocumentPicked(id, "content://docs/b", "b.csv", false);
            historyExport.finish(9, false);
            compare(exportLine(session).text, failure);
            compare(exportLine(idle).text, "Exported 2 entries.", "nor by its result");
        }

        // A save the exporter refuses fails at once: nothing waits for a result, and the next export can start.
        function test_refused_save_fails_at_once() {
            var screen = idleLoader.item;
            historyExport.saveAccepted = false;
            var id = beginExport(screen);
            testContext.answerDocumentPicked(id, "content://docs/a", "a.csv", true);
            compare(calls("save"), JSON.stringify([["save", 7, "content://docs/a"]]));
            compare(exportLine(screen).text, failure);
            verify(Qt.colorEqual(exportLine(screen).color, Ui.Theme.alert));
            compare(screen.exportToken, 0, "the export is let go");
            verify(!testContext.documentCalls().some(function (call) { return call.name === "discardPendingDownload"; }),
                   "the exporter let the Download row go itself");
            verify(exportButton(screen).enabled);
            historyExport.saveAccepted = true;
            historyExport.prepared = {token: 9, count: 2, total: 2};
            exportButton(screen).activate();
            verify(exportPicker(screen).visible, "a second export starts");
            verify(!exportLine(screen).visible, "a new export clears the line");
            testContext.answerDocumentPicked(testContext.lastDocumentRequest().id, "content://docs/b", "b.csv", false);
            historyExport.finish(9, true);
            compare(exportLine(screen).text, "Exported 2 entries.");
        }

        // A result for an export the screen does not hold (an old one, the other screen's) changes nothing.
        function test_stale_finished_is_ignored() {
            var idle = idleLoader.item, session = sessionLoader.item;
            var id = beginExport(idle);
            testContext.answerDocumentPicked(id, "content://docs/a", "a.csv", false);
            historyExport.finish(5, false);
            historyExport.finish(0, false);
            historyExport.setBusy(true);
            for (var screen of screens())
                verify(!exportLine(screen).visible, "a token neither screen holds says nothing");
            compare(idle.exportToken, 7, "and leaves the export pending");
            compare(session.exportToken, 0);
            historyExport.finish(7, true);
            compare(exportLine(idle).text, "Exported 2 entries.");
            verify(!exportLine(session).visible, "the other screen held nothing");
            historyExport.finish(7, false);
            compare(exportLine(idle).text, "Exported 2 entries.", "a repeat changes nothing");
        }

        // Leaving the screen ends the picker as a cancel; nothing is said.
        function test_hiding_the_screen_cancels_the_picker() {
            var screen = idleLoader.item;
            var id = beginExport(screen);
            screen.visible = false;
            verify(!exportPicker(screen).visible);
            compare(calls("discard"), JSON.stringify([["discard", 7]]));
            verify(testContext.documentCalls().some(function (call) { return call.name === "cancelDocumentRequest" && call.id === id; }),
                   JSON.stringify(testContext.documentCalls()));
            screen.visible = true;
            verify(exportButton(screen).enabled);
            verify(!exportLine(screen).visible, "a cancel says nothing");
        }

        // A result that lands while the screen is hidden is kept and read once on return; heard, the next visit
        // starts without it.
        function test_result_while_hidden_is_read_once_on_return() {
            var screen = idleLoader.item;
            var id = beginExport(screen);
            testContext.answerDocumentPicked(id, "content://docs/a", "a.csv", false);
            screen.visible = false;
            var before = screen.announcements;
            historyExport.finish(7, true);
            wait(20);
            compare(screen.announcements, before, "nothing is read to a hidden screen");
            screen.visible = true;
            compare(exportLine(screen).text, "Exported 2 entries.", "kept for the return");
            tryCompare(screen, "announcements", before + 1);
            wait(20);
            compare(screen.announcements, before + 1, "once");
            screen.visible = false;
            screen.visible = true;
            verify(!exportLine(screen).visible, "heard, it is gone on the next visit");
            wait(20);
            compare(screen.announcements, before + 1);
        }

        // Under a sheet the line is out of reach: read once the sheet closes.
        function test_result_under_a_sheet_is_read_once_it_closes_data() {
            return [{tag: "detail sheet", sheet: "historyDetailSheet"}, {tag: "Clear confirmation", sheet: "clearHistoryConfirm"},
                    {tag: "filter choices", sheet: "historyFilterChoices"}];
        }
        function test_result_under_a_sheet_is_read_once_it_closes(data) {
            var screen = idleLoader.item;
            var id = beginExport(screen);
            testContext.answerDocumentPicked(id, "content://docs/a", "a.csv", false);
            var sheet = findChild(screen, data.sheet);
            verify(sheet !== null, data.sheet);
            if (data.sheet === "historyDetailSheet")
                sheet.open(callHistory.recordFor(String(callHistory.newestUid())));
            else if (data.sheet === "historyFilterChoices")
                sheet.open("System", ["All systems", "Test Site"], function () {});
            else
                sheet.open();
            verify(sheet.visible);
            var before = screen.announcements;
            historyExport.finish(7, true);
            wait(20);
            compare(screen.announcements, before, "nothing is read under the sheet");
            var line = exportLine(screen);
            verify(line.visible, "drawn under the sheet");
            compare(line.Accessible.name, "", "nameless behind the sheet");
            sheet.requestDismiss();
            verify(!sheet.visible);
            tryCompare(screen, "announcements", before + 1);
            compare(screen.announced, "Exported 2 entries.");
            compare(line.Accessible.name, "Exported 2 entries.");
            wait(20);
            compare(screen.announcements, before + 1, "once");
        }

        // Title and buttons share a line when they fit; at a large text size on a narrow phone the buttons take
        // their own line under the title, still on the right. Never over the title, never past the edge. The
        // session screen's header holds the buttons alone.
        function test_header_holds_title_and_buttons_data() {
            return [{tag: "idle 1.0", session: false, scale: 1.0}, {tag: "idle 1.6", session: false, scale: 1.6},
                    {tag: "idle 2.0", session: false, scale: 2.0}, {tag: "session 1.0", session: true, scale: 1.0},
                    {tag: "session 2.0", session: true, scale: 2.0}];
        }
        function test_header_holds_title_and_buttons(data) {
            var screen = data.session ? sessionLoader.item : idleLoader.item;
            Ui.Theme.fontScale = data.scale;
            verify(waitForPolish(screen));
            waitForRendering(screen);
            var head = title(screen);
            verify(head !== null, "the title is named");
            compare(head.visible, !data.session);
            var exportRect = rectIn(exportButton(screen), screen), clearRect = rectIn(clearButton(screen), screen);
            for (var r of [exportRect, clearRect]) {
                verify(r.x >= 0 && r.x + r.width <= screen.width, "inside the screen: " + describe(r) + " in " + screen.width);
                verify(r.y >= 0);
            }
            verify(!overlaps(exportRect, clearRect), "the buttons apart: " + describe(exportRect) + " " + describe(clearRect));
            compare(exportButton(screen).height, exportButton(screen).implicitHeight, "a whole button");
            compare(clearButton(screen).width, clearButton(screen).implicitWidth, "Clear unclipped");
            compare(exportButton(screen).width, exportButton(screen).implicitWidth, "Export… unclipped");
            var margin = Ui.Theme.screenPadding;
            verify(Math.abs(clearRect.x + clearRect.width - (screen.width - margin)) < 0.5, "on the right");
            var search = rectIn(findChild(screen, "historySearch"), screen);
            verify(exportRect.y + exportRect.height <= search.y, "above the search field");
            if (data.session) {
                verify(exportRect.y < margin + 0.5, "the buttons alone at the top: " + describe(exportRect));
                return;
            }
            var titleRect = rectIn(head, screen);
            titleRect.width = head.implicitWidth;
            verify(titleRect.x + titleRect.width <= screen.width - margin, "the title whole: " + describe(titleRect));
            for (r of [exportRect, clearRect])
                verify(!overlaps(r, titleRect), "clear of the title: " + describe(r) + " " + describe(titleRect));
            if (data.scale === 1.0)
                verify(exportRect.y < titleRect.y + titleRect.height, "one line at normal text");
            if (data.scale === 2.0)
                verify(exportRect.y >= titleRect.y + titleRect.height, "under the title at the largest text");
        }
    }
}
