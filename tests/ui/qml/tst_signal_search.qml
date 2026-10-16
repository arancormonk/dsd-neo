// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtQuick.Window
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
        // The panel is narrower than a phone: the strip beside it is scrim.
        function tapBeside(sheet) {
            var x = 4, y = sheet.height / 2;
            verify(!sheet.hitsPanel(x, y), "the side margin lies outside the panel");
            mouseClick(sheet, x, y);
        }
        function shownDiscard(item) {
            if (!item || !item.visible)
                return null;
            if (item.accessibleName === "Discard changes?")
                return item;
            for (var child of item.children) {
                var found = shownDiscard(child);
                if (found)
                    return found;
            }
            return null;
        }
        function dismissEditor(editor, method) {
            if (method === "scrim")
                tapBeside(editor);
            else if (method === "back")
                verify(Ui.Navigation.back(editor.Window.window));
            else if (method === "escape")
                keyClick(Qt.Key_Escape);
            else
                findChild(editor, "searchRangeCancel").activate();
        }
        // A range edited, then dismissed by the side margin, Back, Escape or
        // Cancel, asks before the edit goes; an untouched range just closes.
        function test_range_editor_asks_before_dropping_edits_data() {
            return [{tag: "scrim", method: "scrim"}, {tag: "back", method: "back"}, {tag: "escape", method: "escape"}, {tag: "cancel", method: "cancel"}];
        }
        function test_range_editor_asks_before_dropping_edits(data) {
            loader.item.sessionActive = false;
            var editor = findChild(loader.item, "signalSearchRangeEditor");
            editor.openFor(0, model.defaultRange());
            waitForRendering(editor);
            dismissEditor(editor, data.method);
            tryCompare(editor, "visible", false);
            verify(!shownDiscard(loader.item), "an untouched range closes without asking");
            editor.openFor(0, model.defaultRange());
            waitForRendering(editor);
            var numerator = findChild(editor, "searchStepNumerator");
            numerator.text = "9999";
            dismissEditor(editor, data.method);
            verify(editor.visible, "the typed step survives the dismissal");
            var discard = shownDiscard(editor);
            verify(discard !== null, "the editor asks before discarding");
            findChild(discard, "discardDialogKeep").activate();
            verify(!discard.visible);
            verify(editor.visible);
            compare(numerator.text, "9999");
            tryVerify(function () { return Ui.Navigation.contains(editor, editor.Window.window.activeFocusItem); }, 1000, "focus returns into the editor");
            dismissEditor(editor, data.method);
            discard = shownDiscard(editor);
            verify(discard !== null);
            findChild(discard, "discardDialogDiscard").activate();
            tryCompare(editor, "visible", false);
            compare(model.ranges[0].stepNum, 12500, "a discarded edit is never saved");
        }
        // Every field the editor fills counts, including a selector's choice.
        function test_range_editor_counts_every_field() {
            loader.item.sessionActive = false;
            var editor = findChild(loader.item, "signalSearchRangeEditor");
            editor.openFor(0, model.defaultRange());
            waitForRendering(editor);
            findChild(editor, "searchRangeName").text = "Renamed";
            tapBeside(editor);
            verify(shownDiscard(editor) !== null);
            findChild(shownDiscard(editor), "discardDialogDiscard").activate();
            tryCompare(editor, "visible", false);
            editor.openFor(0, model.defaultRange());
            waitForRendering(editor);
            findChild(editor, "searchRangeSquelch").currentIndex = 0;
            tapBeside(editor);
            verify(shownDiscard(editor) !== null);
            findChild(shownDiscard(editor), "discardDialogDiscard").activate();
            tryCompare(editor, "visible", false);
        }
        // An open selector closes on the first Back; only the next one asks.
        function test_range_editor_back_closes_a_selector_first() {
            loader.item.sessionActive = false;
            var editor = findChild(loader.item, "signalSearchRangeEditor");
            editor.openFor(0, model.defaultRange());
            waitForRendering(editor);
            findChild(editor, "searchStepNumerator").text = "9999";
            var preset = findChild(editor, "searchStepPreset");
            preset.popup.open();
            tryCompare(preset.popup, "opened", true);
            verify(Ui.Navigation.back(editor.Window.window));
            tryCompare(preset.popup, "visible", false);
            verify(editor.visible);
            verify(!shownDiscard(editor), "the first Back only closes the selector");
            verify(Ui.Navigation.back(editor.Window.window));
            verify(shownDiscard(editor) !== null);
            findChild(shownDiscard(editor), "discardDialogDiscard").activate();
            tryCompare(editor, "visible", false);
        }
        // Save still closes at once, and Cancel sits beside it.
        function test_range_editor_saves_and_offers_cancel() {
            loader.item.sessionActive = false;
            var editor = findChild(loader.item, "signalSearchRangeEditor");
            editor.openFor(0, model.defaultRange());
            waitForRendering(editor);
            var cancel = findChild(editor, "searchRangeCancel");
            var save = findChild(editor, "searchRangeSave");
            verify(cancel !== null && cancel.visible);
            verify(cancel.height >= Ui.Theme.minimumTouchSize);
            compare(cancel.text, "Cancel");
            verify(cancel.mapToItem(editor, 0, 0).y > save.mapToItem(editor, 0, 0).y, "Cancel follows Save range");
            findChild(editor, "searchStepNumerator").text = "6250";
            save.activate();
            tryCompare(editor, "visible", false);
            verify(!shownDiscard(loader.item));
            compare(Number(model.ranges[0].stepNum), 6250);
            model.ranges = [Object.assign({}, model.ranges[0], {stepNum: 12500})];
        }
        // Idle, the screen's receiver is an edit too: changed, Back asks; the
        // receiver it opened with (seeded by Home) is not a change.
        function test_idle_source_edit_asks_before_closing_data() {
            return [{tag: "chip", chip: true}, {tag: "port", chip: false}];
        }
        function test_idle_source_edit_asks_before_closing(data) {
            var screen = loader.item;
            screen.sessionActive = false;
            screen.seed("rtltcp", "10.0.0.5", 1235);
            compare(findChild(screen, "searchPort").text, "1235");
            screen.requestClose();
            compare(closed.count, 1, "the seeded receiver is not an edit");
            screen.seed("rtltcp", "10.0.0.5", 1235);
            if (data.chip)
                screen.sourceType = "airspy";
            else
                findChild(screen, "searchPort").text = "5555";
            screen.requestClose();
            compare(closed.count, 1, "an edited receiver asks first");
            var discard = shownDiscard(screen);
            verify(discard !== null);
            findChild(discard, "discardDialogKeep").activate();
            compare(closed.count, 1);
            screen.requestClose();
            findChild(shownDiscard(screen), "discardDialogDiscard").activate();
            compare(closed.count, 2);
            // The next visit starts from what Home seeds, not from the discarded port.
            screen.seed("rtltcp", "10.0.0.5", 1235);
            compare(findChild(screen, "searchPort").text, "1235");
            compare(screen.sourceType, "rtltcp");
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
