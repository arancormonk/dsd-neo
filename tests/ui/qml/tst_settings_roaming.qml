// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
//
// #786: Settings' GPS roaming card. The site chooser's switch, and the two
// distances as choices in the unit shown: unset, the unit's default; a value
// picked in the other unit (or restored from a backup made there) shown as it
// converts, checked, after the unit's own choices.
import QtQuick
import QtTest

Item {
    width: 420
    height: 1400

    Loader {
        id: screenLoader
        anchors.fill: parent
    }

    TestCase {
        name: "SettingsRoaming"
        when: windowShown

        readonly property double mile: 1.609344

        // The location feed is constant on a real host, so it is set before the screen loads.
        function openScreen(feed) {
            screenLoader.source = "";
            if (feed === false) {
                testContext.setLifecycleLocationSupported(false);
                testContext.useLifecycleHost(true);
            }
            screenLoader.source = uiDir + "/SettingsScreen.qml";
            verify(screenLoader.item !== null);
        }
        function row(name) {
            var item = findChild(screenLoader.item, name);
            verify(item !== null, name);
            return item;
        }
        function choiceTexts() {
            return row("roamingChoices").choices.slice();
        }
        // Taps the sheet's real choice button.
        function choose(index) {
            var sheet = row("roamingChoices");
            compare(sheet.visible, true);
            var button = findChild(sheet, "roamingChoice" + index);
            verify(button !== null, "choice " + index);
            waitForRendering(screenLoader.item);
            mouseClick(button);
            compare(sheet.visible, false);
        }
        function tap(name) {
            var item = row(name);
            item.tapped();
        }

        // The real AppPrefs (`prefs`) and a host with a location feed, before anything is set.
        function init() {
            testContext.setLifecycleLocationSupported(true);
            testContext.useLifecycleHost(true);
            testContext.storeRoamingDistance("maxDistanceKm", undefined);
            testContext.storeRoamingDistance("marginKm", undefined);
        }
        function cleanup() {
            screenLoader.source = "";
            prefs.roamingEnabled = false;
            prefs.metricUnits = false;
            testContext.storeRoamingDistance("maxDistanceKm", undefined);
            testContext.storeRoamingDistance("marginKm", undefined);
            testContext.useLifecycleHost(false);
            testContext.setLifecycleLocationSupported(false);
        }

        function test_hidden_without_a_location_feed() {
            openScreen(false);
            verify(!row("settingsRoamingCard").visible);
        }

        // A first launch in miles: 60 mi and 1 mi, each checked among the miles choices; in metric, 100 km and 2 km.
        function test_first_launch_defaults_per_unit() {
            prefs.metricUnits = false;
            openScreen();
            verify(row("settingsRoamingCard").visible);
            compare(row("settingsRoamingHeader").text, "GPS roaming");
            compare(row("roamingMaxDistanceRow").title, "Roam to sites within");
            compare(row("roamingMaxDistanceRow").subtitle, "60 mi");
            compare(row("roamingMarginRow").title, "Switch when another site is closer by");
            compare(row("roamingMarginRow").subtitle, "1 mi · Grows when your location is less precise, and needs readings over 20 seconds");
            compare(row("roamingNote").text, "Uses precise location while DSD-neo is on screen.");
            tap("roamingMaxDistanceRow");
            compare(choiceTexts(), ["15 mi", "30 mi", "60 mi", "125 mi"]);
            compare(row("roamingChoices").currentIndex, 2);
            row("roamingChoices").visible = false;
            tap("roamingMarginRow");
            compare(choiceTexts(), ["0.5 mi", "1 mi", "3 mi"]);
            compare(row("roamingChoices").currentIndex, 1);
            row("roamingChoices").visible = false;
            prefs.metricUnits = true;
            compare(row("roamingMaxDistanceRow").subtitle, "100 km", "unset: the default of the unit shown");
            compare(row("roamingMarginRow").subtitle.indexOf("2 km · "), 0);
        }

        // A pick in one unit stays as picked when the unit changes, shown as it converts and checked as an extra
        // choice; both directions.
        function test_unit_switch_keeps_the_pick() {
            prefs.metricUnits = true;
            openScreen();
            tap("roamingMaxDistanceRow");
            compare(choiceTexts(), ["25 km", "50 km", "100 km", "200 km"]);
            choose(1);
            verify(Math.abs(prefs.roamingMaxDistanceKm - 50) < 1e-9);
            compare(row("roamingMaxDistanceRow").subtitle, "50 km");

            prefs.metricUnits = false;
            compare(row("roamingMaxDistanceRow").subtitle, "31.1 mi");
            tap("roamingMaxDistanceRow");
            compare(choiceTexts(), ["15 mi", "30 mi", "60 mi", "125 mi", "31.1 mi"]);
            compare(row("roamingChoices").currentIndex, 4, "the stored value, checked");
            choose(1);
            verify(Math.abs(prefs.roamingMaxDistanceKm - 30 * mile) < 1e-9);

            prefs.metricUnits = true;
            compare(row("roamingMaxDistanceRow").subtitle, "48.3 km");
            tap("roamingMaxDistanceRow");
            compare(choiceTexts(), ["25 km", "50 km", "100 km", "200 km", "48.3 km"]);
            compare(row("roamingChoices").currentIndex, 4);
            choose(4);
            verify(Math.abs(prefs.roamingMaxDistanceKm - 30 * mile) < 1e-9, "the extra choice keeps the value");

            tap("roamingMarginRow");
            choose(2);
            verify(Math.abs(prefs.roamingMarginKm - 5) < 1e-9);
            prefs.metricUnits = false;
            compare(row("roamingMarginRow").subtitle.indexOf("3.1 mi · "), 0);
        }

        // A backup made in the other unit restores its distances as stored: shown as they convert, checked.
        function test_restored_value_from_the_other_unit() {
            testContext.storeRoamingDistance("maxDistanceKm", 125 * mile);
            testContext.storeRoamingDistance("marginKm", 0.5 * mile);
            prefs.metricUnits = true;
            openScreen();
            compare(row("roamingMaxDistanceRow").subtitle, "201.2 km");
            compare(row("roamingMarginRow").subtitle.indexOf("0.8 km · "), 0);
            tap("roamingMarginRow");
            compare(choiceTexts(), ["1 km", "2 km", "5 km", "0.8 km"]);
            compare(row("roamingChoices").currentIndex, 3);
        }

        // The chooser's switch: on asks for precise location; both read the one setting.
        function test_toggle() {
            prefs.metricUnits = true;
            openScreen();
            var toggle = row("roamingToggle");
            compare(toggle.title, "Use GPS roaming");
            compare(toggle.subtitle, "Switch to the nearest saved site of the system you are listening to as you move.");
            verify(!toggle.checked);
            waitForRendering(screenLoader.item);
            mouseClick(toggle);
            verify(siteRoaming.enabled && prefs.roamingEnabled && toggle.checked);
            compare(testContext.roamingHostCalls().permissionRequests.length, 1);
            mouseClick(toggle);
            verify(!siteRoaming.enabled && !toggle.checked);
            prefs.roamingEnabled = true;
            verify(toggle.checked, "a change made elsewhere shows");
        }
    }
}
