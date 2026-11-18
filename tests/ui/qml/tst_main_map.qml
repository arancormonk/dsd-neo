// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// Issue #800 (spec 4.6, navigation): how the map is reached and left in Main. The session menu offers it always;
// History offers it idle and in a session, so kept tracks stay reachable after Stop; Stop leaves it open; while it is
// open nothing under it takes input or starts on its own; a History row's "Show on map" closes the row's sheet first,
// and Back from the map opens that sheet again by the row's uid while the row is still logged, else hands the focus
// back to the screen; a close the app makes (a file another app opened) opens nothing again. Signal search's sheet
// offers no map. Coordinates are made up.
Item {
    width: 420
    height: 900
    Loader {
        id: loader
        source: uiDir + "/Main.qml"
    }
    TestCase {
        name: "MainMap"
        when: windowShown
        readonly property var app: loader.item
        property bool originalOnboarding: false

        function init() {
            verify(app !== null);
            testContext.useLifecycleHost(true);
            testContext.setLifecycleFailure("");
            testContext.setLifecyclePhase(2);
            originalOnboarding = prefs.onboardingDone;
            prefs.onboardingDone = true;
            app.currentTab = 0;
            app.sessionDestination = "";
            testContext.setMetric("radioInput", true);
            app.requestActivate();
            tryCompare(app, "active", true);
            callHistory.clearAll();
        }
        function cleanup() {
            app.closeMap(false);
            app.sessionDestination = "";
            findChild(app, "sessionMenu").visible = false;
            testContext.setLifecyclePhase(0);
            prefs.onboardingDone = originalOnboarding;
            callHistory.clearAll();
            testContext.useLifecycleHost(false);
        }
        function mapScreen() {
            return findChild(app, "mapScreen");
        }
        function chooseFromSessionMenu(key) {
            findChild(app, "monitorScreen").openSessionMenu();
            var menu = findChild(app, "sessionMenu");
            verify(menu.visible);
            var row = findChild(menu, "sessionMenu" + key);
            verify(row !== null && row.visible, key + " is in the session menu");
            row.activate();
            verify(!menu.visible);
            wait(0);
        }
        // A logged call with a saved fix; returns its uid.
        function callWithFix() {
            callHistory.push("TODAY");
            var uid = callHistory.newestUid();
            verify(callHistory.setRowPosition(uid, {latitude: 10.0, longitude: 20.0, errorM: 30, errorLowerBound: false,
                                                    validityUnknown: false, provisional: false, weakCheck: false,
                                                    anonymous: false, emergency: false, kindText: "LRRP"}));
            return uid;
        }

        // The session menu always offers Map; open, it covers the monitor and blocks auto-start; Stop leaves it
        // open with its kept tracks; Back closes it.
        function test_session_menu_opens_the_map_and_stop_keeps_it() {
            chooseFromSessionMenu("Map");
            verify(app.mapOpen);
            var screen = mapScreen();
            verify(screen !== null && screen.visible);
            verify(!findChild(app, "monitorScreen").enabled, "the monitor takes no input under the map");
            compare(uiController.autoStartBlocked, true);
            verify(Ui.Navigation.allows(findChild(screen, "mapBack")));
            testContext.setLifecyclePhase(0);
            verify(app.mapOpen, "Stop leaves the map open");
            verify(screen.visible);
            findChild(screen, "mapBack").activate();
            verify(!app.mapOpen);
            compare(uiController.autoStartBlocked, false);
        }

        // History's Map button, idle (after Stop the kept tracks stay reachable) and in a session.
        function test_history_opens_the_map_idle_and_in_a_session() {
            testContext.setLifecyclePhase(0);
            app.currentTab = 1;
            var idle = findChild(app, "idleHistoryScreen");
            var button = findChild(idle, "historyMapButton");
            verify(button.visible, "offered with no calls logged too");
            button.activate();
            verify(app.mapOpen);
            verify(!findChild(app, "idleHistoryScreen").parent.visible || findChild(app, "idleHistoryScreen").parent.opacity === 0, "the tab shell hides under the map");
            app.requestBack();
            verify(!app.mapOpen);
            compare(app.currentTab, 1);

            testContext.setLifecyclePhase(2);
            chooseFromSessionMenu("History");
            var session = findChild(app, "sessionHistoryScreen");
            findChild(session, "historyMapButton").activate();
            verify(app.mapOpen);
            verify(!findChild(app, "sessionTools").enabled, "the session tools take no input under the map");
            app.requestBack();
            verify(!app.mapOpen);
            compare(app.sessionDestination, "history");
        }

        // "Show on map": the sheet closes first, the map shows the row's pin, and Back opens the sheet again by uid.
        function test_show_on_map_returns_to_the_row() {
            testContext.setLifecyclePhase(0);
            app.currentTab = 1;
            var uid = callWithFix();
            callHistory.push("TODAY");
            var plain = callHistory.newestUid();
            var history = findChild(app, "idleHistoryScreen");
            var sheet = findChild(history, "historyDetailSheet");
            sheet.open(callHistory.recordFor(plain));
            verify(!findChild(sheet, "historyShowOnMap").visible, "a row without a fix has nothing to show");
            sheet.visible = false;
            sheet.open(callHistory.recordFor(uid));
            var show = findChild(sheet, "historyShowOnMap");
            verify(show.visible);
            show.activate();
            verify(!sheet.visible, "the sheet closes before the map opens");
            verify(app.mapOpen);
            compare(Ui.Navigation.modals.length, 0);
            var screen = mapScreen();
            verify(screen.pin !== null);
            compare(screen.pin.uid, uid);
            verify(findChild(screen, "mapPin").visible);
            app.requestBack();
            verify(!app.mapOpen);
            tryVerify(function () { return sheet.visible; }, 1000, "Back opens the row's sheet again");
            compare(sheet.record.uid, uid);
            compare(screen.pin, null, "the pin goes with the map");
            sheet.visible = false;

            // The row trimmed meanwhile: nothing to reopen, the focus goes back to History.
            sheet.open(callHistory.recordFor(uid));
            findChild(sheet, "historyShowOnMap").activate();
            verify(callHistory.trimRow(uid));
            app.requestBack();
            wait(50);
            verify(!sheet.visible);
            verify(!app.mapOpen);
        }

        // A close the app makes (a file another app opened) opens no sheet again.
        function test_an_app_close_does_not_reopen_the_sheet() {
            testContext.setLifecyclePhase(0);
            app.currentTab = 1;
            var uid = callWithFix();
            var sheet = findChild(findChild(app, "idleHistoryScreen"), "historyDetailSheet");
            sheet.open(callHistory.recordFor(uid));
            findChild(sheet, "historyShowOnMap").activate();
            verify(app.mapOpen);
            app.closeIntakeLayers(false);
            verify(!app.mapOpen);
            wait(50);
            verify(!sheet.visible);
        }

        // The recent-calls sheets on Listen and on the monitor offer it too; Signal search's does not.
        function test_sheet_hosts() {
            verify(findChild(app, "homeRecentDetailSheet").showMap);
            verify(findChild(app, "monitorHistoryDetail").showMap);
            var search = findChild(app, "searchCallDetails");
            if (search)
                verify(!search.showMap);
        }
    }
}
