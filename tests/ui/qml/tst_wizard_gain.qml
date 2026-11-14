// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest

// The system editor's tuner gain and PPM correction. Gain is App default,
// Automatic or a set 1-49 dB (saved as -1, 0 or the dB), and every way the
// editor opens shows the mode the system has. A set gain the scan list would
// refuse (50 and up), or a PPM correction outside what the engine runs
// (-200..200), holds the step and the save. Both are an RTL tuner's: on an
// Airspy or a file source they are hidden and never hold anything.
Item {
    id: root

    width: 360
    height: 900

    Loader {
        id: wizardLoader

        anchors.fill: parent
        source: uiDir + "/WizardScreen.qml"
    }

    TestCase {
        id: tc

        name: "WizardGain"
        when: windowShown

        readonly property var wizard: wizardLoader.item
        // savedSystems is the production model on shared test storage: a row a
        // case adds has to go again whatever happens (see tst_wizard_trunking).
        property int savedRow: -1

        function init() {
            verify(tc.wizard !== null, "WizardScreen.qml failed to load");
            tc.wizard.openForAdd(false);
        }

        function cleanup() {
            if (tc.savedRow >= 0) {
                savedSystems.remove(tc.savedRow);
                tc.savedRow = -1;
            }
            testContext.setPrefs("gainDb", 30);
        }

        function control(name) {
            var found = findChild(tc.wizard, name);
            verify(found !== null, name);
            return found;
        }
        function segments(strip) {
            var found = [];
            (function walk(node) {
                for (var child of node.children) {
                    if (child.modelData !== undefined && child.active !== undefined)
                        found.push(child);
                    else
                        walk(child);
                }
            })(strip);
            return found;
        }
        // Chosen as a tap chooses it.
        function choose(label) {
            var strip = control("systemGainMode");
            var index = strip.model.indexOf(label);
            verify(index >= 0, label);
            segments(strip)[index].choose();
        }
        function shownMode() {
            var strip = control("systemGainMode");
            return strip.model[strip.currentIndex];
        }
        function type(text) {
            var input = control("systemGainField").input;
            input.forceActiveFocus();
            input.selectAll();
            keyClick(Qt.Key_Backspace);
            for (var i = 0; i < text.length; ++i)
                keyClick(text.charAt(i));
            compare(control("systemGainField").text, text);
        }
        function openAdvanced() {
            tc.wizard.step = 1;
            tc.wizard.advancedOpen = true;
            waitForRendering(tc.wizard);
        }
        function visibleText(text) {
            var found = null;
            (function walk(node) {
                for (var child of node.children) {
                    if (found)
                        return;
                    if (child.visible && child.text === text && child.input === undefined)
                        found = child;
                    else if (child.visible)
                        walk(child);
                }
            })(tc.wizard);
            return found;
        }
        function addSystem(gainDb, ppm) {
            savedSystems.add({
                "name": "Gain test", "sourceType": "usb", "host": "", "port": 0,
                "freqMhz": "851.375", "decodeFlag": "-fs", "trunking": false,
                "gainDb": gainDb, "ppm": ppm, "bandwidthKhz": -1, "biasTee": -1,
                "extraArgs": "", "filePath": "", "chanCsvPath": "",
                "groupCsvPath": "", "keyCsvPath": "", "keyCsvHex": false
            });
            tc.savedRow = savedSystems.count - 1;
            return tc.savedRow;
        }
        function detection(sourceType, gainDb) {
            return {
                "sourceType": sourceType, "host": "", "port": 0, "freqMhz": "463.552100",
                "decodeFlag": "-fA", "gainDb": gainDb, "bandwidthKhz": 48, "trunking": false,
                "extraArgs": ""
            };
        }
        // Saves the draft as it stands; returns the saved row's map, or null
        // when commit() saved nothing.
        function save() {
            var row = -1;
            var onSaved = function (saved) {
                row = saved;
            };
            if (tc.wizard.nameText.length === 0)
                tc.wizard.nameText = "Gain test";
            tc.wizard.saved.connect(onSaved);
            tc.wizard.commit();
            tc.wizard.saved.disconnect(onSaved);
            if (row < 0)
                return null;
            if (tc.savedRow < 0)
                tc.savedRow = row;
            return savedSystems.get(row);
        }

        function test_loaders_show_the_mode_data() {
            return [
                {tag: "new system", open: "add", mode: "App default", text: ""},
                {tag: "system app default", open: "edit", gainDb: -1, mode: "App default", text: ""},
                {tag: "system automatic", open: "edit", gainDb: 0, mode: "Automatic", text: ""},
                {tag: "system set", open: "edit", gainDb: 36, mode: "Set", text: "36"},
                {tag: "detection automatic", open: "found", gainDb: 0, mode: "Automatic", text: ""},
                {tag: "detection set", open: "found", gainDb: 12, mode: "Set", text: "12"},
                {tag: "detection none", open: "found", gainDb: -1, mode: "App default", text: ""},
                {tag: "explore handoff", open: "explore", mode: "App default", text: ""}
            ];
        }

        function test_loaders_show_the_mode(data) {
            // From a draft in another mode, so each load has to set it.
            tc.wizard.gainMode = data.mode === "Set" ? 1 : 2;
            tc.wizard.gainText = "44";
            if (data.open === "add")
                tc.wizard.openForAdd(false);
            else if (data.open === "edit")
                tc.wizard.openForEdit(addSystem(data.gainDb, ""));
            else if (data.open === "found")
                tc.wizard.openForFound(detection("usb", data.gainDb), "463.552100", detection("usb", data.gainDb));
            else
                tc.wizard.openForFound({"sourceType": "usb", "gainDb": 21}, "851.375");
            openAdvanced();
            compare(JSON.stringify(control("systemGainMode").model), JSON.stringify(["App default", "Automatic", "Set"]));
            compare(shownMode(), data.mode);
            compare(tc.wizard.gainText, data.text);
            compare(control("systemGainField").visible, data.mode === "Set");
            compare(control("systemGainField").text, data.text);
            compare(tc.wizard.draftFingerprint(), tc.wizard.initialDraft, "the draft opens clean");
            choose(data.mode === "Automatic" ? "Set" : "Automatic");
            verify(tc.wizard.hasUnsavedDraft(), "a mode change is a change");
            choose(data.mode);
            compare(tc.wizard.draftFingerprint(), tc.wizard.initialDraft, "back to the opened mode");
        }

        function test_full_width_editor() {
            openAdvanced();
            var strip = control("systemGainMode");
            var row = control("systemBandwidthField").parent.parent;
            compare(strip.width, row.width, "the gain choice takes the whole row");
            choose("Set");
            compare(control("systemGainField").width, row.width);
            compare(control("systemGainField").label, "Gain");
            compare(control("systemGainField").unit, "dB");
            compare(strip.Accessible.name, "Gain");
        }

        // B1: 50 and up reads as a gain here but the scan list refuses it.
        function test_set_50_blocks_next_and_save() {
            openAdvanced();
            choose("Set");
            type("50");
            compare(control("systemGainField").error, "Enter a gain from 1 to 49 dB.");
            verify(!tc.wizard.gainValid());
            verify(!tc.wizard.stepValid(), "Set 50 holds the tune step");
            verify(!control("wizardContinue").inputValid);
            tc.wizard.step = 2;
            tc.wizard.nameText = "Gain test";
            verify(!tc.wizard.stepValid(), "and the name step");
            compare(save(), null, "a gain of 50 dB was saved");
            tc.wizard.step = 1;
            type("49");
            verify(tc.wizard.stepValid());
            compare(save().gainDb, 49);
        }

        function test_empty_set_blocks() {
            openAdvanced();
            choose("Set");
            compare(control("systemGainField").text, "");
            compare(control("systemGainField").error, "Enter a gain from 1 to 49 dB.");
            verify(!tc.wizard.stepValid(), "Set with no gain is not App default");
            compare(save(), null);
        }

        function test_ppm_range_data() {
            return [
                {tag: "empty", ppm: "", valid: true},
                {tag: "zero", ppm: "0", valid: true},
                {tag: "lowest", ppm: "-200", valid: true},
                {tag: "highest", ppm: "200", valid: true},
                {tag: "above", ppm: "201", valid: false},
                {tag: "below", ppm: "-201", valid: false},
                {tag: "old ceiling", ppm: "999", valid: false},
                {tag: "sign only", ppm: "-", valid: false}
            ];
        }

        // B9: the engine clamps PPM to -200..200, so a wider value ran
        // something other than what was saved.
        function test_ppm_range(data) {
            openAdvanced();
            tc.wizard.ppmText = data.ppm;
            compare(tc.wizard.ppmValid(), data.valid);
            compare(control("systemPpmField").error, data.valid ? "" : "Enter a correction from -200 to 200 ppm.");
            compare(tc.wizard.stepValid(), data.valid);
            tc.wizard.step = 2;
            tc.wizard.nameText = "PPM test";
            compare(tc.wizard.stepValid(), data.valid);
            var saved = save();
            if (data.valid)
                compare(saved.ppm, data.ppm);
            else
                compare(saved, null, "PPM " + data.ppm + " was saved");
        }

        // Review focus 4: hidden RTL fields never block Save.
        function test_hiddenRtlFieldsNeverBlock_data() {
            return [
                {tag: "airspy", source: "airspy"},
                {tag: "file", source: "file"}
            ];
        }

        function test_hiddenRtlFieldsNeverBlock(data) {
            openAdvanced();
            choose("Set");
            type("50");
            tc.wizard.ppmText = "201";
            verify(!tc.wizard.stepValid());
            tc.wizard.sourceType = data.source;
            if (data.source === "file")
                tc.wizard.fileText = "capture.bin";
            verify(!control("systemGainField").visible && !control("systemPpmField").visible);
            verify(tc.wizard.gainValid() && tc.wizard.ppmValid(), "a hidden field is no answer");
            verify(tc.wizard.stepValid(), "the tune step goes on");
            tc.wizard.step = 2;
            var saved = save();
            verify(saved !== null, "the hidden fields blocked the save");
            compare(saved.sourceType, data.source);
            // A refused value is never saved, hidden or not.
            compare(saved.gainDb, -1);
            compare(saved.ppm, "");
        }

        function test_hidden_valid_gain_is_kept() {
            openAdvanced();
            choose("Set");
            type("22");
            tc.wizard.ppmText = "-5";
            tc.wizard.sourceType = "airspy";
            var saved = save();
            compare(saved.gainDb, 22);
            compare(saved.ppm, "-5");
        }

        // Review focus 3 of the design: an import finishing after a gain edit
        // keeps the edit, and the draft stays dirty against the opened one.
        function test_radio_reference_keeps_the_gain_draft() {
            tc.wizard.openForEdit(addSystem(36, ""));
            var opened = tc.wizard.initialDraft;
            openAdvanced();
            choose("Automatic");
            tc.wizard.applyRadioReference({"name": "Imported", "freqMhz": "853.0625", "decodeFlag": "-fs", "trunking": false});
            compare(shownMode(), "Automatic");
            compare(tc.wizard.gainMode, 1);
            compare(tc.wizard.initialDraft, opened, "the import is no new baseline");
            verify(tc.wizard.hasUnsavedDraft());
            choose("Set");
            type("27");
            tc.wizard.applyRadioReference({"name": "Imported", "freqMhz": "853.0625", "decodeFlag": "-fs", "trunking": false});
            compare(shownMode(), "Set");
            compare(control("systemGainField").text, "27");
            verify(tc.wizard.hasUnsavedDraft());
        }

        function test_save_writes_each_mode_data() {
            return [
                {tag: "app default", mode: "App default", gainDb: -1},
                {tag: "automatic", mode: "Automatic", gainDb: 0},
                {tag: "set", mode: "Set", text: "23", gainDb: 23},
                {tag: "set lowest", mode: "Set", text: "1", gainDb: 1}
            ];
        }

        function test_save_writes_each_mode(data) {
            tc.wizard.openForEdit(addSystem(36, ""));
            openAdvanced();
            if (data.mode === "Set")
                choose("Automatic");
            choose(data.mode);
            if (data.text)
                type(data.text);
            tc.wizard.step = 2;
            compare(save().gainDb, data.gainDb);
        }

        // Typed under Set and then left: App default and Automatic save their
        // own value, not the typing.
        function test_leaving_set_drops_the_typing_from_the_save() {
            openAdvanced();
            choose("Set");
            type("50");
            choose("Automatic");
            verify(tc.wizard.stepValid(), "an invalid gain under a mode not shown holds nothing");
            compare(save().gainDb, 0);
        }

        // B3: App default names what it runs, not a bare number.
        function test_app_default_note() {
            testContext.setPrefs("gainDb", 0);
            openAdvanced();
            compare(shownMode(), "App default");
            verify(visibleText("App default · Automatic") !== null, "the App default note names Automatic");
            testContext.setPrefs("gainDb", 30);
            verify(visibleText("App default · 30 dB") !== null);
            choose("Automatic");
            compare(visibleText("App default · 30 dB"), null, "the note is App default's");
        }

        // A saved value this editor refuses (an older build took PPM up to
        // +-999) shows its error at once rather than holding Continue behind a
        // closed pane.
        function test_refused_saved_value_opens_advanced() {
            tc.wizard.openForEdit(addSystem(-1, "500"));
            verify(tc.wizard.advancedOpen, "the refused PPM is shown");
            compare(tc.wizard.draftFingerprint(), tc.wizard.initialDraft);
            verify(savedSystems.update(tc.savedRow, {"ppm": "-3"}));
            tc.wizard.openForEdit(tc.savedRow);
            verify(!tc.wizard.advancedOpen, "a valid system opens folded");
        }
    }
}
