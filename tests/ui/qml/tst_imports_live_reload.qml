// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest

// Issue #748: "Update from file" and the RadioReference refresh replace a talkgroup list or channel map in place,
// and the running session may be using that very file. The screen asks the decoder to reload the file whenever a
// session runs, and the decoder reloads it only if the file is its own: the saved system the session started from
// does not decide (a manual Apply or a restart outdates it). A new import, a cancelled pick and a file with no
// usable rows ask for nothing.
Item {
    id: root

    width: 420
    height: 900

    Loader {
        id: screenLoader

        anchors.fill: parent
        source: uiDir + "/ImportsScreen.qml"
    }

    // Stands in for the RadioReference model: the screen hears a refresh's outcome only through this signal.
    QtObject {
        id: fakeRefresh

        signal refreshFinished(int row, var result)
        property bool available: true
        property bool busy: false
        property string errorText: ""
        property int refreshes: 0

        function refreshRow(row) {
            refreshes++;
            return true;
        }
    }

    TestCase {
        id: tc

        name: "ImportsLiveReload"
        when: windowShown

        property var screen: null
        property string groupPath: ""
        property string chanPath: ""

        function initTestCase() {
            tc.screen = screenLoader.item;
            verify(tc.screen !== null, "ImportsScreen.qml failed to load");
        }

        function init() {
            var groups = testContext.writeFixtureCsv("live-groups.csv", "id,mode,name\n1001,A,Dispatch\n");
            var imported = importedFiles.importFile(groups, "live-groups.csv", "group");
            verify(imported.ok && imported.error === "", "the talkgroup fixture did not import");
            tc.groupPath = imported.path;
            var map = testContext.writeFixtureCsv("live-map.csv", "channel,frequency\n1,851012500\n");
            var mapped = importedFiles.importFile(map, "live-map.csv", "chan");
            verify(mapped.ok && mapped.error === "", "the channel map fixture did not import");
            tc.chanPath = mapped.path;
            testContext.resetCommands();
            tc.screen.notice = "";
            tc.screen.noticeIsProblem = false;
        }

        function cleanup() {
            testContext.setHostRunning(false);
            testContext.useRadioReferenceModel(null);
            testContext.resetCommands();
            for (var k = importedFiles.count; k > 0 && importedFiles.count > 0; k--)
                importedFiles.remove(0);
            compare(importedFiles.count, 0, "the library did not empty");
        }

        // Taps Update from file on @p path's row and picks @p source, as the user does; null cancels the picker.
        function update(path, source) {
            tc.screen.actionRow = importedFiles.rowForPath(path);
            verify(tc.screen.actionRow >= 0, "no library row for " + path);
            var sheet = findChild(tc.screen, "importsActionSheet");
            sheet.visible = true;
            var button = findChild(sheet, "updateFromFileButton");
            verify(button !== null && button.visible, "Update from file is missing");
            button.clicked();
            var picker = findChild(findChild(tc.screen, "importsCsvImport"), "csvPrimaryPicker");
            verify(picker !== null);
            if (source === null) {
                picker.reject();
                return;
            }
            picker.selectedFile = "file://" + source;
            picker.accepted();
            picker.close();
        }

        function test_update_reloads_the_file_in_a_running_session_data() {
            return [
                {tag: "talkgroups", kind: "group", name: "live-groups-2.csv",
                 text: "id,mode,name\n1001,A,Dispatch\n1002,A,Fireground\n",
                 want: "reloadGroupList", notice: "Updated · 2 talkgroups · sent to the running session"},
                {tag: "channel map", kind: "chan", name: "live-map-2.csv",
                 text: "channel,frequency\n1,851012500\n2,851512500\n",
                 want: "reloadChannelMap", notice: "Updated · 2 channels · sent to the running session"},
                // A talkgroup list with no rows at all is a list emptied on purpose, which the decoder takes.
                {tag: "empty talkgroup list", kind: "group", name: "live-groups-empty.csv", text: "id,mode,name\n",
                 want: "reloadGroupList", notice: "Updated · empty list · sent to the running session"}
            ];
        }
        function test_update_reloads_the_file_in_a_running_session(data) {
            var path = data.kind === "group" ? tc.groupPath : tc.chanPath;
            testContext.setHostRunning(true);
            update(path, testContext.writeFixtureCsv(data.name, data.text));
            // The path the session was started with, as an expected path: never an import, which would install the
            // file whatever the session uses.
            compare(commands.listCommands(), [data.want + " " + path]);
            compare(tc.screen.notice, data.notice);
            verify(!tc.screen.noticeIsProblem);
        }

        function test_update_without_a_session_sends_nothing() {
            update(tc.groupPath, testContext.writeFixtureCsv("idle-groups.csv", "id,mode,name\n1001,A,Dispatch\n"));
            compare(commands.listCommands(), []);
            compare(tc.screen.notice, "Updated · 1 talkgroup");
            verify(!tc.screen.noticeIsProblem);
        }

        function test_update_says_when_the_decoder_refuses() {
            testContext.setHostRunning(true);
            commands.setListCommandsAccepted(false);
            update(tc.groupPath, testContext.writeFixtureCsv("busy-groups.csv", "id,mode,name\n1001,A,Dispatch\n"));
            compare(commands.listCommands(), ["reloadGroupList " + tc.groupPath]);
            compare(tc.screen.notice, "Updated, but the decoder is not accepting commands");
            verify(tc.screen.noticeIsProblem);
        }

        function test_no_reload_for_nothing_new_data() {
            return [{tag: "cancelled"}, {tag: "no usable rows"}, {tag: "empty channel map"}, {tag: "new import"}];
        }
        function test_no_reload_for_nothing_new(data) {
            testContext.setHostRunning(true);
            if (data.tag === "cancelled") {
                update(tc.groupPath, null);
                compare(tc.screen.notice, "");
            } else if (data.tag === "no usable rows") {
                // Rows that are there but none usable (an Excel CSV saved with ';') mean a wrong file.
                update(tc.groupPath, testContext.writeFixtureCsv("excel-groups.csv", "id;mode;name\n1001;A;Dispatch\n"));
                compare(tc.screen.notice, "Updated — no usable rows. Check the file format, then update it.");
                verify(tc.screen.noticeIsProblem);
            } else if (data.tag === "empty channel map") {
                // Only a talkgroup list is a list emptied on purpose; a channel map with no rows is a wrong file.
                update(tc.chanPath, testContext.writeFixtureCsv("empty-map.csv", "channel,frequency\n"));
                compare(tc.screen.notice, "Updated — no usable rows. Check the file format, then update it.");
                verify(tc.screen.noticeIsProblem);
            } else {
                var flow = findChild(tc.screen, "importsCsvImport");
                tc.screen.pendingRow = -1;
                flow.pick("group");
                var picker = findChild(flow, "csvPrimaryPicker");
                picker.selectedFile = "file://" + testContext.writeFixtureCsv("new-groups.csv", "id,mode,name\n7,A,Seven\n");
                picker.accepted();
                picker.close();
                compare(tc.screen.notice, "Imported · 1 talkgroup");
            }
            compare(commands.listCommands(), []);
        }

        // A refresh asks the decoder about the file it replaced with no saved system in hand: before #748 it pushed
        // only when the saved system the session started from named the file, which a manual Apply outdates.
        function test_refresh_reloads_the_file_in_a_running_session_data() {
            return [
                {tag: "talkgroups", kind: "group", want: "reloadGroupList", accepted: 3,
                 notice: "Refreshed · 3 talkgroups · sent to the running session"},
                {tag: "channel map", kind: "chan", want: "reloadChannelMap", accepted: 3,
                 notice: "Refreshed · 3 channels · sent to the running session"},
                {tag: "empty talkgroup list", kind: "group", want: "reloadGroupList", accepted: 0,
                 notice: "Refreshed · empty list · sent to the running session"}
            ];
        }
        function test_refresh_reloads_the_file_in_a_running_session(data) {
            testContext.useRadioReferenceModel(fakeRefresh);
            testContext.setHostRunning(true);
            var path = data.kind === "group" ? tc.groupPath : tc.chanPath;
            fakeRefresh.refreshFinished(importedFiles.rowForPath(path),
                                        {ok: true, path: path, type: data.kind, accepted: data.accepted, skipped: 0,
                                         error: data.accepted ? "" : "empty"});
            compare(commands.listCommands(), [data.want + " " + path]);
            compare(tc.screen.notice, data.notice);
            verify(!tc.screen.noticeIsProblem);
        }

        function test_refresh_sends_nothing_new_data() {
            return [{tag: "no session", running: false, kind: "group", accepted: 3, skipped: 0, error: ""},
                    {tag: "no usable rows", running: true, kind: "group", accepted: 0, skipped: 2, error: "empty"},
                    {tag: "empty channel map", running: true, kind: "chan", accepted: 0, skipped: 0, error: "empty"}];
        }
        function test_refresh_sends_nothing_new(data) {
            testContext.useRadioReferenceModel(fakeRefresh);
            testContext.setHostRunning(data.running);
            var path = data.kind === "group" ? tc.groupPath : tc.chanPath;
            fakeRefresh.refreshFinished(importedFiles.rowForPath(path),
                                        {ok: true, path: path, type: data.kind, accepted: data.accepted,
                                         skipped: data.skipped, error: data.error});
            compare(commands.listCommands(), []);
            if (!data.running)
                compare(tc.screen.notice, "Refreshed · 3 talkgroups");
            else
                verify(tc.screen.noticeIsProblem, tc.screen.notice);
        }

        // The library row says the same as the notice: an empty talkgroup list is no problem, rows none of which
        // load are.
        function test_row_summary_tells_an_empty_list_from_a_wrong_file() {
            compare(tc.screen.summaryFor("group", 0, 0, 0).indexOf("Empty list · "), 0);
            compare(tc.screen.summaryFor("group", 0, 2, 0), "No usable rows — check the file format");
            compare(tc.screen.summaryFor("chan", 0, 0, 0), "No usable rows — check the file format");
        }
    }
}
