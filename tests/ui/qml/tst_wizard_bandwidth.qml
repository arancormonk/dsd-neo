// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest

// The system editor's bandwidth (B9, D6): App default or one of the eight
// widths every radio runs. App default is saved as -1 and runs 72 kHz for
// TETRA (issue #803), else the app default; opening or saving never turns it
// into a number. A stored width no radio runs (an older build took any
// number) shows as it is, with what it runs at, and is saved unchanged until
// another width is picked.
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

        name: "WizardBandwidth"
        when: windowShown

        readonly property var wizard: wizardLoader.item
        // savedSystems is the production model on shared test storage: a row a
        // case adds has to go again whatever happens (see tst_wizard_trunking).
        property var savedRows: []

        function init() {
            verify(tc.wizard !== null, "WizardScreen.qml failed to load");
            tc.wizard.openForAdd(false);
        }

        function cleanup() {
            var rows = tc.savedRows.slice().sort(function (a, b) {
                return b - a;
            });
            for (var row of rows)
                savedSystems.remove(row);
            tc.savedRows = [];
        }

        function control(name) {
            var found = findChild(tc.wizard, name);
            verify(found !== null, name);
            return found;
        }
        function combo() {
            return control("systemBandwidthField");
        }
        function note() {
            return control("systemBandwidthNote");
        }
        function meaning() {
            return control("systemBandwidthMeaning");
        }
        // A pick as the keyboard makes it: the list's own step, which reports
        // the choice (activated) as a tap on a row does.
        function pick(label) {
            var box = combo();
            var index = box.model.indexOf(label);
            verify(index >= 0, label + " offered in " + JSON.stringify(box.model));
            box.forceActiveFocus();
            while (box.currentIndex < index)
                box.incrementCurrentIndex();
            while (box.currentIndex > index)
                box.decrementCurrentIndex();
            compare(box.currentIndex, index);
        }
        function openAdvanced() {
            tc.wizard.step = 1;
            tc.wizard.advancedOpen = true;
            waitForRendering(tc.wizard);
        }
        function addSystem(decodeFlag, bandwidthKhz) {
            savedSystems.add({
                "name": "Bandwidth test", "sourceType": "usb", "host": "", "port": 0,
                "freqMhz": "851.375", "decodeFlag": decodeFlag, "trunking": false,
                "gainDb": -1, "ppm": "", "bandwidthKhz": bandwidthKhz, "biasTee": -1,
                "extraArgs": "", "filePath": "", "chanCsvPath": "",
                "groupCsvPath": "", "keyCsvPath": "", "keyCsvHex": false
            });
            var row = savedSystems.count - 1;
            tc.savedRows = tc.savedRows.concat([row]);
            return row;
        }
        // Saves the draft as it stands; returns the saved row's map, or null
        // when commit() saved nothing.
        function save() {
            var row = -1;
            var onSaved = function (saved) {
                row = saved;
            };
            if (tc.wizard.nameText.length === 0)
                tc.wizard.nameText = "Bandwidth test";
            tc.wizard.saved.connect(onSaved);
            tc.wizard.commit();
            tc.wizard.saved.disconnect(onSaved);
            if (row < 0)
                return null;
            if (tc.savedRows.indexOf(row) < 0)
                tc.savedRows = tc.savedRows.concat([row]);
            return savedSystems.get(row);
        }

        // Folding Advanced hides the PPM field being typed in: the word is
        // committed first, not dropped with the focus.
        function test_folding_advanced_keeps_a_composing_ppm_word() {
            openAdvanced();
            var input = control("systemPpmField").input;
            input.forceActiveFocus();
            keyClick("1");
            verify(testContext.composeText("2"));
            control("wizardAdvancedHeader").Accessible.pressAction();
            verify(!tc.wizard.advancedOpen);
            compare(tc.wizard.ppmText, "12");
        }

        // D6: App default and the eight widths, nothing to type.
        function test_offers_app_default_and_eight_widths() {
            openAdvanced();
            compare(JSON.stringify(combo().model), JSON.stringify(["App default", "4 kHz", "6 kHz", "8 kHz", "12 kHz", "16 kHz", "24 kHz", "48 kHz", "72 kHz"]));
            compare(combo().displayText, "App default");
            compare(combo().input, undefined, "the bandwidth is a choice, not a text field");
            compare(note().text, "App default · " + sessionArgs.effectiveBandwidthKhz("", "usb", -1) + " kHz");
            pick("24 kHz");
            compare(tc.wizard.bwText, "24");
            verify(!note().visible, "a width the radio runs needs no note");
            verify(tc.wizard.hasUnsavedDraft(), "a pick is a change");
            compare(save().bandwidthKhz, 24);
        }

        // Issue #899 (D5): the line under the choice says what the bandwidth
        // is and where the NFM channel width is set, under App default's note
        // and a picked width alike, wrapped and read as its own words.
        function test_bandwidth_meaning_under_the_choice() {
            openAdvanced();
            var line = meaning();
            verify(line.visible);
            compare(line.text, "The slice of spectrum the decoder processes. The NFM channel width is set in the Radio sheet.");
            compare(line.wrapMode, Text.Wrap);
            verify(line.lineCount > 1, "the line wraps at a phone's width");
            compare(line.width, combo().width);
            verify(line.contentWidth <= line.width, "the line runs past the editor");
            compare(line.Accessible.name, line.text);
            compare(line.Accessible.role, Accessible.StaticText);
            verify(note().visible);
            verify(note().y > combo().y + combo().height, "App default's note is not under the choice");
            verify(line.y > note().y + note().height, "the line is not under App default's note");
            pick("24 kHz");
            verify(!note().visible);
            verify(line.visible);
            verify(line.y > combo().y + combo().height, "the line is not under the choice");
        }

        // Issue #803: App default runs TETRA at 72 kHz and says so, and stays
        // App default (-1) through a save and an edit.
        function test_tetra_inherit_stays_and_shows_72() {
            openAdvanced();
            tc.wizard.pickDecodeFlag("-fT");
            compare(combo().displayText, "App default");
            compare(note().text, "App default · 72 kHz");
            compare(tc.wizard.bwText, "");
            verify(tc.wizard.stepValid(), "TETRA on App default is held");
            var saved = save();
            verify(saved !== null);
            compare(saved.bandwidthKhz, -1, "App default was saved as a number");
            tc.wizard.openForEdit(tc.savedRows[0]);
            tc.wizard.advancedOpen = true;
            compare(combo().displayText, "App default");
            compare(note().text, "App default · 72 kHz");
            compare(tc.wizard.draftFingerprint(), tc.wizard.initialDraft, "the edit opens clean");
            tc.wizard.step = 2;
            compare(save().bandwidthKhz, -1, "an edit saved App default as a number");
        }

        // B9: 25 runs at 48 on an RTL-SDR and at 24 on an Airspy; shown as
        // it is, saved unchanged, until another width is picked.
        function test_odd_stored_width_shown_not_rewritten() {
            var row = addSystem("-fs", 25);
            tc.wizard.openForEdit(row);
            openAdvanced();
            compare(combo().displayText, "25 kHz");
            compare(combo().model.length, 10, "App default, the eight widths and the stored one");
            compare(note().text, "Runs at 48 kHz on an RTL-SDR, 24 kHz on an Airspy.");
            compare(note().Accessible.name, note().text);
            compare(tc.wizard.draftFingerprint(), tc.wizard.initialDraft, "opening rewrote the width");
            tc.wizard.step = 2;
            compare(save().bandwidthKhz, 25, "the stored width was rewritten on save");
            tc.wizard.openForEdit(row);
            openAdvanced();
            pick("16 kHz");
            verify(!note().visible);
            compare(combo().model.length, 10, "the list does not change under the reader");
            pick("25 kHz");
            compare(tc.wizard.bwText, "25", "the stored width can be picked back");
            pick("16 kHz");
            tc.wizard.step = 2;
            compare(save().bandwidthKhz, 16);
        }

        // Another system opened after a pick away from an odd width offers
        // only its own.
        function test_reopen_offers_only_its_own_width() {
            var odd = addSystem("-fs", 25);
            var plain = addSystem("-fs", 16);
            tc.wizard.openForEdit(odd);
            openAdvanced();
            pick("16 kHz");
            tc.wizard.openForEdit(plain);
            compare(combo().displayText, "16 kHz");
            compare(combo().model.indexOf("25 kHz"), -1, "another system's width is offered here");
            tc.wizard.openForAdd(false);
            compare(combo().displayText, "App default");
            compare(combo().model.length, 9);
        }
    }
}
