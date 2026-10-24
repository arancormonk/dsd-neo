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
            sheet.open();
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
                testContext.setMetric("configuredSquelchDb", db);
                testContext.setMetric("configuredSquelchLevelOff", data.tag === "off");
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
            testContext.setMetric("configuredSquelchDb", -65);
            testContext.setMetric("configuredSquelchLevelOff", false);
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
            testContext.setMetric("configuredSquelchDb", -65);
            testContext.setMetric("configuredSquelchLevelOff", false);
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
            testContext.setMetric("configuredSquelchDb", -65);
            testContext.setMetric("configuredSquelchLevelOff", false);
            wait(10);
            compare(savedSystems.get(0).radioSettings, {});
        }
    }
}
