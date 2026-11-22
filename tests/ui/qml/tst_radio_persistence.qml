// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest

Item {
    id: testRoot
    width: 420
    height: 900
    property var app: null
    property var sheet: null

    TestCase {
        name: "RadioPersistence"
        when: windowShown

        function initTestCase() {
            testContext.useLifecycleHost(true, false, true);
            prefs.onboardingDone = true;
            prefs.notificationExplained = true;
            prefs.autoStartOnAttach = false;
            app = Qt.createQmlObject(testContext.androidMainQml(), testRoot, uiDir + "/Main.qml");
            verify(app !== null);
            sheet = findChild(app, "radioSheet");
            verify(sheet !== null);
        }

        function cleanupTestCase() {
            app.destroy();
            testContext.useLifecycleHost(false);
        }

        function init() {
            testContext.resetCommands();
            verify(savedSystems.add({name: "RTL-TCP", sourceType: "rtltcp", host: "127.0.0.1", port: 1234,
                freqMhz: "851.5", decodeFlag: "-f1", trunking: false, gainDb: 36, ppm: "-2", extraArgs: ""}));
            app.startSystem(0);
            compare(app.startError, "");
            compare(decoderHost.sessionState, 2);
            testContext.setMetric("optionsKnown", true);
            testContext.setMetric("radioInput", true);
            testContext.setMetric("scanRowActive", false);
            testContext.setMetric("modulation", 0);
            testContext.setMetric("decodeMode", commands.decodeModeForFlag("-f1"));
            testContext.setMetric("configuredSquelchAuto", false);
            testContext.setMetric("configuredSquelchNoise", false);
            testContext.setMetric("configuredSquelchLevelOff", true);
            testContext.setMetric("configuredSquelchOff", true);
            testContext.setMetric("configuredSquelchDb", 0);
            testContext.setMetric("configuredSquelchMarginDb", 10);
            testContext.setMetric("configuredSquelchLevelWhole", true);
            testContext.setMetric("configuredSquelchLevelWholeDb", 0);
            testContext.setMetric("squelchAudioInput", false);
            sheet.open();
        }

        // Issue #899: a level the configured default keeps, as the metrics model reads it.
        function setLevel(db, whole) {
            testContext.setMetric("configuredSquelchDb", db);
            testContext.setMetric("configuredSquelchLevelOff", db === 0);
            testContext.setMetric("configuredSquelchLevelWhole", whole);
            testContext.setMetric("configuredSquelchLevelWholeDb", whole ? Math.round(db) : 0);
        }

        function cleanup() {
            decoderHost.stop();
            while (savedSystems.count > 0)
                savedSystems.remove(0);
        }

        function test_restart_data() {
            return [{tag: "squelch", settings: {squelch: "-65"}},
                {tag: "off", settings: {squelch: "off"}},
                {tag: "auto", settings: {squelch: "auto+12"}},
                {tag: "noise", settings: {squelch: "noise+8"}},
                {tag: "modulation", settings: {modulation: 1}},
                {tag: "listening", settings: {decodeFlag: "-fA"}},
                {tag: "auto-decode", settings: {decodeFlag: ""}}];
        }

        function test_restart(data) {
            if (data.tag === "squelch" || data.tag === "off") {
                var db = data.tag === "off" ? 0 : -65;
                sheet.requestSquelchDb(db);
                compare(testContext.lastSquelchDb(), db);
                setLevel(db, true);
            } else if (data.tag === "auto" || data.tag === "noise") {
                var noise = data.tag === "noise";
                var margin = noise ? 8 : 12;
                sheet.requestSquelchDynamic(noise ? commands.squelchModeNoise : commands.squelchModeAuto, margin);
                testContext.setMetric("configuredSquelchMarginDb", margin);
                testContext.setMetric(noise ? "configuredSquelchNoise" : "configuredSquelchAuto", true);
            } else if (data.tag === "modulation") {
                findChild(sheet, "radioModulation").selected(1);
                compare(testContext.lastModulation(), 1);
                testContext.setMetric("modulation", 1);
            } else {
                var flag = data.tag === "auto-decode" ? "" : "-fA";
                findChild(sheet, data.tag === "auto-decode" ? "radioDecode_Auto" : "radioDecode_NFM").clicked();
                compare(testContext.lastDecodeMode(), commands.decodeModeForFlag(flag));
                testContext.setMetric("decodeMode", commands.decodeModeForFlag(flag));
            }
            tryVerify(function () { return Object.keys(savedSystems.get(0).radioSettings).length > 0; });
            compare(savedSystems.get(0).radioSettings, data.settings);
            decoderHost.stop();
            app.startSystem(0);
            compare(app.startError, "");
            compare(app.sessionSystem.radioSettings, data.settings);
            compare(app.sessionSystem.decodeFlag, data.settings.decodeFlag !== undefined ? data.settings.decodeFlag : "-f1");
            compare(app.sessionSystem.gainDb, 36);
            compare(app.sessionSystem.ppm, "-2");
            compare(app.sessionSystem.extraArgs, "");
        }

        function test_unconfirmed_or_refused_changes_do_not_save() {
            sheet.requestSquelchDb(-65);
            testContext.setMetric("squelchDb", -65); // an effective row value is not the configured default
            testContext.setMetric("squelchOff", false);
            wait(10);
            compare(savedSystems.get(0).radioSettings, {});
            commands.setRadioAccepted(false);
            findChild(sheet, "radioModulation").selected(1);
            testContext.setMetric("modulation", 1);
            wait(10);
            compare(savedSystems.get(0).radioSettings, {});
            decoderHost.stop();
            app.startSystem(0);
            setLevel(-65, true);
            wait(10);
            compare(savedSystems.get(0).radioSettings, {}); // pending requests cannot cross a session
        }

        function test_preset_replaces_previous_modulation_and_closing_sheet_keeps_pending() {
            findChild(sheet, "radioModulation").selected(1);
            testContext.setMetric("modulation", 1);
            compare(savedSystems.get(0).radioSettings.modulation, 1);
            findChild(sheet, "radioDecode_DMR").clicked();
            sheet.visible = false;
            testContext.setMetric("modulation", 2);
            testContext.setMetric("decodeMode", commands.decodeModeForFlag("-fs"));
            compare(savedSystems.get(0).radioSettings, {decodeFlag: "-fs"});
        }

        function test_selected_decoder_data() {
            return [{tag: "named-preset", flag: "-ft", chip: "P25", settings: {modulation: 1}},
                {tag: "auto-chip", flag: "", chip: "Auto", settings: {decodeFlag: ""}}];
        }

        function test_selected_decoder(data) {
            findChild(sheet, "radioModulation").selected(1);
            testContext.setMetric("modulation", 1);
            compare(savedSystems.get(0).radioSettings, {modulation: 1});
            testContext.setMetric("decodeMode", commands.decodeModeForFlag(data.flag));
            findChild(sheet, "radioDecode_" + data.chip).clicked();
            wait(10);
            compare(savedSystems.get(0).radioSettings, data.settings);
        }

        function test_return_to_current_decoder_replaces_pending_choice() {
            testContext.setMetric("decodeMode", commands.decodeModeForFlag("-ft"));
            findChild(sheet, "radioDecode_DMR").clicked();
            findChild(sheet, "radioDecode_P25").clicked();
            wait(10);
            compare(savedSystems.get(0).radioSettings, {decodeFlag: "-ft"});
        }

        function test_modulation_waits_for_pending_decoder() {
            testContext.setMetric("modulation", 1);
            findChild(sheet, "radioDecode_P25").clicked();
            findChild(sheet, "radioModulation").selected(1);
            wait(10); // the old snapshot already matches the later modulation request
            compare(savedSystems.get(0).radioSettings, {});
            testContext.setMetric("modulation", 0);
            testContext.setMetric("decodeMode", commands.decodeModeForFlag("-ft"));
            compare(savedSystems.get(0).radioSettings, {decodeFlag: "-ft"});
            testContext.setMetric("modulation", 1);
            compare(savedSystems.get(0).radioSettings, {decodeFlag: "-ft", modulation: 1});
        }

        function test_unsaved_and_scan_sessions_do_not_modify_saved_systems_data() {
            return [{tag: "explore"}, {tag: "scan"}, {tag: "search"}, {tag: "deleted"}];
        }

        function test_unsaved_and_scan_sessions_do_not_modify_saved_systems(data) {
            if (data.tag === "explore")
                app.exploring = true;
            else if (data.tag === "scan")
                app.sessionScanList = true;
            else if (data.tag === "search")
                app.sessionSystem = Object.assign({}, app.sessionSystem, {signalSearch: true});
            else
                savedSystems.remove(0);
            sheet.requestSquelchDb(-65);
            setLevel(-65, true);
            wait(10);
            if (data.tag === "deleted")
                compare(savedSystems.count, 0);
            else
                compare(savedSystems.get(0).radioSettings, {});
        }

        function test_scan_row_edits_are_temporary() {
            testContext.setMetric("scanRowActive", true);
            testContext.setMetric("scanRowEditable", commands.scanRowFieldSquelch);
            testContext.setMetric("scanRowSynced", true);
            sheet.rowScopeChosen = true;
            verify(sheet.rowSquelch);
            sheet.requestSquelchDb(-65);
            setLevel(-65, true);
            wait(10);
            compare(savedSystems.get(0).radioSettings, {});
        }

        // Issue #899 (B15): a level the app's controls cannot make -- Advanced `--squelch -65.5` -- is not saved, and
        // the sheet says why. A step from it makes a whole dB, which is saved.
        function test_level_set_outside_the_app_is_not_saved() {
            setLevel(-65.5, false);
            testContext.setMetric("configuredSquelchMarginDb", 12);
            testContext.setMetric("configuredSquelchAuto", true);
            tryVerify(function () { return metrics.configuredSquelchAuto === true });
            sheet.chooseSquelchMode(0);
            compare(testContext.restoreSquelchCalls(), 1);
            testContext.setMetric("configuredSquelchAuto", false);
            tryCompare(sheet, "squelchSaveError", "This level was set outside the app, so it is not saved.");
            compare(savedSystems.get(0).radioSettings, {}, "nothing rounded into the saved system");
            var shown = findChild(sheet, "radioSquelchSaveError");
            verify(shown.visible, "the squelch controls show it");
            compare(shown.text, sheet.squelchSaveError);
            // A mode change clears it: Noise is saved, and the reason is gone.
            sheet.requestSquelchDynamic(commands.squelchModeNoise, 12);
            compare(sheet.squelchSaveError, "");
            verify(!shown.visible);
            testContext.setMetric("configuredSquelchNoise", true);
            tryCompare(savedSystems.get(0), "radioSettings", {squelch: "noise+12"});
            testContext.setMetric("configuredSquelchNoise", false);
            verify(savedSystems.clearRadioSetting(savedSystems.get(0).uid, "squelch"));
            sheet.forgetRequests();
            testContext.setMetric("squelchDb", -65.5);
            testContext.setMetric("squelchOff", false);
            tryVerify(function () { return metrics.squelchDb === -65.5 });
            findChild(sheet, "radioSquelchUp").clicked();
            compare(testContext.lastSquelchDb(), -60);
            setLevel(-60, true);
            tryCompare(savedSystems.get(0), "radioSettings", {squelch: "-60"});
            compare(sheet.squelchSaveError, "");
        }

        // The level kept beneath Auto comes back and is saved when the app's controls make it.
        function test_restored_whole_level_is_saved() {
            setLevel(-60, true);
            testContext.setMetric("configuredSquelchAuto", true);
            tryVerify(function () { return metrics.configuredSquelchAuto === true });
            sheet.chooseSquelchMode(0);
            testContext.setMetric("configuredSquelchAuto", false);
            tryCompare(savedSystems.get(0), "radioSettings", {squelch: "-60"});
            // A refused restore saves nothing and the reading goes back to the engine's at once.
            verify(savedSystems.clearRadioSetting(savedSystems.get(0).uid, "squelch"));
            sheet.forgetRequests();
            testContext.setMetric("configuredSquelchNoise", true);
            tryVerify(function () { return metrics.configuredSquelchNoise === true });
            commands.setRadioAccepted(false);
            sheet.chooseSquelchMode(0);
            compare(findChild(sheet, "radioSquelchMode").currentIndex, 2, "still the engine's Noise");
            testContext.setMetric("configuredSquelchNoise", false);
            wait(10);
            compare(savedSystems.get(0).radioSettings, {});
        }

        // A stepped level confirms only on the same whole dB: -60 asked, -60.08 configured is not it.
        function test_level_confirms_exactly() {
            sheet.requestSquelchDb(-60);
            setLevel(-60.08, false);
            wait(10);
            compare(savedSystems.get(0).radioSettings, {});
            compare(sheet.squelchSaveError, "", "still waiting, no refusal");
            setLevel(-60, true);
            tryCompare(savedSystems.get(0), "radioSettings", {squelch: "-60"});
        }

        // Issue #899 (B11): Clear saved squelch takes the system's own squelch out for the next start, and an older
        // squelch request still waiting cannot put it back.
        function test_clear_saved_squelch() {
            decoderHost.stop();
            verify(savedSystems.updateRadioSettings(savedSystems.get(0).uid, {squelch: "noise+17"}));
            app.startSystem(0);
            compare(app.startError, "");
            sheet.open();
            var clear = findChild(sheet, "radioSquelchClearSaved");
            verify(clear.visible);
            sheet.requestSquelchDynamic(commands.squelchModeNoise, 12);
            clear.clicked();
            compare(savedSystems.get(0).radioSettings, {});
            verify(app.sessionSystem.radioSettings.squelch === undefined, "the session's copy follows");
            verify(!clear.visible);
            var note = findChild(sheet, "radioSquelchClearedNote");
            verify(note.visible);
            compare(note.text, "Saved squelch cleared. The next start runs without a saved squelch.",
                "a P25 system takes no app default");
            testContext.setMetric("configuredSquelchMarginDb", 12);
            testContext.setMetric("configuredSquelchNoise", true);
            wait(10);
            compare(savedSystems.get(0).radioSettings, {}, "the older request does not put it back");
            // A newer edit saves again.
            sheet.forgetRequests();
            sheet.requestSquelchDynamic(commands.squelchModeNoise, 13);
            verify(!note.visible);
            testContext.setMetric("configuredSquelchMarginDb", 13);
            tryCompare(savedSystems.get(0), "radioSettings", {squelch: "noise+13"});
        }

        function test_clear_failure_is_shown() {
            decoderHost.stop();
            verify(savedSystems.updateRadioSettings(savedSystems.get(0).uid, {squelch: "auto+12"}));
            app.startSystem(0);
            sheet.open();
            var uid = savedSystems.get(0).uid;
            savedSystems.remove(0); // the system went away under the session: nothing to clear
            findChild(sheet, "radioSquelchClearSaved").clicked();
            compare(sheet.squelchSaveError, "Could not clear the saved squelch. Check available storage and try again.");
            verify(!findChild(sheet, "radioSquelchClearedNote").visible);
            verify(uid.length > 0);
        }

        // Issue #899 (B7): the audio-input Squelch sheet saves a default-scope change to the session's saved system;
        // "This channel" edits stay with the session, and Clear works there too.
        function test_squelch_sheet_saves_to_its_system() {
            decoderHost.stop();
            while (savedSystems.count > 0)
                savedSystems.remove(0);
            verify(savedSystems.add({name: "Bench", sourceType: "udp", port: 7355, freqMhz: "851.5", decodeFlag: "-fA",
                trunking: false, extraArgs: ""}));
            app.startSystem(0);
            compare(app.startError, "");
            testContext.setMetric("radioInput", false);
            testContext.setMetric("squelchAudioInput", true);
            var squelch = findChild(app, "squelchSheet");
            verify(squelch !== null);
            verify(squelch.savesToSystem);
            squelch.openEditor();
            compare(findChild(squelch, "squelchSaveNote").text, "Squelch changes here are saved to this system.");
            squelch.requestSquelchDb(-50);
            setLevel(-50, true);
            tryCompare(savedSystems.get(0), "radioSettings", {squelch: "-50"});
            compare(testContext.userSquelchNotes(), 1);
            squelch.requestSquelchNoise(9);
            testContext.setMetric("configuredSquelchMarginDb", 9);
            testContext.setMetric("configuredSquelchNoise", true);
            tryCompare(savedSystems.get(0), "radioSettings", {squelch: "noise+9"});
            // A refused restore of the level beneath saves nothing.
            commands.setRadioAccepted(false);
            squelch.chooseSquelchMode(0);
            testContext.setMetric("configuredSquelchNoise", false);
            wait(10);
            compare(savedSystems.get(0).radioSettings, {squelch: "noise+9"});
            compare(squelch.pendingSquelch.toString(), "NaN", "the reading is the engine's again");
            commands.setRadioAccepted(true);
            // "This channel" edits are the session's.
            testContext.setScanRowContext({"active": true, "scanner": 1, "session": 7, "row": 2, "mode": 9,
                "target": "fire", "label": "fire", "key": "1:7:2"});
            testContext.setMetric("scanRowKey", "1:7:2");
            testContext.setMetric("scanRowEditable", commands.scanRowFieldSquelch);
            testContext.setMetric("scanRowSynced", true);
            testContext.setMetric("scanRowActive", true);
            squelch.chooseScope(true);
            verify(squelch.rowSquelch);
            squelch.requestSquelchDb(-40);
            setLevel(-40, true);
            wait(10);
            compare(savedSystems.get(0).radioSettings, {squelch: "noise+9"});
            squelch.chooseScope(false);
            testContext.setMetric("scanRowActive", false);
            // Clear.
            var clear = findChild(squelch, "squelchClearSaved");
            tryCompare(clear, "visible", true);
            clear.clicked();
            compare(savedSystems.get(0).radioSettings, {});
            verify(findChild(squelch, "squelchClearedNote").visible);
            compare(findChild(squelch, "squelchClearedNote").text,
                "Saved squelch cleared. The next start runs without a saved squelch.");
            compare(clear.text, "Remove saved squelch");
            compare(clear.Accessible.name, "Remove Saved Squelch From This System");
            squelch.visible = false;
        }

        // An NFM system's next start takes the app default once its own squelch is cleared, and the note says so.
        function test_clear_on_an_analog_system_names_the_default() {
            decoderHost.stop();
            verify(savedSystems.updateRadioSettings(savedSystems.get(0).uid, {squelch: "auto+12", decodeFlag: "-fA"}));
            app.startSystem(0);
            compare(app.startError, "");
            sheet.open();
            findChild(sheet, "radioSquelchClearSaved").clicked();
            compare(savedSystems.get(0).radioSettings, {decodeFlag: "-fA"});
            compare(findChild(sheet, "radioSquelchClearedNote").text,
                "Saved squelch cleared. The next start uses the default from Settings.");
            compare(sheet.announced, "Saved squelch cleared. The next start uses the default from Settings.");
        }
    }
}
