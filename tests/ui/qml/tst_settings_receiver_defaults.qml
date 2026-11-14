// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest

// Settings > Radio defaults: the PPM correction and the bandwidth run what
// they show (B9). The engine holds a PPM to -200..200, so the row refuses
// anything else rather than saving a value that runs as another. The
// bandwidth is a choice of the eight widths every radio runs; a stored width
// that is not one of them (an older build took any number) shows as it is,
// with what it runs at, and stays until another width is picked.
Item {
    width: 360
    height: 900

    Loader {
        id: screenLoader
        anchors.fill: parent
    }

    TestCase {
        name: "SettingsReceiverDefaults"
        when: windowShown

        function init() {
            testContext.useLifecycleHost(true);
            prefs.ppm = 0;
            prefs.bandwidthKhz = 48;
            load();
        }

        function cleanup() {
            verify(testContext.composeText(""));
            screenLoader.source = "";
            prefs.ppm = 0;
            prefs.bandwidthKhz = 48;
            testContext.useLifecycleHost(false);
        }

        function load() {
            screenLoader.source = "";
            screenLoader.source = uiDir + "/SettingsScreen.qml";
            verify(screenLoader.item !== null);
            screenLoader.item.advancedOpen = true;
            waitForRendering(screenLoader.item);
        }

        function control(name) {
            var found = findChild(screenLoader.item, name);
            verify(found !== null, name);
            return found;
        }
        function typePpm(text) {
            var input = control("settingsPpm").input;
            input.forceActiveFocus();
            input.selectAll();
            keyClick(Qt.Key_Backspace);
            for (var i = 0; i < text.length; ++i)
                keyClick(text.charAt(i));
            compare(control("settingsPpm").text, text);
            keyClick(Qt.Key_Return);
        }
        // A pick as the keyboard makes it: the list's own step, which reports
        // the choice (activated) as a tap on a row does.
        function pick(combo, label) {
            var index = combo.model.indexOf(label);
            verify(index >= 0, label + " offered in " + JSON.stringify(combo.model));
            combo.forceActiveFocus();
            while (combo.currentIndex < index)
                combo.incrementCurrentIndex();
            while (combo.currentIndex > index)
                combo.decrementCurrentIndex();
            compare(combo.currentIndex, index);
        }
        function note() {
            return control("settingsBandwidthNote");
        }

        // B9: the engine holds PPM to +-200; a value outside shows its error
        // and is never written (an older build saved any whole number).
        function test_settingsPpmOutOfRangeRefused_data() {
            return [{tag: "201", text: "201"}, {tag: "-201", text: "-201"}, {tag: "999", text: "999"}, {tag: "empty", text: ""}];
        }
        function test_settingsPpmOutOfRangeRefused(data) {
            typePpm("12");
            compare(prefs.ppm, 12);
            typePpm(data.text);
            compare(control("settingsPpm").error, "Enter a correction from -200 to 200 ppm.");
            compare(prefs.ppm, 12, "a refused correction was written");
        }

        function test_settingsPpmRangeEnds_data() {
            return [{tag: "200", text: "200", value: 200}, {tag: "-200", text: "-200", value: -200}, {tag: "0", text: "0", value: 0}];
        }
        function test_settingsPpmRangeEnds(data) {
            prefs.ppm = 7;
            typePpm(data.text);
            compare(control("settingsPpm").error, "");
            compare(prefs.ppm, data.value);
        }

        // The ppm row names itself and its unit.
        function test_settingsPpmLabel() {
            compare(control("settingsPpm").label, "PPM correction");
            compare(control("settingsPpm").text, "0");
        }

        // D6: the eight widths every radio runs, and nothing else to pick: no
        // 0, no negative, no width the radio would run as another.
        function test_settingsBandwidthOffersEightWidths() {
            var combo = control("settingsBandwidth");
            compare(JSON.stringify(combo.model), JSON.stringify(["4 kHz", "6 kHz", "8 kHz", "12 kHz", "16 kHz", "24 kHz", "48 kHz", "72 kHz"]));
            compare(combo.displayText, "48 kHz");
            compare(combo.Accessible.name, "Bandwidth");
            verify(!note().visible, "a width the radio runs needs no note");
            pick(combo, "72 kHz");
            compare(prefs.bandwidthKhz, 72);
            pick(combo, "4 kHz");
            compare(prefs.bandwidthKhz, 4);
        }

        // B9: 0 and negative bandwidths were accepted as text and silently
        // dropped; the choice offers neither, and no pick writes one.
        function test_settingsBandwidthZeroOrNegativeCannotBeEntered() {
            var combo = control("settingsBandwidth");
            verify(combo.input === undefined, "the bandwidth is a choice, not a text field");
            for (var i = 0; i < combo.count; ++i) {
                pick(combo, combo.model[i]);
                verify(prefs.bandwidthKhz > 0, combo.model[i] + " wrote " + prefs.bandwidthKhz);
                verify([4, 6, 8, 12, 16, 24, 48, 72].indexOf(prefs.bandwidthKhz) >= 0, "wrote " + prefs.bandwidthKhz);
            }
        }

        // B9: an odd stored width (25) shows as it is, with what it runs at on
        // each radio, and is not rewritten until another width is picked.
        function test_settingsOddBandwidthShown() {
            prefs.bandwidthKhz = 25;
            load();
            var combo = control("settingsBandwidth");
            compare(combo.displayText, "25 kHz");
            compare(combo.model[combo.currentIndex], "25 kHz");
            compare(combo.model.length, 9, "the eight widths and the stored one");
            verify(note().visible);
            compare(note().text, "Runs at 48 kHz on an RTL-SDR, 24 kHz on an Airspy.");
            compare(note().Accessible.name, note().text);
            compare(prefs.bandwidthKhz, 25, "opening Settings rewrote the stored width");
            pick(combo, "48 kHz");
            compare(prefs.bandwidthKhz, 48);
            verify(!note().visible);
            compare(combo.displayText, "48 kHz");
            compare(combo.model.length, 9, "the list does not change under the reader");
            pick(combo, "25 kHz");
            compare(prefs.bandwidthKhz, 25, "the stored width can be picked back");
            verify(note().visible);
        }

        // An odd width written from elsewhere (a restore) shows as it is too.
        function test_settingsOddBandwidthFromRestore() {
            var combo = control("settingsBandwidth");
            prefs.bandwidthKhz = 100;
            compare(combo.displayText, "100 kHz");
            compare(note().text, "Runs at 48 kHz on an RTL-SDR, 48 kHz on an Airspy.");
            prefs.bandwidthKhz = 16;
            compare(combo.displayText, "16 kHz");
            verify(!note().visible);
            compare(combo.model.length, 8);
        }
    }
}
