// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

Item {
    id: root
    width: 411
    height: 891
    QtObject {
        id: model
        property var ranges: [{name: "Local UHF", startMHz: "450.000000", endMHz: "460.000000", enabled: true,
            stepNum: 12500, stepDen: 1, bandwidth: 16000, am: false, digital: 1, gainSet: false, gainAuto: true,
            gain: 0, sensitivity: 15, minimumSet: false, minimum: -60, squelchMode: 2, squelchMargin: 10,
            squelchLevel: -60, probe: 2, visit: 30}]
        property var history: []
        property var avoids: []
        property var status: ({phase: 0})
        property string error: ""
        property var actions: []
        property int arms: 0
        function control(action, session, hit) { actions = [action, session, hit]; return true; }
        function arm() { arms++; return ""; }
        function saveRange(row, range) { ranges = [range]; return ""; }
        function defaultRange() { return Object.assign({}, ranges[0]); }
    }
    Loader {
        id: loader
        anchors.fill: parent
        source: uiDir + "/SignalSearchScreen.qml"
        onLoaded: item.searchModel = model
    }
    SignalSpy { id: saved; target: loader.item; signalName: "saveDetection" }
    SignalSpy { id: back; target: loader.item; signalName: "backRequested" }
    SignalSpy { id: closed; target: loader.item; signalName: "closed" }
    TestCase {
        name: "SignalSearch"
        when: windowShown
        function cleanup() {
            Ui.Theme.resetFontScale();
            root.width = 411;
            root.height = 891;
            var editor = findChild(loader.item, "signalSearchRangeEditor");
            editor.visible = false;
        }
        function init() {
            verify(loader.item !== null);
            loader.item.sessionActive = true;
            model.actions = [];
            model.status = {phase: 4, session: "17", pass: 2, range: 0, channel: 80, count: 801,
                hit: {id: "42", session: "17", freqMHz: "463.552100", frequency: 463552100, power: -32.1,
                    noise: -63.2, range: "Local UHF", width: 12500, unresolved: true, ended: false,
                    knownLabel: "Warehouse · 463.5500 MHz", confirmed: false, am: false}};
            saved.clear();
            back.clear();
            closed.clear();
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
        function test_paused_edits_are_applied_on_resume() {
            model.status = {phase: 6, session: "17"};
            var before = model.arms;
            findChild(loader.item, "searchStartPause").clicked();
            compare(model.arms, before + 1);
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
