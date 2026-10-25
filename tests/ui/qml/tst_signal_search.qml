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
        function avoidFrequency(session, frequency) { actions = ["session", session, frequency]; return true; }
        function avoidPermanently(session, frequency) { actions = ["permanent", session, frequency]; return true; }
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
            item.systems = systemsStub;
            // As Main.qml binds it: failure: mainRoot.failureText.
            item.failure = Qt.binding(function () { return root.hostFailure; });
        }
    }
    SignalSpy { id: saved; target: loader.item; signalName: "saveDetection" }
    SignalSpy { id: back; target: loader.item; signalName: "backRequested" }
    SignalSpy { id: closed; target: loader.item; signalName: "closed" }
    SignalSpy { id: started; target: loader.item; signalName: "start" }
    SignalSpy { id: refused; target: loader.item; signalName: "startRefused" }
    SignalSpy { id: explored; target: loader.item; signalName: "exploreDetection" }
    SignalSpy { id: startedSaved; target: loader.item; signalName: "startSavedSystem" }
    // The saved systems a detection's matches name, as savedSystems answers.
    QtObject {
        id: systemsStub
        property var byUid: ({"uid-a": {uid: "uid-a", name: "Warehouse"}, "uid-b": {uid: "uid-b", name: "Dock", avoidSite: true}})
        function getByUid(uid) { return byUid[uid] || {}; }
    }
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
            loader.item.resetView();
        }
        function init() {
            verify(loader.item !== null);
            loader.item.sessionActive = true;
            model.actions = [];
            model.lifted = [];
            model.sessionAvoids = [];
            model.avoids = [];
            model.status = {phase: 4, session: "17", owned: true, pass: 2, range: 0, channel: 80, count: 801,
                rangeName: "Local UHF", rangeStartMHz: "450.000", rangeEndMHz: "460.000",
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
            explored.clear();
            startedSaved.clear();
            var menu = findChild(loader.item, "searchDetectionMenu");
            if (menu)
                menu.visible = false;
        }
        // The panel is narrower than a phone: the strip beside it is scrim.
        function tapBeside(sheet) {
            var x = 4, y = sheet.height / 2;
            verify(!sheet.hitsPanel(x, y), "the side margin lies outside the panel");
            mouseClick(sheet, x, y);
        }
        // Scroll the editor's panel until a control is on screen, as a finger
        // would, leaving the focused field where it is.
        function scrollTo(editor, control) {
            var scroll = findChild(editor, "modalSheetScroll");
            wait(0); // let the sheet's own focus reveal settle first
            scroll.contentY = Math.max(0, Math.min(scroll.contentHeight - scroll.height, control.mapToItem(scroll.contentItem, 0, 0).y));
            waitForRendering(editor);
            var y = control.mapToItem(scroll, 0, 0).y;
            verify(y >= 0 && y + control.height <= scroll.height, "the control is on screen");
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
            if (method === "back")
                verify(Ui.Navigation.back(editor.Window.window));
            else if (method === "escape")
                keyClick(Qt.Key_Escape);
            else
                findChild(editor, "searchRangeCancel").activate();
        }
        // A range edited, then dismissed by Back, Escape or Cancel, asks before
        // the edit goes; an untouched range just closes.
        function test_range_editor_asks_before_dropping_edits_data() {
            return [{tag: "back", method: "back"}, {tag: "escape", method: "escape"}, {tag: "cancel", method: "cancel"}];
        }
        // Issue #718: the range editor is a form. A tap on the side margin keeps
        // it open, adding or editing, changed or not, and asks nothing.
        function test_range_editor_keeps_on_a_side_tap_data() {
            return [{tag: "add", row: -1}, {tag: "edit", row: 0}];
        }
        function test_range_editor_keeps_on_a_side_tap(data) {
            loader.item.sessionActive = false;
            var editor = findChild(loader.item, "signalSearchRangeEditor");
            editor.openFor(data.row, model.defaultRange());
            waitForRendering(editor);
            tapBeside(editor);
            wait(50);
            verify(editor.visible, "an untouched editor stays open");
            var name = findChild(editor, "searchRangeName");
            name.text = "Weekend";
            tapBeside(editor);
            wait(50);
            verify(editor.visible, "an edited editor stays open");
            verify(!shownDiscard(editor), "and asks nothing");
            compare(name.text, "Weekend");
            verify(Ui.Navigation.back(editor.Window.window));
            verify(shownDiscard(editor) !== null, "Back after the change asks");
            findChild(shownDiscard(editor), "discardDialogDiscard").activate();
            tryCompare(editor, "visible", false);
        }
        // The emulator report: the whole name still composing on the keyboard.
        // Save by tap, keyboard or accessibility action keeps it.
        function test_range_editor_saves_a_composing_name_data() {
            return [{tag: "tap", how: "tap"}, {tag: "keyboard or accessibility", how: "activate"}];
        }
        function test_range_editor_saves_a_composing_name(data) {
            loader.item.sessionActive = false;
            var original = model.ranges;
            var editor = findChild(loader.item, "signalSearchRangeEditor");
            editor.openFor(0, model.defaultRange());
            waitForRendering(editor);
            var name = findChild(editor, "searchRangeName");
            name.text = "";
            name.input.forceActiveFocus();
            verify(testContext.composeText("UHFwe"));
            compare(name.text, "", "the keyboard still holds the name");
            var save = findChild(editor, "searchRangeSave");
            if (data.how === "tap") {
                scrollTo(editor, save);
                mouseClick(save, save.width / 2, save.height / 2);
            } else {
                save.activate();
            }
            tryCompare(editor, "visible", false);
            compare(model.ranges[0].name, "UHFwe", "Save keeps the composing name");
            model.ranges = original;
        }
        // A change that exists only as composing text still counts: Cancel (tap
        // or accessibility action), Back and Escape ask instead of closing.
        function test_range_editor_counts_a_composing_edit_data() {
            return [{tag: "cancel tap", method: "cancel tap"}, {tag: "cancel action", method: "cancel"}, {tag: "back", method: "back"}, {tag: "escape", method: "escape"}];
        }
        function test_range_editor_counts_a_composing_edit(data) {
            loader.item.sessionActive = false;
            var editor = findChild(loader.item, "signalSearchRangeEditor");
            editor.openFor(0, model.defaultRange());
            waitForRendering(editor);
            var name = findChild(editor, "searchRangeName");
            name.input.forceActiveFocus();
            name.input.cursorPosition = name.text.length;
            verify(testContext.composeText("We"));
            compare(name.text, "Local UHF");
            if (data.method === "cancel tap") {
                var cancel = findChild(editor, "searchRangeCancel");
                scrollTo(editor, cancel);
                mouseClick(cancel, cancel.width / 2, cancel.height / 2);
            } else {
                dismissEditor(editor, data.method);
            }
            verify(editor.visible, "the composing edit survives");
            verify(shownDiscard(editor) !== null, "the editor asks before discarding it");
            compare(name.text, "Local UHFWe");
            findChild(shownDiscard(editor), "discardDialogDiscard").activate();
            tryCompare(editor, "visible", false);
            compare(model.ranges[0].name, "Local UHF", "a discarded edit is never saved");
        }
        // A tap inside the field being typed in is the keyboard's: it does not
        // finish the word (cursor and suggestion handling stay with the IME).
        function test_range_editor_tap_in_field_keeps_composing() {
            loader.item.sessionActive = false;
            var editor = findChild(loader.item, "signalSearchRangeEditor");
            editor.openFor(0, model.defaultRange());
            waitForRendering(editor);
            var name = findChild(editor, "searchRangeName");
            name.text = "";
            name.input.forceActiveFocus();
            verify(testContext.composeText("We"));
            mouseClick(name.input, 4, name.input.height / 2);
            compare(name.input.preeditText, "We");
            compare(name.text, "");
            verify(name.input.activeFocus);
            verify(testContext.composeText(""));
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
        // An Airspy gain typed into its field commits only when the field loses
        // focus. Dismissed before that, by any route, the typing still counts:
        // the editor asks, whether the text is a valid index yet or not, and
        // Save keeps a valid one.
        function test_range_editor_counts_a_pending_airspy_gain_data() {
            var rows = [];
            for (var method of ["back", "escape", "cancel"]) {
                rows.push({tag: method + ", valid", method: method, text: "15"});
                rows.push({tag: method + ", not yet valid", method: method, text: ""});
            }
            return rows;
        }
        function test_range_editor_counts_a_pending_airspy_gain(data) {
            loader.item.sessionActive = false;
            loader.item.sourceType = "airspy";
            var original = model.ranges;
            var editor = findChild(loader.item, "signalSearchRangeEditor");
            editor.openFor(0, Object.assign(model.defaultRange(), {airspyGainSet: true,
                airspy: {gain_mode: "sensitivity", sensitivity_gain: 10}}));
            waitForRendering(editor);
            var gain = findChild(editor, "airspy_sensitivity_gain");
            verify(gain !== null && gain.visible);
            compare(gain.text, "10");
            gain.forceActiveFocus();
            gain.selectAll();
            if (data.text.length)
                keyClick(Qt.Key_5);
            else
                keyClick(Qt.Key_Backspace);
            if (data.text.length) {
                gain.cursorPosition = 0;
                keyClick(Qt.Key_1);
            }
            compare(gain.text, data.text);
            dismissEditor(editor, data.method);
            verify(editor.visible, "the typed gain survives the dismissal");
            var discard = shownDiscard(editor);
            verify(discard !== null, "the editor asks before discarding a pending gain");
            findChild(discard, "discardDialogKeep").activate();
            verify(editor.visible);
            compare(gain.text, data.text);
            if (data.text.length) {
                gain.forceActiveFocus();
                findChild(editor, "searchRangeSave").activate();
                tryCompare(editor, "visible", false);
                compare(String(model.ranges[0].airspy.sensitivity_gain), "15", "Save keeps the typed gain");
            } else {
                dismissEditor(editor, data.method);
                findChild(shownDiscard(editor), "discardDialogDiscard").activate();
                tryCompare(editor, "visible", false);
            }
            model.ranges = original;
            loader.item.sourceType = "usb";
        }
        // Every field the editor fills counts, including a selector's choice.
        function test_range_editor_counts_every_field() {
            loader.item.sessionActive = false;
            var editor = findChild(loader.item, "signalSearchRangeEditor");
            editor.openFor(0, model.defaultRange());
            waitForRendering(editor);
            findChild(editor, "searchRangeName").text = "Renamed";
            dismissEditor(editor, "back");
            verify(shownDiscard(editor) !== null);
            findChild(shownDiscard(editor), "discardDialogDiscard").activate();
            tryCompare(editor, "visible", false);
            editor.openFor(0, model.defaultRange());
            waitForRendering(editor);
            findChild(editor, "searchRangeSquelch").currentIndex = 0;
            dismissEditor(editor, "back");
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
        // Main binds sessionActive to DecoderHost (sessionStateChanged) and
        // sessionSource to the metrics poll (controlChanged), which can land on
        // either side of the stop. Whichever order, the receiver a stopped
        // session leaves is the baseline, and Back asks nothing.
        function test_stopped_session_leaves_no_phantom_edit_data() {
            return [{tag: "source before stop", sourceFirst: true}, {tag: "source after stop", sourceFirst: false}];
        }
        function test_stopped_session_leaves_no_phantom_edit(data) {
            var screen = loader.item;
            screen.sessionActive = false;
            screen.seed("usb", "192.168.1.10", 1234);
            model.status = {phase: 1, session: "19", owned: true};
            screen.sessionActive = true;
            screen.sessionSource = {sourceType: "rtltcp", host: "10.0.0.5", port: 1235};
            compare(screen.sourceType, "rtltcp");
            if (data.sourceFirst)
                screen.sessionSource = {sourceType: "airspy"};
            screen.sessionActive = false;
            if (!data.sourceFirst) {
                // A poll after the stop: no owned session, so it changes nothing.
                screen.sessionSource = {sourceType: "airspy"};
                compare(screen.sourceType, "rtltcp");
                screen.sessionSource = {};
            }
            model.status = {phase: 0, session: "19", owned: false};
            screen.requestClose();
            compare(closed.count, 1, "a stop is not an edit");
            verify(!shownDiscard(screen));
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
            // The panel's short form; the full phrase is the accessible description.
            var identification = findChild(loader.item, "searchIdentification");
            compare(identification.text, "NFM · digital unresolved");
            compare(identification.Accessible.description, "NFM monitoring · digital identification unresolved");
            model.status = Object.assign({}, model.status, {phase: 3});
            compare(findChild(loader.item, "searchIdentification").text, "Trying digital identification…");
            model.status = Object.assign({}, model.status, {phase: 5, identifying: true});
            compare(findChild(loader.item, "searchIdentification").text, "Trying digital identification…");
        }
        function test_live_strength_is_shown_beside_detection_power() {
            var strength = findChild(loader.item, "searchStrength");
            compare(strength.text, "-32.1 dBFS");
            compare(strength.Accessible.description, "-32.1 dBFS · Local UHF");
            model.status = Object.assign({}, model.status, {livePowerValid: true, livePower: -47.25});
            compare(strength.text, "Now -47.3 · detected -32.1 dBFS");
            compare(strength.Accessible.description, "Now -47.3 · detected -32.1 dBFS · Local UHF");
            // Readings that round to zero carry no minus sign (issue #669).
            model.status = Object.assign({}, model.status, {livePowerValid: true, livePower: -0.04,
                hit: Object.assign({}, model.status.hit, {power: -0.04})});
            compare(strength.text, "Now 0.0 · detected 0.0 dBFS");
            model.status = Object.assign({}, model.status, {livePowerValid: false});
            compare(strength.text, "0.0 dBFS");
        }
        function test_history_power_rounding_to_zero_has_no_sign() {
            model.history = [historyHit({id: "60", power: -0.04})];
            var reading = findChildren(loader.item, "searchHistoryReading")[0];
            verify(reading !== undefined, "the history reading is missing");
            compare(reading.text.replace(/\u00a0/g, " "), "462.575000 MHz · 0.0 dBFS");
            model.history = [];
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
        // The status line names the range the engine is searching, never the
        // retained detection's (which can belong to an earlier range), and a
        // paused range edit changes nothing until Resume.
        function test_status_line_names_the_running_range() {
            var line = findChild(loader.item, "searchStatusLine");
            model.status = Object.assign({}, model.status, {phase: 1, rangeName: "Air band", rangeStartMHz: "118.000", rangeEndMHz: "137.000"});
            compare(line.text, "Pass 2 · Air band · 118.000–137.000 MHz · channel 81 of 801");
            compare(findChild(loader.item, "searchIdentification").text, "Searching Air band…");
            // A detection in range A, then a quiet range B: B is named.
            model.status = Object.assign({}, model.status, {phase: 1, rangeName: "Marine", rangeStartMHz: "156.000", rangeEndMHz: "162.025", channel: 0, count: 242,
                hit: Object.assign({}, model.status.hit, {range: "Air band", ended: true})});
            compare(line.text, "Pass 2 · Marine · 156.000–162.025 MHz · channel 1 of 242");
            // Paused, an edited range is not what the search runs yet.
            model.status = Object.assign({}, model.status, {phase: 6});
            model.saveRange(0, Object.assign({}, model.ranges[0], {name: "Renamed"}));
            compare(line.text, "Pass 2 · Marine · 156.000–162.025 MHz · channel 1 of 242");
            compare(findChild(loader.item, "searchIdentification").text, "Paused");
            model.ranges = [Object.assign({}, model.ranges[0], {name: "Local UHF"})];
        }
        // Collapsed while a search runs; opened by hand it stays open through
        // status updates, until the screen closes or a new session starts.
        // Paused or idle, the ranges are always open, and opening them does not
        // take focus.
        function test_ranges_disclosure() {
            var screen = loader.item;
            var toggle = findChild(screen, "searchRangesToggle");
            var ranges = findChild(screen, "searchRanges");
            verify(toggle.visible && !ranges.visible, "collapsed while searching");
            compare(toggle.title, "Search ranges · 1 enabled");
            compare(toggle.accessibleName, "Show search ranges, 1 enabled");
            verify(toggle.Accessible.checkable);
            verify(!toggle.Accessible.checked);
            compare(toggle.Accessible.role, Accessible.Button);
            verify(toggle.height >= Ui.Theme.minimumTouchSize);
            toggle.activate();
            verify(ranges.visible);
            compare(toggle.accessibleName, "Hide search ranges");
            verify(toggle.Accessible.checked);
            model.status = Object.assign({}, model.status, {channel: 81});
            verify(ranges.visible, "a status update keeps it open");
            toggle.activate();
            verify(!ranges.visible);
            // Paused: forced open, with the edit controls, and focus stays put.
            var pause = findChild(screen, "searchStartPause");
            pause.forceActiveFocus();
            model.status = Object.assign({}, model.status, {phase: 6});
            verify(ranges.visible && screen.editable);
            verify(pause.activeFocus, "forced expansion does not move focus");
            root.forceActiveFocus();
            verify(!toggle.tapEnabled);
            compare(toggle.accessibleName, "Search ranges, 1 enabled");
            compare(toggle.Accessible.role, Accessible.Heading, "paused, the row is the ranges' heading, not a button");
            verify(!toggle.Accessible.checkable);
            model.status = Object.assign({}, model.status, {phase: 1});
            verify(!ranges.visible, "a Resume returns to the manual state");
            // A manual expansion resets with a new session and when the screen closes.
            toggle.activate();
            verify(ranges.visible);
            model.status = Object.assign({}, model.status, {session: "18"});
            verify(!ranges.visible);
            toggle.activate();
            verify(ranges.visible);
            screen.visible = false;
            screen.visible = true;
            verify(!ranges.visible);
            // Idle: open, under its plain heading.
            screen.sessionActive = false;
            verify(ranges.visible && !toggle.visible);
        }
        function test_recent_detections_show_five_then_more() {
            var rows = [];
            for (var i = 0; i < 40; ++i)
                rows.push({id: String(100 + i), freqMHz: "451.0" + i, power: -40, confirmed: false, ended: true});
            model.history = rows;
            var screen = loader.item;
            function shown() { return findChildren(screen, "searchHistoryRow").filter(function (r) { return r.visible; }).length; }
            tryCompare(screen, "historyLimit", 5);
            compare(shown(), 5);
            var more = findChild(screen, "searchShowMore");
            verify(more.visible);
            more.clicked();
            compare(shown(), 30);
            more = findChild(screen, "searchShowMore");
            more.clicked();
            compare(shown(), 40);
            verify(!findChild(screen, "searchShowMore").visible);
            // A new session starts from five again, and so does the next visit.
            model.status = Object.assign({}, model.status, {session: "18"});
            compare(shown(), 5);
            findChild(screen, "searchShowMore").clicked();
            screen.visible = false;
            screen.visible = true;
            compare(shown(), 5);
            model.history = [];
        }
        function buttonLabel(button) {
            for (var child of button.children) {
                if (child.text === button.text)
                    return child;
            }
            return null;
        }
        function findChildren(item, name) {
            var found = [];
            if (!item)
                return found;
            if (item.objectName === name)
                found.push(item);
            for (var child of item.children)
                found = found.concat(findChildren(child, name));
            return found;
        }
        // The Now panel's height depends on width and font scale only: every
        // state and every content extreme below measures the same.
        function test_now_panel_height_is_steady_data() {
            return [{tag: "default text", scale: 1.0, width: 411}, {tag: "largest text", scale: 1.6, width: 411},
                {tag: "360 dp, default text", scale: 1.0, width: 360}, {tag: "360 dp, largest text", scale: 1.6, width: 360}];
        }
        function test_now_panel_height_is_steady(data) {
            root.width = data.width;
            Ui.Theme.fontScale = data.scale;
            var screen = loader.item;
            var panel = findChild(screen, "searchNowPanel");
            var base = model.status;
            var hit = base.hit;
            var long = "Regional interoperability talkgroup band with a very long name";
            var states = [
                {tag: "searching", status: {phase: 1, rangeName: "Air band", hit: {}}},
                {tag: "tuning", status: {phase: 2, hit: {}}},
                {tag: "tuning to detection", status: {phase: 2}},
                {tag: "listening", status: {phase: 4}},
                {tag: "held", status: {phase: 5}},
                {tag: "paused", status: {phase: 6, hit: Object.assign({}, hit, {ended: true})}},
                {tag: "long range name", status: {phase: 4, rangeName: long, livePowerValid: true, livePower: -47.2, hit: Object.assign({}, hit, {range: long})}},
                {tag: "three saved matches", status: {phase: 4, hit: Object.assign({}, hit, {knownNames: ["Warehouse", "Dock security", "Yard"], knownLabel: "Warehouse / Dock security / Yard"})}},
                {tag: "unresolved", status: {phase: 4, hit: Object.assign({}, hit, {unresolved: true})}},
                {tag: "longest detail", status: {phase: 4, hit: Object.assign({}, hit, {confirmed: true, protocol: "DMR", access: "CC 15 · " + long, tone: "CTCSS 250.3 Hz", targetText: "Target " + long, sourceText: "Source " + long,
                    site: longestSite, slot: 2, crypt: "ALG 84 · KID 0001"})}}
            ];
            var heights = [];
            var hold = findChild(screen, "searchHold");
            var holdX = -1, holdWidth = -1;
            for (var st of states) {
                model.status = Object.assign({}, base, st.status);
                waitForRendering(panel);
                heights.push(st.tag + ": " + panel.height);
                compare(panel.height, findChild(screen, "searchNowPanel").height);
                if (st.tag === "searching")
                    var expected = panel.height;
                compare(panel.height, expected, st.tag + " keeps the panel's height");
                // A frequency never elides, and no line outgrows its slot.
                verify(!findChild(screen, "searchFrequency").truncated, "the frequency shows whole in " + st.tag);
                var frequency = findChild(screen, "searchFrequency");
                verify(frequency.contentWidth <= frequency.width + 0.5, "the frequency fits its slot in " + st.tag);
                var detail = findChild(screen, "searchDetail");
                verify(detail.height <= detail.parent.height + 0.5, "the detail keeps its two-line slot in " + st.tag);
                for (var name of ["searchStrength", "searchIdentification", "searchSaved"]) {
                    var line = findChild(screen, name);
                    verify(line.contentHeight <= line.height + 0.5, name + " keeps its one-line slot in " + st.tag);
                }
                var more = findChild(screen, "searchSavedMore");
                if (st.tag === "three saved matches") {
                    verify(more.visible && !more.truncated, "the count of further matches always shows");
                    compare(more.text, "+1");
                    verify(more.x + more.width <= more.parent.width + 0.5);
                }
                if (screen.receiving) {
                    if (holdX < 0) {
                        holdX = hold.x;
                        holdWidth = hold.width;
                    }
                    compare(hold.x, holdX, "Hold and Release Hold share one cell");
                    compare(hold.width, holdWidth);
                    for (var cellName of ["searchHold", "searchSkip", "searchAvoidSession", "searchAvoidPermanent", "searchSave", "searchMore"]) {
                        var label = buttonLabel(findChild(screen, cellName));
                        verify(!label.truncated, cellName + " shows its whole label in " + st.tag);
                    }
                }
            }
            var actions = findChild(screen, "searchActions");
            var narrowLarge = data.width === 360 && data.scale > 1;
            // A narrow phone at the largest text cannot fit "permanently" in half
            // a row: one column, so no word breaks mid-way.
            compare(actions.columns, narrowLarge ? 1 : 2, "a phone is two columns wide, or one where a word would break");
            compare(actions.rows, narrowLarge ? 6 : 3, "six cells");
            var more = findChild(screen, "searchMore");
            compare(more.y, (actions.rows - 1) * (actions.cellHeight + actions.spacing), "More closes the last row");
            if (data.width === 411)
                compare(actions.labelLines, data.scale > 1 ? 2 : 1);
            else if (data.scale > 1)
                compare(actions.labelLines, 1, "a full-width cell holds every label on one line");
            model.status = Object.assign({}, base, {phase: 1});
            verify(!findChild(screen, "searchStatusLine").truncated, "the status line wraps onto its second line");
            verify(actions.y + actions.height <= panel.height);
            model.status = Object.assign({}, base, {phase: 5});
            compare(hold.text, "Release Hold");
            model.status = base;
        }
        // The longest site a detection can carry: 95 bytes, its field's limit.
        readonly property string longestSite: "WACN BEE00 · SYS 1A3 · RFSS 255 · Site 255 · TIII Large · Net 1024 · Site 4096.3 · SYS 3"
        // What the detail shows is whole values: every one, or the leading
        // ones that fit followed by "· …", never a value cut short.
        function verifyWholeValues(detail, tag) {
            var shown = detail.text.replace(/\u00a0/g, " ");
            var full = detail.Accessible.description;
            verify(!detail.truncated, "nothing is cut by elision in " + tag);
            verify(detail.lineCount <= 2, "two lines at most in " + tag);
            if (shown !== full) {
                verify(shown.endsWith(" · …"), "\"" + shown + "\" ends after a whole value in " + tag);
                verify((full + " · ").startsWith(shown.slice(0, -1)), "\"" + shown + "\" is the full text's leading values in " + tag);
            }
            verify(!/[^·] /.test(detail.text), "a value never splits across lines in " + tag);
            return shown;
        }
        // The detail reads site, access code, the call with its slot, then
        // its encryption; what the two lines cannot hold is left off whole
        // values at a time, and the whole text is the accessible
        // description.
        function test_detail_reads_site_call_then_encryption_data() {
            return [{tag: "default text", scale: 1.0}, {tag: "largest text", scale: 1.6}];
        }
        function test_detail_reads_site_call_then_encryption(data) {
            Ui.Theme.fontScale = data.scale;
            var screen = loader.item;
            var detail = findChild(screen, "searchDetail");
            var panel = findChild(screen, "searchNowPanel");
            waitForRendering(panel);
            var height = panel.height;
            var hit = model.status.hit;
            model.status = Object.assign({}, model.status, {hit: Object.assign({}, hit, {confirmed: true, protocol: "P25 Phase 1", unresolved: false,
                site: "WACN BEE00 · SYS 1A3 · RFSS 1 · Site 2", access: "NAC 293", targetText: "", target: 2401, sourceText: "", source: 1234567})});
            compare(detail.Accessible.description, "WACN BEE00 · SYS 1A3 · RFSS 1 · Site 2 · NAC 293 · Target 2401 · Source 1234567");
            compare(detail.Accessible.name, detail.Accessible.description);
            // Shown, it breaks only after a separator: every other space is non-breaking.
            var p25 = verifyWholeValues(detail, "P25");
            if (data.scale === 1.0)
                compare(p25, detail.Accessible.description, "the P25 detail fits whole at default text");
            model.status = Object.assign({}, model.status, {hit: Object.assign({}, hit, {confirmed: true, protocol: "DMR", unresolved: false,
                site: "TIII Small · Net 12 · Site 3 · SYS 1183", access: "CC 1", slot: 2, target: 100, source: 2001, crypt: "Encrypted"})});
            compare(detail.Accessible.description, "TIII Small · Net 12 · Site 3 · SYS 1183 · CC 1 · Slot 2 · Target 100 · Source 2001 · Encrypted");
            var dmr = verifyWholeValues(detail, "DMR");
            if (data.scale === 1.0)
                compare(dmr, detail.Accessible.description, "the DMR detail fits whole at default text");
            // The longest: the values that fit and "· …", whole for a screen reader.
            var longest = {confirmed: true, protocol: "DMR", unresolved: false, site: longestSite, access: "CC 15", slot: 1,
                targetText: "Regional interoperability talkgroup band with a very long name", sourceText: "Unit 7 portable", crypt: "ALG 84 · KID 0001"};
            model.status = Object.assign({}, model.status, {hit: Object.assign({}, hit, longest)});
            waitForRendering(panel);
            var shown = verifyWholeValues(detail, "the longest detail");
            verify(shown.endsWith(" · …"), "the longest detail leaves values off");
            verify(detail.height <= detail.parent.height + 0.5, "inside its two lines");
            verify(detail.Accessible.description.endsWith("Slot 1 · Regional interoperability talkgroup band with a very long name · Unit 7 portable · ALG 84 · KID 0001"));
            verify(detail.Accessible.description.startsWith(longestSite + " · CC 15"));
            compare(panel.height, height, "the panel keeps its height");
            // A clear call on a one-slot protocol names no slot and no encryption.
            model.status = Object.assign({}, model.status, {hit: Object.assign({}, hit, {confirmed: true, protocol: "NXDN48", unresolved: false, site: "Site 12 · Sys 3", slot: 0, crypt: "", target: 5, source: 6})});
            compare(detail.Accessible.description, "Site 12 · Sys 3 · Target 5 · Source 6");
        }
        // A history row keeps the site and encryption it was checked with.
        function test_history_row_shows_site_and_encryption() {
            model.history = [historyHit({id: "50", confirmed: true, protocol: "DMR", site: "TIII Small · Net 12 · Site 3 · SYS 1183", slot: 2, crypt: "ALG 21 · KID 0001", knownLabel: "Warehouse · 462.5750 MHz"}),
                historyHit({id: "51", confirmed: true, protocol: "P25 Phase 1", site: "WACN BEE00 · SYS 1A3 · RFSS 1 · Site 2"}),
                historyHit({id: "52"})];
            var rows = findChildren(loader.item, "searchHistoryDetail");
            compare(rows.length, 3);
            compare(rows[0].fullText, "DMR · confirmed · ended\nTIII Small · Net 12 · Site 3 · SYS 1183 · ALG 21 · KID 0001\nWarehouse · 462.5750 MHz");
            compare(rows[1].fullText, "P25 Phase 1 · confirmed · ended\nWACN BEE00 · SYS 1A3 · RFSS 1 · Site 2");
            compare(rows[2].fullText, "RF only · ended");
            compare(rows[0].text.replace(/\u00a0/g, " "), rows[0].fullText);
            verify(!/[^·\n] /.test(rows[0].text), "a value never splits across lines");
            compare(rows[0].Accessible.name, "DMR · confirmed · ended · TIII Small · Net 12 · Site 3 · SYS 1183 · ALG 21 · KID 0001 · Warehouse · 462.5750 MHz");
            model.history = [];
        }
        // Wider, every cell fits the widest label: three equal columns.
        function test_now_actions_use_three_columns_when_they_fit() {
            root.width = 900;
            var actions = findChild(loader.item, "searchActions");
            tryCompare(actions, "columns", 3);
            var save = findChild(loader.item, "searchSave");
            var skip = findChild(loader.item, "searchSkip");
            compare(save.width, skip.width);
            compare(actions.rows, 2);
            compare(findChild(loader.item, "searchMore").width, skip.width);
            verify(actions.widest + 2 * Ui.Theme.cardPadding <= actions.cellWidth);
        }
        // Each fresh live measurement marks the cue, even one that reads the same
        // level; a withdrawn live level hides it and shows the detection level.
        // Every identification the panel can show fits one line on a phone at
        // default text: the verdict is never cut off.
        function test_identification_fits_a_phone() {
            var screen = loader.item;
            var line = findChild(screen, "searchIdentification");
            var base = model.status;
            var hit = base.hit;
            var states = [
                {phase: 0, hit: {}}, {phase: 1, hit: {}}, {phase: 2, hit: {}}, {phase: 6, hit: {}}, {phase: 7, hit: {}},
                {phase: 2}, {phase: 3}, {phase: 5, identifying: true},
                {phase: 4, hit: Object.assign({}, hit, {confirmed: true, protocol: "NXDN48"})},
                {phase: 4, hit: Object.assign({}, hit, {unresolved: true})},
                {phase: 4, hit: Object.assign({}, hit, {unresolved: false, am: true})},
                {phase: 4, hit: Object.assign({}, hit, {unresolved: false, am: false})},
                {phase: 5, hit: Object.assign({}, hit, {unresolved: true})}
            ];
            for (var st of states) {
                model.status = Object.assign({}, base, st);
                verify(line.text.length > 0);
                verify(!line.truncated, "\"" + line.text + "\" fits one line at 411 dp");
            }
        }
        // Between detections the reserved panel teaches what it is for, at the
        // same height, and reads in order.
        function test_now_panel_waiting_state() {
            var screen = loader.item;
            var panel = findChild(screen, "searchNowPanel");
            var listening = panel.height;
            model.status = Object.assign({}, model.status, {phase: 1, hit: {}});
            compare(panel.height, listening);
            var frequency = findChild(screen, "searchFrequency");
            compare(frequency.text, "Waiting for a signal");
            compare(frequency.font.family, Ui.Theme.sans, "not the frequency's mono face");
            verify(Qt.colorEqual(frequency.color, Ui.Theme.textSecondary));
            verify(!frequency.Accessible.ignored);
            compare(findChild(screen, "searchIdentification").text, "Searching Local UHF…");
            var detail = findChild(screen, "searchDetail");
            compare(detail.text, "Strong signals open here to be identified and monitored.");
            verify(!detail.truncated && !detail.Accessible.ignored);
            verify(findChild(screen, "searchStrength").Accessible.ignored);
            verify(!findChild(screen, "searchHold").visible && !findChild(screen, "searchSave").visible);
            var y = [frequency, findChild(screen, "searchIdentification"), detail].map(function (item) { return item.mapToItem(panel, 0, 0).y; });
            verify(y[0] < y[1] && y[1] < y[2], "reading order is top to bottom");
            model.status = Object.assign({}, model.status, {phase: 2, hit: {}});
            compare(findChild(screen, "searchIdentification").text, "Tuning…");
            compare(frequency.text, "Waiting for a signal");
            model.status = Object.assign({}, model.status, {phase: 6, hit: {}});
            compare(findChild(screen, "searchIdentification").text, "Paused");
        }
        // A 63-character range name gives way, so the channel count always shows.
        function test_status_line_keeps_the_channel_count() {
            Ui.Theme.fontScale = 1.6;
            var line = findChild(loader.item, "searchStatusLine");
            var name = "Regional interoperability and public safety mutual aid channels";
            compare(name.length, 63);
            model.status = Object.assign({}, model.status, {rangeName: name, rangeStartMHz: "450.000000", rangeEndMHz: "459.987500"});
            waitForRendering(line);
            verify(!line.truncated, "the line fits its two lines");
            verify(line.text.indexOf("channel 81 of 801") > 0);
            verify(line.text.indexOf("450.000000–459.987500 MHz") > 0);
            verify(line.text.indexOf("…") > 0, "the range name is shortened");
            Ui.Theme.fontScale = 1.0;
            verify(!line.truncated);
            verify(line.text.indexOf("channel 81 of 801") > 0);
        }
        // Long valid bounds on a narrow phone at the largest text: the pass and
        // the channel count still show whole, whatever the range name, in a
        // line count the width and the font scale alone decide.
        function test_status_line_keeps_progress_with_long_bounds_data() {
            var rows = [];
            for (var width of [360, 411]) {
                for (var scale of [1.0, 1.6])
                    rows.push({tag: width + " dp, scale " + scale, width: width, scale: scale});
            }
            return rows;
        }
        function test_status_line_keeps_progress_with_long_bounds(data) {
            root.width = data.width;
            Ui.Theme.fontScale = data.scale;
            var screen = loader.item;
            var line = findChild(screen, "searchStatusLine");
            var slot = line.parent;
            var heights = [];
            for (var name of ["Air", "Regional interoperability and public safety mutual aid channels"]) {
                for (var bounds of [["118.000", "137.000"], ["118.008333", "136.991667"]]) {
                    model.status = Object.assign({}, model.status, {phase: 1, pass: 12, channel: 99998, count: 99999,
                        rangeName: name, rangeStartMHz: bounds[0], rangeEndMHz: bounds[1]});
                    waitForRendering(line);
                    verify(!line.truncated, "the line fits its slot for " + bounds.join("–"));
                    verify(line.contentHeight <= slot.height + 0.5);
                    verify(line.text.indexOf("Pass 12") === 0, "the pass shows whole");
                    verify(line.text.indexOf("channel 99999 of 99999") > 0, "the channel count shows whole: " + line.text);
                    heights.push(slot.height);
                }
            }
            for (var h of heights)
                compare(h, heights[0], "the slot's height depends on width and scale only");
        }
        function test_live_cue_marks_each_measurement() {
            var cue = findChild(loader.item, "searchLiveCue");
            var strength = findChild(loader.item, "searchStrength");
            verify(!cue.visible);
            model.status = Object.assign({}, model.status, {livePowerValid: true, livePower: -47.21, liveSeq: 1});
            verify(cue.visible);
            var marks = cue.marks;
            model.status = Object.assign({}, model.status, {livePowerValid: true, livePower: -47.21, liveSeq: 2});
            compare(cue.marks, marks + 1, "a same-value fresh measurement is marked");
            compare(strength.text, "Now -47.2 · detected -32.1 dBFS");
            model.status = Object.assign({}, model.status, {channel: 3});
            compare(cue.marks, marks + 1, "a status update without a measurement is not");
            verify(cue.Accessible.ignored);
            model.status = Object.assign({}, model.status, {livePowerValid: false});
            verify(!cue.visible, "a withdrawn level hides the cue");
            compare(strength.text, "-32.1 dBFS");
            compare(strength.Accessible.description, "-32.1 dBFS · Local UHF");
            model.status = Object.assign({}, model.status, {liveSeq: 3});
            compare(cue.marks, marks + 1);
        }
        // Both levels show whole on a phone up to the largest text, even at
        // their widest values; the range is left to the status line.
        function test_strength_line_fits_a_phone_data() {
            return [{tag: "default text", scale: 1.0}, {tag: "largest text", scale: 1.6}];
        }
        function test_strength_line_fits_a_phone(data) {
            Ui.Theme.fontScale = data.scale;
            var strength = findChild(loader.item, "searchStrength");
            model.status = Object.assign({}, model.status, {livePowerValid: true, livePower: -100.04, liveSeq: 30,
                hit: Object.assign({}, model.status.hit, {power: -100.04})});
            // Two lines at the largest text break between the two levels.
            compare(strength.text, data.scale > 1 ? "Now -100.0 dBFS\ndetected -100.0 dBFS" : "Now -100.0 · detected -100.0 dBFS");
            compare(strength.Accessible.name, data.scale > 1 ? "Now -100.0 dBFS · detected -100.0 dBFS" : "Now -100.0 · detected -100.0 dBFS");
            verify(!strength.truncated, "both levels fit at 411 dp");
            verify(strength.height <= strength.parent.height + 0.5, "inside its slot");
            compare(strength.parent.lines, data.scale > 1 ? 2 : 1, "one line, or two at the largest text");
            model.status = Object.assign({}, model.status, {livePowerValid: false});
            compare(strength.text, "-100.0 dBFS");
            verify(!strength.truncated);
        }
        // A stopped or failed search shows only its phase and message, not the
        // hint that strong signals open here.
        function test_waiting_hint_only_while_searching() {
            var detail = findChild(loader.item, "searchDetail");
            var hint = "Strong signals open here to be identified and monitored.";
            var expected = {0: "", 1: hint, 2: hint, 6: hint, 7: ""};
            for (var phase of [0, 1, 2, 6, 7]) {
                model.status = Object.assign({}, model.status, {phase: phase, hit: {}, message: phase === 7 ? "No samples from the radio input" : ""});
                compare(detail.text, expected[phase], "phase " + phase);
                compare(detail.Accessible.ignored, expected[phase].length === 0);
            }
            compare(findChild(loader.item, "searchIdentification").text, "Search error");
            compare(findChild(loader.item, "searchMessage").text, "No samples from the radio input");
        }
        // The strength text keeps its place as a live level comes and goes.
        function test_strength_text_does_not_shift() {
            var strength = findChild(loader.item, "searchStrength");
            var x = strength.x;
            model.status = Object.assign({}, model.status, {livePowerValid: true, livePower: -50, liveSeq: 7});
            compare(strength.x, x);
            model.status = Object.assign({}, model.status, {livePowerValid: false});
            compare(strength.x, x);
        }
        // One refresh can bring a detection and its first measurement together;
        // that first reading is marked too.
        function test_first_measurement_is_marked() {
            var cue = findChild(loader.item, "searchLiveCue");
            var listening = model.status;
            model.status = Object.assign({}, listening, {phase: 1, hit: {}, livePowerValid: false, liveSeq: 20});
            var marks = cue.marks;
            model.status = Object.assign({}, listening, {phase: 4, livePowerValid: true, livePower: -51, liveSeq: 21});
            compare(cue.marks, marks + 1);
            verify(cue.visible);
        }
        function menuTexts() {
            return findChild(loader.item, "searchDetectionMenu").actions.map(function (a) { return a.text + (a.enabled === false ? " (disabled)" : ""); });
        }
        function menuItem(name) {
            var menu = findChild(loader.item, "searchDetectionMenu");
            verify(menu.visible);
            var item = findChild(menu, name);
            verify(item !== null && item.visible, name);
            return item;
        }
        function historyHit(extra) {
            return Object.assign({id: "40", session: "17", freqMHz: "462.575000", frequency: 462575000, power: -40, confirmed: false, ended: true, matches: []}, extra || {});
        }
        // Each detection row offers what can be done with it, by state: saved
        // systems it matched (an avoided one disabled, saying why), avoiding it
        // this session only while one runs, and the permanent avoid both ways.
        function test_history_menu_per_state_data() {
            var matched = [{systemUid: "uid-a"}, {systemUid: "uid-a"}, {systemUid: "uid-b"}, {systemUid: "gone"}];
            return [
                {tag: "unmatched, searching", hit: {}, session: true, avoided: false,
                    want: ["Explore here", "Save as system", "Avoid this session", "Avoid permanently"]},
                {tag: "matched, searching", hit: {matches: matched}, session: true, avoided: false,
                    want: ["Explore here", "Start “Warehouse”", "Start “Dock” (disabled)", "Save as system", "Avoid this session", "Avoid permanently"]},
                {tag: "no session", hit: {}, session: false, avoided: false,
                    want: ["Explore here", "Save as system", "Avoid permanently"]},
                {tag: "permanently avoided", hit: {}, session: true, avoided: true,
                    want: ["Explore here", "Save as system", "Avoid this session", "Remove permanent avoid"]},
                {tag: "permanently avoided, no session", hit: {matches: matched}, session: false, avoided: true,
                    want: ["Explore here", "Start “Warehouse”", "Start “Dock” (disabled)", "Save as system", "Remove permanent avoid"]}
            ];
        }
        function test_history_menu_per_state(data) {
            var screen = loader.item;
            screen.sessionActive = data.session;
            if (!data.session)
                model.status = {phase: 0, session: "17", owned: false};
            model.avoids = data.avoided ? [462575000] : [];
            model.history = [historyHit(data.hit)];
            wait(0);
            var more = findChild(screen, "searchHistoryActions");
            verify(more !== null && more.visible);
            compare(more.accessibleName, "Actions for 462.5750 MHz");
            verify(more.width >= Ui.Theme.minimumTouchSize && more.height >= Ui.Theme.minimumTouchSize);
            more.activate();
            var menu = findChild(screen, "searchDetectionMenu");
            verify(menu.visible);
            compare(menu.title, "462.5750 MHz");
            compare(menuTexts(), data.want);
            if (data.hit.matches) {
                compare(findChild(menu, "actionMenuDescription2").text, "Avoided; remove the avoid in its site list to start it");
                verify(!menuItem("searchActionStart1").enabled);
            }
            verify(!findChild(screen, "searchStartPause").enabled || !findChild(screen, "searchStartPause").parent.enabled, "the screen under the menu takes no taps");
            menu.cancel();
            model.history = [];
        }
        // Every row action does what it says, on the row it was opened for.
        function test_history_menu_actions() {
            var screen = loader.item;
            model.history = [historyHit({matches: [{systemUid: "uid-a"}]})];
            wait(0);
            var open = function () { findChild(screen, "searchHistoryActions").activate(); };
            open();
            menuItem("searchActionExplore").activate();
            compare(explored.count, 1);
            compare(explored.signalArguments[0][0].frequency, 462575000);
            open();
            menuItem("searchActionStart0").activate();
            compare(startedSaved.count, 1);
            compare(startedSaved.signalArguments[0][0], "uid-a");
            open();
            menuItem("searchActionSave").activate();
            compare(saved.count, 1);
            compare(saved.signalArguments[0][0].id, "40");
            open();
            menuItem("searchActionAvoidSession").activate();
            compare(model.actions.join(","), "session,17,462575000");
            open();
            menuItem("searchActionAvoidPermanent").activate();
            compare(model.actions.join(","), "permanent,17,462575000");
            model.avoids = [462575000];
            open();
            menuItem("searchActionRemovePermanent").activate();
            compare(model.lifted.join(","), "permanent:462575000");
            // The menu acts on the row it opened for, even if the list moved meanwhile.
            open();
            model.history = [historyHit({id: "41", frequency: 451000000, freqMHz: "451.000000"})];
            menuItem("searchActionExplore").activate();
            compare(explored.signalArguments[1][0].frequency, 462575000);
            model.history = [];
        }
        // The Now panel's More leaves for the current detection: Explore here
        // and the saved systems it matched; the rest are its own buttons.
        function test_now_more_offers_leaving_for_the_detection() {
            var screen = loader.item;
            model.status = Object.assign({}, model.status, {hit: Object.assign({}, model.status.hit, {matches: [{systemUid: "uid-a"}, {systemUid: "uid-b"}]})});
            var more = findChild(screen, "searchMore");
            verify(more.visible);
            compare(more.accessibleName, "More actions for this detection");
            more.clicked();
            compare(findChild(screen, "searchDetectionMenu").title, "463.5521 MHz");
            compare(menuTexts(), ["Explore here", "Start “Warehouse”", "Start “Dock” (disabled)"]);
            menuItem("searchActionExplore").activate();
            compare(explored.count, 1);
            compare(explored.signalArguments[0][0].id, "42");
            more.clicked();
            menuItem("searchActionStart0").activate();
            compare(startedSaved.signalArguments[0][0], "uid-a");
            // Hidden with the other actions between detections.
            model.status = Object.assign({}, model.status, {phase: 1, hit: {}});
            verify(!more.visible);
        }
        // An avoid the decoder refuses (either list full, from the Now panel or
        // a row) is the status message, which the screen shows; with no session
        // a refused permanent avoid is the model's error.
        function test_avoid_refusals_are_shown_data() {
            return [{tag: "session list full", message: "The avoid list is full"},
                {tag: "permanent list full", message: "The permanent avoid list is full"},
                {tag: "permanent saved, session full", message: "Saved as a permanent avoid; this session's avoid list is full"}];
        }
        function test_avoid_refusals_are_shown(data) {
            var message = findChild(loader.item, "searchMessage");
            verify(!message.visible);
            findChild(loader.item, "searchAvoidPermanent").clicked();
            compare(model.actions.join(","), "6,17,42");
            model.status = Object.assign({}, model.status, {message: data.message});
            verify(message.visible);
            compare(message.text, data.message);
            loader.item.sessionActive = false;
            model.status = {phase: 0, session: "17"};
            model.error = "The permanent avoid list is full";
            compare(message.text, "The permanent avoid list is full");
            model.error = "";
        }
        // While a session runs the message sits under the Now panel: a refusal
        // that clears at the next visit tune, as a detection arrives, never
        // moves the panel. Idle, it leads the setup.
        function test_message_never_moves_the_now_panel() {
            var screen = loader.item;
            var hit = model.status.hit;
            var scroll = findChild(screen, "searchScroll");
            var panel = findChild(screen, "searchNowPanel");
            var message = findChild(screen, "searchMessage");
            model.status = Object.assign({}, model.status, {phase: 1, hit: {}});
            waitForRendering(screen);
            verify(!message.visible);
            var panelY = panel.mapToItem(scroll, 0, 0).y;
            model.status = Object.assign({}, model.status, {message: "The avoid list is full"});
            waitForRendering(screen);
            verify(message.visible);
            compare(message.text, "The avoid list is full");
            compare(panel.mapToItem(scroll, 0, 0).y, panelY);
            verify(message.mapToItem(scroll, 0, 0).y >= panelY + panel.height, "the message must sit under the Now panel");
            root.hostFailure = "rtl_tcp: the connection to the server was refused, so the radio input stopped";
            waitForRendering(screen);
            compare(message.text, root.hostFailure);
            compare(panel.mapToItem(scroll, 0, 0).y, panelY);
            // The refusal clears at the next visit tune as a detection arrives.
            root.hostFailure = "";
            model.status = Object.assign({}, model.status, {phase: 4, message: "", hit: hit});
            waitForRendering(screen);
            verify(!message.visible);
            compare(panel.mapToItem(scroll, 0, 0).y, panelY);
            screen.sessionActive = false;
            model.status = {phase: 0, session: "17"};
            model.error = "The permanent avoid list is full";
            waitForRendering(screen);
            verify(message.visible);
            compare(message.mapToItem(scroll, 0, 0).y, panelY);
            model.error = "";
        }
        function test_capture_menu() {
            model.history = [historyHit({matches: [{systemUid: "uid-a"}, {systemUid: "uid-b"}]})];
            wait(0);
            findChild(loader.item, "searchHistoryActions").activate();
            wait(100);
            grabImage(root).save("/tmp/dsd-signal-search-menu.png");
            findChild(loader.item, "searchDetectionMenu").cancel();
            model.history = [historyHit({matches: [{systemUid: "uid-a"}], knownLabel: "Warehouse · 462.5750 MHz", confirmed: true, protocol: "P25 Phase 1", site: "WACN BEE00 · SYS 1A3 · RFSS 1 · Site 2"}),
                historyHit({id: "39", confirmed: true, protocol: "DMR", ended: false, site: "TIII Small · Net 12 · Site 3 · SYS 1183", slot: 2, crypt: "Encrypted"})];
            var scroll = findChild(loader.item, "searchScroll");
            for (var scale of [1.0, 1.6]) {
                Ui.Theme.fontScale = scale;
                wait(50);
                var row = findChild(loader.item, "searchHistoryRow");
                scroll.contentY = Math.max(0, row.mapToItem(scroll.contentItem, 0, 0).y - 60);
                wait(100);
                grabImage(root).save(scale > 1 ? "/tmp/dsd-signal-search-history-large.png" : "/tmp/dsd-signal-search-history.png");
                var actions = findChild(loader.item, "searchHistoryActions");
                verify(actions.mapToItem(row, 0, 0).x + actions.width <= row.width + 0.5, "the row's actions stay inside it");
            }
            scroll.contentY = 0;
            model.history = [];
        }
        function test_capture_searching() {
            model.status = Object.assign({}, model.status, {phase: 1, rangeName: "Local UHF", rangeStartMHz: "450.000", rangeEndMHz: "460.000", hit: {}});
            wait(100);
            grabImage(root).save("/tmp/dsd-signal-search-searching.png");
        }
        // A P25 detection with its checked site, over a history row for an
        // encrypted DMR call.
        function test_capture_phone() {
            model.status = Object.assign({}, model.status, {hit: Object.assign({}, model.status.hit, {confirmed: true, unresolved: false, protocol: "P25 Phase 1",
                site: "WACN BEE00 · SYS 1A3 · RFSS 1 · Site 2", access: "NAC 293", target: 2401, source: 1234567})});
            model.history = [historyHit({id: "41", confirmed: true, protocol: "DMR", site: "TIII Small · Net 12 · Site 3 · SYS 1183", slot: 2, crypt: "ALG 21 · KID 0001"})];
            wait(100);
            var img = grabImage(root);
            compare(img.width, 411);
            // Optional artifact for native finish review; no screenshot comparison in the test.
            img.save("/tmp/dsd-signal-search-phone.png");
            model.history = [];
        }
        function test_capture_large_text() {
            Ui.Theme.fontScale = 1.6;
            // An encrypted DMR call on slot 2 with its Tier III site.
            model.status = Object.assign({}, model.status, {livePowerValid: true, livePower: -47.2, liveSeq: 1,
                hit: Object.assign({}, model.status.hit, {knownNames: ["Warehouse", "Dock security", "Yard"], confirmed: true, unresolved: false, protocol: "DMR",
                    site: "TIII Small · Net 12 · Site 3 · SYS 1183", access: "CC 1", slot: 2, target: 100, source: 2001, crypt: "ALG 21 · KID 0001"})});
            wait(100);
            grabImage(root).save("/tmp/dsd-signal-search-large.png");
            var button = findChild(loader.item, "searchStartPause");
            verify(button.y + button.height <= loader.item.height);
            // A narrow phone, with long valid bounds and counts.
            root.width = 360;
            model.status = Object.assign({}, model.status, {pass: 12, channel: 99998, count: 99999, rangeName: "Regional interoperability",
                rangeStartMHz: "118.008333", rangeEndMHz: "136.991667"});
            wait(100);
            grabImage(root).save("/tmp/dsd-signal-search-large-360.png");
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
