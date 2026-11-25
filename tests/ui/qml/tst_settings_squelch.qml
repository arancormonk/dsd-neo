// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest

// Issue #899: Settings > Radio defaults > Squelch, the squelch NFM and AM
// sessions start with. Off, Auto or Noise, and for the last two a margin of
// 3 to 30 dB stepped 1 dB at a time; only "off", "auto+N" or "noise+N" is ever
// written, through the real AppPrefs.
Item {
    width: 360
    height: 1400

    Loader {
        id: screenLoader
        anchors.fill: parent
    }

    TestCase {
        name: "SettingsSquelch"
        when: windowShown

        function init() {
            testContext.useLifecycleHost(true);
            prefs.squelch = "noise+10";
            load();
        }

        function cleanup() {
            screenLoader.source = "";
            prefs.squelch = "noise+10";
            testContext.useLifecycleHost(false);
        }

        function load() {
            screenLoader.source = "";
            screenLoader.source = uiDir + "/SettingsScreen.qml";
            verify(screenLoader.item !== null);
            screenLoader.item.advancedOpen = true;
            waitForRendering(screenLoader.item);
        }

        function item(name) {
            var found = findChild(screenLoader.item, name);
            verify(found !== null, name);
            return found;
        }
        function segments(control) {
            var found = [];
            (function walk(node) {
                for (var child of node.children) {
                    if (child.modelData !== undefined && child.active !== undefined)
                        found.push(child);
                    else
                        walk(child);
                }
            })(control);
            return found;
        }
        function choose(label) {
            var control = item("settingsSquelchMode");
            var index = control.model.indexOf(label);
            verify(index >= 0, label);
            segments(control)[index].choose();
        }
        function shownMode() {
            var control = item("settingsSquelchMode");
            return control.model[control.currentIndex];
        }

        function test_default_is_noise_plus_10() {
            compare(JSON.stringify(item("settingsSquelchMode").model), JSON.stringify(["Off", "Auto", "Noise"]));
            compare(item("settingsSquelchMode").Accessible.name, "Squelch");
            compare(shownMode(), "Noise");
            compare(item("settingsSquelchMargin").text, "+10 dB");
            compare(item("settingsSquelchMargin").Accessible.name, "Squelch margin 10 dB");
            compare(item("settingsSquelchHint").text,
                "Quiet until a signal comes in. Higher numbers need stronger signals. AM and narrow channels use Auto.");
            compare(item("settingsSquelchMarginDown").Accessible.name, "Decrease Squelch Margin");
            compare(item("settingsSquelchMarginUp").Accessible.name, "Increase Squelch Margin");
            verify(item("settingsSquelchMarginDown").height >= 48 && item("settingsSquelchMarginDown").width >= 48);
            verify(item("settingsSquelchMarginUp").height >= 48 && item("settingsSquelchMarginUp").width >= 48);
        }

        function test_modes_write_the_setting() {
            choose("Off");
            compare(prefs.squelch, "off");
            compare(shownMode(), "Off");
            verify(!item("settingsSquelchMargin").visible, "Off has no margin");
            compare(item("settingsSquelchHint").text, "Plays the receiver's hiss between transmissions.");
            choose("Auto");
            compare(prefs.squelch, "auto+10", "Off kept the margin it left");
            compare(item("settingsSquelchHint").text,
                "Opens over each channel's own noise, learned in the first moments. Higher numbers need stronger signals.");
            item("settingsSquelchMarginUp").clicked();
            compare(prefs.squelch, "auto+11");
            choose("Off");
            choose("Noise");
            compare(prefs.squelch, "noise+11", "and through Off again");
        }

        function test_margin_bounds() {
            prefs.squelch = "noise+4";
            item("settingsSquelchMarginDown").clicked();
            compare(prefs.squelch, "noise+3");
            verify(!item("settingsSquelchMarginDown").enabled, "3 dB is the least margin");
            item("settingsSquelchMarginDown").clicked();
            compare(prefs.squelch, "noise+3");
            prefs.squelch = "auto+29";
            item("settingsSquelchMarginUp").clicked();
            compare(prefs.squelch, "auto+30");
            verify(!item("settingsSquelchMarginUp").enabled, "30 dB is the most");
            item("settingsSquelchMarginUp").clicked();
            compare(prefs.squelch, "auto+30");
            verify(item("settingsSquelchMarginDown").enabled);
        }

        function test_a_change_from_elsewhere_shows() {
            prefs.squelch = "auto+17";
            compare(shownMode(), "Auto");
            compare(item("settingsSquelchMargin").text, "+17 dB");
            prefs.squelch = "off";
            compare(shownMode(), "Off");
            // A refused value writes nothing: the shown setting stays what AppPrefs holds.
            prefs.squelch = "noise+40";
            compare(prefs.squelch, "off");
            compare(shownMode(), "Off");
        }

        function test_persists_across_a_reload() {
            choose("Auto");
            compare(prefs.squelch, "auto+10");
            load();
            compare(shownMode(), "Auto");
            compare(item("settingsSquelchMargin").text, "+10 dB");
        }
    }
}
