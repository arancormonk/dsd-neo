// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

Item {
    id: root
    width: 411
    height: 891
    // What Main binds the screen's failure to: the host's failure text.
    property string hostFailure: ""
    QtObject {
        id: model
        property var ranges: [{name: "Local UHF", startMHz: "450.000000", endMHz: "460.000000", enabled: true,
            stepNum: 12500, stepDen: 1, bandwidth: 16000, am: false, digital: 1, gainSet: false, gainAuto: true,
            gain: 0, sensitivity: 15, minimumSet: false, minimum: -60, squelchMode: 2, squelchMargin: 10,
            squelchLevel: -60, probe: 2, visit: 30}]
        property var history: []
        property var avoids: []
        property var sessionAvoids: []
        property var status: ({phase: 0})
        property string error: ""
        property var actions: []
        property var lifted: []
        property int arms: 0
        property string armError: ""
        function control(action, session, hit) { actions = [action, session, hit]; return true; }
        function removeSessionAvoid(frequency) { lifted = lifted.concat(["session:" + frequency]); return true; }
        function removeAvoid(frequency) { lifted = lifted.concat(["permanent:" + frequency]); }
        function arm() { arms++; return armError; }
        function saveRange(row, range) { ranges = [range]; return ""; }
        function defaultRange() { return Object.assign({}, ranges[0]); }
    }
    Loader {
        id: loader
        anchors.fill: parent
        source: uiDir + "/SignalSearchScreen.qml"
        onLoaded: {
            item.searchModel = model;
            // As Main.qml binds it: failure: mainRoot.failureText.
            item.failure = Qt.binding(function () { return root.hostFailure; });
        }
    }
    SignalSpy { id: saved; target: loader.item; signalName: "saveDetection" }
    SignalSpy { id: back; target: loader.item; signalName: "backRequested" }
    SignalSpy { id: closed; target: loader.item; signalName: "closed" }
    SignalSpy { id: started; target: loader.item; signalName: "start" }
    SignalSpy { id: refused; target: loader.item; signalName: "startRefused" }
    TestCase {
        name: "SignalSearch"
        when: windowShown
        function cleanup() {
            Ui.Theme.resetFontScale();
            root.width = 411;
            root.height = 891;
            var editor = findChild(loader.item, "signalSearchRangeEditor");
            editor.visible = false;
            loader.item.sessionSource = {};
            loader.item.sourceType = "usb";
        }
        function init() {
            verify(loader.item !== null);
            loader.item.sessionActive = true;
            model.actions = [];
            model.lifted = [];
            model.sessionAvoids = [];
            model.avoids = [];
            model.status = {phase: 4, session: "17", owned: true, pass: 2, range: 0, channel: 80, count: 801,
                hit: {id: "42", session: "17", freqMHz: "463.552100", frequency: 463552100, power: -32.1,
                    noise: -63.2, range: "Local UHF", width: 12500, unresolved: true, ended: false,
                    knownLabel: "Warehouse · 463.5500 MHz", confirmed: false, am: false}};
            model.armError = "";
            root.hostFailure = "";
            saved.clear();
            back.clear();
            closed.clear();
            started.clear();
            refused.clear();
        }
        function test_back_is_delegated_to_session_owner() {
            loader.item.requestClose();
            compare(back.count, 1);
            compare(closed.count, 0);
            loader.item.sessionActive = false;
            loader.item.requestClose();
            compare(closed.count, 1);
        }
        function test_step_preset_tracks_range_and_manual_edits() {
            var editor = findChild(loader.item, "signalSearchRangeEditor");
            var preset = findChild(editor, "searchStepPreset");
            editor.openFor(0, model.defaultRange());
            compare(preset.currentIndex, 4);
            editor.openFor(1, Object.assign({}, model.defaultRange(), {stepNum: 25000, stepDen: 3}));
            compare(preset.currentIndex, 3);
            findChild(editor, "searchStepNumerator").text = "9999";
            compare(preset.currentIndex, 0);
            preset.activated(2);
            compare(findChild(editor, "searchStepNumerator").text, "6250");
            compare(findChild(editor, "searchStepDenominator").text, "1");
            compare(preset.currentIndex, 2);
        }
        function test_controls_are_tagged_and_save_is_a_snapshot() {
            findChild(loader.item, "searchSkip").clicked();
            compare(model.actions.join(","), "4,17,42");
            findChild(loader.item, "searchSave").clicked();
            compare(saved.count, 1);
            var snapshot = saved.signalArguments[0][0];
            model.status = {phase: 1, session: "17", hit: {id: "43", freqMHz: "450.125000"}};
            compare(snapshot.id, "42");
            compare(snapshot.freqMHz, "463.552100");
        }
        function test_unresolved_is_not_analog_confirmation() {
            compare(findChild(loader.item, "searchIdentification").text, "NFM monitoring · digital identification unresolved");
            model.status = Object.assign({}, model.status, {phase: 3});
            compare(findChild(loader.item, "searchIdentification").text, "Trying digital identification…");
            model.status = Object.assign({}, model.status, {phase: 5, identifying: true});
            compare(findChild(loader.item, "searchIdentification").text, "Trying digital identification…");
        }
        function test_live_strength_is_shown_beside_detection_power() {
            var strength = findChild(loader.item, "searchStrength");
            compare(strength.text, "-32.1 dBFS · Local UHF");
            model.status = Object.assign({}, model.status, {livePowerValid: true, livePower: -47.25});
            compare(strength.text, "Now -47.3 dBFS · detected -32.1 dBFS · Local UHF");
        }
        function test_session_and_permanent_avoids_can_be_lifted_while_searching() {
            model.sessionAvoids = [463550000];
            model.avoids = [154430000];
            wait(0);
            var session = findChild(loader.item, "searchSessionAvoid");
            verify(session && session.visible && session.enabled);
            session.clicked();
            var permanent = findChild(loader.item, "searchPermanentAvoid");
            verify(permanent && permanent.enabled);
            permanent.clicked();
            compare(model.lifted.join(","), "session:463550000,permanent:154430000");
            // Session avoids end with the session: nothing to list once it stopped.
            loader.item.sessionActive = false;
            wait(0);
            verify(!findChild(loader.item, "searchSessionAvoid"));
        }
        function test_paused_edits_are_applied_on_resume() {
            model.status = {phase: 6, session: "17"};
            var before = model.arms;
            findChild(loader.item, "searchStartPause").clicked();
            compare(model.arms, before + 1);
        }
        // Start and Resume must leave Main's failure binding in place: a refused
        // connection, USB access or configuration failure the host reports
        // afterwards has to reach this screen, which covers Home's banner.
        function test_host_failure_after_start_reaches_the_screen_data() {
            return [{tag: "start", active: false}, {tag: "resume", active: true}];
        }
        function test_host_failure_after_start_reaches_the_screen(data) {
            loader.item.sessionActive = data.active;
            if (data.active)
                model.status = {phase: 6, session: "17"};
            findChild(loader.item, "searchStartPause").clicked();
            compare(started.count, data.active ? 0 : 1);
            root.hostFailure = "rtl_tcp: connection refused";
            var message = findChild(loader.item, "searchMessage");
            verify(message.visible, "the host failure must be shown on the search screen");
            compare(message.text, "rtl_tcp: connection refused");
            root.hostFailure = "";
            verify(!message.visible);
        }
        // The screen's own refusal shows beside the host's failure; neither
        // hides the other, and a start that passes clears only the refusal.
        function test_refusal_and_host_failure_both_show() {
            loader.item.sessionActive = false;
            root.hostFailure = "No permission for RTL-SDR";
            model.armError = "Enable at least one range.";
            var button = findChild(loader.item, "searchStartPause");
            button.clicked();
            compare(started.count, 0);
            var message = findChild(loader.item, "searchMessage");
            compare(message.text, "Enable at least one range.\nNo permission for RTL-SDR");
            root.hostFailure = "Device detached: RTL-SDR";
            compare(message.text, "Enable at least one range.\nDevice detached: RTL-SDR");
            model.armError = "";
            button.clicked();
            compare(started.count, 1);
            compare(message.text, "Device detached: RTL-SDR");
        }
        // A refused Resume is shown; it belongs to that attempt, so leaving the
        // screen discards it while the host's failure stays bound.
        function test_resume_refusal_is_shown_until_the_screen_closes() {
            model.status = {phase: 6, session: "17"};
            model.armError = "Check the numeric range settings.";
            findChild(loader.item, "searchStartPause").clicked();
            var message = findChild(loader.item, "searchMessage");
            compare(message.text, "Check the numeric range settings.");
            loader.item.sessionActive = false;
            loader.item.requestClose();
            compare(closed.count, 1);
            verify(!message.visible, "a refusal must not greet the next visit");
            root.hostFailure = "No permission for RTL-SDR";
            compare(message.text, "No permission for RTL-SDR");
        }
        // A refused Start tells Main, which abandons any start this screen asked
        // for earlier that is still waiting for USB access.
        function test_refused_start_is_announced() {
            loader.item.sessionActive = false;
            model.armError = "Enable at least one range.";
            var button = findChild(loader.item, "searchStartPause");
            button.clicked();
            compare(refused.count, 1);
            compare(started.count, 0);
            model.armError = "";
            button.clicked();
            compare(refused.count, 1);
            compare(started.count, 1);
        }
        // Main binds sessionSource to the receiver in the decoder's options
        // snapshot. While a search the decoder owns runs, the source fields follow
        // it (a screen reopened over that search after Activity recreation never
        // had Home seed them), and they keep it once the session stops.
        function test_source_follows_the_owned_session() {
            var screen = loader.item;
            var editor = findChild(screen, "signalSearchRangeEditor");
            screen.host = "192.168.1.10";
            screen.port = 1234;
            screen.sessionSource = {sourceType: "airspy"};
            compare(screen.sourceType, "airspy");
            compare(editor.sourceType, "airspy");
            compare(screen.host, "192.168.1.10", "an Airspy leaves the rtl_tcp server alone");
            screen.sessionSource = {sourceType: "rtltcp", host: "10.0.0.5", port: 1235};
            compare(screen.sourceType, "rtltcp");
            compare(screen.host, "10.0.0.5");
            compare(screen.port, 1235);
            // A cleared reading and a receiver no Qt start offers change nothing.
            screen.sessionSource = {};
            screen.sessionSource = {sourceType: "soapy"};
            compare(screen.sourceType, "rtltcp");
            // Stopped: the receiver it ran on stays for a restart.
            screen.sessionActive = false;
            screen.sessionSource = {sourceType: "usb"};
            compare(screen.sourceType, "rtltcp");
            // A session no search owns is not the search's receiver, until one does.
            model.status = {phase: 1, session: "18", owned: false};
            screen.sessionActive = true;
            screen.sessionSource = {sourceType: "airspy"};
            compare(screen.sourceType, "rtltcp");
            model.status = {phase: 1, session: "18", owned: true};
            compare(screen.sourceType, "airspy");
        }
        function test_reception_is_visible_before_tune_finishes() {
            model.status = Object.assign({}, model.status, {phase: 2});
            verify(loader.item.receiving);
            compare(findChild(loader.item, "searchIdentification").text, "Tuning to detection…");
        }
        function test_capture_phone() {
            wait(100);
            var img = grabImage(root);
            compare(img.width, 411);
            // Optional artifact for native finish review; no screenshot comparison in the test.
            img.save("/tmp/dsd-signal-search-phone.png");
        }
        function test_capture_large_text() {
            Ui.Theme.fontScale = 1.4;
            wait(100);
            grabImage(root).save("/tmp/dsd-signal-search-large.png");
            var button = findChild(loader.item, "searchStartPause");
            verify(button.y + button.height <= loader.item.height);
        }
        function test_capture_editor() {
            loader.item.sessionActive = false;
            var editor = findChild(loader.item, "signalSearchRangeEditor");
            editor.openFor(0, model.defaultRange());
            verify(editor.visible);
            wait(100);
            grabImage(root).save("/tmp/dsd-signal-search-editor.png");
        }
    }
}
