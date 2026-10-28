// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest

// Issue #708: Settings → Call recording. The switches are the preferences (AppPrefs, through the lifecycle host)
// and apply to a running session too (D8); the lines under them say how the session took the change
// (recordingStatus) and what the store reports (recordings).
Item {
    width: 420
    height: 900
    Loader { id: screenLoader; anchors.fill: parent }

    TestCase {
        name: "CallRecordingSettings"
        when: windowShown

        function openScreen() {
            screenLoader.source = uiDir + "/SettingsScreen.qml";
            verify(screenLoader.item !== null);
            return screenLoader.item;
        }
        function item(name) {
            return findChild(screenLoader.item, name);
        }
        function notices() {
            return screenLoader.item.recordingNotices.map(function (note) { return note.text; });
        }
        function problems() {
            return screenLoader.item.recordingNotices.filter(function (note) { return note.problem; }).map(function (note) { return note.text; });
        }
        function lastRequest() {
            var requests = commands.recordingRequests();
            return requests.length > 0 ? requests[requests.length - 1] : null;
        }

        function init() {
            testContext.useLifecycleHost(true);
            testContext.resetCommands();
            testContext.resetRecordingStatus();
            recordings.reset();
            prefs.recordCalls = false;
            prefs.recordUnknownOnly = false;
            prefs.recordingLimitMb = 1000;
            commands.setRecordingAccepted(true);
        }
        function cleanup() {
            screenLoader.source = "";
            prefs.recordCalls = false;
            prefs.recordUnknownOnly = false;
            prefs.recordingLimitMb = 1000;
            testContext.setLifecyclePhase(0);
            testContext.useLifecycleHost(false);
            testContext.resetCommands();
            testContext.resetRecordingStatus();
            recordings.reset();
        }

        function test_rest() {
            openScreen();
            compare(item("settingsCallRecordingHeader").text, "Call recording");
            var record = item("recordCallsToggle");
            compare(record.title, "Record calls");
            compare(record.subtitle, "Saves each call you hear so you can replay it from History");
            verify(!record.checked);
            var unknown = item("recordUnknownOnlyToggle");
            compare(unknown.title, "Only unknown talkgroups");
            compare(unknown.subtitle, "Skip talkgroups in your talkgroup list");
            verify(!unknown.enabled, "unknown-only waits for Record calls");
            compare(item("recordingLimitRow").subtitle, "1 GB · Oldest recordings are removed first");
            compare(item("manageCallRecordingsRow").subtitle, "No recordings yet");
            verify(!item("recordingNotices").visible);
            // The card follows Decoding.
            verify(item("settingsCallRecordingHeader").mapToItem(null, 0, 0).y > item("settingsDecodingHeader").mapToItem(null, 0, 0).y);
            verify(item("settingsCallRecordingHeader").mapToItem(null, 0, 0).y < item("settingsRadioDefaultsHeader").mapToItem(null, 0, 0).y);
        }

        function test_idle_change_waits_for_the_next_start() {
            openScreen();
            item("recordCallsToggle").activate();
            verify(prefs.recordCalls);
            verify(item("recordCallsToggle").checked);
            verify(item("recordUnknownOnlyToggle").enabled);
            item("recordUnknownOnlyToggle").activate();
            verify(prefs.recordUnknownOnly);
            compare(commands.recordingRequests().length, 0, "nothing runs, nothing is sent");
        }

        // D8: a running session takes the change at once; unknown-only travels with Record calls.
        function test_live_change() {
            testContext.setLifecyclePhase(2);
            openScreen();
            item("recordCallsToggle").activate();
            compare(lastRequest().kind, "recordCalls");
            compare(lastRequest().on, true);
            compare(lastRequest().unknownOnly, false);
            item("recordUnknownOnlyToggle").activate();
            compare(lastRequest().kind, "recordUnknownOnly");
            compare(lastRequest().on, true);
            item("recordCallsToggle").activate();
            compare(lastRequest().kind, "recordCalls");
            compare(lastRequest().on, false);
            compare(lastRequest().unknownOnly, true, "the saved unknown-only goes with it");
            verify(!item("recordUnknownOnlyToggle").enabled);
            var sent = commands.recordingRequests().length;
            item("recordUnknownOnlyToggle").activate();
            compare(commands.recordingRequests().length, sent, "a disabled switch sends nothing");
            verify(prefs.recordUnknownOnly, "nor changes the preference");
            // Unknown-only applies only while calls record: changed with Record calls off, it waits for the next on.
            screenLoader.item.setRecordUnknownOnly(false);
            verify(!prefs.recordUnknownOnly);
            compare(commands.recordingRequests().length, sent, "it is saved, not sent");
            item("recordCallsToggle").activate();
            compare(lastRequest().kind, "recordCalls");
            compare(lastRequest().unknownOnly, false, "and travels with the next Record calls on");
        }

        function test_session_notices_data() {
            return [
                {tag: "applying", key: "commandState", value: "pending", text: "Applying…", problem: false},
                {tag: "signal-search", key: "commandState", value: "refused", text: "Not applied while Signal search runs. Applies at the next start.", problem: false},
                {tag: "failed", key: "commandState", value: "failed", text: "The decoder couldn't apply the change. Applies at the next start.", problem: true},
                {tag: "applied", key: "commandState", value: "applied", text: "", problem: false},
                {tag: "no-storage", key: "refusal", value: "storage-unavailable", text: "This session has no storage for call recordings. Applies at the next start.", problem: false},
                {tag: "static-wav", key: "refusal", value: "static-wav", text: "This session records to one file (-w in Extra decoder arguments). Applies at the next start.", problem: false},
                {tag: "starting", key: "refusal", value: "starting", text: "The session is still starting. Applies at the next start.", problem: false},
                {tag: "no-session", key: "refusal", value: "no-session", text: "", problem: false},
                {tag: "journal", key: "journal", value: "failed", text: "New call recordings can't be listed in the app this session. Stop and start listening to try again.", problem: true}
            ];
        }
        function test_session_notices(data) {
            testContext.setLifecyclePhase(2);
            openScreen();
            testContext.setRecordingStatus(data.key, data.value);
            if (data.text.length === 0) {
                compare(notices().length, 0);
                verify(!item("recordingNotices").visible);
                return;
            }
            compare(notices(), [data.text]);
            compare(problems().length, data.problem ? 1 : 0);
            verify(item("recordingNotices").visible);
            // A session that ended took its readings with it.
            testContext.setLifecyclePhase(0);
            compare(notices().length, 0);
        }

        function test_recording_failures_data() {
            return [
                {tag: "full", kind: "write", count: 1, low: true, text: "A call couldn't be recorded. This phone's storage is full."},
                {tag: "write", kind: "write", count: 3, low: false, text: "3 calls couldn't be recorded. Writing the file failed."},
                {tag: "open", kind: "open", count: 1, low: false, text: "A call couldn't be recorded. The file couldn't be created."},
                {tag: "publish", kind: "publish", count: 1, low: false, text: "A call couldn't be recorded. The file couldn't be finished. It's kept under Unfinished recordings."}
            ];
        }
        function test_recording_failures(data) {
            testContext.setLifecyclePhase(2);
            openScreen();
            recordings.lowSpace = data.low;
            testContext.setRecordingStatus("lastError", data.kind);
            testContext.setRecordingStatus("failures", data.count);
            verify(problems().indexOf(data.text) >= 0, problems().join(" | "));
        }

        function test_store_notices() {
            openScreen();
            recordings.available = false;
            compare(problems(), ["Storage for call recordings isn't available right now. Saved recordings are kept."]);
            recordings.available = true;
            recordings.lowSpace = true;
            recordings.freeBytes = 150000000;
            compare(notices().length, 0, "low space matters only while calls record");
            prefs.recordCalls = true;
            compare(problems(), ["Only 150 MB free on this phone. Calls stop recording when it's full."]);
            recordings.freeBytes = -1;
            compare(problems(), ["This phone is low on space. Calls stop recording when it's full."]);
            recordings.lowSpace = false;
            recordings.removedByLimit = 1;
            compare(notices(), ["The storage limit removed the oldest recording."]);
            recordings.removedByLimit = 1204;
            compare(notices(), ["The storage limit removed the " + Number(1204).toLocaleString(Qt.locale(), "f", 0) + " oldest recordings."]);
            compare(problems().length, 0, "the limit doing its job is not a problem");
        }

        function test_usage() {
            openScreen();
            var row = item("manageCallRecordingsRow");
            recordings.indexed = false;
            compare(row.subtitle, "Counting recordings…");
            recordings.indexed = true;
            recordings.usedBytes = 312000000;
            recordings.recordingCount = 3;
            compare(row.subtitle, "312 MB of 1 GB · 3 recordings");
            recordings.recordingCount = 0;
            recordings.unfinishedCount = 1;
            compare(row.subtitle, "312 MB of 1 GB · 1 recording", "unfinished ones count");
            prefs.recordingLimitMb = 0;
            compare(row.subtitle, "312 MB · 1 recording");
            prefs.recordingLimitMb = 250;
            recordings.usedBytes = 4200000;
            compare(row.subtitle, "4.2 MB of 250 MB · 1 recording");
        }

        function test_storage_limit_choice() {
            openScreen();
            item("recordingLimitRow").activate();
            var sheet = item("screenChoices");
            verify(sheet.visible);
            compare(sheet.accessibleName, "Storage limit");
            compare(sheet.choices, ["250 MB", "500 MB", "1 GB", "2 GB", "5 GB", "No limit"]);
            findChild(sheet, "screenChoice5").activate();
            compare(prefs.recordingLimitMb, 0);
            compare(item("recordingLimitRow").subtitle, "No limit · Oldest recordings are removed first");
            item("recordingLimitRow").activate();
            findChild(sheet, "screenChoice3").activate();
            compare(prefs.recordingLimitMb, 2000);
        }

        function test_manage_row_opens_the_screen() {
            var settings = openScreen();
            var opened = 0;
            settings.openCallRecordings.connect(function () { opened++; });
            item("manageCallRecordingsRow").activate();
            compare(opened, 1);
        }
    }
}
