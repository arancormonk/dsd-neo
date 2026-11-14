// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// Issue #830: DSD-neo's own file browser, on the fixture's file browser host,
// which answers only when a case says (testContext.answerFileRoots() and the
// like). The screen serves the FileBrowser singleton's request as Main's does.
Item {
    id: root
    width: 420
    height: 800

    Component {
        id: screenComponent
        Ui.FileBrowserScreen {
            anchors.fill: parent
        }
    }
    Component {
        id: spyComponent
        SignalSpy {}
    }
    Component {
        id: openerComponent
        Ui.OutlineButton {
            text: "Import file"
            width: 200
        }
    }
    Component {
        id: sheetComponent
        Ui.ModalSheet {
            Ui.OutlineButton {
                objectName: "sheetButton"
                width: parent.width
                text: "Unlock"
            }
        }
    }
    Component {
        id: mainComponent
        Loader {
            anchors.fill: parent
            source: uiDir + "/Main.qml"
        }
    }

    TestCase {
        id: testCase
        name: "FileBrowserScreen"
        when: windowShown

        readonly property string inboxPath: "/storage/emulated/0/Android/data/io.github.arancormonk.dsdneo/files/Inbox"
        readonly property string storagePath: "/storage/emulated/0"
        readonly property string adbAccessLine: "adb shell appops set --uid io.github.arancormonk.dsdneo MANAGE_EXTERNAL_STORAGE allow"
        property var screen: null

        function access(fields) {
            var reading = {declared: true, granted: false, route: "settings", apiLevel: 36, rootsGeneration: 1};
            for (var key in (fields || {}))
                reading[key] = fields[key];
            return reading;
        }
        function init() {
            testContext.useLifecycleHost(false);
            testContext.setFileBrowserSupported(true);
            testContext.setFileAccess(access());
            testContext.resetHostPlayback();
            testContext.setCloseRefused(false);
            if (restartCloser.active)
                testContext.finishClose("stopFailed");
            testContext.setHostReading("sessionActive", false);
            testContext.setBackup("restorePending", false);
            root.width = 420;
            root.height = 800;
            Ui.Theme.fontScale = 1;
            compare(Ui.FileBrowser.request, null, "no browse request is left from an earlier case");
        }
        function cleanup() {
            if (Ui.FileBrowser.request)
                Ui.FileBrowser.dismiss(Ui.FileBrowser.request.token);
            Ui.Theme.resetFontScale();
            testContext.setHostReading("sessionActive", true);
            testContext.setBackup("restorePending", false);
            testContext.setCloseRefused(false);
            testContext.setFileAccess(access());
            testContext.setFileBrowserSupported(false);
            testContext.useLifecycleHost(false);
            testContext.resetHostPlayback();
            screen = null;
        }

        // ---- helpers ----

        function makeScreen() {
            screen = createTemporaryObject(screenComponent, root);
            verify(screen, "a browser screen");
            return screen;
        }
        function spy(target, signalName) {
            return createTemporaryObject(spyComponent, testCase, {target: target, signalName: signalName});
        }
        function open(kind, allowOtherApp) {
            return Ui.FileBrowser.open(kind || "csv", "Choose a CSV file", allowOtherApp !== false);
        }
        function callsNamed(name) {
            return testContext.fileCalls().filter(function (call) {
                return call.name === name;
            });
        }
        function callNames() {
            return testContext.fileCalls().map(function (call) {
                return call.name;
            });
        }
        function last(name) {
            var call = testContext.lastFileCall(name);
            verify(call.name === name, "a " + name + " call");
            return call;
        }
        function session() {
            return last("openFileBrowserSession").session;
        }
        function item(name, parent) {
            var found = findChild(parent || screen, name);
            verify(found !== null, name + " exists");
            return found;
        }
        function rootRecord(key, fields) {
            var kinds = {inbox: "inbox", saved: "saved", storage: "storage"};
            var paths = {inbox: inboxPath, saved: "", storage: storagePath};
            var record = {key: key, kind: kinds[key] || "removable", title: "", path: paths[key] !== undefined ? paths[key] : "/storage/6187-25A8", state: "ready", detail: "mounted"};
            for (var field in (fields || {}))
                record[field] = fields[field];
            return record;
        }
        function standardRoots() {
            return [rootRecord("inbox"), rootRecord("saved"), rootRecord("storage")];
        }
        function answerRoots(roots, generation) {
            testContext.answerFileRoots(last("requestFileRoots").id, {status: "ok", generation: generation || 7, roots: roots});
        }
        function entry(folder, name, dir, size, modifiedMs) {
            var path = folder + "/" + name;
            return {name: name, path: path, reference: dir ? "" : "file://" + path, dir: dir, size: size || 0, modifiedMs: modifiedMs || 0};
        }
        function manyEntries(folder, prefix, count) {
            var list = [];
            for (var i = 0; i < count; ++i)
                list.push(entry(folder, prefix + i + ".csv", false, 1000, 0));
            return list;
        }
        function answerListing(entries, fields) {
            var result = {status: "ok", snapshot: 11, offset: 0, entries: entries, more: false, hiddenByFilter: 0};
            for (var key in (fields || {}))
                result[key] = fields[key];
            testContext.answerFolderListing(last("requestFolderListing").id, result);
        }
        function answerStatus(status) {
            testContext.answerFolderListing(last("requestFolderListing").id, {status: status});
        }
        // An open browser at its locations, answered.
        function openAtRoots(roots, kind) {
            makeScreen();
            var token = open(kind);
            answerRoots(roots || standardRoots());
            waitForRendering(screen);
            return token;
        }
        function rootRow(key) {
            return item("fileBrowserRoot:" + key);
        }
        function openRoot(key) {
            rootRow(key).activate();
            compare(last("requestFolderListing").rootKey, key, "the location is listed");
        }
        function entryRow(path) {
            var list = item("fileBrowserList");
            list.forceLayout();
            return item("fileBrowserEntry:" + path, list);
        }
        function window() {
            return screen.Window.window;
        }
        function back() {
            Ui.Navigation.back(window());
        }
        function thisYearNov9() {
            var year = new Date(savedSystems.realtimeNowMs()).getFullYear();
            return new Date(year, 10, 9, 12, 0, 0).getTime();
        }

        // ---- cases ----

        function test_open_lists_roots() {
            makeScreen();
            testContext.setFileAccess(access({granted: true}));
            testContext.resetHostPlayback();
            verify(!screen.visible, "nothing shows before a request");
            open();
            verify(screen.visible, "a request shows the browser");
            compare(callNames(), ["openFileBrowserSession", "refreshFileAccess", "requestFileRoots"]);
            compare(last("requestFileRoots").session, session(), "the roots are asked for in the browser's session");
            compare(Ui.Navigation.topModal, screen, "the browser is the top modal");

            answerRoots([rootRecord("removable:6187-25A8"), rootRecord("storage"), rootRecord("saved"), rootRecord("inbox")]);
            waitForRendering(screen);
            compare(screen.rootRows.map(function (r) {
                return r.key;
            }), ["inbox", "saved", "storage", "removable:6187-25A8"], "inbox, saved, storage, removable");
            var keys = ["inbox", "saved", "storage", "removable:6187-25A8"];
            for (var i = 1; i < keys.length; ++i) {
                var above = rootRow(keys[i - 1]);
                var below = rootRow(keys[i]);
                verify(above.mapToItem(screen, 0, 0).y < below.mapToItem(screen, 0, 0).y, keys[i - 1] + " is drawn above " + keys[i]);
            }
            compare(rootRow("inbox").title, "DSD-neo inbox");
            compare(rootRow("inbox").subtitle, inboxPath, "the inbox shows its exact path");
            compare(rootRow("saved").title, "Saved by DSD-neo");
            compare(rootRow("saved").subtitle, "Files DSD-neo saved to Download/DSD-neo");
            compare(rootRow("storage").title, "Device storage");
            compare(rootRow("removable:6187-25A8").title, "Removable storage", "a drive with no description");

            openRoot("storage");
            answerListing([]);
            back();
            compare(screen.view, "roots");
            compare(callsNamed("requestFileRoots").length, 2, "every return to the locations lists them again");
            compare(callsNamed("refreshFileAccess").length, 2, "and reads the access again");
            answerRoots([rootRecord("inbox"), rootRecord("removable:1", {title: "SanDisk"})]);
            waitForRendering(screen);
            compare(rootRow("removable:1").title, "SanDisk", "a drive is named as Android describes it");
            compare(screen.rootRows.length, 2, "the new answer replaces the old");
        }

        function test_reading_line_after_250ms() {
            makeScreen();
            open();
            var line = item("fileBrowserReading");
            wait(200);
            verify(!line.visible, "no reading line in the first 250 ms");
            wait(100);
            verify(line.visible, "Reading folder… once the read takes a moment");
            compare(line.text, "Reading folder…");
            answerRoots(standardRoots());
            verify(!line.visible, "gone with the answer");

            openRoot("storage");
            wait(200);
            verify(!line.visible, "a folder read has the same delay");
            wait(100);
            verify(line.visible);
            answerListing([]);
            verify(!line.visible);
        }

        function test_rows_and_names() {
            openAtRoots();
            openRoot("storage");
            var nov9 = thisYearNov9();
            var lastYear = new Date(nov9);
            lastYear.setFullYear(lastYear.getFullYear() - 2);
            answerListing([entry(storagePath, "Lists", true), entry(storagePath, "Radio", true), entry(storagePath, "dmr.csv", false, 4200000, nov9), entry(storagePath, "old.csv", false, 4200000, lastYear.getTime()), entry(storagePath, "fat.csv", false, 4200000, 0)]);
            waitForRendering(screen);
            var paths = ["Lists", "Radio", "dmr.csv", "old.csv", "fat.csv"].map(function (name) {
                return storagePath + "/" + name;
            });
            for (var i = 1; i < paths.length; ++i)
                verify(entryRow(paths[i - 1]).y < entryRow(paths[i]).y, "the rows keep the order given: " + paths[i]);
            compare(entryRow(paths[0]).Accessible.name, "Lists, folder");
            compare(entryRow(paths[2]).Accessible.name, "dmr.csv, file, 4.2 MB, Nov 9");
            compare(screen.entrySubtitle(screen.entries[2]), "4.2 MB · Nov 9");
            var year = new Date(lastYear).getFullYear();
            compare(screen.entrySubtitle(screen.entries[3]), "4.2 MB · Nov 9, " + year, "another year's stamp shows its year");
            compare(entryRow(paths[3]).Accessible.name, "old.csv, file, 4.2 MB, Nov 9, " + year);
            compare(screen.entrySubtitle(screen.entries[4]), "4.2 MB", "a file with no time shows its size only");
            compare(entryRow(paths[4]).Accessible.name, "fat.csv, file, 4.2 MB");
            compare(screen.entrySubtitle(screen.entries[0]), "", "a folder has no subtitle");
        }

        function test_states_data() {
            return [
                {tag: "empty", status: "ok", entries: [], hidden: 0, text: "This folder is empty.", problem: false},
                {tag: "filtered", status: "ok", entries: [], hidden: 3, text: "No CSV files here. Show all files lists the other 3 items.", problem: false},
                {tag: "filtered one", status: "ok", entries: [], hidden: 1, text: "No CSV files here. Show all files lists the other item.", problem: false},
                {tag: "filtered backup", kind: "backup", status: "ok", entries: [], hidden: 2, text: "No backups here. Show all files lists the other 2 items.", problem: false},
                {tag: "filtered replay", kind: "replay", status: "ok", entries: [], hidden: 2, text: "No replay files here. Show all files lists the other 2 items.", problem: false},
                {tag: "saved empty", rootKey: "saved", status: "ok", entries: [], hidden: 0, text: "Nothing saved here by this install of DSD-neo.", problem: false},
                {tag: "unreadable", status: "unreadable", text: "DSD-neo can't read this folder.", problem: true},
                {tag: "outside", status: "outside", text: "DSD-neo can't read this folder.", problem: true},
                {tag: "gone", status: "gone", text: "That folder is gone.", problem: true},
                {tag: "tooMany", status: "tooMany", text: "This folder has too many items to show.", problem: true},
                {tag: "timedOut", status: "timedOut", text: "DSD-neo could not read this in time. Try again.", problem: true, retry: true},
                {tag: "busy", status: "busy", text: "The storage is not answering. Try again in a moment.", problem: true, retry: true},
                {tag: "failed", status: "failed", text: "The storage is not answering. Try again in a moment.", problem: true, retry: true}
            ];
        }
        function test_states(data) {
            openAtRoots(null, data.kind);
            openRoot(data.rootKey || "storage");
            var before = screen.announcements;
            if (data.status === "ok")
                answerListing(data.entries, {hiddenByFilter: data.hidden});
            else
                answerStatus(data.status);
            var line = item("fileBrowserState");
            verify(line.visible);
            compare(line.text, data.text);
            compare(line.Accessible.role, data.problem ? Accessible.AlertMessage : Accessible.StaticText);
            compare(screen.announcements - before, data.problem ? 1 : 0, "a problem is read out once");
            compare(item("fileBrowserRetry").visible, data.retry === true);
            if (data.retry) {
                var listings = callsNamed("requestFolderListing").length;
                item("fileBrowserRetry").clicked();
                compare(callsNamed("requestFolderListing").length, listings + 1, "Try again reads the folder again");
            }
        }

        function test_removed_drive() {
            testContext.setFileAccess(access({granted: true}));
            openAtRoots([rootRecord("inbox"), rootRecord("removable:1", {title: "SanDisk"})]);
            openRoot("removable:1");
            var before = screen.announcements;
            var roots = callsNamed("requestFileRoots").length;
            answerStatus("gone");
            compare(screen.view, "roots", "a removed drive goes back to the locations");
            compare(callsNamed("requestFileRoots").length, roots + 1, "which are listed again");
            var notice = item("fileBrowserNotice");
            verify(notice.visible);
            compare(notice.text, "The drive was removed.");
            compare(notice.Accessible.role, Accessible.AlertMessage);
            compare(screen.announcements - before, 1, "read out once");
            compare(screen.announced, "The drive was removed.");

            // A drive pulled while its folder is open: the volumes change.
            answerRoots([rootRecord("inbox"), rootRecord("removable:1", {title: "SanDisk"})]);
            openRoot("removable:1");
            answerListing([entry("/storage/6187-25A8", "a.csv", false, 10, 0)]);
            testContext.setFileAccess(access({granted: true, rootsGeneration: 2}));
            compare(screen.view, "folder", "the folder stays while its drive is checked");
            answerRoots([rootRecord("inbox")], 8);
            compare(screen.view, "roots", "the drive is gone");
            compare(item("fileBrowserNotice").text, "The drive was removed.");
            answerRoots([rootRecord("inbox")], 8);
            verify(item("fileBrowserNotice").visible, "a list of the places without the drive keeps the notice");

            // Put back (mounted again): the next list of the places has it,
            // and the notice no longer holds.
            testContext.setFileAccess(access({granted: true, rootsGeneration: 3}));
            answerRoots([rootRecord("inbox"), rootRecord("removable:1", {title: "SanDisk"})], 9);
            verify(!item("fileBrowserNotice").visible, "the drive is back: the notice clears");
            compare(screen.notice, "");

            // A folder read can fail before Android reports the drive gone:
            // a list that still has it then is not the drive coming back.
            openRoot("removable:1");
            answerStatus("gone");
            answerRoots([rootRecord("inbox"), rootRecord("removable:1", {title: "SanDisk"})], 9);
            compare(item("fileBrowserNotice").text, "The drive was removed.", "the drive was never seen gone");
            testContext.setFileAccess(access({granted: true, rootsGeneration: 4}));
            answerRoots([rootRecord("inbox")], 10);
            verify(item("fileBrowserNotice").visible);
            testContext.setFileAccess(access({granted: true, rootsGeneration: 5}));
            answerRoots([rootRecord("inbox"), rootRecord("removable:1", {title: "SanDisk"})], 11);
            verify(!item("fileBrowserNotice").visible, "gone, then back: the notice clears");
        }

        function test_show_all_and_more() {
            openAtRoots();
            openRoot("storage");
            compare(last("requestFolderListing").filter, {kind: "csv", showAll: false});
            answerListing(manyEntries(storagePath, "a", 500), {snapshot: 21, more: true});
            item("fileBrowserShowAll").activate();
            compare(last("requestFolderListing").filter, {kind: "csv", showAll: true}, "Show all asks again for every file");
            compare(last("requestFolderListing").path, storagePath);
            answerListing(manyEntries(storagePath, "b", 500), {snapshot: 22, more: true});
            compare(screen.entries.length, 500);
            var list = item("fileBrowserList");
            list.positionViewAtEnd();
            var more = item("fileBrowserShowMore", list);
            verify(more.visible);
            more.clicked();
            var page = last("requestMorePage");
            compare(page.session, session());
            compare(page.snapshot, 22);
            compare(page.offset, 500);
            testContext.answerFolderListing(page.id, {status: "ok", snapshot: 22, offset: 500, entries: manyEntries(storagePath, "c", 500), more: true, hiddenByFilter: 0});
            compare(screen.entries.length, 1000, "the page is added below");
            compare(screen.entries[500].name, "c0.csv");
            compare(screen.entries[0].name, "b0.csv", "the first page stays");
            more.clicked();
            compare(last("requestMorePage").offset, 1000);
            testContext.answerFolderListing(last("requestMorePage").id, {status: "ok", snapshot: 22, offset: 1000, entries: manyEntries(storagePath, "d", 3), more: false, hiddenByFilter: 0});
            compare(screen.entries.length, 1003);
            verify(!more.visible, "nothing more to show");
        }

        function test_stale_and_superseded() {
            makeScreen();
            open();
            var olderRoots = last("requestFileRoots");
            screen.showRoots();
            verify(last("requestFileRoots").id !== olderRoots.id);
            testContext.answerFileRoots(olderRoots.id, {status: "ok", generation: 2, roots: [rootRecord("inbox")]});
            compare(screen.rootRows.length, 0, "an older roots answer is ignored");
            answerRoots(standardRoots());
            compare(screen.rootRows.length, 3);
            waitForRendering(screen);
            openRoot("storage");
            var first = last("requestFolderListing");
            back();
            compare(screen.view, "roots");
            verify(callsNamed("cancelFileRequest").some(function (call) {
                return call.id === first.id;
            }), "leaving a folder cancels its listing");
            testContext.answerFolderListing(first.id, {status: "ok", snapshot: 3, offset: 0, entries: [entry(storagePath, "late.csv", false, 1, 0)], more: false, hiddenByFilter: 0});
            compare(screen.view, "roots", "an answer for a listing given up is dropped");
            compare(screen.entries.length, 0);

            answerRoots(standardRoots());
            openRoot("storage");
            var older = last("requestFolderListing");
            item("fileBrowserShowAll").activate();
            var newer = last("requestFolderListing");
            verify(newer.id !== older.id);
            testContext.answerFolderListing(older.id, {status: "ok", snapshot: 3, offset: 0, entries: [entry(storagePath, "old.csv", false, 1, 0)], more: false, hiddenByFilter: 0});
            compare(screen.entries.length, 0, "an older listing's answer is ignored");
            answerListing([entry(storagePath, "new.csv", false, 1, 0)]);
            compare(screen.entries.length, 1);
            compare(screen.entries[0].name, "new.csv");

            var roots = callsNamed("requestFileRoots").length;
            item("fileBrowserShowAll").activate();
            answerStatus("stale");
            compare(screen.view, "roots", "a listing of an older drive set goes back to the locations");
            compare(callsNamed("requestFileRoots").length, roots + 1, "and lists them again");
        }

        // The places are being read again (a slow drive) when the user opens
        // a folder from the rows still showing: that late answer only checks
        // the folder's place.
        function test_late_roots_answer_after_a_folder_opened() {
            openAtRoots();
            screen.showRoots();
            var late = last("requestFileRoots");
            var before = screen.announcements;
            openRoot("storage");
            testContext.answerFileRoots(late.id, {status: "timedOut"});
            compare(screen.announcements, before, "no problem is reported for places the user left");
            compare(screen.view, "folder");
            verify(!item("fileBrowserRootsProblem").visible, "and none waits for the return");
            var line = item("fileBrowserReading");
            tryVerify(function () {
                return line.visible;
            }, 1000, "the folder's own reading line still shows");
            answerListing([entry(storagePath, "a.csv", false, 1, 0)]);
            tryVerify(function () {
                return window().activeFocusItem === entryRow(storagePath + "/a.csv");
            }, 1000, "and its listing still gives its first row the focus");

            // An ok answer then: the folder stays, on the newer generation.
            back();
            var again = last("requestFileRoots");
            openRoot("storage");
            testContext.answerFileRoots(again.id, {status: "ok", generation: 9, roots: standardRoots()});
            compare(screen.view, "folder", "its place is still there");
            answerListing([entry(storagePath, "b.csv", false, 1, 0)]);
            tryVerify(function () {
                return window().activeFocusItem === entryRow(storagePath + "/b.csv");
            }, 1000);
            entryRow(storagePath + "/b.csv").activate();
            compare(last("requestFileHandoff").generation, 9, "later requests carry the newer generation");
        }

        function folderDown(names) {
            var at = storagePath;
            for (var name of names) {
                answerListing([entry(at, name, true)]);
                waitForRendering(screen);
                entryRow(at + "/" + name).activate();
                at += "/" + name;
                compare(last("requestFolderListing").path, at);
            }
            return at;
        }
        function test_crumbs() {
            openAtRoots();
            openRoot("storage");
            folderDown(["Radio", "Lists"]);
            answerListing([]);
            compare(screen.crumbLine, "Device storage › Radio › Lists");
            var radio = item("fileBrowserCrumb1");
            compare(radio.Accessible.name, "Go to Radio");
            compare(item("fileBrowserCrumb0").Accessible.name, "Go to Device storage");
            radio.activate();
            compare(last("requestFolderListing").path, storagePath + "/Radio", "a crumb lists its folder");
            answerListing([]);
            compare(screen.crumbLine, "Device storage › Radio");

            // Long names at a large font on a phone: one line, each crumb no
            // wider than it, and the list below.
            root.width = 360;
            Ui.Theme.fontScale = 1.6;
            item("fileBrowserCrumb0").activate();
            folderDown(["Radio scanner captures from the county", "Talkgroup lists for the regional system"]);
            answerListing([entry(storagePath, "x.csv", false, 1, 0)]);
            waitForRendering(screen);
            var line = item("fileBrowserCrumbs");
            for (var i = 0; i < 3; ++i) {
                var crumb = findChild(line, "fileBrowserCrumb" + i);
                verify(crumb !== null, "crumb " + i + " exists");
                verify(crumb.height >= Ui.Theme.minimumTouchSize, "crumb " + i + " is a touch target");
                verify(crumb.width <= line.width + 0.5, "crumb " + i + " fits the screen");
            }
            compare(line.height, Math.max(Ui.Theme.minimumTouchSize, Ui.Theme.fontSize(14) * 1.4 + 12), "one line, however deep");
            var list = item("fileBrowserList");
            verify(list.mapToItem(screen, 0, 0).y >= line.mapToItem(screen, 0, line.height).y, "the list starts below the crumbs");
        }

        // A deep path of long names on a short, wide screen (a car's) at a
        // large font: the crumbs keep to one line that scrolls, showing the
        // open folder, and the folder keeps room for two rows.
        function test_crumbs_keep_the_list_room_data() {
            return [
                {tag: "640x360 at 1.6", width: 640, height: 360, scale: 1.6},
                {tag: "800x360 at 1.3", width: 800, height: 360, scale: 1.3}
            ];
        }
        function test_crumbs_keep_the_list_room(data) {
            root.width = data.width;
            root.height = data.height;
            Ui.Theme.fontScale = data.scale;
            openAtRoots();
            openRoot("storage");
            var names = ["Radio scanner captures from the county", "Talkgroup lists for the regional system", "Recordings made at the fire station", "Exported keys for the encrypted channels", "Backups from the previous head unit"];
            var at = folderDown(names);
            answerListing([entry(at, "First talkgroup list for the county.csv", false, 4200, 0), entry(at, "Second talkgroup list for the county.csv", false, 4200, 0), entry(at, "third.csv", false, 4200, 0)]);
            waitForRendering(screen);
            var line = item("fileBrowserCrumbs");
            compare(line.count, 6, "every step of the path");
            verify(!item("fileBrowserTitle").visible, "on a short screen the path takes the title's place");
            compare(line.mapToItem(screen, 0, 0).y, item("fileBrowserBack").mapToItem(screen, 0, 0).y + (item("fileBrowserBack").height - line.height) / 2, "beside Back");
            compare(line.height, Math.max(Ui.Theme.minimumTouchSize, Ui.Theme.fontSize(14) * 1.4 + 12), "on one line");
            verify(line.contentWidth > line.width, "longer than the screen: it scrolls");
            var list = item("fileBrowserList");
            var first = entryRow(at + "/First talkgroup list for the county.csv");
            var second = entryRow(at + "/Second talkgroup list for the county.csv");
            verify(list.height >= first.height + second.height, "the folder keeps room for two rows: " + list.height + " < " + (first.height + second.height));
            tryVerify(function () {
                var current = findChild(line, "fileBrowserCrumb5");
                if (!current)
                    return false;
                var x = current.mapToItem(line, 0, 0).x;
                return x >= -0.5 && x + current.width <= line.width + 0.5;
            }, 1000, "the open folder's crumb is in view");
            compare(screen.crumbLine, ["Device storage"].concat(names).join(" › "), "the whole path is still there");

            // A crumb scrolled out of view comes back into it with the focus.
            var top = findChild(line, "fileBrowserCrumb0");
            top.forceActiveFocus();
            tryVerify(function () {
                var x = top.mapToItem(line, 0, 0).x;
                return x >= -0.5 && x + top.width <= line.width + 0.5;
            }, 1000, "a focused crumb scrolls into view");
            top.activate();
            compare(last("requestFolderListing").path, storagePath, "and still goes there");
        }

        function test_back_levels() {
            var token = openAtRoots();
            var rejected = spy(Ui.FileBrowser, "rejected");
            openRoot("storage");
            folderDown(["Radio", "Lists"]);
            answerListing([]);
            back();
            compare(last("requestFolderListing").path, storagePath + "/Radio", "Back from Lists: Radio");
            answerListing([]);
            back();
            compare(last("requestFolderListing").path, storagePath, "then the location");
            answerListing([]);
            var roots = callsNamed("requestFileRoots").length;
            back();
            compare(screen.view, "roots", "then the locations");
            compare(callsNamed("requestFileRoots").length, roots + 1);
            compare(rejected.count, 0);
            var s = session();
            back();
            compare(rejected.count, 1, "Back at the locations backs out once");
            compare(rejected.signalArguments[0][0], token);
            compare(Ui.FileBrowser.request, null);
            verify(!screen.visible);
            compare(last("closeFileBrowserSession").session, s, "its session closes");
            verify(Ui.Navigation.modals.indexOf(screen) < 0, "and the browser is no longer a modal");
        }

        function test_keyboard() {
            openAtRoots();
            tryVerify(function () {
                return window().activeFocusItem === rootRow("inbox");
            }, 1000, "the first row takes the focus");
            item("fileBrowserBack").forceActiveFocus();
            keyClick(Qt.Key_Tab);
            var focused = window().activeFocusItem;
            verify(Ui.Navigation.contains(item("fileBrowserRootsPanel"), focused), "Tab reaches a row");
            var listings = callsNamed("requestFolderListing").length;
            keyClick(Qt.Key_Return);
            compare(callsNamed("requestFolderListing").length, listings + 1, "Return opens it");
            answerListing([entry(screen.path, "a.csv", false, 1, 0)]);
            tryVerify(function () {
                return window().activeFocusItem === entryRow(screen.path + "/a.csv");
            }, 1000, "the first file takes the focus");
            keyClick(Qt.Key_Return);
            compare(last("requestFileHandoff").reference, "file://" + screen.path + "/a.csv", "Return picks it");
            keyClick(Qt.Key_Escape);
            compare(screen.view, "roots", "Escape goes up");
        }

        function test_large_font() {
            root.width = 360;
            Ui.Theme.fontScale = 1.6;
            makeScreen();
            Ui.FileBrowser.open("replay", "Choose a replay file", true);
            answerRoots(standardRoots());
            waitForRendering(screen);
            var title = item("fileBrowserTitle");
            verify(title.lineCount > 1, "the title wraps at a large font");
            var header = title.parent;
            verify(header.height >= title.height, "the header grows with it");
            verify(title.x + title.width <= header.width + 0.5, "and holds it inside the screen");
            for (var key of ["inbox", "saved", "storage"])
                verify(rootRow(key).height >= Ui.Theme.minimumTouchSize, key + " is a touch target");
            var panel = item("fileBrowserRootsPanel");
            verify(panel.mapToItem(screen, 0, 0).y >= header.mapToItem(screen, 0, header.height).y, "rows start below the header");
            openRoot("storage");
            var name = "A recording from the county fire dispatch channel on a Saturday night.wav";
            answerListing([entry(storagePath, name, false, 1000, 0), entry(storagePath, "b.wav", false, 1000, 0)]);
            waitForRendering(screen);
            var row = entryRow(storagePath + "/" + name);
            verify(row.height >= Ui.Theme.minimumTouchSize);
            var text = findChild(row, "fileBrowserEntryName");
            verify(text.lineCount > 1, "a long name wraps, never elides");
            compare(text.elide, Text.ElideNone);
            verify(entryRow(storagePath + "/b.wav").height >= Ui.Theme.minimumTouchSize);
        }

        function test_handoff() {
            var token = openAtRoots();
            var picked = spy(Ui.FileBrowser, "picked");
            openRoot("storage");
            answerListing([entry(storagePath, "dmr.csv", false, 10, 0)]);
            waitForRendering(screen);
            var s = session();
            entryRow(storagePath + "/dmr.csv").activate();
            var handoff = last("requestFileHandoff");
            compare(handoff.session, s);
            compare(handoff.rootKey, "storage");
            compare(handoff.generation, 7, "the roots answer's generation");
            compare(handoff.reference, "file://" + storagePath + "/dmr.csv");

            testContext.answerFileHandoff(handoff.id, {ok: false, reference: "", name: "", reason: "notFile"});
            compare(picked.count, 0);
            compare(item("fileBrowserNotice").text, "DSD-neo can't open that file. Pick another one.");
            verify(Ui.FileBrowser.request !== null, "the browser stays");
            verify(screen.visible);

            entryRow(storagePath + "/dmr.csv").activate();
            var reference = "file:///storage/emulated/0/a%20b%231%25.csv";
            testContext.answerFileHandoff(last("requestFileHandoff").id, {ok: true, reference: reference, name: "a b#1%.csv", reason: ""});
            compare(picked.count, 1, "an accepted hand-off picks once");
            compare(picked.signalArguments[0][0], token);
            compare(picked.signalArguments[0][1], reference, "the answer's reference, as it came");
            compare(picked.signalArguments[0][2], "a b#1%.csv", "and its name, never decoded");
            compare(Ui.FileBrowser.request, null);
            compare(last("closeFileBrowserSession").session, s, "an accepted pick closes its session");
        }

        // A slow drive: the tap says it is opening the file, and taps on
        // files wait for its answer; a problem answer ends it with its line.
        function test_handoff_opening_line_data() {
            return [
                {tag: "timedOut", reason: "timedOut", text: "DSD-neo could not read this in time. Try again."},
                {tag: "busy", reason: "busy", text: "The storage is not answering. Try again in a moment."}
            ];
        }
        function test_handoff_opening_line(data) {
            openAtRoots();
            openRoot("storage");
            answerListing([entry(storagePath, "dmr.csv", false, 10, 0), entry(storagePath, "b.csv", false, 10, 0)]);
            waitForRendering(screen);
            var line = item("fileBrowserOpening");
            entryRow(storagePath + "/dmr.csv").activate();
            var handoff = last("requestFileHandoff");
            verify(screen.opening, "the file is being opened");
            wait(300);
            verify(line.visible, "a hand-off that takes a moment says so");
            compare(line.text, "Opening dmr.csv…");
            verify(!item("fileBrowserReading").visible, "not as a folder read");

            var cancels = callsNamed("cancelFileRequest").length;
            entryRow(storagePath + "/dmr.csv").activate();
            entryRow(storagePath + "/b.csv").activate();
            compare(callsNamed("requestFileHandoff").length, 1, "taps on files while it runs ask nothing");
            compare(callsNamed("cancelFileRequest").length, cancels, "and cancel nothing");
            compare(last("requestFileHandoff").id, handoff.id);

            testContext.answerFileHandoff(handoff.id, {ok: false, reference: "", name: "", reason: data.reason});
            verify(!line.visible, "the answer ends it");
            verify(!screen.opening);
            compare(item("fileBrowserNotice").text, data.text, "with its own line");
            entryRow(storagePath + "/b.csv").activate();
            compare(callsNamed("requestFileHandoff").length, 2, "a tap after it asks again");
            compare(last("requestFileHandoff").reference, "file://" + storagePath + "/b.csv");

            // Leaving the folder gives the check up, line and all.
            back();
            verify(!screen.opening);
            verify(!line.visible);
        }

        // The device's drives changed between the listing and the tap (a
        // drive plugged in): the places are read again and the file is
        // checked once more, quietly; a second stale answer says so.
        function test_handoff_stale_retries_once() {
            var token = openAtRoots();
            var picked = spy(Ui.FileBrowser, "picked");
            openRoot("storage");
            answerListing([entry(storagePath, "dmr.csv", false, 10, 0)]);
            waitForRendering(screen);
            entryRow(storagePath + "/dmr.csv").activate();
            var roots = callsNamed("requestFileRoots").length;
            testContext.answerFileHandoff(last("requestFileHandoff").id, {ok: false, reference: "", name: "", reason: "stale"});
            compare(screen.view, "folder", "the folder stays");
            compare(screen.notice, "", "nothing is said yet");
            verify(screen.opening, "the file is still being opened");
            compare(callsNamed("requestFileRoots").length, roots + 1, "the places are read again");
            compare(callsNamed("requestFileHandoff").length, 1, "the check waits for them");
            testContext.answerFileRoots(last("requestFileRoots").id, {status: "ok", generation: 9, roots: standardRoots()});
            compare(screen.view, "folder");
            compare(callsNamed("requestFileHandoff").length, 2, "then the file is checked again");
            var again = last("requestFileHandoff");
            compare(again.generation, 9, "on the new generation");
            compare(again.reference, "file://" + storagePath + "/dmr.csv", "the same file");
            testContext.answerFileHandoff(again.id, {ok: true, reference: again.reference, name: "dmr.csv", reason: ""});
            compare(picked.count, 1, "and picked");
            compare(picked.signalArguments[0][0], token);

            // Stale twice: once is a retry, the second is said.
            open();
            answerRoots(standardRoots());
            waitForRendering(screen);
            openRoot("storage");
            answerListing([entry(storagePath, "dmr.csv", false, 10, 0)]);
            waitForRendering(screen);
            entryRow(storagePath + "/dmr.csv").activate();
            testContext.answerFileHandoff(last("requestFileHandoff").id, {ok: false, reference: "", name: "", reason: "stale"});
            testContext.answerFileRoots(last("requestFileRoots").id, {status: "ok", generation: 10, roots: standardRoots()});
            testContext.answerFileHandoff(last("requestFileHandoff").id, {ok: false, reference: "", name: "", reason: "stale"});
            compare(screen.view, "roots", "the second stale answer goes back to the places");
            compare(item("fileBrowserNotice").text, "DSD-neo can't open that file. Pick another one.");
            verify(!screen.opening);

            // The places cannot be read for the retry: that problem is said.
            answerRoots(standardRoots(), 10);
            openRoot("storage");
            answerListing([entry(storagePath, "dmr.csv", false, 10, 0)]);
            waitForRendering(screen);
            entryRow(storagePath + "/dmr.csv").activate();
            testContext.answerFileHandoff(last("requestFileHandoff").id, {ok: false, reference: "", name: "", reason: "stale"});
            var handoffs = callsNamed("requestFileHandoff").length;
            testContext.answerFileRoots(last("requestFileRoots").id, {status: "timedOut"});
            compare(callsNamed("requestFileHandoff").length, handoffs, "no check without the places");
            compare(item("fileBrowserNotice").text, "DSD-neo could not read this in time. Try again.");
            verify(!screen.opening);
            compare(screen.view, "folder");
        }

        // A name that is not valid UTF-8 is never listed; the folder says how
        // many it left out.
        function test_names_it_cannot_read() {
            openAtRoots();
            openRoot("storage");
            answerListing([entry(storagePath, "a.csv", false, 1, 0)], {hiddenByName: 1});
            var line = item("fileBrowserHiddenByName");
            verify(line.visible);
            compare(line.text, "1 item here isn't listed because DSD-neo can't read its name. Rename it on a computer to see it.");
            item("fileBrowserShowAll").activate();
            verify(!line.visible, "not while the folder is read");
            answerListing([], {hiddenByName: 2});
            compare(line.text, "2 items here aren't listed because DSD-neo can't read their names. Rename them on a computer to see them.");
            verify(!item("fileBrowserState").visible, "the line says why the folder shows nothing");
            back();
            answerRoots(standardRoots());
            openRoot("inbox");
            answerListing([], {hiddenByName: 1});
            verify(!item("fileBrowserInboxHelp").visible, "an inbox holding such a file is not empty");
            verify(item("fileBrowserHiddenByName").visible);
            back();
            verify(!item("fileBrowserHiddenByName").visible, "only in the folder it is about");
        }

        // Settings may close again before its "settingsOpened" answer is read
        // (an OEM screen that finishes at once): the return still shows how
        // else to allow access, in either order.
        function test_settings_return_before_its_answer_data() {
            return [
                {tag: "answer first", returnFirst: false},
                {tag: "return first", returnFirst: true}
            ];
        }
        function test_settings_return_before_its_answer(data) {
            testContext.setFileAccess(access({apiLevel: 31}));
            openAtRoots([rootRecord("inbox"), rootRecord("storage", {state: "needsAccess"})]);
            var help = item("fileBrowserAccessHelp");
            item("fileBrowserAllowAccess").clicked();
            var id = last("requestFileAccess").id;
            if (data.returnFirst)
                screen.applicationActivated();
            verify(!help.visible, "nothing before the answer");
            testContext.answerFileAccess(id, "settingsOpened");
            if (!data.returnFirst) {
                verify(!help.visible, "nothing while Settings is open");
                screen.applicationActivated();
            }
            verify(help.visible, "back without the grant: the computer route");
            verify(item("fileBrowserAppInfoStep").visible);

            // A plain activation with no request out shows nothing.
            Ui.FileBrowser.dismiss(Ui.FileBrowser.request.token);
            open();
            answerRoots([rootRecord("inbox"), rootRecord("storage", {state: "needsAccess"})]);
            screen.applicationActivated();
            verify(!item("fileBrowserAccessHelp").visible, "no return without a request");
        }

        // While DSD-neo closes for a restart, nothing new can be picked: a
        // pick would start a copy the close would cut short.
        function test_no_pick_while_closing() {
            testContext.setFileAccess(access({granted: true}));
            openAtRoots([rootRecord("inbox"), rootRecord("removable:6187-25A8", {title: "SanDisk", path: "/mnt/media_rw/6187-25A8", state: "needsRestart"})]);
            var other = item("fileBrowserOtherApp");
            verify(other.enabled);
            item("fileBrowserClose:removable:6187-25A8").clicked();
            item("confirmActionButton", item("fileBrowserCloseConfirm")).clicked();
            verify(restartCloser.begin(), "the close begins");
            verify(!other.enabled, "another app's picker is not offered while closing");
            var token = Ui.FileBrowser.request.token;
            other.activate();
            compare(Ui.FileBrowser.request.token, token, "and does nothing");
            openRoot("inbox");
            answerListing([entry(inboxPath, "a.csv", false, 10, 0), entry(inboxPath, "Lists", true)]);
            waitForRendering(screen);
            var notice = item("fileBrowserClosingNotice");
            verify(notice.visible, "the folder says why");
            compare(notice.text, "DSD-neo is closing, so files can't be picked now.");
            var row = entryRow(inboxPath + "/a.csv");
            verify(row.opacity < 1, "files show dimmed");
            row.activate();
            compare(callsNamed("requestFileHandoff").length, 0, "a file tapped while closing is not opened");
            compare(entryRow(inboxPath + "/Lists").opacity, 1, "folders still open");
            back();
            answerRoots([rootRecord("inbox"), rootRecord("removable:6187-25A8", {title: "SanDisk", path: "/mnt/media_rw/6187-25A8", state: "needsRestart"})]);

            // A copy that began while listening stopped: the close gives way.
            testContext.finishClose("refused");
            compare(item("fileBrowserCloseProblem:removable:6187-25A8").text, "Wait for the copy to finish, then close DSD-neo.");
            verify(other.enabled, "the close over, picks work again");
            openRoot("inbox");
            answerListing([entry(inboxPath, "a.csv", false, 10, 0)]);
            waitForRendering(screen);
            verify(!item("fileBrowserClosingNotice").visible);
            entryRow(inboxPath + "/a.csv").activate();
            compare(callsNamed("requestFileHandoff").length, 1);
        }

        function test_access_rows() {
            openAtRoots([rootRecord("inbox"), rootRecord("saved"), rootRecord("storage", {state: "needsAccess"})]);
            verify(findChild(screen, "fileBrowserRoot:storage") === null || !findChild(screen, "fileBrowserRoot:storage").visible, "storage waits behind the access row");
            var block = item("fileBrowserAccess");
            verify(block.visible);
            compare(item("fileBrowserAccessTitle").text, "Download and USB drives");
            compare(item("fileBrowserAccessSubtitle").text, "Needs All files access");
            compare(item("fileBrowserAccessRevoke").text, "Turning All files access off later closes DSD-neo (Android does that).");
            var allow = item("fileBrowserAllowAccess");
            compare(allow.text, "Allow access");
            allow.clicked();
            compare(last("requestFileAccess").session, session());
            var help = item("fileBrowserAccessHelp");
            verify(!help.visible);
            testContext.answerFileAccess(last("requestFileAccess").id, "noRoute");
            verify(help.visible, "no Settings screen: the computer route");
            compare(item("fileBrowserAdbCommandText0").text, adbAccessLine);
            verify(!item("fileBrowserAppInfoStep").visible, "Android 13 and later have no App info step");
            item("fileBrowserCopyAdb").clicked();
            compare(testContext.copiedTexts(), [adbAccessLine], "Copy copies the exact line");

            // Settings opened, and the user came back without turning it on.
            Ui.FileBrowser.dismiss(Ui.FileBrowser.request.token);
            testContext.setFileAccess(access({apiLevel: 31}));
            open();
            answerRoots([rootRecord("inbox"), rootRecord("storage", {state: "needsAccess"})]);
            help = item("fileBrowserAccessHelp");
            item("fileBrowserAllowAccess").clicked();
            testContext.answerFileAccess(last("requestFileAccess").id, "settingsOpened");
            verify(!help.visible, "nothing more while Settings is open");
            testContext.setFileAccess(access({apiLevel: 31, rootsGeneration: 2}));
            verify(help.visible, "back without the grant: the computer route");
            verify(item("fileBrowserAppInfoStep").visible, "Android 11 and 12 also name App info");
            compare(item("fileBrowserAppInfoStep").text, "In App info, open Permissions › Files and media, then allow management of all files.");

            // The grant: the locations are listed again, without the access row.
            var roots = callsNamed("requestFileRoots").length;
            testContext.setFileAccess(access({apiLevel: 31, granted: true, rootsGeneration: 3}));
            compare(callsNamed("requestFileRoots").length, roots + 1, "a grant lists the locations again");
            verify(!item("fileBrowserAccess").visible);

            // Android 10: the runtime dialog.
            Ui.FileBrowser.dismiss(Ui.FileBrowser.request.token);
            testContext.setFileAccess(access({declared: false, route: "permission", apiLevel: 29}));
            open();
            answerRoots([rootRecord("inbox"), rootRecord("storage", {state: "needsAccess"})]);
            compare(item("fileBrowserAccessTitle").text, "Device storage");
            compare(item("fileBrowserAllowAccess").text, "Allow access");
            verify(!item("fileBrowserAccessRevoke").visible, "no All files access to turn off");
            item("fileBrowserAllowAccess").clicked();
            testContext.answerFileAccess(last("requestFileAccess").id, "blocked");
            var blocked = item("fileBrowserAccessBlocked");
            verify(blocked.visible);
            compare(blocked.text, "Allow Files and media for DSD-neo in App info, then come back.");
            item("fileBrowserAllowAccess").clicked();
            testContext.answerFileAccess(last("requestFileAccess").id, "noRoute");
            verify(!item("fileBrowserAccessHelp").visible, "no computer route on Android 10");
            verify(item("fileBrowserAccessBlocked").visible, "App info is the way left");

            item("fileBrowserAllowAccess").clicked();
            testContext.answerFileAccess(last("requestFileAccess").id, "failed");
            verify(item("fileBrowserAccessBlocked").visible, "a dialog that could not show points to App info too");

            // "Don't allow" once: the dialog can show again, so Allow access
            // is the way, not App info.
            Ui.FileBrowser.dismiss(Ui.FileBrowser.request.token);
            open();
            answerRoots([rootRecord("inbox"), rootRecord("storage", {state: "needsAccess"})]);
            var denied = item("fileBrowserAccessDenied");
            verify(!denied.visible);
            item("fileBrowserAllowAccess").clicked();
            testContext.answerFileAccess(last("requestFileAccess").id, "denied");
            verify(denied.visible, "a refused dialog says Allow access asks again");
            compare(denied.text, "Access was not allowed. Allow access asks again.");
            verify(!item("fileBrowserAccessBlocked").visible, "and does not send the user to App info");
            item("fileBrowserAllowAccess").clicked();
            verify(!denied.visible, "asking again clears it");
            testContext.answerFileAccess(last("requestFileAccess").id, "blocked");
            verify(item("fileBrowserAccessBlocked").visible, "refused for good: App info");
            verify(!denied.visible);

            // A build without All files access has no access row on Android 11 and later.
            Ui.FileBrowser.dismiss(Ui.FileBrowser.request.token);
            testContext.setFileAccess(access({declared: false}));
            open();
            answerRoots([rootRecord("inbox"), rootRecord("storage", {state: "needsAccess"})]);
            verify(!item("fileBrowserAccess").visible);
        }

        function test_inbox_empty() {
            openAtRoots();
            openRoot("inbox");
            compare(last("requestFolderListing").path, inboxPath);
            answerListing([]);
            var help = item("fileBrowserInboxHelp");
            verify(help.visible);
            verify(!item("fileBrowserState").visible, "the help stands for the empty line");
            var text = inboxPath + "\nadb push FILE " + inboxPath + "/";
            compare(item("fileBrowserInboxCommandText0").text, inboxPath, "the path in a block of its own");
            compare(item("fileBrowserInboxCommandText1").text, "adb push FILE " + inboxPath + "/", "and the adb line in another");
            item("fileBrowserCopyInbox").clicked();
            compare(testContext.copiedTexts(), [text], "Copy copies the path and the adb line");
            verify(findChild(help, "fileBrowserInboxCommand") !== null);

            back();
            answerRoots(standardRoots());
            openRoot("inbox");
            answerListing([], {hiddenByFilter: 2});
            verify(!help.visible, "an inbox holding other files says so instead");
            compare(item("fileBrowserState").text, "No CSV files here. Show all files lists the other 2 items.");
        }

        function test_restart_row_data() {
            return [
                {tag: "nothing ends", active: false, kind: "replay", pending: false, message: "Then open DSD-neo again."},
                {tag: "listening", active: true, kind: "replay", pending: false, message: "Listening stops.\nThen open DSD-neo again."},
                {tag: "everything", active: true, kind: "csv", pending: true, draft: "add", message: "Listening stops.\nThis import is dropped.\nThe system you're adding is dropped.\nYour backup is restored when DSD-neo opens again.\nThen open DSD-neo again."},
                {tag: "import", active: false, kind: "csv", pending: false, message: "This import is dropped.\nThen open DSD-neo again."},
                {tag: "restore", active: false, kind: "backup", pending: false, message: "This restore is dropped.\nThen open DSD-neo again."},
                {tag: "adding a system", active: false, kind: "replay", pending: false, draft: "add", message: "The system you're adding is dropped.\nThen open DSD-neo again."},
                {tag: "editing a system", active: true, kind: "replay", pending: false, draft: "edit", message: "Listening stops.\nYour changes to this system are dropped.\nThen open DSD-neo again."},
                {tag: "import into an edit", active: false, kind: "csv", pending: false, draft: "edit", message: "This import is dropped.\nYour changes to this system are dropped.\nThen open DSD-neo again."},
                {tag: "adding a scan list", active: false, kind: "csv", pending: false, scanList: "add", message: "This import is dropped.\nThe scan list you're adding is dropped.\nThen open DSD-neo again."},
                {tag: "editing a scan list", active: true, kind: "csv", pending: false, scanList: "edit", message: "Listening stops.\nThis import is dropped.\nYour changes to this scan list are dropped.\nThen open DSD-neo again."},
                {tag: "a system and a scan list", active: false, kind: "csv", pending: false, draft: "add", scanList: "edit", message: "This import is dropped.\nThe system you're adding is dropped.\nYour changes to this scan list are dropped.\nThen open DSD-neo again."}
            ];
        }
        function test_restart_row(data) {
            testContext.setFileAccess(access({granted: true}));
            openAtRoots([rootRecord("inbox"), rootRecord("removable:6187-25A8", {title: "SanDisk", path: "/mnt/media_rw/6187-25A8", state: "needsRestart"})], data.kind);
            var key = "removable:6187-25A8";
            var state = item("fileBrowserRootState:" + key);
            verify(state.visible);
            var close = item("fileBrowserClose:" + key);
            compare(close.text, "Close DSD-neo");
            compare(screen.rootSubtitle(screen.rootRows[1]), "DSD-neo can open this drive after it restarts.");
            var closed = spy(screen, "closeConfirmed");
            var dialog = item("fileBrowserCloseConfirm");

            testContext.setCloseRefused(true);
            close.clicked();
            verify(!dialog.visible, "a refused close asks nothing");
            compare(item("fileBrowserCloseProblem:" + key).text, "Wait for the copy to finish, then close DSD-neo.");
            testContext.setCloseRefused(false);

            testContext.setHostReading("sessionActive", data.active);
            testContext.setBackup("restorePending", data.pending);
            screen.unsavedDraft = function () {
                return data.draft || "";
            };
            screen.unsavedScanList = function () {
                return data.scanList || "";
            };
            close.clicked();
            verify(dialog.visible, "the close asks first");
            compare(dialog.message, data.message, "and names what ends");
            compare(Ui.Navigation.topModal, dialog);
            item("confirmActionButton", dialog).clicked();
            compare(closed.count, 1, "confirmed");
            verify(Ui.FileBrowser.request !== null, "the browser stays while the app closes");
            var closing = item("fileBrowserClosing:" + key);
            verify(!closing.visible, "nothing is closing yet: Main begins the close");
            verify(restartCloser.begin(), "the close begins");
            verify(closing.visible, "and says so while it runs");
            compare(closing.text, "Closing DSD-neo…");
            verify(!close.enabled, "one close at a time");

            testContext.finishClose("quitFailed");
            verify(!closing.visible);
            compare(item("fileBrowserCloseProblem:" + key).text, "DSD-neo could not close itself. Close it from Android's recent apps, then open it again.");
            verify(Ui.FileBrowser.request !== null, "a failed close keeps the browser and its request");
            verify(screen.visible);
        }

        function test_sessions_close_every_way() {
            makeScreen();
            // Supersede: a newer request closes the older session.
            open();
            var first = session();
            open();
            var second = session();
            verify(second !== first);
            compare(callsNamed("closeFileBrowserSession").map(function (c) {
                return c.session;
            }), [first], "a superseded request's session closes");
            // The owner ends it.
            Ui.FileBrowser.dismiss(Ui.FileBrowser.request.token);
            compare(last("closeFileBrowserSession").session, second);
            // Another app's picker.
            open();
            var third = session();
            answerRoots(standardRoots());
            waitForRendering(screen);
            item("fileBrowserOtherApp").activate();
            compare(Ui.FileBrowser.request, null);
            compare(last("closeFileBrowserSession").session, third, "Choose with another app closes it");
            // The screen goes away with a request still showing.
            open();
            var fourth = session();
            screen.destroy();
            wait(0);
            compare(last("closeFileBrowserSession").session, fourth, "a screen that goes away closes its session");
            compare(callsNamed("openFileBrowserSession").length, callsNamed("closeFileBrowserSession").length, "every session opened is closed");
        }

        function test_focus_returns_to_the_opener() {
            makeScreen();
            var opener = createTemporaryObject(openerComponent, root);
            opener.forceActiveFocus();
            open();
            verify(!opener.activeFocus, "the browser takes the focus");
            answerRoots(standardRoots());
            back();
            compare(Ui.FileBrowser.request, null);
            verify(opener.activeFocus, "Back out: the focus returns to what opened the browser");

            // An opener gone from the screen meanwhile: the focus stays usable, not on it.
            opener.forceActiveFocus();
            open();
            opener.visible = false;
            back();
            verify(!opener.activeFocus);
        }

        function test_sheet_opened_by_the_pick_keeps_focus() {
            makeScreen();
            var opener = createTemporaryObject(openerComponent, root);
            var sheet = createTemporaryObject(sheetComponent, root);
            opener.forceActiveFocus();
            var token = open("backup");
            var picked = spy(Ui.FileBrowser, "picked");
            // As the backup's unlock sheet does: it opens and puts the focus on its field.
            var opensSheet = function () {
                sheet.open();
                findChild(sheet, "sheetButton").forceActiveFocus();
            };
            Ui.FileBrowser.picked.connect(opensSheet);
            answerRoots(standardRoots());
            openRoot("storage");
            answerListing([entry(storagePath, "b.dsdbak", false, 10, 0)]);
            waitForRendering(screen);
            entryRow(storagePath + "/b.dsdbak").activate();
            testContext.answerFileHandoff(last("requestFileHandoff").id, {ok: true, reference: "file:///storage/emulated/0/b.dsdbak", name: "b.dsdbak", reason: ""});
            Ui.FileBrowser.picked.disconnect(opensSheet);
            compare(picked.count, 1);
            compare(Ui.Navigation.topModal, sheet, "the pick opened a sheet");
            wait(0);
            wait(0);
            compare(window().activeFocusItem, findChild(sheet, "sheetButton"), "the sheet keeps the focus it gave itself");
            verify(!opener.activeFocus, "the opener does not take it back");
            sheet.visible = false;
        }

        function test_main_integration() {
            testContext.useLifecycleHost(true);
            testContext.setFileBrowserSupported(true);
            testContext.resetHostPlayback();
            var oldOnboarding = prefs.onboardingDone;
            prefs.onboardingDone = true;
            var loader = createTemporaryObject(mainComponent, root);
            tryCompare(loader, "status", Loader.Ready);
            var app = loader.item;
            var browser = item("fileBrowserScreen", app);
            var backupFlow = item("backupFlow", app);
            verify(browser.z > backupFlow.z, "above the backup flow");
            verify(!browser.visible);
            app.importsOpen = true;
            var imports = item("importsScreen", app);
            tryCompare(imports, "visible", true);

            testContext.setFileAccess(access({granted: true}));
            open("csv");
            compare(Ui.Navigation.topModal, browser, "the browser is the top modal");
            verify(browser.visible);
            verify(imports.visible, "the owner stays visible underneath");
            verify(uiController.autoStartBlocked, "automatic starts wait");

            answerRoots([rootRecord("inbox"), rootRecord("removable:1", {state: "needsRestart"})]);
            waitForRendering(browser);

            // A sheet drawn under the browser (an owner's) that opens while it
            // shows waits under it: the browser keeps the top and the focus.
            var closeRow = item("fileBrowserClose:removable:1", browser);
            var ownerSheet = createTemporaryObject(sheetComponent, imports);
            ownerSheet.open();
            wait(0);
            compare(Ui.Navigation.topModal, browser, "the browser stays the top modal over a sheet drawn under it");
            verify(Ui.Navigation.modals.indexOf(ownerSheet) >= 0, "the sheet waits under it");
            verify(Ui.Navigation.allows(closeRow), "the browser's controls still work");
            verify(Ui.Navigation.contains(browser, browser.Window.window.activeFocusItem), "and keep the focus");
            ownerSheet.visible = false;
            compare(Ui.Navigation.topModal, browser);
            // A Main-level sheet is drawn above the browser: it is what the
            // user sees, so it takes the top until it closes.
            var failureSheet = item("failureDetails", app);
            verify(failureSheet.z > browser.z, "drawn above the browser");
            failureSheet.open();
            wait(0);
            compare(Ui.Navigation.topModal, failureSheet, "a sheet drawn above keeps the top");
            Ui.Navigation.back(browser.Window.window);
            verify(!failureSheet.visible, "Back closes it");
            compare(Ui.Navigation.topModal, browser, "and the browser has the top again");
            verify(Ui.Navigation.allows(closeRow));

            // What a close would drop: a draft the system editor holds.
            var wizard = item("wizardScreen", app);
            compare(browser.unsavedDraft(), "", "no draft while the editor is closed");
            wizard.openForAdd(false);
            app.wizardOpen = true;
            compare(browser.unsavedDraft(), "", "a new system not changed yet loses nothing");
            wizard.freqText = "852.0125";
            compare(browser.unsavedDraft(), "add", "a new system with changes");
            wizard.editRow = 0;
            compare(browser.unsavedDraft(), "edit", "a saved system with changes");
            wizard.editRow = -1;
            app.wizardOpen = false;
            compare(browser.unsavedDraft(), "");

            // And one the scan-list editor holds, told apart from the system's.
            var scanEditor = item("scanListScreen", app);
            compare(browser.unsavedScanList(), "", "no scan list while its editor is closed");
            scanEditor.openFor(-1);
            app.scanListOpen = true;
            compare(browser.unsavedScanList(), "", "a new scan list not changed yet loses nothing");
            scanEditor.setDraft("name", "Unsaved list");
            compare(browser.unsavedScanList(), "add", "a new scan list with changes");
            scanEditor.editUid = "saved-list";
            compare(browser.unsavedScanList(), "edit", "a saved scan list with changes");
            wizard.openForAdd(false);
            app.wizardOpen = true;
            wizard.freqText = "852.0125";
            compare(browser.unsavedDraft(), "add", "both editors are reported");
            compare(browser.unsavedScanList(), "edit");
            app.wizardOpen = false;
            scanEditor.editUid = "";
            app.scanListOpen = false;
            compare(browser.unsavedScanList(), "");

            app.pendingStart = {sourceType: "usb"};
            var begins = testContext.closeBegins();
            item("fileBrowserClose:removable:1", browser).clicked();
            item("confirmActionButton", item("fileBrowserCloseConfirm", browser)).clicked();
            compare(app.pendingStart, null, "a confirmed close drops the waiting start");
            compare(testContext.closeBegins(), begins + 1, "and begins the close");
            verify(restartCloser.active);
            // With nothing else holding them, only the close holds the starts.
            Ui.FileBrowser.dismiss(Ui.FileBrowser.request.token);
            app.importsOpen = false;
            tryCompare(imports, "visible", false);
            compare(Ui.Navigation.modals.length, 0);
            verify(uiController.autoStartBlocked, "automatic starts wait while the app closes");
            testContext.finishClose("quitFailed");
            verify(!uiController.autoStartBlocked, "a failed close lets them start again");
            prefs.onboardingDone = oldOnboarding;
        }
    }
}
