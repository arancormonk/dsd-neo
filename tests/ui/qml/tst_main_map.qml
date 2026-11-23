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
            app.spectrumOpen = false;
            app.exploring = false;
            historyView.filterText = "";
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
            button.forceActiveFocus();
            button.activate();
            verify(app.mapOpen);
            verify(!findChild(app, "idleHistoryScreen").parent.visible || findChild(app, "idleHistoryScreen").parent.opacity === 0, "the tab shell hides under the map");
            app.requestBack();
            verify(!app.mapOpen);
            compare(app.currentTab, 1);
            tryVerify(function () { return button.activeFocus; }, 1000, "the focus comes back to History's Map");

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

        // Issue #902: a control that hides with the keyboard on it hands it to Main's one Escape and Back owner (its
        // safe area, Navigation.backTarget), never to a screen that passes Escape to nothing: neither the idle History
        // tab (the tab shell is anchored to the safe area, not parented under it) nor the Spectrum's loaded screen.
        function test_hidden_control_keeps_back_working_on_the_history_tab() {
            testContext.setLifecyclePhase(0);
            callHistory.push("TODAY");
            app.currentTab = 1;
            var idle = findChild(app, "idleHistoryScreen");
            historyView.filterText = "no call matches this";
            var clear = findChild(idle, "historyClearFilters");
            tryVerify(function () { return clear.visible; }, 1000, "the filters leave nothing: Clear filters shows");
            clear.forceActiveFocus();
            tryVerify(function () { return clear.activeFocus; }, 1000, "Clear filters has the keyboard");
            keyClick(Qt.Key_Space);
            tryVerify(function () { return !clear.visible && !clear.activeFocus; }, 1000, "Clear filters hides and gives the keyboard up");
            verify(app.activeFocusItem !== null && app.activeFocusItem.objectName === "safeArea", "to Main's Back owner, not "
                   + (app.activeFocusItem ? app.activeFocusItem.objectName : "nothing"));
            keyClick(Qt.Key_Escape);
            compare(app.currentTab, 0, "Escape goes back to Listen");
        }
        function test_hidden_control_keeps_back_working_on_the_spectrum() {
            testContext.setLifecyclePhase(2);
            testContext.setMetric("centerFreqHz", 460000000);
            app.exploring = true;
            findChild(app, "spectrumLoader").active = true;
            app.spectrumOpen = true;
            var rail = findChild(app, "spectrumBandRail");
            verify(rail !== null);
            tryVerify(function () { return rail.visible; }, 1000, "exploring: the tuning rail shows");
            rail.forceActiveFocus();
            verify(rail.activeFocus);
            app.exploring = false;
            tryVerify(function () { return !rail.visible && !rail.activeFocus; }, 1000, "view only: the rail hides and gives the keyboard up");
            keyClick(Qt.Key_Escape);
            verify(!app.spectrumOpen, "Escape closes the Spectrum");
        }

        // Issue #902: Resume scan disabled under the keyboard drops it to the Spectrum's loader (its focus scope), not to
        // the window root; hidden then, it is handed to the Back owner all the same, so Escape still closes the Spectrum.
        function test_disabled_resume_keeps_back_working_on_the_spectrum() {
            testContext.setLifecyclePhase(2);
            testContext.setMetric("centerFreqHz", 460000000);
            testContext.setMetric("scanPaused", true);
            testContext.setMetric("scanResumable", true);
            findChild(app, "spectrumLoader").active = true;
            app.spectrumOpen = true;
            var resume = findChild(app, "spectrumResumeScan");
            verify(resume !== null);
            tryVerify(function () { return resume.visible && resume.enabled; }, 1000, "paused: Resume scan shows");
            resume.forceActiveFocus();
            verify(resume.activeFocus);
            testContext.setMetric("scanResumable", false);
            testContext.setMetric("scanPaused", false);
            tryVerify(function () { return !resume.visible && !resume.activeFocus; }, 1000, "Resume scan hides without the keyboard");
            wait(50);
            keyClick(Qt.Key_Escape);
            verify(!app.spectrumOpen, "Escape closes the Spectrum");
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
            tryVerify(function () { return findChild(history, "callLogList").activeFocus; }, 1000,
                      "the row gone, the focus goes to the History list");
        }

        // Review r1: from the keyboard, a History row's details, Show on map, then Back twice: the first Back opens
        // the row's sheet again, the second gives the focus back to that row, found by uid (the row the sheet was
        // first opened from is gone: the covered list held no rows).
        function test_back_twice_returns_to_the_row() {
            testContext.setLifecyclePhase(0);
            app.currentTab = 1;
            callHistory.push("TODAY");
            var uid = callWithFix();
            callHistory.push("TODAY");
            var history = findChild(app, "idleHistoryScreen");
            var list = findChild(history, "callLogList");
            var sheet = findChild(history, "historyDetailSheet");
            var rowOf = function () {
                for (var i = 0; i < list.count; ++i) {
                    var r = list.itemAtIndex(i);
                    if (r && r.rowUid === uid)
                        return r;
                }
                return null;
            };
            tryVerify(function () { return rowOf() !== null; }, 1000, "the row is listed");
            var first = rowOf();
            first.node.forceActiveFocus();
            verify(first.node.activeFocus);
            keyClick(Qt.Key_Return);
            tryVerify(function () { return sheet.visible; }, 1000, "the row's sheet opens from the keyboard");
            findChild(sheet, "historyShowOnMap").activate();
            verify(app.mapOpen);
            app.requestBack();
            tryVerify(function () { return sheet.visible; }, 1000, "the first Back opens the sheet again");
            compare(sheet.record.uid, uid);
            sheet.requestDismiss();
            tryVerify(function () {
                var row = rowOf();
                return row !== null && row.node.activeFocus;
            }, 1000, "the second Back focuses the row: " + app.activeFocusItem);
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
