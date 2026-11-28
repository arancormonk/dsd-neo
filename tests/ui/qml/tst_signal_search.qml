// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtQuick.Window
import QtTest
import "../../../src/ui/qt/qml" as Ui
import "../../../src/ui/qt/qml/Util.js" as Util

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
            squelchLevel: -60, probe: 3, visit: 30}]
        property var history: []
        property var avoids: []
        property var sessionAvoids: []
        property var status: ({phase: 0})
        property string error: ""
        property string storeMessage: ""
        property var actions: []
        property var lifted: []
        property int arms: 0
        property string armError: ""
        function control(action, session, hit) { actions = [action, session, hit]; return true; }
        function avoidFrequency(session, frequency) { actions = ["session", session, frequency]; return true; }
        function avoidPermanently(frequency) { actions = ["permanent", frequency]; return true; }
        function removeSessionAvoid(frequency) { lifted = lifted.concat(["session:" + frequency]); return true; }
        function removeAvoid(frequency) { lifted = lifted.concat(["permanent:" + frequency]); }
        function arm() { arms++; return armError; }
        function saveRange(row, range) { ranges = [range]; return ""; }
        function defaultRange() { return Object.assign({}, ranges[0]); }
        // Issue #826: what prepareExport() answers per kind, whether saveExport() takes the pick, and the export
        // calls the screen made, oldest first.
        property var prepared: ({})
        property bool saveAccepted: true
        property var exportCalls: []
        signal exportFinished(int token, bool ok)
        function prepareExport(kind) {
            exportCalls = exportCalls.concat([["prepare", kind]]);
            return Object.assign({}, prepared[kind] || {token: 0, count: -1});
        }
        function saveExport(token, reference) { exportCalls = exportCalls.concat([["save", token, reference]]); return saveAccepted; }
        function discardExport(token) { exportCalls = exportCalls.concat([["discard", token]]); }
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
    // The app's one Escape and Back owner, as Main registers its safe area (Navigation.backTarget).
    Item {
        id: appBack
        property int escapes: 0
        Keys.onEscapePressed: escapes++
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
        // A fixed clock: local noon on 2026-11-09, so an export's suggested name is known. A case may move it.
        readonly property double noon: new Date(2026, 10, 9, 12, 0, 0).getTime()
        property double nowMs: noon
        function realtimeNowMs() { return nowMs; }
        // Issue #902: the local day, as savedSystems.localDay notifies it: 2026-11-09.
        readonly property var firstDay: ({startMs: new Date(2026, 10, 9).getTime(), revision: 1})
        property var localDay: firstDay
    }
    // R58: History's now-playing bar and the playlist's status, made for the case that checks the bar stays silent.
    Component {
        id: playlistStatusComponent
        Ui.PlaylistStatus {}
    }
    Component {
        id: nowPlayingBarComponent
        Ui.NowPlayingBar {}
    }
    TestCase {
        name: "SignalSearch"
        when: windowShown
        // Issue #782: moving readings name themselves at the screen reader's pace (SpeechPacer); a short period and
        // no step floor keep these checks of what is read quick. The app's pace is restored for the next file in the
        // run.
        property int appSpeechPeriod: 0
        function initTestCase() {
            appSpeechPeriod = Ui.Navigation.speechPeriodMs;
            Ui.Navigation.speechPeriodMs = 50;
            Ui.Navigation.speechStepFloorMs = 1;
        }
        function cleanupTestCase() {
            Ui.Navigation.speechPeriodMs = appSpeechPeriod;
            Ui.Navigation.speechStepFloorMs = 1000;
        }
        function cleanup() {
            // Issue #826: no export, result line or hidden screen outlives its case, even one that failed part way.
            loader.item.visible = true;
            Ui.Navigation.rootSurfaces = [];
            // A case that sent the window away brings it back, even one that failed part way.
            testContext.sendWindowFocus(loader.item, true);
            Ui.Navigation.backTarget = null;
            var exportPicker = findChild(loader.item, "searchExportPicker");
            if (exportPicker && exportPicker.visible)
                exportPicker.cancel();
            if (loader.item.exportToken > 0)
                model.exportFinished(loader.item.exportToken, false);
            if (typeof loader.item.clearExportMessages === "function")
                loader.item.clearExportMessages();
            Ui.Theme.resetFontScale();
            root.width = 411;
            root.height = 891;
            var editor = findChild(loader.item, "signalSearchRangeEditor");
            editor.visible = false;
            loader.item.sessionSource = {};
            loader.item.sourceType = "usb";
            loader.item.resetView();
            // What a case left on the message line, even one that failed part way.
            model.error = "";
            model.storeMessage = "";
            root.hostFailure = "";
            model.armError = "";
            loader.item.localFailure = "";
            // A test that renames the range restores it even when it fails.
            if (model.ranges.length > 0 && model.ranges[0].name !== "Local UHF")
                model.ranges = [Object.assign({}, model.ranges[0], {name: "Local UHF"})];
            // Issue #902: the clock, the day and the zone a case moved end with
            // it, before the next case's _data() rows are built.
            loader.item.systems = systemsStub;
            systemsStub.nowMs = systemsStub.noon;
            systemsStub.localDay = systemsStub.firstDay;
            testContext.resetLocalDay();
        }
        function init() {
            verify(loader.item !== null);
            Ui.Navigation.backTarget = appBack;
            appBack.escapes = 0;
            loader.item.sessionActive = true;
            model.actions = [];
            model.lifted = [];
            model.sessionAvoids = [];
            model.avoids = [];
            model.status = {phase: 4, session: "17", owned: true, pass: 2, range: 0, channel: 80, count: 801,
                rangeName: "Local UHF", rangeStartMHz: "450.000", rangeEndMHz: "460.000",
                overloaded: false, overloadRange: "",
                hit: {id: "42", session: "17", freqMHz: "463.552100", frequency: 463552100, power: -32.1,
                    noise: -63.2, range: "Local UHF", width: 12500, unresolved: true, ended: false,
                    knownLabel: "Warehouse · 463.5500 MHz", confirmed: false, am: false}};
            model.armError = "";
            model.prepared = {detections: {token: 7, count: 2}, avoids: {token: 8, count: 1}};
            model.saveAccepted = true;
            model.exportCalls = [];
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
            // Issue #809: no recordings, no History, nothing playing, Record calls off (the app's default).
            for (var name of ["detectionRecordingsSheet", "searchCallDetails"]) {
                var sheet = findChild(loader.item, name);
                if (sheet)
                    sheet.visible = false;
            }
            recordings.reset();
            callHistory.clearAll();
            // Issue #909: a host with a player, unless a case says otherwise.
            testContext.setPlaybackSupported(true);
            testContext.resetHostPlayback();
            testContext.setPrefs("recordCalls", false);
            testContext.setPrefs("appearance", 0);
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
        // Issue #748: TETRA is the eleventh choice, stored as 10. Picking it
        // fills in a 25 kHz channel and a four-second probe; both stay
        // editable, and a range opened again keeps the values it was saved
        // with. Another choice changes neither.
        function test_range_editor_offers_tetra_with_presets() {
            var ranges = model.ranges;
            loader.item.sessionActive = false;
            var editor = findChild(loader.item, "signalSearchRangeEditor");
            var digital = findChild(editor, "searchRangeDigital");
            var width = findChild(editor, "searchRangeBandwidth");
            var probe = findChild(editor, "searchRangeProbe");
            editor.openFor(0, model.defaultRange());
            waitForRendering(editor);
            compare(digital.count, 11);
            compare(digital.textAt(9), "M17");
            compare(digital.textAt(10), "TETRA");
            compare(width.text, "16000");
            compare(probe.text, "3");
            digital.currentIndex = 10;
            digital.activated(10);
            compare(width.text, "25000");
            compare(probe.text, "4");
            compare(editor.formValue().digital, 10);
            width.text = "20000";
            probe.text = "6";
            findChild(editor, "searchRangeSave").activate();
            tryCompare(editor, "visible", false);
            compare(model.ranges[0].digital, 10);
            compare(String(model.ranges[0].bandwidth), "20000");
            compare(String(model.ranges[0].probe), "6");
            editor.openFor(0, model.ranges[0]);
            waitForRendering(editor);
            compare(digital.currentIndex, 10);
            compare(width.text, "20000", "an opened TETRA range keeps its own width");
            compare(probe.text, "6", "and its own probe");
            digital.currentIndex = 3;
            digital.activated(3);
            compare(width.text, "20000", "another choice leaves the width alone");
            compare(probe.text, "6");
            editor.visible = false;
            model.ranges = ranges;
        }
        // The hint under the choice says what a TETRA range changes, and wraps
        // whole inside the sheet at the narrowest phone and the largest text.
        function test_range_editor_tetra_hint_wraps_data() {
            return [{tag: "411 dp, default text", scale: 1.0, width: 411}, {tag: "360 dp, largest text", scale: 1.6, width: 360}];
        }
        function test_range_editor_tetra_hint_wraps(data) {
            root.width = data.width;
            Ui.Theme.fontScale = data.scale;
            loader.item.sessionActive = false;
            var editor = findChild(loader.item, "signalSearchRangeEditor");
            editor.openFor(0, model.defaultRange());
            waitForRendering(editor);
            var hint = findChild(editor, "searchDigitalHint");
            scrollTo(editor, hint);
            verify(hint.text.indexOf("Auto does not include TETRA.") >= 0);
            verify(hint.text.indexOf("72 kHz instead of 48 kHz") >= 0);
            verify(hint.text.indexOf("The default bandwidth in Settings does not apply to Signal search.") >= 0);
            verify(hint.lineCount > 2, "the hint wraps");
            verify(!hint.truncated);
            verify(hint.contentWidth <= hint.width + 0.5, "no line runs past the hint");
            verify(hint.height >= hint.contentHeight - 0.5, "no line is clipped");
            var panel = hint.parent;
            verify(hint.mapToItem(panel, 0, 0).x + hint.width <= panel.width + 0.5, "the hint stays inside the sheet");
            compare(hint.Accessible.name, hint.text);
            editor.visible = false;
        }
        // Issue #748: the editor's pairs of fields (Start and End, the step's
        // numerator and denominator, the probe and the longest visit) sit side
        // by side while half the form shows each field's whole value, and one
        // per row once it would not. At the narrowest phone with the largest
        // text, Start and End had scrolled their leading digits out of sight
        // ("0.000000"); a placeholder is shown whole too.
        function test_range_editor_pairs_show_whole_values_data() {
            return [{tag: "desktop", scale: 1.0, width: 1024, stacked: false},
                {tag: "411 dp, default text", scale: 1.0, width: 411, stacked: false},
                {tag: "360 dp, default text", scale: 1.0, width: 360, stacked: false},
                {tag: "360 dp, largest text", scale: 1.6, width: 360, stacked: true}];
        }
        function test_range_editor_pairs_show_whole_values(data) {
            root.width = data.width;
            Ui.Theme.fontScale = data.scale;
            loader.item.sessionActive = false;
            var editor = findChild(loader.item, "signalSearchRangeEditor");
            editor.openFor(0, model.defaultRange());
            waitForRendering(editor);
            var start = findChild(editor, "searchRangeStart");
            var end = findChild(editor, "searchRangeEnd");
            var visit = findChild(editor, "searchRangeVisit");
            var pairs = [[start, end],
                [findChild(editor, "searchStepNumerator"), findChild(editor, "searchStepDenominator")],
                [findChild(editor, "searchRangeProbe"), visit]];
            var form = start.parent.parent;
            function check(what) {
                for (var pair of pairs) {
                    for (var field of pair) {
                        var at = field.mapToItem(form, 0, 0);
                        verify(at.x >= -0.5 && at.x + field.width <= form.width + 0.5, field.label + " stays inside the form" + what);
                        verify(field.input.contentWidth + field.input.cursorRectangle.width <= field.input.width + 0.5,
                            field.label + " shows \"" + field.text + "\" whole" + what);
                    }
                    var side = Math.abs(pair[0].mapToItem(form, 0, 0).y - pair[1].mapToItem(form, 0, 0).y) < 0.5;
                    if (side)
                        verify(pair[1].mapToItem(form, 0, 0).x >= pair[0].mapToItem(form, 0, 0).x + pair[0].width, "side by side, apart");
                    else
                        verify(pair[1].mapToItem(form, 0, 0).y >= pair[0].mapToItem(form, 0, 0).y + pair[0].height, "one per row, apart");
                }
            }
            check("");
            compare(Math.abs(start.mapToItem(form, 0, 0).y - end.mapToItem(form, 0, 0).y) >= 0.5, data.stacked, "Start and End stacked");
            if (!data.stacked) {
                for (var pair of pairs)
                    compare(pair[0].width, pair[1].width, "two equal halves");
                compare(start.width, (form.width - 12) / 2, "the halves of the form, as before");
            }
            // A four-digit frequency, as a wide-band receiver takes, and the
            // longest visit's placeholder.
            start.text = "1240.000000";
            end.text = "1300.000000";
            visit.text = "";
            waitForRendering(editor);
            check(" (four digits)");
            var placeholder = null;
            for (var child of visit.children) {
                if (child.text === visit.placeholderText && child.visible)
                    placeholder = child;
            }
            verify(placeholder !== null, "the visit shows its placeholder");
            verify(!placeholder.truncated, "\"" + visit.placeholderText + "\" shown whole");
            editor.visible = false;
        }
        // Issue #748: the editor's check boxes take the form's width and wrap
        // their label inside it. At the narrowest phone with the largest text,
        // "Also require a minimum RF level" had run past the sheet and been cut
        // at "minimum"; the Airspy one the same.
        function test_range_editor_check_boxes_wrap_data() {
            return [{tag: "411 dp, default text", scale: 1.0, width: 411, source: "usb", name: "searchRangeMinimumSwitch", lines: 1},
                {tag: "360 dp, largest text", scale: 1.6, width: 360, source: "usb", name: "searchRangeMinimumSwitch", lines: 2},
                {tag: "Airspy, 411 dp, default text", scale: 1.0, width: 411, source: "airspy", name: "searchRangeAirspyOverride", lines: 1},
                {tag: "Airspy, 360 dp, largest text", scale: 1.6, width: 360, source: "airspy", name: "searchRangeAirspyOverride", lines: 2}];
        }
        function test_range_editor_check_boxes_wrap(data) {
            root.width = data.width;
            Ui.Theme.fontScale = data.scale;
            loader.item.sessionActive = false;
            loader.item.sourceType = data.source;
            var editor = findChild(loader.item, "signalSearchRangeEditor");
            editor.openFor(0, model.defaultRange());
            waitForRendering(editor);
            var box = findChild(editor, data.name);
            verify(box.visible);
            scrollTo(editor, box);
            var form = box.parent;
            var label = box.contentItem;
            verify(box.mapToItem(form, 0, 0).x + box.width <= form.width + 0.5, "the check box stays inside the form");
            verify(label.contentWidth + label.leftPadding + label.rightPadding <= label.width + 0.5, "no line runs past the label");
            verify(label.height >= label.contentHeight + label.topPadding + label.bottomPadding - 0.5, "no line is cut off");
            verify(box.height >= label.y + label.height - 0.5, "the box holds every line");
            compare(label.lineCount, data.lines);
            editor.visible = false;
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
            // The panel's short form; the full phrase is the accessible name,
            // read once (issue #828: a description repeating it read it twice).
            var identification = findChild(loader.item, "searchIdentification");
            compare(identification.text, "NFM · digital unresolved");
            compare(identification.Accessible.name, "NFM monitoring · digital identification unresolved");
            compare(identification.Accessible.description, "");
            model.status = Object.assign({}, model.status, {phase: 3});
            compare(findChild(loader.item, "searchIdentification").text, "Trying digital identification…");
            model.status = Object.assign({}, model.status, {phase: 5, identifying: true});
            compare(findChild(loader.item, "searchIdentification").text, "Trying digital identification…");
        }
        function test_live_strength_is_shown_beside_detection_power() {
            var strength = findChild(loader.item, "searchStrength");
            compare(strength.text, "-32.1 dBFS");
            // The levels are the name; the description adds only the range
            // (issue #828: Android reads the name, then the description).
            tryCompare(strength.Accessible, "name", "-32.1 dBFS");
            compare(strength.Accessible.description, "Local UHF");
            model.status = Object.assign({}, model.status, {livePowerValid: true, livePower: -47.25});
            compare(strength.text, "Now -47.3 · detected -32.1 dBFS");
            tryCompare(strength.Accessible, "name", "Now -47.3 · detected -32.1 dBFS");
            compare(strength.Accessible.description, "Local UHF");
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
            // A failed search has no session list and refuses to lift from it
            // (issue #828): nothing to offer, while permanent avoids stay listed.
            model.status = Object.assign({}, model.status, {phase: 7, hit: {}});
            wait(0);
            verify(!findChild(loader.item, "searchSessionAvoid"));
            verify(findChild(loader.item, "searchPermanentAvoid"));
            // Session avoids end with the session: nothing to list once it stopped.
            model.status = Object.assign({}, model.status, {phase: 4});
            wait(0);
            verify(findChild(loader.item, "searchSessionAvoid"));
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
        // Issue #902: the Hold button has the keyboard when the detection ends and the button hides. A key queued in
        // the same turn as the hide does nothing (no Back, no Hold); then the keyboard goes to the app's Back owner
        // (Main's safe area), never the window root, so Escape still reaches the app's Back.
        function test_escape_after_the_hold_button_hides() {
            var screen = loader.item;
            Ui.Navigation.rootSurfaces = [screen];
            var hold = findChild(screen, "searchHold");
            verify(hold.visible, "a detection is received");
            hold.forceActiveFocus();
            verify(hold.activeFocus);
            back.clear();
            var actions = model.actions.length;
            model.status = {phase: 1, session: "17", owned: true, pass: 2, range: 0, rangeName: "Local UHF", hit: {}};
            verify(!hold.visible, "the detection ended");
            verify(hold.activeFocus, "the focus has not moved on yet");
            for (var key of [Qt.Key_Escape, Qt.Key_Space]) {
                testContext.sendKey(hold, key, 0, true, false);
                testContext.sendKey(hold, key, 0, false, false);
            }
            compare(back.count, 0, "the queued Escape did nothing");
            compare(model.actions.length, actions, "the queued Space held nothing");
            tryVerify(function () { return !hold.activeFocus && !hold.focus; }, 1000, "the hidden Hold gives the keyboard up");
            verify(appBack.activeFocus, "the app's Back owner has the keyboard");
            keyClick(Qt.Key_Escape);
            compare(appBack.escapes, 1, "Escape reaches the app's Back");
        }
        // Issue #902: the same with the window away as the detection ends: the Hold button holds only the window's
        // stored focus then, which it gives to the app's Back owner without making anything active, so the window
        // that comes back hands the keyboard to it, not to the window root, and Escape still reaches the app's Back.
        function test_escape_after_the_hold_button_hides_while_away() {
            var screen = loader.item;
            Ui.Navigation.rootSurfaces = [screen];
            var hold = findChild(screen, "searchHold");
            verify(hold.visible, "a detection is received");
            hold.forceActiveFocus();
            testContext.sendWindowFocus(hold, false);
            verify(hold.focus && !hold.activeFocus, "away: the Hold button holds the stored focus");
            back.clear();
            model.status = {phase: 1, session: "17", owned: true, pass: 2, range: 0, rangeName: "Local UHF", hit: {}};
            verify(!hold.visible, "the detection ended");
            tryVerify(function () { return !hold.focus; }, 1000, "the hidden Hold lets the stored focus go");
            verify(!appBack.activeFocus, "away: nothing is made active");
            testContext.sendWindowFocus(hold, true);
            verify(appBack.activeFocus, "back: the app's Back owner has the keyboard");
            keyClick(Qt.Key_Escape);
            compare(appBack.escapes, 1, "Escape reaches the app's Back");
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
        // Issue #828: one line each, in a fixed order: the screen's refusal, the
        // host's failure or else the search's message, the model's own error,
        // then the store's lines. The store's text is its own line, never the
        // model's error as well, and it stays under the Now panel while a
        // search runs.
        function test_message_lines_in_order() {
            var message = findChild(loader.item, "searchMessage");
            var store = "Signal search history couldn't be read completely, so some entries may be missing. The file won't be overwritten.\n"
                + "Signal search permanent avoids couldn't be saved. Check available storage. DSD-neo tries again with the next change.";
            model.storeMessage = store;
            compare(message.text, store, "the store's lines alone");
            verify(message.visible);
            compare(message.parent.objectName, "searchSessionMessageSlot", "under the Now panel while the search runs");
            model.error = "The decoder did not accept this action.";
            model.status = Object.assign({}, model.status, {message: "The avoid list is full"});
            compare(message.text, "The avoid list is full\nThe decoder did not accept this action.\n" + store);
            root.hostFailure = "Device detached: RTL-SDR";
            compare(message.text, "Device detached: RTL-SDR\nThe decoder did not accept this action.\n" + store,
                    "the host's failure stands in for the search's message");
            loader.item.sessionActive = false;
            model.armError = "Enable at least one range.";
            findChild(loader.item, "searchStartPause").clicked();
            compare(message.text, "Enable at least one range.\nDevice detached: RTL-SDR\nThe decoder did not accept this action.\n" + store);
            compare(message.parent.objectName, "searchIdleMessageSlot");
            compare(message.text.split(store).length, 2, "the store's text once");
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
            verify(toggle.Accessible.focusable, "a stop while in the tree");
            verify(toggle.height >= Ui.Theme.minimumTouchSize);
            toggle.activate();
            verify(ranges.visible);
            compare(toggle.accessibleName, "Hide search ranges");
            verify(toggle.Accessible.checked);
            model.status = Object.assign({}, model.status, {channel: 81});
            verify(ranges.visible, "a status update keeps it open");
            toggle.activate();
            verify(!ranges.visible);
            // Issue #902: while a search runs the row is a button, a screen reader's stop, and takes its keys.
            toggle.forceActiveFocus();
            keyClick(Qt.Key_Space);
            verify(ranges.visible, "Space on the focused row opens the ranges");
            keyClick(Qt.Key_Space);
            verify(!ranges.visible, "and closes them");
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
            verify(toggle.Accessible.focusable, "a heading TalkBack can move to");
            verify(!toggle.activeFocusOnTab, "and no Tab stop");
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
            var full = detail.Accessible.name;
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
        // values at a time, and the whole text is the accessible name.
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
            tryCompare(detail.Accessible, "name", "WACN BEE00 · SYS 1A3 · RFSS 1 · Site 2 · NAC 293 · Target 2401 · Source 1234567");
            compare(detail.Accessible.description, "", "read once (issue #828)");
            // Shown, it breaks only after a separator: every other space is non-breaking.
            var p25 = verifyWholeValues(detail, "P25");
            if (data.scale === 1.0)
                compare(p25, detail.Accessible.name, "the P25 detail fits whole at default text");
            model.status = Object.assign({}, model.status, {hit: Object.assign({}, hit, {confirmed: true, protocol: "DMR", unresolved: false,
                site: "TIII Small · Net 12 · Site 3 · SYS 1183", access: "CC 1", slot: 2, target: 100, source: 2001, crypt: "Encrypted"})});
            tryCompare(detail.Accessible, "name", "TIII Small · Net 12 · Site 3 · SYS 1183 · CC 1 · Slot 2 · Target 100 · Source 2001 · Encrypted");
            var dmr = verifyWholeValues(detail, "DMR");
            if (data.scale === 1.0)
                compare(dmr, detail.Accessible.name, "the DMR detail fits whole at default text");
            // The longest: the values that fit and "· …", whole for a screen reader.
            var longest = {confirmed: true, protocol: "DMR", unresolved: false, site: longestSite, access: "CC 15", slot: 1,
                targetText: "Regional interoperability talkgroup band with a very long name", sourceText: "Unit 7 portable", crypt: "ALG 84 · KID 0001"};
            model.status = Object.assign({}, model.status, {hit: Object.assign({}, hit, longest)});
            waitForRendering(panel);
            tryVerify(function () { return detail.Accessible.name.indexOf("KID 0001") >= 0; });
            var shown = verifyWholeValues(detail, "the longest detail");
            verify(shown.endsWith(" · …"), "the longest detail leaves values off");
            verify(detail.height <= detail.parent.height + 0.5, "inside its two lines");
            verify(detail.Accessible.name.endsWith("Slot 1 · Regional interoperability talkgroup band with a very long name · Unit 7 portable · ALG 84 · KID 0001"));
            verify(detail.Accessible.name.startsWith(longestSite + " · CC 15"));
            compare(panel.height, height, "the panel keeps its height");
            // A clear call on a one-slot protocol names no slot and no encryption.
            model.status = Object.assign({}, model.status, {hit: Object.assign({}, hit, {confirmed: true, protocol: "NXDN48", unresolved: false, site: "Site 12 · Sys 3", slot: 0, crypt: "", target: 5, source: 6})});
            tryCompare(detail.Accessible, "name", "Site 12 · Sys 3 · Target 5 · Source 6");
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
        // Issue #802: under its verdict a row reads the site, access code, tone
        // and encryption the detection last had, in the Now panel's order
        // without the call; a value it never had is left out, with no
        // placeholder. The saved names and the recordings stay on their own
        // lines after it.
        function test_history_row_shows_access_and_tone() {
            recordings.setDetectionRecordings("17:40", [rec("c", 1700000300, 4000), rec("b", 1700000200, 3000), rec("a", 1700000100, 2000)]);
            model.history = [historyHit({confirmed: true, protocol: "DMR", site: "TIII Small · Net 12 · Site 3 · SYS 1183", access: "CC 1", slot: 2, target: 100,
                    source: 2001, crypt: "ALG 21 · KID 0001", knownLabel: "Warehouse / Dock security"}),
                historyHit({id: "61", recordingKey: "17:61", tone: "CTCSS 127.3 Hz"}),
                historyHit({id: "62", recordingKey: "17:62", tone: "DCS D023N / D047I"}),
                historyHit({id: "63", recordingKey: "17:63", confirmed: true, protocol: "P25", site: "WACN BEE00 · SYS 1A3 · RFSS 1 · Site 2", access: "NAC 293", crypt: "Encrypted"}),
                historyHit({id: "64", recordingKey: "17:64", confirmed: true, protocol: "DMR", access: "CC 1"}),
                historyHit({id: "65", recordingKey: "17:65", confirmed: true, protocol: "NXDN48", site: "Site 12 · Sys 3", access: "RAN 5"}),
                historyHit({id: "66", recordingKey: "17:66"}),
                historyHit({id: "67", recordingKey: "17:67", ended: false, tone: "CTCSS 127.3 Hz"})];
            loader.item.historyLimit = 8;
            var rows = findChildren(loader.item, "searchHistoryDetail");
            compare(rows.length, 8);
            var expected = ["DMR · confirmed · ended\nTIII Small · Net 12 · Site 3 · SYS 1183 · CC 1 · ALG 21 · KID 0001\nWarehouse / Dock security\n3 recordings",
                "RF only · ended\nCTCSS 127.3 Hz",
                "RF only · ended\nDCS D023N / D047I",
                "P25 · confirmed · ended\nWACN BEE00 · SYS 1A3 · RFSS 1 · Site 2 · NAC 293 · Encrypted",
                "DMR · confirmed · ended\nCC 1",
                "NXDN48 · confirmed · ended\nSite 12 · Sys 3 · RAN 5",
                "RF only · ended",
                "RF only\nCTCSS 127.3 Hz"];
            for (var i = 0; i < expected.length; ++i) {
                compare(rows[i].fullText, expected[i], "row " + i);
                compare(rows[i].Accessible.name, expected[i].replace(/\n/g, " · "), "row " + i + " reads its whole text");
                compare(rows[i].text.replace(/\u00a0/g, " "), expected[i], "row " + i + " draws its whole text");
                verify(!/[^·\n] /.test(rows[i].text), "a value never splits across lines in row " + i);
            }
            model.history = [];
        }
        // The narrowest row (360 dp, the largest text), in both appearances:
        // the longest site with its NAC and encryption, and a DCS pair, wrap
        // only between values and stay inside their row.
        function test_history_access_and_tone_large_text_360_data() {
            return [{tag: "dark", appearance: 2}, {tag: "light", appearance: 1}];
        }
        function test_history_access_and_tone_large_text_360(data) {
            testContext.setPrefs("appearance", data.appearance);
            root.width = 360;
            Ui.Theme.fontScale = 1.6;
            model.history = [historyHit({confirmed: true, protocol: "P25", site: longestSite, access: "NAC 293", crypt: "ALG 132 · KID 0001"}),
                historyHit({id: "61", recordingKey: "17:61", tone: "DCS D023N / D047I"}),
                historyHit({id: "62", recordingKey: "17:62", tone: "CTCSS 127.3 Hz"})];
            wait(0);
            var rows = findChildren(loader.item, "searchHistoryRow");
            var details = findChildren(loader.item, "searchHistoryDetail");
            compare(details.length, 3);
            for (var i = 0; i < details.length; ++i) {
                var detail = details[i];
                var drawn = renderedLineStarts(detail, detail.text, detail.width);
                compare(drawn.lines, detail.lineCount, "the mirror wraps as row " + i + " does");
                for (var start of drawn.starts)
                    verify(start.trim().charAt(0) !== "·", "a drawn line of row " + i + " starts with a separator: " + JSON.stringify(drawn.starts));
                verify(detail.mapToItem(rows[i], 0, 0).x + detail.width <= rows[i].width + 0.5, "row " + i + "'s detail stays inside its row");
                verify(detail.mapToItem(rows[i], 0, 0).y + detail.height <= rows[i].height + 0.5, "row " + i + "'s detail stays inside its row");
            }
            verify(details[0].lineCount > 2, "the longest site wraps at this size");
            verify(details[0].text.indexOf("NAC\u00a0293") >= 0 && details[0].text.indexOf("ALG\u00a0132") >= 0, "the code and encryption stay whole");
            verify(details[1].text.indexOf("DCS\u00a0D023N\u00a0/\u00a0D047I") >= 0, "a DCS pair stays whole");
            verify(details[2].text.indexOf("CTCSS\u00a0127.3\u00a0Hz") >= 0, "a CTCSS tone stays whole");
            model.history = [];
        }
        // A larger text after the rows exist re-decides which values stay
        // whole: the row reads its metrics' font, so a value that fit at the
        // old size but no longer does wraps at its words instead of breaking
        // before its separator.
        function test_history_row_rewraps_on_font_scale() {
            root.width = 360;
            model.history = [historyHit({confirmed: true, protocol: "DMR", access: "CC 1", knownLabel: "Northern District Fire / Engine 42"})];
            wait(0);
            var detail = findChild(loader.item, "searchHistoryDetail");
            var before = detail.text;
            Ui.Theme.fontScale = 1.6;
            wait(0);
            var metrics = Qt.createQmlObject("import QtQuick; FontMetrics {}", root);
            metrics.font = detail.font;
            var expected = loader.item.keepValues(detail.fullText, detail.width, metrics);
            metrics.destroy();
            verify(expected !== before, "the larger text changes which values stay whole");
            compare(detail.text, expected);
            model.history = [];
        }
        // Issue #802 B4: text from the air (a D-STAR callsign), a saved name
        // and a range name are drawn as written, never as markup, so what is
        // drawn and measured is what a screen reader reads.
        function plainWidth(text) {
            var mirror = Qt.createQmlObject("import QtQuick; Text { visible: false; textFormat: Text.PlainText }", root);
            mirror.font = text.font;
            mirror.leftPadding = text.leftPadding;
            mirror.rightPadding = text.rightPadding;
            mirror.text = text.text;
            var width = mirror.implicitWidth;
            mirror.destroy();
            return width;
        }
        function verifyPlain(text, tag) {
            compare(text.textFormat, Text.PlainText, tag + " is plain text");
            verify(text.text.indexOf("<b>") >= 0, tag + " holds the tag as written");
            verify(Math.abs(text.implicitWidth - plainWidth(text)) <= 0.5, tag + " measures as plain text: " + text.implicitWidth + " vs " + plainWidth(text));
        }
        function test_air_and_typed_text_is_plain() {
            var screen = loader.item;
            model.status = Object.assign({}, model.status, {hit: Object.assign({}, model.status.hit, {confirmed: true, protocol: "D-STAR", unresolved: false,
                targetText: "<b>CQCQCQ", sourceText: "N0CALL", knownNames: ["<b>Warehouse</b>", "Dock"]})});
            var detail = findChild(screen, "searchDetail");
            tryCompare(detail.Accessible, "name", "<b>CQCQCQ · N0CALL");
            verifyPlain(detail, "the Now detail");
            var saved = findChild(screen, "searchSaved");
            verifyPlain(saved, "the saved names");
            compare(saved.Accessible.name, "Saved frequency: <b>Warehouse</b>, Dock");
            // A range name between detections: the phase and the status line.
            model.status = Object.assign({}, model.status, {phase: 1, rangeName: "<b>North</b>", hit: {}});
            var identification = findChild(screen, "searchIdentification");
            compare(identification.text, "Searching <b>North</b>…");
            verifyPlain(identification, "the phase line");
            compare(identification.Accessible.name, "Searching <b>North</b>…");
            verifyPlain(findChild(screen, "searchStatusLine"), "the status line");
            // A failure can carry a device or path name.
            root.hostFailure = "Cannot open <b>/dev/rtl0</b>";
            var message = findChild(screen, "searchMessage");
            verifyPlain(message, "the message");
            tryCompare(message.Accessible, "name", "Cannot open <b>/dev/rtl0</b>");
            root.hostFailure = "";
            // A saved name on a history row. Its first line is the fixed
            // verdict, which AutoText alone would read, so the row drew the
            // name as written before too: plain text keeps it so should a
            // value ever lead the row.
            model.history = [historyHit({knownLabel: "<b>Dock</b>"})];
            var row = findChild(screen, "searchHistoryDetail");
            compare(row.textFormat, Text.PlainText);
            verify(row.text.indexOf("<b>Dock</b>") >= 0, "the row holds the tag as written");
            var mirror = Qt.createQmlObject("import QtQuick; Text { visible: false; textFormat: Text.PlainText }", root);
            mirror.font = row.font;
            mirror.width = row.width;
            mirror.wrapMode = row.wrapMode;
            mirror.text = row.text;
            compare(row.lineCount, mirror.lineCount, "the row wraps as plain text does");
            verify(Math.abs(row.contentWidth - mirror.contentWidth) <= 0.5, "the row draws as plain text: " + row.contentWidth + " vs " + mirror.contentWidth);
            mirror.destroy();
            compare(row.Accessible.name, "RF only · ended · <b>Dock</b>");
            model.history = [];
            // The range list (cleanup() restores its name).
            model.ranges = [Object.assign({}, model.ranges[0], {name: "<b>North</b>"})];
            screen.rangesExpanded = true;
            var range = findChild(screen, "searchRangeEnabled");
            verify(range !== null && range.visible);
            compare(range.contentItem.text, "<b>North</b> · 450.000000–460.000000 MHz");
            verifyPlain(range.contentItem, "the range list");
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
            tryCompare(strength.Accessible, "name", "-32.1 dBFS");
            compare(strength.Accessible.description, "Local UHF");
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
            tryCompare(strength.Accessible, "name", data.scale > 1 ? "Now -100.0 dBFS · detected -100.0 dBFS" : "Now -100.0 · detected -100.0 dBFS");
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
        // Each action as the menu offers it now: `enabled` may be a function
        // the menu evaluates (issue #902 B8), so its own verdict is read.
        function menuTexts() {
            var menu = findChild(loader.item, "searchDetectionMenu");
            return menu.actions.map(function (a, i) { return a.text + (menu.actionEnabled(i) ? "" : " (disabled)"); });
        }
        function menuItem(name) {
            var menu = findChild(loader.item, "searchDetectionMenu");
            verify(menu.visible);
            var item = findChild(menu, name);
            verify(item !== null && item.visible, name);
            return item;
        }
        function historyHit(extra) {
            return Object.assign({id: "40", session: "17", freqMHz: "462.575000", frequency: 462575000, power: -40, confirmed: false, ended: true, matches: [], recordingKey: "17:40"}, extra || {});
        }
        // Each detection row offers what can be done with it, by state: saved
        // systems it matched (an avoided one disabled, saying why), avoiding it
        // this session only while one runs and the frequency is no permanent
        // avoid (issue #828: it is excluded already), never in a failed search,
        // and the permanent avoid both ways.
        // With Record calls off (the app's default) and no recordings kept, a
        // disabled Recordings comes last (issue #809).
        function test_history_menu_per_state_data() {
            var matched = [{systemUid: "uid-a"}, {systemUid: "uid-a"}, {systemUid: "uid-b"}, {systemUid: "gone"}];
            return [
                {tag: "unmatched, searching", hit: {}, session: true, avoided: false,
                    want: ["Explore here", "Save as system", "Avoid this session", "Avoid permanently", "Recordings (disabled)"]},
                {tag: "matched, searching", hit: {matches: matched}, session: true, avoided: false,
                    want: ["Explore here", "Start “Warehouse”", "Start “Dock” (disabled)", "Save as system", "Avoid this session", "Avoid permanently", "Recordings (disabled)"]},
                {tag: "no session", hit: {}, session: false, avoided: false,
                    want: ["Explore here", "Save as system", "Avoid permanently", "Recordings (disabled)"]},
                {tag: "permanently avoided", hit: {}, session: true, avoided: true,
                    want: ["Explore here", "Save as system", "Remove permanent avoid", "Recordings (disabled)"]},
                {tag: "search failed", hit: {}, session: true, avoided: false, phase: 7,
                    want: ["Explore here", "Save as system", "Avoid permanently", "Recordings (disabled)"]},
                {tag: "paused", hit: {}, session: true, avoided: false, phase: 6,
                    want: ["Explore here", "Save as system", "Avoid this session", "Avoid permanently", "Recordings (disabled)"]},
                {tag: "permanently avoided, no session", hit: {matches: matched}, session: false, avoided: true,
                    want: ["Explore here", "Start “Warehouse”", "Start “Dock” (disabled)", "Save as system", "Remove permanent avoid", "Recordings (disabled)"]},
                // Issue #809: with Record calls on, a detection that kept no recordings offers none.
                {tag: "recording, none kept", hit: {}, session: true, avoided: false, record: true,
                    want: ["Explore here", "Save as system", "Avoid this session", "Avoid permanently"]}
            ];
        }
        function test_history_menu_per_state(data) {
            var screen = loader.item;
            screen.sessionActive = data.session;
            if (!data.session)
                model.status = {phase: 0, session: "17", owned: false};
            else if (data.phase)
                model.status = Object.assign({}, model.status, {phase: data.phase, hit: {}});
            model.avoids = data.avoided ? [462575000] : [];
            testContext.setPrefs("recordCalls", data.record === true);
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
            compare(model.actions.join(","), "permanent,462575000");
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
        // Issue #748 B18 and B6: a detection made on a receiver the app cannot
        // open, or confirmed as a protocol this build does not decode (its
        // model row carries the reason), is neither explored nor saved: both
        // actions are disabled and say why, from its history row and from the
        // Now panel, whose Save as system is disabled too.
        function test_unopenable_receiver_disables_explore_and_save_data() {
            return [{tag: "SoapySDR receiver", reason: "Found on a SoapySDR receiver, which this app cannot open", refused: {inputSpec: "soapy:driver=rtlsdr"}},
                {tag: "unknown protocol", reason: "This version cannot decode Tetrapol", refused: {inputSpec: "rtl:0:462.575M", confirmed: true, protocol: "Tetrapol"}}];
        }
        function test_unopenable_receiver_disables_explore_and_save(data) {
            var screen = loader.item;
            var reason = data.reason;
            model.history = [historyHit(Object.assign({handoffRefusal: reason}, data.refused))];
            wait(0);
            findChild(screen, "searchHistoryActions").activate();
            compare(menuTexts(), ["Explore here (disabled)", "Save as system (disabled)", "Avoid this session", "Avoid permanently", "Recordings (disabled)"]);
            var menu = findChild(screen, "searchDetectionMenu");
            compare(findChild(menu, "actionMenuDescription0").text, reason);
            compare(findChild(menu, "actionMenuDescription1").text, reason);
            verify(!menuItem("searchActionExplore").enabled);
            verify(!menuItem("searchActionSave").enabled);
            menuItem("searchActionExplore").activate();
            menuItem("searchActionSave").activate();
            compare(explored.count, 0);
            compare(saved.count, 0);
            menu.cancel();
            // A receiver the app opens offers both.
            model.history = [historyHit({inputSpec: "rtl:0:462.575M", handoffRefusal: ""})];
            wait(0);
            findChild(screen, "searchHistoryActions").activate();
            verify(menuItem("searchActionExplore").enabled && menuItem("searchActionSave").enabled);
            menu.cancel();
            model.history = [];

            var hit = model.status.hit;
            model.status = Object.assign({}, model.status, {hit: Object.assign({}, hit, data.refused, {handoffRefusal: reason})});
            var save = findChild(screen, "searchSave");
            verify(save.visible && !save.enabled);
            findChild(screen, "searchMore").clicked();
            compare(menuTexts(), ["Explore here (disabled)"]);
            compare(findChild(menu, "actionMenuDescription0").text, reason);
            menu.cancel();
            model.status = Object.assign({}, model.status, {hit: hit});
            verify(save.enabled);
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
        // An avoid the decoder refuses (either list full, from the Now panel) is
        // the status message, which the screen shows; a refused row permanent
        // avoid is the model's error. A permanent avoid takes no room in the
        // session's list, so no "saved, but" message exists (issue #828).
        function test_avoid_refusals_are_shown_data() {
            return [{tag: "session list full", message: "The avoid list is full"},
                {tag: "permanent list full", message: "The permanent avoid list is full"},
                {tag: "no channel", message: "This frequency cannot be avoided"}];
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

        // Issue #828: the overload warning as the screen words it, for the
        // phase (searching, or paused) and for the gain controls the range
        // editor offers the radio.
        function overloadText(range, paused, airspy) {
            var fix = paused ? (airspy ? "Set a lower Airspy gain for the range, then Resume." : "Lower the range's receiver gain, then Resume.")
                : (airspy ? "Pause, then set a lower Airspy gain for the range." : "Pause, then lower the range's receiver gain.");
            return (range.length > 0 ? "Part of “" + range + "”" : "Part of a range") + " was skipped because the receiver is overloaded. " + fix;
        }
        function overload(range, extra) {
            model.status = Object.assign({}, model.status, {overloaded: true, overloadRange: range}, extra || {});
        }
        // The warning is the message's last line: after the screen's refusal,
        // the host's failure, the model's error and the store's lines.
        function test_overload_line_comes_last() {
            var message = findChild(loader.item, "searchMessage");
            var store = "Signal search history couldn't be read completely, so some entries may be missing. The file won't be overwritten.";
            overload("Marine", {phase: 6, hit: {}});
            compare(message.text, overloadText("Marine", true, false), "the warning alone");
            verify(message.visible);
            model.storeMessage = store;
            model.error = "The decoder did not accept this action.";
            root.hostFailure = "Device detached: RTL-SDR";
            model.armError = "Check the numeric range settings.";
            findChild(loader.item, "searchStartPause").clicked();
            compare(message.text, ["Check the numeric range settings.", "Device detached: RTL-SDR",
                "The decoder did not accept this action.", store, overloadText("Marine", true, false)].join("\n"));
            root.hostFailure = "";
            model.status = Object.assign({}, model.status, {message: "The avoid list is full"});
            compare(message.text, ["Check the numeric range settings.", "The avoid list is full",
                "The decoder did not accept this action.", store, overloadText("Marine", true, false)].join("\n"),
                "the search's message stands where the host's failure was");
            compare(message.text.split("overloaded").length, 2, "the warning once");
        }
        function test_overload_text_per_phase_and_radio_data() {
            return [{tag: "searching, USB", phase: 1, source: "usb", paused: false},
                {tag: "listening, USB", phase: 4, source: "usb", paused: false},
                {tag: "held, rtl_tcp", phase: 5, source: "rtltcp", paused: false},
                {tag: "tuning, Airspy", phase: 2, source: "airspy", paused: false},
                {tag: "identifying, USB", phase: 3, source: "usb", paused: false},
                {tag: "paused, USB", phase: 6, source: "usb", paused: true},
                {tag: "paused, rtl_tcp", phase: 6, source: "rtltcp", paused: true},
                {tag: "paused, Airspy", phase: 6, source: "airspy", paused: true}];
        }
        function test_overload_text_per_phase_and_radio(data) {
            loader.item.sourceType = data.source;
            overload("Air band", {phase: data.phase});
            var message = findChild(loader.item, "searchMessage");
            compare(message.text, overloadText("Air band", data.paused, data.source === "airspy"));
            verify(message.visible);
            tryCompare(message.Accessible, "name", message.text);
            verify(!message.Accessible.ignored);
            // A range name is typed: shown as written.
            overload("<b>North</b>");
            compare(message.textFormat, Text.PlainText);
            compare(message.text, overloadText("<b>North</b>", data.paused, data.source === "airspy"));
        }
        // A range with no name (the model refuses one; the engine copies what
        // it is given) reads as "a range", never as empty quotes.
        function test_overload_without_a_range_name_data() {
            return [{tag: "searching", phase: 1, source: "usb", paused: false},
                {tag: "paused, Airspy", phase: 6, source: "airspy", paused: true}];
        }
        function test_overload_without_a_range_name(data) {
            loader.item.sourceType = data.source;
            var message = findChild(loader.item, "searchMessage");
            overload("", {phase: data.phase});
            compare(message.text, "Part of a range was skipped because the receiver is overloaded. "
                + (data.paused ? "Set a lower Airspy gain for the range, then Resume." : "Pause, then lower the range's receiver gain."));
            compare(message.text, overloadText("", data.paused, data.source === "airspy"));
            verify(message.text.indexOf("“”") < 0);
            tryCompare(message.Accessible, "name", message.text);
        }
        // The status keeps the warning after the search fails or stops: the
        // screen drops it then, as it does with no session.
        function test_overload_hidden_once_the_search_ends_data() {
            return [{tag: "failed", phase: 7, active: true, message: "No uninterrupted samples for a measurement"},
                {tag: "stopped", phase: 0, active: true, message: ""},
                {tag: "no session", phase: 1, active: false, message: ""}];
        }
        function test_overload_hidden_once_the_search_ends(data) {
            var message = findChild(loader.item, "searchMessage");
            overload("Marine", {phase: 1, hit: {}});
            verify(message.visible);
            loader.item.sessionActive = data.active;
            model.status = Object.assign({}, model.status, {phase: data.phase, message: data.message});
            verify(model.status.overloaded, "the status still carries the warning");
            compare(message.text, data.message);
            compare(message.visible, data.message.length > 0);
            tryCompare(message.Accessible, "name", data.message);
            model.storeMessage = "Signal search permanent avoids couldn't be saved. Check available storage. DSD-neo tries again with the next change.";
            compare(message.text, [data.message, model.storeMessage].filter(function (s) { return s.length > 0; }).join("\n"));
        }
        // The warning appearing and clearing never moves the Now panel: it
        // sits under it with the rest of the message, and wraps there at a
        // phone's width with the largest text.
        function test_overload_never_moves_the_now_panel_data() {
            return [{tag: "default text", scale: 1.0, width: 411}, {tag: "large text at 360 px", scale: 1.6, width: 360}];
        }
        function test_overload_never_moves_the_now_panel(data) {
            var screen = loader.item;
            var scroll = findChild(screen, "searchScroll");
            var panel = findChild(screen, "searchNowPanel");
            var message = findChild(screen, "searchMessage");
            Ui.Theme.fontScale = data.scale;
            root.width = data.width;
            model.status = Object.assign({}, model.status, {phase: 1, hit: {}});
            waitForRendering(screen);
            verify(!message.visible);
            var panelY = panel.mapToItem(scroll, 0, 0).y;
            overload("Marine");
            waitForRendering(screen);
            verify(message.visible);
            compare(message.parent.objectName, "searchSessionMessageSlot");
            compare(panel.mapToItem(scroll, 0, 0).y, panelY);
            verify(message.mapToItem(scroll, 0, 0).y >= panelY + panel.height, "the warning must sit under the Now panel");
            verify(message.lineCount > 1 && message.contentWidth <= message.width, "it wraps inside the column");
            verify(message.mapToItem(screen, message.width, 0).x <= screen.width, "and inside the screen");
            model.status = Object.assign({}, model.status, {overloaded: false, overloadRange: ""});
            waitForRendering(screen);
            verify(!message.visible);
            compare(panel.mapToItem(scroll, 0, 0).y, panelY);
        }
        // Two ranges that take turns clipping rename the warning every pass.
        // Its name keeps the screen reader's pace (SpeechPacer, at most every
        // period), while the warning appearing or clearing is heard at once,
        // and the text on screen stays live.
        function test_overload_name_is_paced() {
            var message = findChild(loader.item, "searchMessage");
            var store = "Signal search history couldn't be read completely, so some entries may be missing. The file won't be overwritten.";
            model.status = Object.assign({}, model.status, {phase: 1, hit: {}});
            model.storeMessage = store;
            tryCompare(message.Accessible, "name", store);
            Ui.Navigation.speechPeriodMs = 60000;
            try {
                overload("Marine");
                tryCompare(message.Accessible, "name", store + "\n" + overloadText("Marine", false, false), 1000,
                           "the warning appearing is heard at once");
                for (var pass = 0; pass < 6; ++pass) {
                    var range = pass % 2 === 0 ? "Air band" : "Marine";
                    overload(range, {pass: 3 + pass});
                    compare(message.text, store + "\n" + overloadText(range, false, false), "the screen is live");
                    wait(30);
                    compare(message.Accessible.name, store + "\n" + overloadText("Marine", false, false),
                            "a range's new name waits for the period, pass " + (3 + pass));
                }
                model.status = Object.assign({}, model.status, {overloaded: false, overloadRange: ""});
                tryCompare(message.Accessible, "name", store, 1000, "the warning clearing is heard at once");
                overload("Air band");
                tryCompare(message.Accessible, "name", store + "\n" + overloadText("Air band", false, false), 1000);
                overload("Marine");
                Ui.Navigation.speechPeriodMs = 50;
                tryCompare(message.Accessible, "name", store + "\n" + overloadText("Marine", false, false), 1000,
                           "and the name catches up at the next period");
            } finally {
                Ui.Navigation.speechPeriodMs = 50;
            }
        }
        // Behind an open sheet the message leaves the accessibility tree and
        // holds no name, as every node a screen reader reaches does; it is
        // named again once the sheet closes. A warning renamed under the sheet
        // names nothing there.
        function test_message_nameless_behind_sheet() {
            var message = findChild(loader.item, "searchMessage");
            model.history = [historyHit({confirmed: true, protocol: "DMR"})];
            root.hostFailure = "Device detached: RTL-SDR";
            tryCompare(message.Accessible, "name", root.hostFailure);
            verify(!message.Accessible.ignored);
            findChild(loader.item, "searchHistoryActions").activate();
            verify(findChild(loader.item, "searchDetectionMenu").visible);
            verify(message.visible, "still drawn under the sheet");
            verify(message.Accessible.ignored, "behind the menu, out of the tree");
            compare(message.Accessible.name, "");
            overload("Marine");
            overload("Air band");
            wait(100);
            compare(message.Accessible.name, "", "a warning under the sheet names nothing");
            findChild(loader.item, "searchDetectionMenu").cancel();
            verify(!message.Accessible.ignored);
            tryCompare(message.Accessible, "name", root.hostFailure + "\n" + overloadText("Air band", false, false));
            model.history = [];
        }

        // Issue #809: a detection's recordings, from its Recent detections row.
        // A recording as the store lists it (RecordingsFake fills the keys left out).
        function rec(id, start, ms, extra) {
            return Object.assign({id: id, path: "/rec/Explore/" + id + ".wav", fileName: id + ".wav", folderUid: "explore", start: start, durationMs: ms, rate: 8000, bytes: 96000,
                identitySize: 96044, mtimeSec: start + 60, mtimeNanos: 0}, extra || {});
        }
        function recordingsSheet() {
            return findChild(loader.item, "detectionRecordingsSheet");
        }
        // The sheet's rows holding a recording, top to bottom.
        function recordingRows() {
            return findChildren(recordingsSheet(), "detectionRecordingRow").filter(function (row) {
                return row.live && row.opacity > 0;
            }).sort(function (a, b) {
                return a.index - b.index;
            });
        }
        function recordingRow(id) {
            var rows = recordingRows().filter(function (row) { return row.key === id; });
            return rows.length === 1 ? rows[0] : null;
        }
        function lastHostCall() {
            var calls = testContext.hostCalls();
            return calls.length > 0 ? calls[calls.length - 1] : ({});
        }
        // Seed detection 17:40 with @p list (newest first) and open its sheet from the row's actions.
        function openRecordings(list, extra) {
            recordings.setDetectionRecordings("17:40", list);
            model.history = [historyHit(extra)];
            wait(0);
            findChild(loader.item, "searchHistoryActions").activate();
            menuItem("searchActionRecordings").activate();
            var sheet = recordingsSheet();
            verify(sheet.visible, "the recordings sheet opens");
            waitForRendering(sheet);
            return sheet;
        }
        function sheetTap(item) {
            verify(item !== null && item.visible && item.enabled);
            mouseClick(item, item.width / 2, item.height / 2);
        }
        // Scroll the sheet until @p item is on screen, as a finger would.
        function revealInSheet(sheet, item) {
            var scroll = findChild(sheet, "modalSheetScroll");
            sheet.reveal(item);
            waitForRendering(sheet);
            var y = item.mapToItem(scroll, 0, 0).y;
            return y >= 0 && y + item.height <= scroll.height + 0.5;
        }

        // R58: a History playlist's Resume refused before the sheet opens on that file is read as the sheet opens,
        // once, and not again after it closes: History's bar, shown once the sheet has closed, stays silent.
        function test_sheet_reads_a_standing_refusal_on_open() {
            var a = rec("a", 1700000100, 2000);
            testContext.setPlaybackSupported(true);
            var status = playlistStatusComponent.createObject(loader.item);
            var bar = nowPlayingBarComponent.createObject(loader.item, {status: status, visible: false, width: 360});
            testContext.publishPlaybackReading({playbackGeneration: 7, playbackOrigin: "history", playbackMode: 2, playbackState: 3, playbackToken: 4, playbackPath: a.path,
                                                playbackSize: a.identitySize, playbackMtimeSec: a.mtimeSec, playbackMtimeNanos: 0,
                                                playbackDurationMs: 2000, playbackPausedReason: "unavailable", playbackRefusals: 1});
            var player = findChild(recordingsSheet(), "detectionRecordingPlayer");
            var said = player.announcements;
            var sheet = openRecordings([a]);
            tryCompare(findChild(sheet, "recordingNotice"), "text", "Can't play right now. Another app has the audio, such as a phone call.", 2000,
                       "the standing refusal is said as the sheet opens");
            compare(player.announcements, said + 1, "and read out once");
            compare(Ui.Navigation.refusalSaid, Ui.Navigation.refusalKey(7, 1), "heard: the bar will not read it again");
            verify(status.live && !status.eventHeard && status.eventRefusal, "the bar's status holds the refusal unread");
            sheet.visible = false;
            bar.visible = true;
            tryVerify(function () { return status.eventHeard; }, 2000, "the bar takes the refusal as heard");
            wait(50);
            compare(player.announcements, said + 1, "closing reads nothing more");
            compare(bar.announcements, 0, "and the bar stays silent");
            bar.destroy();
            status.destroy();
            testContext.resetHostPlayback();
            testContext.setPlaybackSupported(false);
        }

        // A row that kept recordings says how many under its details, and reads
        // it in its name; one that kept none says nothing. It follows the store.
        function test_row_recordings_count() {
            recordings.setDetectionRecordings("17:40", [rec("c", 1700000300, 4000), rec("b", 1700000200, 3000), rec("a", 1700000100, 2000)]);
            model.history = [historyHit(), historyHit({id: "41", recordingKey: "17:41"}), historyHit({id: "42", recordingKey: ""})];
            var rows = findChildren(loader.item, "searchHistoryDetail");
            compare(rows.length, 3);
            compare(rows[0].fullText, "RF only · ended\n3 recordings");
            compare(rows[0].Accessible.name, "RF only · ended · 3 recordings");
            compare(rows[1].fullText, "RF only · ended");
            compare(rows[1].Accessible.name, "RF only · ended");
            compare(rows[2].fullText, "RF only · ended", "no detection, no recordings");
            recordings.setDetectionRecordings("17:40", [rec("c", 1700000300, 4000)]);
            compare(rows[0].fullText, "RF only · ended\n1 recording", "read again when the store's recordings change");
            recordings.setDetectionRecordings("17:41", [rec("d", 1700000400, 4000), rec("e", 1700000300, 4000)]);
            compare(rows[1].Accessible.name, "RF only · ended · 2 recordings");
            recordings.setDetectionRecordings("17:40", []);
            compare(rows[0].fullText, "RF only · ended");
            model.history = [];
        }
        // Issue #809 B2: a row's details leave the accessibility tree behind an
        // open sheet, as every node a screen reader reaches does, and hold no name.
        function test_detail_text_gated_behind_sheet() {
            model.history = [historyHit({confirmed: true, protocol: "DMR"})];
            var detail = findChild(loader.item, "searchHistoryDetail");
            verify(!detail.Accessible.ignored);
            compare(detail.Accessible.name, "DMR · confirmed · ended");
            findChild(loader.item, "searchHistoryActions").activate();
            verify(findChild(loader.item, "searchDetectionMenu").visible);
            verify(detail.Accessible.ignored, "behind the menu, out of the tree");
            compare(detail.Accessible.name, "");
            findChild(loader.item, "searchDetectionMenu").cancel();
            verify(!detail.Accessible.ignored);
            compare(detail.Accessible.name, "DMR · confirmed · ended");
            model.history = [];
        }
        // A row that kept recordings offers them first among its actions; the
        // action opens them and plays nothing by itself. With none while Record
        // calls is off, it is offered last, disabled, saying why; with Record
        // calls on, a detection that kept none offers nothing. The Now panel's
        // More only leaves for the detection.
        function test_play_recordings_action_per_state_data() {
            var rest = ["Explore here", "Save as system", "Avoid this session", "Avoid permanently"];
            var hint = "Record calls is off. Turn it on in Settings to keep the audio of later detections.";
            return [{tag: "kept, recording off", count: 2, record: false, want: ["Recordings (2)"].concat(rest), at: 0, enabled: true, description: ""},
                {tag: "kept, recording on", count: 2, record: true, want: ["Recordings (2)"].concat(rest), at: 0, enabled: true, description: ""},
                {tag: "none, recording off", count: 0, record: false, want: rest.concat(["Recordings (disabled)"]), at: 4, enabled: false, description: hint},
                {tag: "none, recording on", count: 0, record: true, want: rest, at: -1}];
        }
        function test_play_recordings_action_per_state(data) {
            testContext.setPrefs("recordCalls", data.record);
            var list = [];
            for (var i = 0; i < data.count; ++i)
                list.push(rec("r" + i, 1700000000 - i, 2000));
            recordings.setDetectionRecordings("17:40", list);
            model.history = [historyHit()];
            wait(0);
            findChild(loader.item, "searchHistoryActions").activate();
            var menu = findChild(loader.item, "searchDetectionMenu");
            compare(menuTexts(), data.want);
            compare(menu.actions.findIndex(function (a) { return a.key === "recordings"; }), data.at);
            if (data.at >= 0) {
                var action = menuItem("searchActionRecordings");
                compare(action.enabled, data.enabled);
                compare(findChild(menu, "actionMenuDescription" + data.at).text, data.description);
                compare(action.accessibleName, data.description.length ? "Recordings. " + data.description : "Recordings (2)");
                if (data.enabled) {
                    action.activate();
                    verify(recordingsSheet().visible, "it opens the recordings");
                    compare(testContext.hostCalls().length, 0, "and plays nothing by itself");
                    recordingsSheet().visible = false;
                } else {
                    action.activate();
                    verify(menu.visible && !recordingsSheet().visible, "a disabled action does nothing");
                    menu.cancel();
                }
            } else {
                menu.cancel();
            }
            // The Now panel's detection: only leaving for it.
            model.status = Object.assign({}, model.status, {hit: Object.assign({}, model.status.hit, {recordingKey: "17:40"})});
            findChild(loader.item, "searchMore").clicked();
            compare(menuTexts(), ["Explore here"]);
            menu.cancel();
            model.history = [];
        }
        // Newest first, the newest selected; a tap on a row selects it and plays
        // it, and the player plays what is selected.
        function test_sheet_lists_newest_first_and_plays_selected() {
            var sheet = openRecordings([rec("c", 1700000300, 42000, {target: "1234", source: "5678", system: "DMR · CC 1"}),
                rec("b", 1700000200, 3000, {target: "Fire Dispatch", source: "", system: "NFM"}), rec("a", 1700000100, 500)]);
            compare(findChild(sheet, "detectionRecordingsTitle").text, "462.5750 MHz");
            compare(findChild(sheet, "detectionRecordingsCount").text, "3 recordings");
            compare(sheet.accessibleName, "Recordings, 462.5750 MHz");
            var rows = recordingRows();
            compare(rows.map(function (row) { return row.key; }), ["c", "b", "a"]);
            var time = function (start) { return Qt.formatDateTime(new Date(start * 1000), "hh:mm:ss"); };
            compare(rows[0].line, time(1700000300) + " · 0:42\nTarget 1234 · Source 5678 · DMR · CC 1");
            compare(rows[1].line, time(1700000200) + " · 0:03\nFire Dispatch · NFM");
            compare(rows[2].line, time(1700000100) + " · 0:01\nNo target or source recorded");
            verify(rows[0].selected && !rows[1].selected && !rows[2].selected, "the newest is selected");
            // Shown without colour too: a heavier border and the play mark.
            var marked = function (row) { return findChild(row, "detectionRecordingSelectedMark").visible && row.border.width === 2; };
            verify(marked(rows[0]) && !findChild(rows[1], "detectionRecordingSelectedMark").visible && rows[1].border.width === 1);
            compare(rows[0].Accessible.name, time(1700000300) + " · 0:42, Target 1234 · Source 5678 · DMR · CC 1, selected");
            compare(rows[1].Accessible.name, time(1700000200) + " · 0:03, Fire Dispatch · NFM");
            for (var row of rows)
                verify(row.height >= Ui.Theme.minimumTouchSize);
            var player = findChild(sheet, "detectionRecordingPlayer");
            verify(player.visible);
            compare(player.part.id, "c");
            compare(findChild(player, "recordingHeader").text, "Call recording");
            verify(!findChild(player, "recordingPlayAll").visible, "one recording at a time: no parts");
            sheetTap(findChild(player, "recordingPlayButton"));
            compare(lastHostCall().name, "playRecordingOwned");
            compare(lastHostCall().path, "/rec/Explore/c.wav");
            testContext.publishPlayback("/rec/Explore/c.wav", 2, 1000, 42000, "");
            // Another row: selected, and played in its place.
            sheetTap(rows[1]);
            verify(rows[1].selected && !rows[0].selected);
            verify(marked(rows[1]) && !marked(rows[0]), "the mark follows the selection");
            compare(rows[1].Accessible.name, time(1700000200) + " · 0:03, Fire Dispatch · NFM, selected");
            compare(player.part.id, "b");
            compare(lastHostCall().name, "playRecordingOwned");
            compare(lastHostCall().path, "/rec/Explore/b.wav");
            verify(loader.item.menuOpen, "the screen under the sheet takes no taps");
            // Closing stops what it played.
            findChild(sheet, "detectionRecordingsClose").activate();
            verify(!sheet.visible);
            compare(lastHostCall().name, "stopPlaybackGeneration");
            verify(!loader.item.menuOpen);
            // Reopened, it starts on the newest again.
            findChild(loader.item, "searchHistoryActions").activate();
            menuItem("searchActionRecordings").activate();
            verify(recordingRow("c").selected);
            sheet.visible = false;
            model.history = [];
        }
        // The selected recording deleted (here or from the Recordings screen):
        // the row that takes its place is selected (the next older one), and
        // focus moves to it if it was on the list; the oldest gone hands it to
        // the one before it (newer). Deleting says only the audio goes, for a
        // recording with no History row and one whose row History trimmed (B3).
        function test_sheet_selected_recording_deleted_selects_next_older() {
            var uid = callHistory.pushWithRecordings([]);
            verify(callHistory.setFragmentRow("F:7:2", uid));
            var sheet = openRecordings([rec("c", 1700000300, 4000), rec("b", 1700000200, 3000, {fragment: "F:7:2"}), rec("a", 1700000100, 2000)]);
            var player = findChild(sheet, "detectionRecordingPlayer");
            recordingRow("b").activate();
            compare(lastHostCall().path, "/rec/Explore/b.wav");
            testContext.publishPlayback("/rec/Explore/b.wav", 2, 1000, 3000, "");
            recordingRow("b").forceActiveFocus();
            // History trims b's row: the recording stays, with no row now.
            verify(callHistory.trimRow(uid));
            sheetTap(findChild(player, "recordingDelete"));
            var confirm = findChild(sheet, "recordingDeleteConfirm");
            verify(confirm.visible);
            compare(findChild(confirm, "confirmMessage").text, "Only the audio is deleted. History and detections are unchanged.");
            findChild(confirm, "confirmActionButton").activate();
            compare(lastHostCall().name, "stopPlaybackGeneration", "what plays stops first");
            var calls = recordings.calls();
            compare(calls[calls.length - 1].name, "deleteRecordingChecked");
            compare(calls[calls.length - 1].id, "b");
            compare(calls[calls.length - 1].path, "/rec/Explore/b.wav", "R68: the path pressed goes with the delete");
            // The store drops it.
            recordings.setDetectionRecordings("17:40", [rec("c", 1700000300, 4000), rec("a", 1700000100, 2000)]);
            compare(sheet.selectedId, "a", "the next older one, in its place");
            verify(recordingRow("a").selected);
            compare(player.part.id, "a");
            tryVerify(function () { return recordingRow("a").activeFocus; }, 1000, "focus moves to it");
            sheetTap(findChild(player, "recordingPlayButton"));
            compare(lastHostCall().name, "playRecordingOwned");
            compare(lastHostCall().path, "/rec/Explore/a.wav");
            // Rowless from the start (never linked): the same words.
            sheetTap(findChild(player, "recordingDelete"));
            compare(findChild(confirm, "confirmMessage").text, "Only the audio is deleted. History and detections are unchanged.");
            findChild(confirm, "confirmCancelButton").activate();
            // The oldest gone: the one before it.
            recordings.setDetectionRecordings("17:40", [rec("d", 1700000400, 4000), rec("c", 1700000300, 4000)]);
            compare(sheet.selectedId, "c");
            compare(recordingRows().map(function (row) { return row.key; }), ["d", "c"]);
            // A refresh that keeps it keeps the selection on it.
            recordings.setDetectionRecordings("17:40", [rec("e", 1700000500, 4000), rec("d", 1700000400, 4000), rec("c", 1700000300, 4000)]);
            compare(sheet.selectedId, "c");
            compare(findChild(sheet, "detectionRecordingsCount").text, "3 recordings");
            sheet.visible = false;
            model.history = [];
        }
        // Issue #902: a row whose recording left the list (StableRows lets go
        // of it, still drawn until the end of the turn) takes no key from that
        // moment: Escape queued on it in the same turn dismisses nothing, and
        // once the row is gone the sheet takes Escape.
        function test_departed_recording_row_takes_no_key() {
            var sheet = openRecordings([rec("b", 1700000200, 3000), rec("a", 1700000100, 2000)]);
            var row = recordingRow("a");
            row.forceActiveFocus();
            verify(row.activeFocus);
            recordings.setDetectionRecordings("17:40", [rec("b", 1700000200, 3000)]);
            verify(!row.live && row.visible, "the row is let go of, not yet removed");
            testContext.sendKey(row, Qt.Key_Escape, 0, true, false);
            testContext.sendKey(row, Qt.Key_Escape, 0, false, false);
            verify(sheet.visible, "a queued Escape on the departed row dismisses nothing");
            tryVerify(function () { return findChildren(sheet, "detectionRecordingRow").length === 1; }, 1000, "the row goes");
            tryVerify(function () { return Ui.Navigation.contains(sheet, sheet.Window.activeFocusItem); }, 1000,
                      "the sheet keeps the keyboard");
            keyClick(Qt.Key_Escape);
            tryVerify(function () { return !sheet.visible; }, 1000, "then Escape dismisses the sheet");
            model.history = [];
        }
        // The last recording gone, the sheet closes and its playback stops.
        function test_sheet_closes_when_empty() {
            var sheet = openRecordings([rec("a", 1700000100, 2000)]);
            compare(findChild(sheet, "detectionRecordingsCount").text, "1 recording");
            sheetTap(findChild(sheet, "recordingPlayButton"));
            testContext.publishPlayback("/rec/Explore/a.wav", 2, 500, 2000, "");
            recordings.setDetectionRecordings("17:40", []);
            compare(lastHostCall().name, "stopPlaybackGeneration", "no control is left for it");
            tryVerify(function () { return !sheet.visible; }, 1000, "the sheet closes");
            verify(!loader.item.menuOpen);
            verify(findChild(loader.item, "searchStartPause").enabled, "the screen takes taps again");
            model.history = [];
        }
        // Issue #909 (B3): the last recording found gone when played: the sheet
        // says so (read out once) and, as the store drops it and the sheet
        // closes, the screen's message line shows it; the next recordings sheet
        // takes it away.
        function test_sheet_last_recording_missing_reaches_the_screen() {
            var sheet = openRecordings([rec("a", 1700000100, 2000)]);
            var player = findChild(sheet, "detectionRecordingPlayer");
            var said = player.announcements;
            sheetTap(findChild(sheet, "recordingPlayButton"));
            testContext.publishPlayback("/rec/Explore/a.wav", 4, 0, 0, "missing");
            compare(findChild(sheet, "recordingNotice").text, "This recording is gone.");
            verify(findChild(sheet, "recordingNotice").visible, "on the sheet, though the player has gone");
            compare(player.announcements, said + 1);
            recordings.setDetectionRecordings("17:40", []);
            tryVerify(function () { return !sheet.visible; }, 1000, "the sheet closes");
            var message = findChild(loader.item, "searchMessage");
            verify(message.text.indexOf("This recording is gone.") >= 0, "the screen shows it: " + message.text);
            recordings.setDetectionRecordings("17:40", [rec("b", 1700000200, 2000)]);
            findChild(loader.item, "searchHistoryActions").activate();
            menuItem("searchActionRecordings").activate();
            verify(sheet.visible);
            verify(message.text.indexOf("This recording is gone.") < 0, "the next sheet takes it away");
            sheet.visible = false;
            model.history = [];
        }
        // Issue #909 (B6): with no player a row only selects; Save and Delete
        // act on it, and closing stops nothing.
        function test_sheet_without_a_player_selects_only() {
            testContext.setPlaybackSupported(false);
            var sheet = openRecordings([rec("b", 1700000200, 3000), rec("a", 1700000100, 2000)]);
            var player = findChild(sheet, "detectionRecordingPlayer");
            verify(!findChild(player, "recordingPlayButton").visible);
            compare(findChild(player, "recordingProblem").text, "Recordings can't be played on this device.");
            recordingRow("a").activate();
            verify(recordingRow("a").selected);
            compare(player.part.id, "a");
            compare(testContext.hostCalls().length, 0, "nothing plays");
            sheetTap(findChild(player, "recordingDelete"));
            findChild(findChild(sheet, "recordingDeleteConfirm"), "confirmActionButton").activate();
            var calls = recordings.calls();
            compare(calls[calls.length - 1].name, "deleteRecordingChecked");
            compare(calls[calls.length - 1].id, "a");
            findChild(sheet, "detectionRecordingsClose").activate();
            compare(testContext.hostCalls().length, 0, "and nothing stops");
            model.history = [];
        }
        // A recording on storage that went away stays listed and says so; it
        // plays once the storage is back, the selection kept.
        function test_sheet_away_recording() {
            var sheet = openRecordings([rec("b", 1700000200, 3000, {available: false}), rec("a", 1700000100, 2000)]);
            var row = recordingRow("b");
            verify(row.selected);
            compare(row.line.split("\n")[2], "Storage not available");
            verify(findChild(row, "detectionRecordingAway").visible);
            verify(row.Accessible.name.indexOf("Storage not available") > 0);
            verify(!findChild(recordingRow("a"), "detectionRecordingAway").visible);
            var player = findChild(sheet, "detectionRecordingPlayer");
            compare(findChild(player, "recordingProblem").text, "The storage this recording is on isn't available right now.");
            verify(!findChild(player, "recordingPlayButton").enabled);
            row.activate();
            compare(testContext.hostCalls().length, 0, "nothing plays from away storage");
            recordings.setDetectionRecordings("17:40", [rec("b", 1700000200, 3000), rec("a", 1700000100, 2000)]);
            compare(sheet.selectedId, "b");
            verify(!findChild(recordingRow("b"), "detectionRecordingAway").visible);
            sheetTap(findChild(player, "recordingPlayButton"));
            compare(lastHostCall().path, "/rec/Explore/b.wav");
            sheet.visible = false;
            model.history = [];
        }
        // A held visit that kept 50: the count is right, every row is there and
        // reachable by scrolling, and the last one plays.
        function test_sheet_many_recordings_scroll() {
            var list = [];
            for (var i = 0; i < 50; ++i)
                list.push(rec("r" + (49 - i), 1700000000 + 600 - i * 10, 8000));
            recordings.setDetectionRecordings("17:40", list);
            model.history = [historyHit()];
            wait(0);
            compare(findChild(loader.item, "searchHistoryDetail").fullText, "RF only · ended\n50 recordings");
            findChild(loader.item, "searchHistoryActions").activate();
            compare(menuTexts()[0], "Recordings (50)");
            menuItem("searchActionRecordings").activate();
            var sheet = recordingsSheet();
            waitForRendering(sheet);
            compare(findChild(sheet, "detectionRecordingsCount").text, "50 recordings");
            var rows = recordingRows();
            compare(rows.length, 50);
            compare(rows[0].key, "r49");
            compare(rows[49].key, "r0");
            for (var r = 0; r < rows.length; r += 7)
                verify(revealInSheet(sheet, rows[r]), "row " + r + " scrolls into view");
            verify(revealInSheet(sheet, rows[49]), "the last row scrolls into view");
            verify(revealInSheet(sheet, findChild(sheet, "detectionRecordingsClose")), "and Close under it");
            sheetTap(rows[49]);
            compare(lastHostCall().path, "/rec/Explore/r0.wav");
            sheet.visible = false;
            model.history = [];
        }
        // Call details is enabled while History holds the selected recording's
        // call, following History alone (no recording change): a late
        // adoption, a merge into another row, a trim and a clear. Disabled, it
        // stays where it is and says why, so a selection never moves the list.
        function test_call_details_resolution() {
            var sheet = openRecordings([rec("b", 1700000200, 3000, {fragment: "F:3:1"}), rec("a", 1700000100, 2000)]);
            var details = findChild(sheet, "detectionCallDetails");
            var reason = findChild(sheet, "detectionCallDetailsReason");
            var list = findChild(sheet, "detectionRecordingList");
            var revision = recordings.revision;
            var identity = findChild(recordingRow("b"), "detectionRecordingIdentity");
            verify(details.visible && !details.enabled, "no row holds it yet");
            verify(reason.opacity > 0.5 && !reason.Accessible.ignored, "and says why");
            compare(reason.text, "This call is not in History");
            compare(details.Accessible.description, reason.text);
            // Issue #902 B9: described only while in the tree.
            var modals = Ui.Navigation.modals;
            var cover = Qt.createQmlObject("import QtQuick; Item {}", sheet.parent);
            Ui.Navigation.modals = modals.concat([cover]);
            try {
                compare(details.Accessible.description, "", "covered, Call details describes nothing");
            } finally {
                Ui.Navigation.modals = modals;
                cover.destroy();
            }
            compare(details.Accessible.description, reason.text);
            waitForRendering(sheet);
            var listY = list.mapToItem(sheet, 0, 0).y;
            // The recording names no target or source: its line says only that,
            // which stays true with or without the History row Call details opens.
            compare(identity.text, "No\u00a0target\u00a0or\u00a0source\u00a0recorded");
            var uid = callHistory.pushWithRecordings([]);
            verify(!details.enabled);
            verify(callHistory.setFragmentRow("F:3:1", uid));
            verify(details.enabled, "adopted late");
            verify(reason.opacity < 0.5 && reason.Accessible.ignored, "with nothing to explain");
            compare(details.Accessible.description, "");
            waitForRendering(sheet);
            compare(list.mapToItem(sheet, 0, 0).y, listY, "the list stays where it was");
            compare(sheet.detailsUid, uid);
            compare(identity.text, "No\u00a0target\u00a0or\u00a0source\u00a0recorded");
            verify(identity.text.indexOf("details") < 0, "never contradicts Call details");
            var other = callHistory.pushWithRecordings([]);
            verify(callHistory.setFragmentRow("F:3:1", other));
            compare(sheet.detailsUid, other, "merged into another row");
            verify(details.enabled);
            verify(callHistory.trimRow(other));
            verify(!details.enabled, "its row trimmed");
            verify(callHistory.setFragmentRow("F:3:1", uid));
            verify(details.enabled);
            callHistory.clearAll();
            verify(!details.enabled, "History cleared");
            compare(recordings.revision, revision, "none of it moved the store");
            // Selecting a recording with no fragment: none.
            verify(callHistory.setFragmentRow("F:3:1", callHistory.pushWithRecordings([])));
            verify(details.enabled);
            recordingRow("a").activate();
            verify(details.visible && !details.enabled);
            // Neither while it opens (the player says "Opening…") nor once it plays.
            compare(findChild(sheet, "recordingStatus").text, "Opening…");
            waitForRendering(sheet);
            compare(list.mapToItem(sheet, 0, 0).y, listY, "selecting another recording moves no row");
            testContext.publishPlayback("/rec/Explore/a.wav", 2, 0, 2000, "");
            waitForRendering(sheet);
            compare(list.mapToItem(sheet, 0, 0).y, listY, "nor its playback starting");
            sheet.visible = false;
            model.history = [];
        }
        // Call details closes the sheet, stopping what it played, and opens the
        // History details here with Hold hidden; Back returns to the screen.
        function test_call_details_replaces_sheet() {
            var uid = callHistory.pushWithRecordings([]);
            verify(callHistory.setFragmentRow("F:3:1", uid));
            var sheet = openRecordings([rec("b", 1700000200, 3000, {fragment: "F:3:1"})]);
            sheetTap(findChild(sheet, "recordingPlayButton"));
            testContext.publishPlayback("/rec/Explore/b.wav", 2, 1000, 3000, "");
            var details = findChild(sheet, "detectionCallDetails");
            sheetTap(details);
            verify(!sheet.visible, "the recordings sheet closes");
            compare(lastHostCall().name, "stopPlaybackGeneration", "and what it played stops");
            var callDetails = findChild(loader.item, "searchCallDetails");
            verify(callDetails.visible);
            compare(callDetails.record.uid, uid);
            verify(callDetails.rowTg > 0, "a talkgroup call, which would offer Hold");
            verify(!findChild(callDetails, "historyHoldButton").visible, "no Hold during a search");
            verify(!findChild(callDetails, "historyHoldReason").visible);
            verify(loader.item.menuOpen, "the screen under it takes no taps");
            verify(Ui.Navigation.back(callDetails.Window.window));
            verify(!callDetails.visible);
            verify(!sheet.visible, "Back returns to the search screen");
            verify(!loader.item.menuOpen);
            // Gone before the click: the click resolves again and stays.
            sheet = openRecordings([rec("b", 1700000200, 3000, {fragment: "F:3:1"})]);
            verify(details.enabled);
            callHistory.clearAll();
            sheet.detailsUid = uid;
            sheet.showDetails();
            verify(sheet.visible && !callDetails.visible, "nothing to show");
            sheet.visible = false;
            model.history = [];
        }
        // The first characters of each line @p words wrap to at @p width in
        // @p text's font, read from a TextEdit laid out the same way (a Text gives
        // no line's characters), each line's whole text, and how many lines that is.
        function renderedLineStarts(text, words, width) {
            var mirror = Qt.createQmlObject("import QtQuick; TextEdit { readOnly: true; visible: false; textFormat: TextEdit.PlainText }", root);
            mirror.width = width;
            mirror.font = text.font;
            mirror.wrapMode = text.wrapMode;
            mirror.text = words;
            var starts = [];
            var at = [];
            var lastY = -1;
            for (var i = 0; i < mirror.length; ++i) {
                var y = mirror.positionToRectangle(i).y;
                if (y > lastY + 0.5) {
                    starts.push(mirror.getText(i, Math.min(mirror.length, i + 2)));
                    at.push(i);
                    lastY = y;
                }
            }
            // Each drawn line's whole text.
            var texts = at.map(function (from, k) { return mirror.getText(from, k + 1 < at.length ? at[k + 1] : mirror.length); });
            var lines = mirror.lineCount;
            mirror.destroy();
            return {starts: starts, texts: texts, lines: lines};
        }
        // The lines a plain Text.Wrap Text in @p font draws @p words in at @p width.
        function drawnLines(font, words, width) {
            var drawn = Qt.createQmlObject("import QtQuick; Text { visible: false; textFormat: Text.PlainText; wrapMode: Text.Wrap }", root);
            drawn.font = font;
            drawn.width = width;
            drawn.text = words;
            var lines = drawn.lineCount;
            drawn.destroy();
            return lines;
        }
        // Issue #902 B10: the Now panel counts lines as Qt lays them out, so
        // what it fits is what is drawn. A word exactly as wide as the line
        // takes one line (the arithmetic counted another), and a run of
        // glyphs wider than the line breaks where Qt breaks it (11 W at 5.6
        // W's width draws three lines; the arithmetic said two).
        function test_now_fitting_counts_lines_as_drawn_data() {
            return [{tag: "default text", scale: 1.0}, {tag: "largest text", scale: 1.6}];
        }
        function test_now_fitting_counts_lines_as_drawn(data) {
            Ui.Theme.fontScale = data.scale;
            var screen = loader.item;
            var detail = findChild(screen, "searchDetail");
            var metrics = Qt.createQmlObject("import QtQuick; FontMetrics {}", root);
            metrics.font = detail.font;
            var exact = metrics.advanceWidth("12345678");
            compare(drawnLines(detail.font, "A · 12345678", exact), 2, "Qt draws the exact-width word on one line");
            compare(screen.wrappedLines("A · 12345678", exact, metrics), 2, "counted as drawn");
            var glyphs = metrics.advanceWidth("W") * 5.6;
            compare(drawnLines(detail.font, "WWWWWWWWWWW", glyphs), 3, "Qt draws 11 glyphs in three lines");
            compare(screen.wrappedLines("WWWWWWWWWWW", glyphs, metrics), 3, "counted as drawn");
            // Two lines hold both values, as drawn: nothing is left off.
            var fitted = screen.fitValues(["A", "12345678"], exact, 2, metrics);
            compare(fitted, "A\u00a0· 12345678", "both values on the two lines");
            compare(drawnLines(detail.font, fitted, exact), 2);
            // A value given up is given up for what is drawn: one line holds
            // neither "A · 12345678" nor "A · …" plus the other value.
            compare(screen.fitValues(["A", "12345678"], exact, 1, metrics), "A\u00a0· …");
            metrics.destroy();
        }
        // Issue #902 B9: under a sheet the Now panel's readings are plain panes
        // (Qt keeps a static text focusable, with SetFocus, whatever its flags).
        function test_now_readings_leave_the_tree_under_a_sheet() {
            var screen = loader.item;
            model.status = Object.assign({}, model.status, {phase: 4, livePowerValid: true, livePower: -40, liveSeq: 7,
                hit: Object.assign({}, model.status.hit, {confirmed: true, protocol: "DMR", site: "Site 3", knownNames: ["Warehouse"], knownLabel: "Warehouse"})});
            var names = ["searchStrength", "searchIdentification", "searchDetail", "searchSaved"];
            for (var name of names)
                compare(findChild(screen, name).Accessible.role, Accessible.StaticText, name + " in the tree");
            var cover = Qt.createQmlObject("import QtQuick; Item {}", root);
            var modals = Ui.Navigation.modals;
            Ui.Navigation.modals = modals.concat([cover]);
            try {
                for (var covered of names)
                    compare(findChild(screen, covered).Accessible.role, Accessible.Pane, covered + " covered");
            } finally {
                Ui.Navigation.modals = modals;
                cover.destroy();
            }
        }
        // Issue #902: a first value that fits alone, but not with "· …", shows
        // alone rather than cut by the Text's own elision.
        function test_fit_keeps_a_first_value_that_fits_alone() {
            var screen = loader.item;
            var detail = findChild(screen, "searchDetail");
            var metrics = Qt.createQmlObject("import QtQuick; FontMetrics {}", root);
            metrics.font = detail.font;
            var width = metrics.advanceWidth("Alpha Bravo") + 1;
            verify(drawnLines(detail.font, "Alpha\u00a0Bravo\u00a0· …", width) > 1, "with the marker it needs a second line");
            compare(screen.fitValues(["Alpha Bravo", "Charlie"], width, 1, metrics), "Alpha\u00a0Bravo");
            metrics.destroy();
        }
        // Issue #902: a screen built while a search runs fits its status line
        // and its detail once its measure is made. The fit read before it
        // (every text one line) is never kept, so the channel count shows and
        // the detail ends on a whole value, exactly as the same screen fits
        // them after a resize.
        function test_fits_a_screen_built_mid_search_data() {
            return [{tag: "360", width: 360}, {tag: "410", width: 410}];
        }
        function test_fits_a_screen_built_mid_search(data) {
            var long = "Northern industrial park repeater cluster by the harbour approach";
            model.status = Object.assign({}, model.status, {phase: 4, rangeName: long,
                hit: Object.assign({}, model.status.hit, {range: long, unresolved: false, confirmed: true, protocol: "DMR",
                    access: "CC 15", tone: "CTCSS 250.3 Hz", targetText: "Target 1234567", sourceText: "Source 7654321",
                    site: "Site 3 · Net 12 · Northern industrial park", slot: 2, crypt: "ALG 21 · KID 0001"})});
            var component = Qt.createComponent(uiDir + "/SignalSearchScreen.qml");
            compare(component.status, Component.Ready, component.errorString());
            var screen = createTemporaryObject(component, root, {width: data.width, height: 891, searchModel: model,
                systems: systemsStub, sessionActive: true});
            verify(screen !== null);
            waitForRendering(screen);
            var status = findChild(screen, "searchStatusLine");
            var detail = findChild(screen, "searchDetail");
            var built = {status: status.text, detail: detail.text};
            verify(built.status.indexOf("channel 81 of 801") >= 0, "the channel count shows: " + built.status);
            verify(!status.truncated, "the status line shows whole, the channel count included: " + built.status);
            verify(!detail.truncated, "the detail shows whole, no value cut: " + built.detail);
            screen.width = data.width + 1;
            screen.width = data.width;
            waitForRendering(screen);
            compare(status.text, built.status, "the status line as a resize fits it");
            compare(detail.text, built.detail, "the detail as a resize fits it");
        }
        // Issue #902: a status update that changes nothing the status line or
        // the detail shows (a live level, every 250 ms) runs no layout, with a
        // detail of many values shown; a new channel does, and so does a
        // changed detail value.
        function test_live_level_updates_run_no_layout() {
            root.width = 360;
            Ui.Theme.fontScale = 1.6;
            var screen = loader.item;
            model.status = Object.assign({}, model.status, {phase: 4, rangeName: "WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW",
                rangeStartMHz: "450.000000", rangeEndMHz: "459.987500", livePowerValid: true, livePower: -40, liveSeq: 1,
                hit: Object.assign({}, model.status.hit, {unresolved: false, confirmed: true, protocol: "DMR", access: "CC 15",
                    tone: "CTCSS 250.3 Hz", targetText: "Target 1234567", sourceText: "Source 7654321",
                    site: "Site 3 · Net 12", slot: 2, crypt: "ALG 21 · KID 0001"})});
            waitForRendering(findChild(screen, "searchNowPanel"));
            var detail = findChild(screen, "searchDetail");
            verify(detail.text.indexOf("CC\u00a015") >= 0 && detail.text.split("·").length > 2, "a detail of many values shows: " + detail.text);
            var layouts = screen.measureTally.layouts;
            // As the model hands them: a new map each update, its hit a new copy too.
            for (var seq = 2; seq < 6; ++seq)
                model.status = Object.assign({}, model.status, {livePower: -40 - seq, liveSeq: seq, hit: Object.assign({}, model.status.hit)});
            compare(screen.measureTally.layouts, layouts, "a live level re-lays out nothing");
            model.status = Object.assign({}, model.status, {hit: Object.assign({}, model.status.hit, {access: "CC 14"})});
            verify(screen.measureTally.layouts > layouts, "a changed detail value fits the detail again");
            verify(detail.text.indexOf("CC\u00a014") >= 0, "the new value is shown: " + detail.text);
            layouts = screen.measureTally.layouts;
            model.status = Object.assign({}, model.status, {livePower: -47, liveSeq: 7, hit: Object.assign({}, model.status.hit)});
            compare(screen.measureTally.layouts, layouts, "then a live level still re-lays out nothing");
            model.status = Object.assign({}, model.status, {channel: (model.status.channel || 0) + 1});
            verify(screen.measureTally.layouts > layouts, "a new channel fits the line again");
        }
        // A Recent detections row's details wrap only between values, at every
        // width from the narrowest row (360 dp, the largest text) up: a value that
        // fits alone but not with its separator wraps at its words, as one kept
        // whole would break before its separator and start a line with it.
        function test_history_detail_never_starts_a_line_with_a_separator() {
            root.width = 360;
            Ui.Theme.fontScale = 1.6;
            var words = "TIII Small · Net 12 · Site 3 · SYS 1183 · ALG 21 · KID 0001 · Northern District Fire Dispatch · Engine 42 Officer";
            model.history = [historyHit({confirmed: true, protocol: "DMR", site: words})];
            wait(0);
            var detail = findChild(loader.item, "searchHistoryDetail");
            var metrics = Qt.createQmlObject("import QtQuick; FontMetrics {}", root);
            metrics.font = detail.font;
            for (var w = Math.floor(detail.width); w < detail.width + 160; w += 1) {
                var drawn = renderedLineStarts(detail, loader.item.keepValues(words, w, metrics), w).starts;
                for (var start of drawn)
                    verify(start.trim().charAt(0) !== "·", "at " + w + " px a drawn line starts with a separator: " + JSON.stringify(drawn));
            }
            metrics.destroy();
            model.history = [];
        }
        // Largest text on a 360 dp phone: the rows wrap inside the sheet, every
        // control keeps a 48 dp target, and the sheet scrolls to its end. The
        // row's menu with Record calls off and nothing kept shows the disabled
        // Recordings and its hint last.
        function test_sheet_large_text_360_data() {
            return [{tag: "dark", appearance: 2}, {tag: "light", appearance: 1}];
        }
        function test_sheet_large_text_360(data) {
            testContext.setPrefs("appearance", data.appearance);
            root.width = 360;
            Ui.Theme.fontScale = 1.6;
            var uid = callHistory.pushWithRecordings([]);
            verify(callHistory.setFragmentRow("F:9:1", uid));
            model.history = [historyHit({recordingKey: "17:40", confirmed: true, protocol: "DMR", site: "TIII Small · Net 12 · Site 3 · SYS 1183", crypt: "ALG 21 · KID 0001", knownLabel: "Warehouse · 462.5750 MHz"}),
                historyHit({id: "39", recordingKey: "17:39"})];
            recordings.setDetectionRecordings("17:40", [rec("c", 1700000300, 125000, {target: "Northern District Fire Dispatch", source: "Engine 42 Officer", system: "DMR · TIII Small · Net 12 · Site 3", fragment: "F:9:1"}),
                rec("b", 1700000200, 3000, {target: "1234", source: "5678"}), rec("a", 1700000100, 500, {available: false})]);
            wait(50);
            var scroll = findChild(loader.item, "searchScroll");
            var row = findChild(loader.item, "searchHistoryRow");
            scroll.contentY = Math.max(0, row.mapToItem(scroll.contentItem, 0, 0).y - 60);
            wait(100);
            var detail = findChild(loader.item, "searchHistoryDetail");
            verify(detail.mapToItem(row, 0, 0).x + detail.width <= row.width + 0.5);
            // The row that kept none, with Record calls off: the hint comes last.
            findChildren(loader.item, "searchHistoryActions")[1].activate();
            var menu = findChild(loader.item, "searchDetectionMenu");
            compare(menuTexts()[menu.actions.length - 1], "Recordings (disabled)");
            var hint = findChild(menu, "actionMenuDescription" + (menu.actions.length - 1));
            verify(hint.visible && hint.lineCount >= 2, "the hint wraps");
            verify(hint.mapToItem(menu, 0, 0).x + hint.width <= menu.width, "inside the menu");
            wait(50);
            menu.cancel();
            findChild(loader.item, "searchHistoryActions").activate();
            wait(50);
            menuItem("searchActionRecordings").activate();
            var sheet = recordingsSheet();
            waitForRendering(sheet);
            wait(50);
            var panel = findChild(sheet, "modalSheetScroll");
            var inside = function (item) {
                var p = item.mapToItem(panel, 0, 0);
                return p.x >= -0.5 && p.x + item.width <= panel.width + 0.5;
            };
            for (var r of recordingRows()) {
                verify(inside(r), r.key + " stays inside the sheet");
                verify(r.height >= Ui.Theme.minimumTouchSize);
                for (var name of ["detectionRecordingTime", "detectionRecordingIdentity", "detectionRecordingAway"]) {
                    var text = findChild(r, name);
                    if (!text.visible)
                        continue;
                    verify(text.mapToItem(r, 0, 0).x + text.width <= r.width + 0.5, r.key + " " + name + " inside its row");
                    verify(text.mapToItem(r, 0, 0).y + text.height <= r.height + 0.5, r.key + " " + name + " inside its row");
                }
            }
            verify(findChild(recordingRow("a"), "detectionRecordingAway").visible, "the away line is among those checked");
            // Values wrap whole: no drawn line of the longest identity starts with a
            // separator, here (the narrowest a row gets: 360 dp at the largest
            // text) and at every width up to 160 px wider, where some break falls
            // next to each separator.
            var identity = findChild(recordingRow("c"), "detectionRecordingIdentity");
            var words = "Northern District Fire Dispatch · Engine 42 Officer · DMR · TIII Small · Net 12 · Site 3";
            compare(identity.text.replace(/\u00a0/g, " "), words);
            verify(identity.lineCount > 1, "it wraps at this size");
            compare(renderedLineStarts(identity, identity.text, identity.width).lines, identity.lineCount, "the mirror wraps as the row does");
            for (var w = identity.width; w < identity.width + 160; w += 1) {
                var drawn = renderedLineStarts(identity, recordingsSheet().keepValues(words, w), w).starts;
                for (var start of drawn)
                    verify(start.trim().charAt(0) !== "·", "at " + w + " px a drawn line starts with a separator: " + JSON.stringify(drawn));
            }
            for (var control of ["recordingPlayButton", "recordingSave", "recordingDelete", "detectionCallDetails", "detectionRecordingsClose"]) {
                var item = findChild(sheet, control);
                verify(item.visible, control);
                verify(inside(item), control + " stays inside the sheet");
                verify(item.height >= Ui.Theme.minimumTouchSize && item.width >= Ui.Theme.minimumTouchSize, control + " keeps its target");
            }
            verify(revealInSheet(sheet, findChild(sheet, "detectionRecordingsClose")), "the sheet scrolls to Close");
            Ui.Theme.fontScale = 1.0;
            sheet.visible = false;
            wait(0);
            findChild(loader.item, "searchHistoryActions").activate();
            menuItem("searchActionRecordings").activate();
            waitForRendering(sheet);
            recordingRow("b").activate();
            wait(50);
            sheet.visible = false;
            scroll.contentY = 0;
            model.history = [];
        }
        // ---- Issue #826: exporting detections and avoids as CSV ----
        function exportButton(kind) {
            return findChild(loader.item, kind === "detections" ? "searchExportDetections" : "searchExportAvoids");
        }
        function exportLine(kind) {
            return findChild(loader.item, kind === "detections" ? "searchExportDetectionsMessage" : "searchExportAvoidsMessage");
        }
        function exportPicker() {
            return findChild(loader.item, "searchExportPicker");
        }
        function exportCalls(name) {
            return JSON.stringify(model.exportCalls.filter(function (call) { return call[0] === name; }));
        }
        // Rows for both buttons: two detections and one permanent avoid.
        function fillForExport() {
            model.history = [historyHit(), historyHit({id: "41", frequency: 462600000, freqMHz: "462.600000"})];
            model.avoids = [154430000];
            wait(0);
        }
        // Press @p kind's button over its rows: the id of the save request it opens.
        function beginExport(kind) {
            fillForExport();
            var button = exportButton(kind);
            verify(button && button.visible && button.enabled, kind + " can be exported");
            button.clicked();
            var request = testContext.lastDocumentRequest();
            compare(request.kind, "save");
            compare(request.mime, "text/csv");
            verify(exportPicker().visible, "the picker waits");
            return request.id;
        }
        function noRequest() {
            return JSON.stringify(testContext.lastDocumentRequest()) === "{}";
        }
        // Export detections… ends Recent detections, after Show more, while the history has rows: searching or idle.
        function test_export_detections_button_follows_history() {
            var screen = loader.item;
            model.history = [];
            wait(0);
            verify(!exportButton("detections").visible, "no history, nothing to export");
            fillForExport();
            var button = exportButton("detections");
            verify(button.visible && button.enabled);
            compare(button.text, "Export detections…");
            compare(button.width, findChild(screen, "searchHistoryRow").width, "full width");
            var rows = [];
            for (var i = 0; i < 40; ++i)
                rows.push(historyHit({id: String(100 + i)}));
            model.history = rows;
            waitForRendering(screen);
            var more = findChild(screen, "searchShowMore");
            verify(more.visible);
            verify(button.mapToItem(null, 0, 0).y > more.mapToItem(null, 0, 0).y, "after Show more");
            more.clicked();
            verify(exportButton("detections").visible, "still there after Show more");
            screen.sessionActive = false;
            wait(0);
            verify(exportButton("detections").visible, "idle, under the ranges");
        }
        // Export avoids… follows the Permanent avoids list while either list the screen shows has an entry.
        function test_export_avoids_button_follows_the_shown_lists() {
            var screen = loader.item;
            wait(0);
            verify(!exportButton("avoids").visible, "no avoids, nothing to export");
            model.avoids = [154430000];
            verify(exportButton("avoids").visible, "permanent avoids alone");
            compare(exportButton("avoids").text, "Export avoids…");
            waitForRendering(screen);
            var avoid = findChild(screen, "searchPermanentAvoid");
            verify(exportButton("avoids").mapToItem(null, 0, 0).y > avoid.mapToItem(null, 0, 0).y, "after the list");
            model.avoids = [];
            verify(!exportButton("avoids").visible);
            model.sessionAvoids = [463550000];
            verify(exportButton("avoids").visible, "a live session-only list");
            model.status = Object.assign({}, model.status, {phase: 7, hit: {}});
            verify(!exportButton("avoids").visible, "a failed search shows no session list");
            model.status = Object.assign({}, model.status, {phase: 4});
            verify(exportButton("avoids").visible);
            screen.sessionActive = false;
            verify(!exportButton("avoids").visible, "the session list ended with the search");
        }
        // One CSV save picker for both, named for the day; one export at a time.
        function test_export_opens_a_csv_save_picker_data() {
            return [{tag: "detections", kind: "detections", name: "Signal search detections 2026-11-09.csv"},
                    {tag: "avoids", kind: "avoids", name: "Signal search avoids 2026-11-09.csv"}];
        }
        function test_export_opens_a_csv_save_picker(data) {
            var id = beginExport(data.kind);
            compare(testContext.lastDocumentRequest().name, data.name);
            compare(exportCalls("prepare"), JSON.stringify([["prepare", data.kind]]));
            var picker = exportPicker();
            compare(picker.mode, "save");
            compare(picker.mimeType, "text/csv");
            compare(JSON.stringify(picker.nameFilters), JSON.stringify(["CSV files (*.csv)"]));
            compare(picker.defaultSuffix, "csv");
            verify(!exportButton("detections").enabled && !exportButton("avoids").enabled, "both wait while the picker asks");
            testContext.answerDocumentCancelled(id);
            verify(exportButton("detections").enabled && exportButton("avoids").enabled);
        }
        // No rows to take: nothing to pick, and the line says so.
        function test_export_with_nothing_to_export_data() {
            return [{tag: "detections", kind: "detections", text: "No detections to export."},
                    {tag: "avoids", kind: "avoids", text: "No avoids to export."}];
        }
        function test_export_with_nothing_to_export(data) {
            model.prepared = {detections: {token: 0, count: 0}, avoids: {token: 0, count: 0}};
            fillForExport();
            exportButton(data.kind).clicked();
            verify(noRequest(), "no picker");
            verify(!exportPicker().visible);
            var line = exportLine(data.kind);
            compare(line.text, data.text);
            verify(line.visible);
            verify(exportButton(data.kind).enabled, "nothing pending");
        }
        function test_export_while_the_avoid_lists_change() {
            model.prepared = {avoids: {token: 0, count: -1}};
            fillForExport();
            exportButton("avoids").clicked();
            verify(noRequest(), "no picker");
            compare(exportLine("avoids").text, "The avoid lists were changing. Try again.");
            verify(exportButton("avoids").enabled);
        }
        // The day for the suggested name is read before the export is held: a press that fails there leaves
        // neither button waiting.
        function test_export_that_cannot_name_the_file_holds_nothing() {
            var screen = loader.item;
            fillForExport();
            screen.systems = null;
            var threw = false;
            try {
                screen.beginExport("detections");
            } catch (e) {
                threw = true;
            }
            screen.systems = systemsStub;
            verify(threw, "no clock to name the file by");
            compare(exportCalls("prepare"), "[]", "the model took no rows (issue #902 B5)");
            verify(noRequest(), "no picker");
            compare(screen.exportToken, 0);
            verify(exportButton("detections").enabled && exportButton("avoids").enabled, "nothing pending");
        }
        // Backing out of the picker drops the export and says nothing.
        function test_export_picker_cancel_discards_quietly() {
            var id = beginExport("detections");
            testContext.answerDocumentCancelled(id);
            compare(exportCalls("discard"), JSON.stringify([["discard", 7]]));
            compare(exportCalls("save"), "[]");
            verify(!exportLine("detections").visible, "nothing to say");
            compare(loader.item.exportToken, 0);
            verify(exportButton("detections").enabled);
        }
        function test_export_picker_unavailable() {
            var id = beginExport("avoids");
            testContext.answerDocumentUnavailable(id, "noSavePicker");
            compare(exportCalls("discard"), JSON.stringify([["discard", 8]]));
            compare(exportLine("avoids").text, "This device has no save picker, and DSD-neo could not save to the Download folder.");
            verify(exportLine("avoids").visible);
            verify(exportButton("avoids").enabled);
        }
        // An accepted pick is saved; the line under the button says how it went, read out once.
        function test_export_saves_the_pick_then_says_how_it_went_data() {
            return [{tag: "detections", kind: "detections", token: 7, ok: true, text: "Exported 2 detections."},
                    {tag: "one avoid", kind: "avoids", token: 8, ok: true, text: "Exported 1 avoid."},
                    {tag: "detections to Download", kind: "detections", token: 7, ok: true, saved: "Signal search detections 2026-11-09 (1).csv",
                     text: "Exported 2 detections to Download/DSD-neo as Signal search detections 2026-11-09 (1).csv."},
                    {tag: "one avoid to Download", kind: "avoids", token: 8, ok: true, saved: "Signal search avoids 2026-11-09.csv",
                     text: "Exported 1 avoid to Download/DSD-neo as Signal search avoids 2026-11-09.csv."},
                    {tag: "failure", kind: "avoids", token: 8, ok: false, text: "The export couldn't be written. Try another location."}];
        }
        function test_export_saves_the_pick_then_says_how_it_went(data) {
            var screen = loader.item;
            var id = beginExport(data.kind);
            var reference = "content://docs/" + data.kind;
            if (data.saved)
                testContext.setSavedDocumentName(reference, data.saved);
            testContext.answerDocumentPicked(id, reference, "export.csv", !!data.saved);
            compare(exportCalls("save"), JSON.stringify([["save", data.token, reference]]));
            verify(!exportButton("detections").enabled && !exportButton("avoids").enabled, "both wait while the file is written");
            verify(!exportLine(data.kind).visible, "nothing to say yet");
            var before = screen.announcements;
            model.exportFinished(data.token, data.ok);
            var line = exportLine(data.kind);
            compare(line.text, data.text);
            verify(line.visible);
            verify(Qt.colorEqual(line.color, data.ok ? Ui.Theme.textSecondary : Ui.Theme.alert), "error tone for a failure only");
            compare(line.Accessible.role, Accessible.AlertMessage);
            compare(line.Accessible.name, data.text);
            tryCompare(screen, "announcements", before + 1);
            compare(screen.announced, data.text);
            verify(!exportLine(data.kind === "detections" ? "avoids" : "detections").visible, "the other line says nothing");
            verify(exportButton("detections").enabled && exportButton("avoids").enabled, "free for the next export");
            compare(exportCalls("discard"), "[]");
            wait(20);
            compare(screen.announcements, before + 1, "read once");
        }
        // A save the model refuses fails at once: nothing waits for a result, and the next export can start.
        function test_refused_save_fails_at_once() {
            model.saveAccepted = false;
            var id = beginExport("detections");
            testContext.answerDocumentPicked(id, "content://docs/a", "a.csv", true);
            compare(exportCalls("save"), JSON.stringify([["save", 7, "content://docs/a"]]));
            compare(exportLine("detections").text, "The export couldn't be written. Try another location.");
            compare(loader.item.exportToken, 0, "the export is let go");
            verify(!testContext.documentCalls().some(function (call) { return call.name === "discardPendingDownload"; }),
                   "the model let the Download row go itself");
            verify(exportButton("detections").enabled);
            model.saveAccepted = true;
            model.prepared = {detections: {token: 9, count: 2}};
            exportButton("detections").clicked();
            verify(exportPicker().visible, "a second export starts");
            verify(!exportLine("detections").visible, "a new export clears the line");
            testContext.answerDocumentPicked(testContext.lastDocumentRequest().id, "content://docs/b", "b.csv", false);
            model.exportFinished(9, true);
            compare(exportLine("detections").text, "Exported 2 detections.");
        }
        // A result for an export the screen no longer holds changes nothing.
        function test_stale_export_result_is_ignored() {
            var id = beginExport("detections");
            testContext.answerDocumentCancelled(id);
            model.prepared = {detections: {token: 9, count: 2}};
            exportButton("detections").clicked();
            testContext.answerDocumentPicked(testContext.lastDocumentRequest().id, "content://docs/b", "b.csv", false);
            model.exportFinished(7, false);
            model.exportFinished(0, false);
            verify(!exportLine("detections").visible, "an old token says nothing");
            compare(loader.item.exportToken, 9, "and leaves the newer export pending");
            verify(!exportButton("detections").enabled);
            model.exportFinished(9, true);
            compare(exportLine("detections").text, "Exported 2 detections.");
            model.exportFinished(9, false);
            compare(exportLine("detections").text, "Exported 2 detections.", "a repeat changes nothing");
        }
        // The line is its own: a search failure neither hides it nor is hidden, and an emptied list keeps it.
        function test_export_line_beside_a_search_failure() {
            root.hostFailure = "Device detached: RTL-SDR";
            var id = beginExport("avoids");
            testContext.answerDocumentPicked(id, "content://docs/a", "a.csv", false);
            model.exportFinished(8, true);
            var message = findChild(loader.item, "searchMessage");
            compare(message.text, "Device detached: RTL-SDR");
            verify(message.visible);
            verify(exportLine("avoids").visible);
            compare(exportLine("avoids").text, "Exported 1 avoid.");
            model.avoids = [];
            verify(!exportButton("avoids").visible);
            verify(exportLine("avoids").visible, "the result outlives its list");
            root.hostFailure = "";
            verify(exportLine("avoids").visible);
        }
        // Leaving the screen ends the picker as a cancel; nothing is said.
        function test_hiding_the_screen_cancels_the_picker() {
            var screen = loader.item;
            var id = beginExport("detections");
            screen.visible = false;
            verify(!exportPicker().visible);
            compare(exportCalls("discard"), JSON.stringify([["discard", 7]]));
            verify(testContext.documentCalls().some(function (call) { return call.name === "cancelDocumentRequest" && call.id === id; }),
                   JSON.stringify(testContext.documentCalls()));
            screen.visible = true;
            verify(exportButton("detections").enabled);
            verify(!exportLine("detections").visible, "a cancel says nothing");
        }
        // A result that lands while the screen is hidden is kept and read once on return; heard, the next visit
        // starts without it.
        function test_result_while_hidden_is_read_once_on_return() {
            var screen = loader.item;
            var id = beginExport("detections");
            testContext.answerDocumentPicked(id, "content://docs/a", "a.csv", false);
            screen.visible = false;
            var before = screen.announcements;
            model.exportFinished(7, true);
            wait(20);
            compare(screen.announcements, before, "nothing is read to a hidden screen");
            screen.visible = true;
            compare(exportLine("detections").text, "Exported 2 detections.", "kept for the return");
            tryCompare(screen, "announcements", before + 1);
            wait(20);
            compare(screen.announcements, before + 1, "once");
            screen.visible = false;
            screen.visible = true;
            verify(!exportLine("detections").visible, "heard, it is gone on the next visit");
            wait(20);
            compare(screen.announcements, before + 1);
        }
        // Under the detection menu the line is out of reach: read once the menu closes.
        function test_result_under_a_sheet_is_read_once_it_closes() {
            var screen = loader.item;
            var id = beginExport("detections");
            testContext.answerDocumentPicked(id, "content://docs/a", "a.csv", false);
            findChild(screen, "searchHistoryActions").activate();
            var menu = findChild(screen, "searchDetectionMenu");
            verify(menu.visible);
            var before = screen.announcements;
            model.exportFinished(7, true);
            wait(20);
            compare(screen.announcements, before, "nothing is read under the menu");
            var line = exportLine("detections");
            verify(line.visible, "drawn under the menu");
            verify(line.Accessible.ignored, "out of the tree behind the menu");
            menu.cancel();
            tryCompare(screen, "announcements", before + 1);
            compare(screen.announced, "Exported 2 detections.");
            verify(!line.Accessible.ignored);
            wait(20);
            compare(screen.announcements, before + 1, "once");
        }
        // The search stopping moves Recent detections under the ranges (a new Loader item): the export goes on, and
        // its line follows without being read twice.
        function test_loader_swap_keeps_the_export_and_its_line() {
            var screen = loader.item;
            var id = beginExport("detections");
            testContext.answerDocumentPicked(id, "content://docs/a", "a.csv", false);
            screen.sessionActive = false;
            wait(0);
            verify(exportButton("detections").visible && !exportButton("detections").enabled, "still writing");
            var before = screen.announcements;
            model.exportFinished(7, true);
            compare(exportLine("detections").text, "Exported 2 detections.");
            tryCompare(screen, "announcements", before + 1);
            screen.sessionActive = true;
            wait(20);
            verify(exportLine("detections").visible);
            compare(exportLine("detections").text, "Exported 2 detections.", "the rebuilt line says it still");
            compare(screen.announcements, before + 1, "and is not read again");
        }
        // A result that lands as the Loader swaps is read once, from the new line.
        function test_result_during_a_loader_swap_is_read_once() {
            var screen = loader.item;
            var id = beginExport("detections");
            testContext.answerDocumentPicked(id, "content://docs/a", "a.csv", false);
            var before = screen.announcements;
            model.exportFinished(7, true);
            screen.sessionActive = false;
            tryCompare(screen, "announcements", before + 1);
            compare(screen.announced, "Exported 2 detections.");
            verify(exportLine("detections").visible);
            screen.sessionActive = true;
            wait(20);
            compare(screen.announcements, before + 1);
        }

        // ---- Issue #826 B1: a hidden screen keeps its lists as they were ----
        // Every Repeater under @p item.
        function repeatersIn(item) {
            var found = [];
            if (!item)
                return found;
            if (typeof item.itemAdded === "function")
                found.push(item);
            for (var child of item.children)
                found = found.concat(repeatersIn(child));
            return found;
        }
        function shownTexts(name) {
            return JSON.stringify(findChildren(loader.item, name).filter(function (b) { return b.visible; }).map(function (b) { return b.text; }));
        }
        // Hidden, the screen builds and destroys no rows as the history and both avoid lists change (each one an
        // accessibility event on Android); shown again, it lists them as they are.
        function test_hidden_screen_builds_no_rows_data() {
            return [{tag: "searching", active: true}, {tag: "idle", active: false}];
        }
        function test_hidden_screen_builds_no_rows(data) {
            var screen = loader.item;
            screen.sessionActive = data.active;
            model.history = [historyHit()];
            model.sessionAvoids = [463550000];
            model.avoids = [154430000];
            wait(0);
            var changes = [];
            var note = function (index, item) { changes.push(item ? item.objectName || String(index) : String(index)); };
            var repeaters = repeatersIn(screen);
            verify(repeaters.length >= 3, "the history and avoid lists are Repeaters");
            for (var r of repeaters) {
                r.itemAdded.connect(note);
                r.itemRemoved.connect(note);
            }
            screen.visible = false;
            var rows = [];
            for (var i = 0; i < 8; ++i)
                rows.push(historyHit({id: String(50 + i), frequency: 462000000 + i * 12500, freqMHz: (462 + i * 0.0125).toFixed(6)}));
            model.history = rows;
            model.sessionAvoids = [463550000, 463575000];
            model.avoids = [154430000, 155000000, 156000000];
            model.avoids = [155000000, 156000000];
            wait(20);
            // A Repeater inside a row destroyed meanwhile is gone with it.
            for (r of repeaters.filter(function (each) { return typeof each.itemAdded === "function"; })) {
                r.itemAdded.disconnect(note);
                r.itemRemoved.disconnect(note);
            }
            compare(JSON.stringify(changes), "[]", "rows built or destroyed while hidden");
            screen.visible = true;
            wait(0);
            var shown = findChildren(screen, "searchHistoryRow").filter(function (row) { return row.visible; });
            compare(shown.length, 5);
            compare(findChild(shown[0], "searchHistoryReading").text.replace(/ /g, " "), "462.000000 MHz · -40.0 dBFS");
            compare(shownTexts("searchPermanentAvoid"), JSON.stringify(["Remove avoid · 155.000000 MHz", "Remove avoid · 156.000000 MHz"]));
            compare(shownTexts("searchSessionAvoid"), data.active ? JSON.stringify(["Stop avoiding · 463.550000 MHz", "Stop avoiding · 463.575000 MHz"]) : "[]");
        }
        // Frozen, not emptied: a reader near the end of a long page keeps their place across a hide and a show.
        function test_hidden_screen_keeps_the_readers_place_data() {
            return [{tag: "searching", active: true}, {tag: "idle", active: false}];
        }
        function test_hidden_screen_keeps_the_readers_place(data) {
            var screen = loader.item;
            screen.sessionActive = data.active;
            var rows = [], avoids = [], session = [];
            for (var i = 0; i < 5; ++i)
                rows.push(historyHit({id: String(60 + i)}));
            for (i = 0; i < 40; ++i)
                avoids.push(150000000 + i * 25000);
            for (i = 0; i < 10; ++i)
                session.push(460000000 + i * 12500);
            model.history = rows;
            model.avoids = avoids;
            model.sessionAvoids = session;
            var scroll = findChild(screen, "searchScroll");
            waitForRendering(screen);
            scroll.contentY = Math.max(0, scroll.contentHeight - scroll.height - 40);
            waitForRendering(screen);
            var place = scroll.contentY;
            verify(place > 0, "the page scrolls");
            screen.visible = false;
            wait(20);
            compare(scroll.contentY, place, "hidden");
            screen.visible = true;
            waitForRendering(screen);
            wait(20);
            compare(scroll.contentY, place, "reopened at the reader's place");
            scroll.contentY = 0;
        }
        // Show more, then hidden: the shrink back to five lands on the return, not while hidden.
        function test_hidden_screen_shrinks_on_return() {
            var screen = loader.item;
            var rows = [];
            for (var i = 0; i < 40; ++i)
                rows.push(historyHit({id: String(100 + i)}));
            model.history = rows;
            wait(0);
            findChild(screen, "searchShowMore").clicked();
            wait(0);
            var history = repeatersIn(screen).filter(function (r) { return r.count === 30; });
            compare(history.length, 1, "the history rows");
            var changes = 0;
            var note = function () { changes++; };
            history[0].itemRemoved.connect(note);
            screen.visible = false;
            wait(20);
            history[0].itemRemoved.disconnect(note);
            compare(changes, 0, "no row destroyed while hidden");
            compare(history[0].count, 30);
            screen.visible = true;
            wait(0);
            compare(findChildren(screen, "searchHistoryRow").filter(function (row) { return row.visible; }).length, 5);
        }
        // ---- Issue #902: when each detection was found ----
        function at(y, m, d, hh, mm, ss) {
            return new Date(y, m - 1, d, hh, mm, ss).getTime();
        }
        function nextDay() {
            return {startMs: new Date(2026, 10, 10).getTime(), revision: 2};
        }
        // The found time leads the verdict: a clock time on the screen's day,
        // with its date on any other, before or after it. A row with no time
        // (missing, 0 or less, not finite) reads as it always did.
        function test_history_row_says_when_it_was_found_data() {
            return [
                {tag: "today", started: at(2026, 11, 9, 14, 32, 5), text: "14:32:05 · RF only · ended", found: "14:32:05"},
                {tag: "today, confirmed", started: at(2026, 11, 9, 0, 0, 0), confirmed: true, text: "00:00:00 · DMR · confirmed · ended", found: "00:00:00"},
                {tag: "another day", started: at(2026, 11, 8, 9, 5, 7), text: "2026-11-08 09:05:07 · RF only · ended", found: "2026-11-08 09:05:07"},
                {tag: "later, after a clock correction", started: at(2026, 11, 10, 0, 0, 1), text: "2026-11-10 00:00:01 · RF only · ended", found: "2026-11-10 00:00:01"},
                {tag: "zero", started: 0, text: "RF only · ended", found: ""},
                {tag: "negative", started: -1000, text: "RF only · ended", found: ""},
                {tag: "not a number", started: NaN, text: "RF only · ended", found: ""},
                {tag: "infinite", started: Infinity, text: "RF only · ended", found: ""},
                {tag: "missing", text: "RF only · ended", found: ""}
            ];
        }
        function test_history_row_says_when_it_was_found(data) {
            var extra = data.confirmed ? {confirmed: true, protocol: "DMR"} : {};
            if (data.started !== undefined)
                extra.started = data.started;
            model.history = [historyHit(extra)];
            wait(0);
            var detail = findChild(loader.item, "searchHistoryDetail");
            compare(detail.fullText, data.text);
            compare(detail.text.replace(/\u00a0/g, " "), data.text, "drawn as written, the time one value");
            compare(detail.Accessible.name, data.text, "the detail's name carries the time");
            compare(findChild(loader.item, "searchHistoryActions").accessibleName,
                    data.found.length ? "Actions for 462.5750 MHz, " + data.found : "Actions for 462.5750 MHz");
            model.history = [];
        }
        // With no saved systems to say the day there is no today: every time has its date.
        function test_history_row_without_a_day_has_the_date() {
            model.history = [historyHit({started: at(2026, 11, 9, 14, 32, 5)})];
            wait(0);
            loader.item.systems = null;
            compare(loader.item.shownDay, null);
            compare(findChild(loader.item, "searchHistoryDetail").fullText, "2026-11-09 14:32:05 · RF only · ended");
            loader.item.systems = systemsStub;
            compare(findChild(loader.item, "searchHistoryDetail").fullText, "14:32:05 · RF only · ended");
            model.history = [];
        }
        // A day change on screen re-texts the row in place: no row is rebuilt.
        function test_history_row_follows_the_day_in_place() {
            model.history = [historyHit({started: at(2026, 11, 9, 14, 32, 5)})];
            wait(0);
            var history = repeatersIn(loader.item).filter(function (r) { return r.count === 1 && r.itemAt(0).objectName === "searchHistoryRow"; });
            compare(history.length, 1);
            var row = history[0].itemAt(0);
            var changes = 0;
            var note = function () { changes++; };
            history[0].itemAdded.connect(note);
            history[0].itemRemoved.connect(note);
            systemsStub.localDay = nextDay();
            history[0].itemAdded.disconnect(note);
            history[0].itemRemoved.disconnect(note);
            compare(changes, 0, "no row rebuilt");
            compare(history[0].itemAt(0), row);
            compare(findChild(row, "searchHistoryDetail").fullText, "2026-11-09 14:32:05 · RF only · ended");
            compare(findChild(row, "searchHistoryActions").accessibleName, "Actions for 462.5750 MHz, 2026-11-09 14:32:05");
            model.history = [];
        }
        // Hidden, a row keeps its words, its height and the reader's place
        // through a midnight and a recordings change (B7: three recordings to
        // none shrank a hidden row); both land on show.
        function test_hidden_screen_freezes_the_day_and_counts_data() {
            return [{tag: "searching", active: true}, {tag: "idle", active: false}];
        }
        function test_hidden_screen_freezes_the_day_and_counts(data) {
            var screen = loader.item;
            screen.sessionActive = data.active;
            recordings.setDetectionRecordings("17:40", [rec("c", 1700000300, 4000), rec("b", 1700000200, 3000), rec("a", 1700000100, 2000)]);
            var rows = [historyHit({started: at(2026, 11, 9, 14, 32, 5), site: "TIII Small · Net 12 · Site 3 · SYS 1183"})];
            for (var i = 1; i < 5; ++i)
                rows.push(historyHit({id: String(60 + i), recordingKey: "17:" + (60 + i)}));
            var avoids = [];
            for (i = 0; i < 40; ++i)
                avoids.push(150000000 + i * 25000);
            model.history = rows;
            model.avoids = avoids;
            var scroll = findChild(screen, "searchScroll");
            waitForRendering(screen);
            var row = findChildren(screen, "searchHistoryRow")[0];
            var detail = findChild(row, "searchHistoryDetail");
            // The count as the run's locale writes a whole number; the time is written the one way.
            var before = "14:32:05 · RF only · ended\nTIII Small · Net 12 · Site 3 · SYS 1183\n" + Number(3).toLocaleString(Qt.locale(), "f", 0) + " recordings";
            compare(detail.fullText, before);
            scroll.contentY = Math.max(0, scroll.contentHeight - scroll.height - 40);
            waitForRendering(screen);
            var place = scroll.contentY;
            var height = row.height;
            verify(place > 0, "the page scrolls");
            // The recordings alone: the row keeps its line while hidden and
            // loses it on show.
            screen.visible = false;
            recordings.setDetectionRecordings("17:40", []);
            wait(20);
            compare(findChildren(screen, "searchHistoryRow")[0], row, "the same row");
            compare(detail.fullText, before, "hidden, the row keeps its count");
            compare(row.height, height, "and its height");
            compare(scroll.contentY, place, "and the reader's place");
            screen.visible = true;
            waitForRendering(screen);
            var shown = "14:32:05 · RF only · ended\nTIII Small · Net 12 · Site 3 · SYS 1183";
            compare(detail.fullText, shown, "shown, the count goes");
            verify(row.height < height, "the row is shorter by its recordings line");
            // A midnight alone: the row keeps its time while hidden and takes
            // its date on show.
            scroll.contentY = Math.max(0, scroll.contentHeight - scroll.height - 40);
            waitForRendering(screen);
            place = scroll.contentY;
            height = row.height;
            screen.visible = false;
            systemsStub.localDay = nextDay();
            wait(20);
            compare(findChildren(screen, "searchHistoryRow")[0], row, "the same row");
            compare(detail.fullText, shown, "hidden, the row keeps its time");
            compare(row.height, height, "and its height");
            compare(scroll.contentY, place, "and the reader's place");
            screen.visible = true;
            waitForRendering(screen);
            compare(detail.fullText, "2026-11-09 14:32:05 · RF only · ended\nTIII Small · Net 12 · Site 3 · SYS 1183", "shown, the date lands");
            scroll.contentY = 0;
            model.history = [];
            model.avoids = [];
        }
        // The menu names its detection with the found time, as its row; a
        // midnight while it is open dates it.
        function test_detection_menu_title_follows_the_day() {
            model.history = [historyHit({started: at(2026, 11, 9, 14, 32, 5)})];
            wait(0);
            findChild(loader.item, "searchHistoryActions").activate();
            var menu = findChild(loader.item, "searchDetectionMenu");
            verify(menu.visible);
            compare(menu.title, "462.5750 MHz · 14:32:05");
            compare(menu.accessibleName, "462.5750 MHz · 14:32:05");
            systemsStub.localDay = nextDay();
            compare(menu.title, "462.5750 MHz · 2026-11-09 14:32:05");
            menu.cancel();
            // The Now panel's More, for the live detection, by the same rule.
            model.status = Object.assign({}, model.status, {hit: Object.assign({}, model.status.hit, {started: at(2026, 11, 10, 8, 1, 2)})});
            findChild(loader.item, "searchMore").clicked();
            compare(menu.title, "463.5521 MHz · 08:01:02");
            menu.cancel();
            model.history = [];
        }
        // Two visits to one frequency are two rows, told apart by their times;
        // each menu acts on its own detection.
        function test_two_visits_on_one_frequency() {
            model.history = [historyHit({id: "44", started: at(2026, 11, 9, 15, 1, 10)}), historyHit({id: "40", started: at(2026, 11, 9, 14, 32, 5)})];
            wait(0);
            var buttons = findChildren(loader.item, "searchHistoryActions");
            compare(buttons.length, 2);
            compare(buttons[0].accessibleName, "Actions for 462.5750 MHz, 15:01:10");
            compare(buttons[1].accessibleName, "Actions for 462.5750 MHz, 14:32:05");
            buttons[1].activate();
            compare(findChild(loader.item, "searchDetectionMenu").title, "462.5750 MHz · 14:32:05");
            menuItem("searchActionSave").activate();
            compare(saved.count, 1);
            compare(saved.signalArguments[0][0].id, "40");
            compare(saved.signalArguments[0][0].session, "17");
            buttons[0].activate();
            compare(findChild(loader.item, "searchDetectionMenu").title, "462.5750 MHz · 15:01:10");
            menuItem("searchActionExplore").activate();
            compare(explored.signalArguments[0][0].id, "44");
            model.history = [];
        }
        // B8: Avoid this session is for the session running as the menu
        // opened. Once that one ends, fails or another takes its place while
        // the menu is open, the action is disabled, says why, and sends
        // nothing to any session.
        function test_avoid_this_session_keeps_its_session_data() {
            return [{tag: "replaced", change: function () { model.status = Object.assign({}, model.status, {session: "18"}); }},
                    {tag: "ended", change: function () { loader.item.sessionActive = false; }},
                    {tag: "failed", change: function () { model.status = Object.assign({}, model.status, {phase: 7, hit: {}}); }}];
        }
        function test_avoid_this_session_keeps_its_session(data) {
            // A detection an earlier search (16) found, listed while 17 runs:
            // the avoid is for the running search, not the detection's.
            model.history = [historyHit({session: "16", recordingKey: "16:40"})];
            wait(0);
            var more = findChild(loader.item, "searchHistoryActions");
            more.activate();
            menuItem("searchActionAvoidSession").activate();
            compare(model.actions.join(","), "session,17,462575000", "the session the menu opened in");
            model.actions = [];
            more.activate();
            var action = menuItem("searchActionAvoidSession");
            verify(action.enabled);
            data.change();
            verify(!action.enabled, "disabled once its session is gone");
            verify(menuTexts().indexOf("Avoid this session (disabled)") >= 0, JSON.stringify(menuTexts()));
            var menu = findChild(loader.item, "searchDetectionMenu");
            var index = menu.actions.findIndex(function (a) { return a.key === "avoidSession"; });
            compare(findChild(menu, "actionMenuDescription" + index).text, "That search session has ended.");
            verify(action.accessibleName.indexOf("That search session has ended.") > 0, action.accessibleName);
            menu.activate(index);
            compare(model.actions.join(","), "", "nothing sent to any session");
            verify(menu.visible, "the disabled action keeps the menu");
            menu.cancel();
            model.history = [];
        }
        // A date on a narrow phone at the largest text stays one value: it
        // never splits, and no line starts with a separator.
        function test_found_date_whole_at_360_large_text() {
            root.width = 360;
            Ui.Theme.fontScale = 1.6;
            model.history = [historyHit({started: at(2026, 11, 8, 9, 5, 7), confirmed: true, protocol: "P25 Phase 1", ended: false,
                site: "WACN BEE00 · SYS 1A3 · RFSS 1 · Site 2", access: "NAC 293", crypt: "ALG 84 · KID 0001"})];
            waitForRendering(loader.item);
            var row = findChild(loader.item, "searchHistoryRow");
            var detail = findChild(row, "searchHistoryDetail");
            verify(detail.text.indexOf("2026-11-08\u00a009:05:07\u00a0·") === 0, JSON.stringify(detail.text));
            verify(!/[^·\n] /.test(detail.text), "a value never splits across lines");
            verify(detail.contentWidth <= detail.width + 0.5, "nothing drawn past the line");
            // As Qt draws it: the whole time on the first line, and no later line starting with a separator.
            var drawn = renderedLineStarts(detail, detail.text, detail.width);
            compare(drawn.lines, detail.lineCount, "the mirror wraps as the row does");
            verify(drawn.texts[0].replace(/\u00a0/g, " ").indexOf("2026-11-08 09:05:07") === 0, JSON.stringify(drawn.texts));
            for (var k = 1; k < drawn.texts.length; ++k)
                verify(drawn.texts[k].trim().charAt(0) !== "·", "a drawn line starts with a separator: " + JSON.stringify(drawn.texts));
            var actions = findChild(row, "searchHistoryActions");
            verify(actions.mapToItem(row, 0, 0).x + actions.width <= row.width + 0.5, "the row's actions stay inside it");
            model.history = [];
        }
        // B11: the export's day is the local date, also at 00:00 UTC, which is the day before west of UTC.
        function test_export_name_is_the_local_date() {
            if (!testContext.localTimeZoneIs("America/Los_Angeles"))
                skip("this run is not in America/Los_Angeles (CTest starts it with TZ set; Windows reads no IANA name in TZ)");
            systemsStub.nowMs = Date.UTC(2026, 0, 1);
            var id = beginExport("detections");
            compare(testContext.lastDocumentRequest().name, "Signal search detections 2025-12-31.csv");
            testContext.answerDocumentCancelled(id);
        }
        function test_capture_menu() {
            model.history = [historyHit({matches: [{systemUid: "uid-a"}, {systemUid: "uid-b"}]})];
            wait(0);
            findChild(loader.item, "searchHistoryActions").activate();
            wait(100);
            grabImage(root);
            findChild(loader.item, "searchDetectionMenu").cancel();
            model.history = [historyHit({matches: [{systemUid: "uid-a"}], knownLabel: "Warehouse · 462.5750 MHz", confirmed: true, protocol: "P25 Phase 1", site: "WACN BEE00 · SYS 1A3 · RFSS 1 · Site 2", access: "NAC 293"}),
                historyHit({id: "39", confirmed: true, protocol: "DMR", ended: false, site: "TIII Small · Net 12 · Site 3 · SYS 1183", access: "CC 1", slot: 2, crypt: "Encrypted"}),
                historyHit({id: "38", recordingKey: "17:38", tone: "DCS D023N / D047I"})];
            var scroll = findChild(loader.item, "searchScroll");
            for (var scale of [1.0, 1.6]) {
                Ui.Theme.fontScale = scale;
                wait(50);
                var row = findChild(loader.item, "searchHistoryRow");
                scroll.contentY = Math.max(0, row.mapToItem(scroll.contentItem, 0, 0).y - 60);
                wait(100);
                grabImage(root);
                var actions = findChild(loader.item, "searchHistoryActions");
                verify(actions.mapToItem(row, 0, 0).x + actions.width <= row.width + 0.5, "the row's actions stay inside it");
            }
            scroll.contentY = 0;
            model.history = [];
        }
        function test_capture_searching() {
            model.status = Object.assign({}, model.status, {phase: 1, rangeName: "Local UHF", rangeStartMHz: "450.000", rangeEndMHz: "460.000", hit: {}});
            wait(100);
            grabImage(root);
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
            model.history = [];
        }
        function test_capture_large_text() {
            Ui.Theme.fontScale = 1.6;
            // An encrypted DMR call on slot 2 with its Tier III site.
            model.status = Object.assign({}, model.status, {livePowerValid: true, livePower: -47.2, liveSeq: 1,
                hit: Object.assign({}, model.status.hit, {knownNames: ["Warehouse", "Dock security", "Yard"], confirmed: true, unresolved: false, protocol: "DMR",
                    site: "TIII Small · Net 12 · Site 3 · SYS 1183", access: "CC 1", slot: 2, target: 100, source: 2001, crypt: "ALG 21 · KID 0001"})});
            wait(100);
            grabImage(root);
            var button = findChild(loader.item, "searchStartPause");
            verify(button.y + button.height <= loader.item.height);
            // A narrow phone, with long valid bounds and counts.
            root.width = 360;
            model.status = Object.assign({}, model.status, {pass: 12, channel: 99998, count: 99999, rangeName: "Regional interoperability",
                rangeStartMHz: "118.008333", rangeEndMHz: "136.991667"});
            wait(100);
            grabImage(root);
            verify(button.y + button.height <= loader.item.height);
        }
        function test_capture_editor() {
            loader.item.sessionActive = false;
            var editor = findChild(loader.item, "signalSearchRangeEditor");
            editor.openFor(0, model.defaultRange());
            verify(editor.visible);
            wait(100);
            grabImage(root);
        }
    }
}
