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
        id: incomingSpy
        signalName: "incomingDocumentChanged"
    }

    SignalSpy {
        id: requestSpy
        signalName: "documentRequestChanged"
    }

    // Issue #831: the incoming document slot and the picker-in-flight reading, on both hosts a case can install.
    function test_incoming_document_readings() {
        var ref = "content://com.Example.Provider/document/primary%3ADownload%2Fa%20b.csv";
        for (var lifecycle = 0; lifecycle < 2; ++lifecycle) {
            testContext.useLifecycleHost(lifecycle === 1);
            incomingSpy.target = decoderHost;
            requestSpy.target = decoderHost;
            incomingSpy.clear();
            requestSpy.clear();
            compare(decoderHost.incomingDocumentPending, false);
            compare(decoderHost.documentRequestPending, false);
            compare(decoderHost.takeIncomingDocument(), "", "an empty take");
            compare(incomingSpy.count, 0, "an empty take tells nothing");

            testContext.emitIncomingDocument("content://com.Other.Files/document/old.csv");
            testContext.emitIncomingDocument(ref);
            compare(incomingSpy.count, 2);
            compare(decoderHost.incomingDocumentPending, true);
            var taken = decoderHost.takeIncomingDocument();
            compare(typeof taken, "string");
            compare(taken, ref, "the newer one, as it was sent");
            compare(decoderHost.incomingDocumentPending, false);
            compare(incomingSpy.count, 3);
            compare(decoderHost.takeIncomingDocument(), "", "taken once");

            testContext.setDocumentRequestPending(true);
            compare(decoderHost.documentRequestPending, true);
            testContext.setDocumentRequestPending(false);
            compare(decoderHost.documentRequestPending, false);
            compare(requestSpy.count, 2);
        }
        testContext.useLifecycleHost(false);
    }

    SignalSpy {
        id: knownSpy
        signalName: "sessionStateKnownChanged"
    }

    // Issue #831: whether the session state is known, which Main waits for before it takes a document: known on both
    // hosts until a case says otherwise, and known again after the reset.
    function test_session_state_known_reading() {
        for (var lifecycle = 0; lifecycle < 2; ++lifecycle) {
            testContext.useLifecycleHost(lifecycle === 1);
            knownSpy.target = decoderHost;
            knownSpy.clear();
            compare(decoderHost.sessionStateKnown, true);
            testContext.setSessionStateKnown(false);
            compare(decoderHost.sessionStateKnown, false);
            compare(knownSpy.count, 1);
        }
        testContext.resetHostPlayback();
        for (lifecycle = 0; lifecycle < 2; ++lifecycle) {
            testContext.useLifecycleHost(lifecycle === 1);
            compare(decoderHost.sessionStateKnown, true, "the reset makes it known again");
        }
        testContext.useLifecycleHost(false);
    }

    // Issue #831: the reset drops an incoming document not taken and a picker request in flight, on every host.
    function test_reset_clears_incoming_document_and_request() {
        for (var lifecycle = 0; lifecycle < 2; ++lifecycle) {
            testContext.useLifecycleHost(lifecycle === 1);
            testContext.emitIncomingDocument("content://com.Other.Files/document/left.csv");
            testContext.setDocumentRequestPending(true);
        }
        testContext.resetHostPlayback();
        for (lifecycle = 0; lifecycle < 2; ++lifecycle) {
            testContext.useLifecycleHost(lifecycle === 1);
            compare(decoderHost.incomingDocumentPending, false);
            compare(decoderHost.documentRequestPending, false);
            compare(decoderHost.takeIncomingDocument(), "", "nothing reaches a later case");
        }
        testContext.useLifecycleHost(false);
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
        var base = recordings.revision;
        compare(recordings.recordingCountForDetection("17:4"), 0);
        compare(recordings.recordingsForDetection("17:4").length, 0);

        recordings.setDetectionRecordings("17:4", [{"id": "b", "fragment": "F:0:2"}, {"id": "a", "start": 5}]);
        compare(revisionSpy.count, 1, "a seeding moves the revision");
        compare(recordings.revision, base + 1);
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
        compare(revisionSpy.count, 2, "a reset with nothing seeded leaves it");
        recordings.setDetectionRecordings("17:9", [{"id": "c"}]);
        recordings.reset();
        compare(revisionSpy.count, 4, "a reset that drops seeded recordings moves it on");
        compare(recordings.revision, base + 4, "never back");
        compare(recordings.recordingCountForDetection("17:9"), 0);
    }

    function test_01_the_fixture_carries_every_reading_the_screens_read() {
        // The screens the other cases load, plus Theme.qml: the screens pull it
        // in as a singleton and it reads prefs itself.
        var missing = testContext.missingContextKeys(
            ["HistoryScreen.qml", "HistoryDetailSheet.qml", "CallRecordingPlayer.qml", "CallRecordingsScreen.qml", "CallRow.qml", "MonitorScreen.qml", "SpectrumScreen.qml", "ExploreSetupScreen.qml", "RadioSheet.qml",
             "ImportsScreen.qml", "WizardScreen.qml", "HomeScreen.qml", "SettingsScreen.qml", "RadioReferenceAccountEditor.qml", "RadioReferenceAccountScreen.qml", "LicensesScreen.qml", "Main.qml",
             "SiteChooserSheet.qml", "ScanListScreen.qml", "ScanListCard.qml", "ScanEntryRow.qml", "DiagnosticsScreen.qml", "RadioReferenceScreen.qml", "TalkgroupsScreen.qml", "TalkgroupCard.qml", "TalkgroupEditSheet.qml", "TalkgroupSaveFlow.qml", "ToneFilterSheet.qml", "ReceivePolicySheet.qml", "SquelchSheet.qml", "PassbandSheet.qml", "ScanAvoidSheet.qml", "ScanListsSheet.qml", "ScanTogetherSheet.qml", "PinButton.qml", "PinnedSheet.qml", "BackupFlow.qml", "BackupResultCard.qml", "DocumentPicker.qml", "FileBrowser.qml", "FileBrowserScreen.qml", "FileBrowserLocations.qml", "FileBrowserFolder.qml", "StoreNoticeCard.qml", "SignalSearchScreen.qml", "DetectionRecordingsSheet.qml", "Theme.qml"])

        compare(missing.length, 0,
                "read by the screens, missing from the fixture: " + missing.join(", "))
    }
}
