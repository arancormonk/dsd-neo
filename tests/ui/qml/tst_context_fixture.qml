// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest

// The engine-facing context properties are plain maps (see qml_test_context.h),
// and QML answers a read of a key a map does not carry with `undefined` — no
// warning, no error. So a screen that grew a binding on a reading the fixture
// lacks would pass every case in this suite and render `undefined` or NaN on the
// phone. This is the case that makes that loud.
TestCase {
    name: "QmlContextFixtureIsComplete"

    function test_host_support_methods() {
        testContext.useLifecycleHost(false);
        compare(decoderHost.licenseNotices(), "License notices fixture");
        compare(decoderHost.copyText("fixture"), false);
    }

    SignalSpy {
        id: revisionSpy
        target: recordings
        signalName: "revisionChanged"
    }

    // Issue #809: the store's detection queries and History's fragment lookup, as the search screen reads them.
    function test_detection_recordings_and_fragment_lookup() {
        recordings.reset();
        callHistory.clearAll();
        revisionSpy.clear();
        compare(recordings.recordingCountForDetection("17:4"), 0);
        compare(recordings.recordingsForDetection("17:4").length, 0);

        recordings.setDetectionRecordings("17:4", [{"id": "b", "fragment": "F:0:2"}, {"id": "a", "start": 5}]);
        compare(revisionSpy.count, 1, "a seeding moves the revision");
        compare(recordings.revision, 1);
        compare(recordings.recordingCountForDetection("17:4"), 2);
        compare(recordings.recordingCountForDetection("17:5"), 0);
        var list = recordings.recordingsForDetection("17:4");
        compare(list[0].id, "b");
        compare(list[1].start, 5);
        compare(Object.keys(list[1]).sort(), ["available", "bytes", "durationMs", "failed", "fileName", "folderUid",
                                              "fragment", "id", "path", "rate", "source", "start", "system", "target"]);
        compare(list[1].available, true);
        recordings.setDetectionRecordings("17:4", []);
        compare(revisionSpy.count, 2);
        compare(recordings.recordingCountForDetection("17:4"), 0);

        var uid = callHistory.pushWithRecordings([]);
        compare(callHistory.uidForFragment("F:0:2"), 0, "a fragment no row holds");
        verify(callHistory.setFragmentRow("F:0:2", uid));
        compare(callHistory.uidForFragment("F:0:2"), uid);
        var other = callHistory.pushWithRecordings([]);
        verify(callHistory.setFragmentRow("F:0:2", other), "a merge moves it");
        compare(callHistory.uidForFragment("F:0:2"), other);
        verify(callHistory.trimRow(other));
        compare(callHistory.uidForFragment("F:0:2"), 0, "its row trimmed");
        verify(callHistory.setFragmentRow("F:0:3", uid));
        callHistory.clearAll();
        compare(callHistory.uidForFragment("F:0:3"), 0, "the log cleared");
        verify(!callHistory.setFragmentRow("F:0:3", uid));
        recordings.reset();
        compare(revisionSpy.count, 3, "a reset back to rest moves it too");
        compare(recordings.revision, 0);
    }

    function test_01_the_fixture_carries_every_reading_the_screens_read() {
        // The screens the other cases load, plus Theme.qml: the screens pull it
        // in as a singleton and it reads prefs itself.
        var missing = testContext.missingContextKeys(
            ["HistoryScreen.qml", "HistoryDetailSheet.qml", "CallRecordingPlayer.qml", "CallRecordingsScreen.qml", "CallRow.qml", "MonitorScreen.qml", "SpectrumScreen.qml", "ExploreSetupScreen.qml", "RadioSheet.qml",
             "ImportsScreen.qml", "WizardScreen.qml", "HomeScreen.qml", "SettingsScreen.qml", "RadioReferenceAccountEditor.qml", "RadioReferenceAccountScreen.qml", "LicensesScreen.qml", "Main.qml",
             "SiteChooserSheet.qml", "ScanListScreen.qml", "ScanListCard.qml", "ScanEntryRow.qml", "DiagnosticsScreen.qml", "RadioReferenceScreen.qml", "TalkgroupsScreen.qml", "TalkgroupCard.qml", "TalkgroupEditSheet.qml", "TalkgroupSaveFlow.qml", "ToneFilterSheet.qml", "ReceivePolicySheet.qml", "SquelchSheet.qml", "PassbandSheet.qml", "ScanAvoidSheet.qml", "ScanListsSheet.qml", "ScanTogetherSheet.qml", "BackupFlow.qml", "BackupResultCard.qml", "SignalSearchScreen.qml", "Theme.qml"])

        compare(missing.length, 0,
                "read by the screens, missing from the fixture: " + missing.join(", "))
    }
}
