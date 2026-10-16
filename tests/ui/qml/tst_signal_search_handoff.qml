// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// Leaving Signal search for a detection: Explore here or a saved system it
// matched, from every decoder state, and a failed search retried as a search.
Item {
    width: 420
    height: 900
    Loader {
        id: loader
        source: uiDir + "/Main.qml"
    }
    QtObject {
        id: searchStub
        property var status: ({phase: 0, owned: false, session: "0", hit: {}})
        property var sessionAvoids: []
        property var ranges: [{name: "Local UHF", startMHz: "450.000000", endMHz: "460.000000", enabled: true}]
        property var history: []
        property var avoids: []
        property string error: ""
        property int arms: 0
        property int cancels: 0
        function control(action, session, hit) { return true; }
        function arm() { arms++; return ""; }
        function cancelArm() { cancels++; }
        function removeAvoid(frequency) {}
        function avoidFrequency(session, frequency) { return true; }
        function avoidPermanently(session, frequency) { return true; }
        // The shape SignalSearchModel::systemForHit() gives a detection without a recorded receiver: the source it is
        // handed, retuned to the detection with its mode and gain.
        function systemForHit(hit, source) {
            return Object.assign({}, source, {freqMhz: hit.freqMHz, decodeFlag: "-fA", gainDb: 21, trunking: false,
                searchUnresolved: false});
        }
    }
    TestCase {
        name: "SignalSearchHandoff"
        when: windowShown
        readonly property var app: loader.item
        property bool originalOnboarding: false
        readonly property var detection: ({id: "42", session: "17", freqMHz: "463.552100", frequency: 463552100,
            power: -32.1, ended: false, confirmed: false, matches: []})
        function screen() {
            return findChild(app, "signalSearchScreen");
        }
        function init() {
            verify(app !== null);
            testContext.useLifecycleHost(true);
            testContext.setLifecycleFailure("");
            testContext.setLifecyclePhase(0);
            testContext.setDelayedStop(false);
            originalOnboarding = prefs.onboardingDone;
            prefs.onboardingDone = true;
            prefs.exploreSourceType = "usb";
            prefs.exploreHost = "";
            prefs.explorePort = 1234;
            prefs.exploreFreqMhz = "851.0125";
            prefs.searchSourceType = "rtltcp";
            prefs.searchHost = "10.0.0.7";
            prefs.searchPort = 1234;
            app.currentTab = 0;
            app.dismissedFailure = "";
            app.startError = "";
            searchStub.arms = 0;
            searchStub.cancels = 0;
            searchStub.status = {phase: 0, owned: false, session: "0", hit: {}};
            screen().searchModel = searchStub;
            while (savedSystems.count)
                savedSystems.remove(0);
            app.requestActivate();
            tryCompare(app, "active", true);
        }
        function cleanup() {
            app.signalSearchOpen = false;
            app.pendingStart = null;
            app.awaitingUsbAccess = false;
            app.cancelPendingRestart();
            app.attemptedSource = null;
            screen().searchModel = null;
            testContext.setDongleStatus(false, 0, "");
            testContext.setDelayedStop(false);
            testContext.setLifecyclePhase(0);
            testContext.setLifecycleFailure("");
            while (savedSystems.count)
                savedSystems.remove(0);
            prefs.exploreSourceType = "";
            prefs.exploreFreqMhz = "";
            prefs.searchSourceType = "";
            prefs.searchHost = "";
            prefs.onboardingDone = originalOnboarding;
            testContext.useLifecycleHost(false);
        }
        // Start a search from Home's entry on the remembered rtl_tcp server.
        function startSearch() {
            findChild(app, "homeScreen").signalSearch();
            verify(app.signalSearchOpen);
            wait(0);
            findChild(screen(), "searchStartPause").activate();
            compare(decoderHost.sessionState, 1);
            verify(app.sessionSystem && app.sessionSystem.signalSearch);
        }
        // Bring the decoder to @p state with the search screen open over it.
        function enterState(state) {
            if (state === 0) {
                findChild(app, "homeScreen").signalSearch();
                verify(app.signalSearchOpen);
                return;
            }
            startSearch();
            searchStub.status = {phase: 4, owned: true, session: "17", hit: detection};
            if (state === 3) {
                testContext.setLifecyclePhase(2);
                testContext.setDelayedStop(true);
                decoderHost.stop();
            } else if (state !== 1) {
                testContext.setLifecyclePhase(state);
            }
            compare(decoderHost.sessionState, state);
            app.signalSearchOpen = true;
        }
        function activateMenuItem(name) {
            var menu = findChild(screen(), "searchDetectionMenu");
            verify(menu.visible, "the detection's menu is open");
            var item = findChild(menu, name);
            verify(item !== null && item.visible && item.enabled, name);
            item.activate();
            verify(!menu.visible);
        }

        // Retrying a failed Signal search start runs the search again, from its
        // screen and with its ranges armed, never a plain Explore session.
        function test_retry_of_a_failed_search_is_a_search() {
            startSearch();
            testContext.setLifecycleFailure("rtl_tcp: connection refused");
            testContext.setLifecyclePhase(4);
            screen().requestClose();
            verify(!app.signalSearchOpen);
            var arms = searchStub.arms;
            app.retrySource();
            tryCompare(decoderHost, "sessionState", 1);
            verify(app.sessionSystem.signalSearch, "the retry is a search, not Explore");
            verify(app.signalSearchOpen, "it runs on its own screen");
            compare(searchStub.arms, arms + 1, "its ranges are armed again");
            compare(app.sessionSystem.sourceType, "rtltcp");
            compare(app.sessionSystem.host, "10.0.0.7");
            verify(!app.exploring);
        }
        // Editing a failed search opens Signal search, not Explore setup.
        function test_edit_of_a_failed_search_opens_the_search() {
            startSearch();
            testContext.setLifecyclePhase(4);
            screen().requestClose();
            verify(!app.signalSearchOpen);
            app.editFailedSource();
            verify(app.signalSearchOpen);
            verify(!app.exploreSetupOpen);
            compare(screen().sourceType, "rtltcp");
            compare(screen().host, "10.0.0.7");
        }

        // Explore here from every decoder state: the search's start is
        // abandoned, its screen closes, and Explore starts on the detection once
        // the decoder is idle.
        function test_explore_here_from_every_state_data() {
            return [{tag: "idle", state: 0}, {tag: "starting", state: 1}, {tag: "running", state: 2},
                {tag: "stopping", state: 3}, {tag: "failed", state: 4}];
        }
        function test_explore_here_from_every_state(data) {
            enterState(data.state);
            var cancels = searchStub.cancels;
            var stops = decoderHost.stopCalls();
            screen().exploreDetection(detection);
            verify(searchStub.cancels > cancels, "the search's arm is cancelled");
            if (data.state === 3) {
                compare(decoderHost.stopCalls(), stops, "a stop already under way is not repeated");
                verify(app.pendingRestart !== null);
                testContext.setDelayedStop(false);
                testContext.setLifecyclePhase(0);
            } else {
                compare(decoderHost.stopCalls(), stops + (data.state === 0 ? 0 : 1));
            }
            tryCompare(decoderHost, "sessionState", 1);
            verify(!app.signalSearchOpen, "the search screen gave way to the new session");
            verify(!app.leavingSearch);
            compare(app.sessionSystem.name, "Exploring");
            compare(app.sessionSystem.freqMhz, "463.552100");
            compare(app.sessionSystem.decodeFlag, "-fA");
            compare(app.sessionSystem.gainDb, 21);
            verify(!app.sessionSystem.signalSearch, "Explore here is no search");
            verify(!app.sessionSystem.trunking);
            verify(app.exploring);
            compare(app.pendingRestart, null);
            // Tuned elsewhere, then stopped: Explore's remembered frequency stays.
            testContext.setLifecyclePhase(2);
            app.lastExploreFreqMhz = "463.600000";
            decoderHost.stop();
            verify(!app.monitorMode);
            compare(prefs.exploreFreqMhz, "851.0125", "Explore here never rewrites where Explore resumes");
            // An ordinary Explore session still remembers where it got to.
            prefs.exploreSourceType = "rtltcp";
            prefs.exploreHost = "127.0.0.1";
            app.startExploring();
            compare(decoderHost.sessionState, 1);
            testContext.setLifecyclePhase(2);
            app.lastExploreFreqMhz = "852.000000";
            decoderHost.stop();
            compare(prefs.exploreFreqMhz, "852.000000");
        }
        // A failed Explore-here start is retried on the detection, still
        // without becoming where Explore resumes.
        function test_retry_of_a_failed_explore_here() {
            enterState(0);
            screen().exploreDetection(detection);
            compare(decoderHost.sessionState, 1);
            testContext.setLifecyclePhase(4);
            app.retrySource();
            tryCompare(decoderHost, "sessionState", 1);
            compare(app.sessionSystem.name, "Exploring");
            compare(app.sessionSystem.freqMhz, "463.552100");
            verify(app.attemptedSource.remember === false);
            testContext.setLifecyclePhase(2);
            app.lastExploreFreqMhz = "463.600000";
            decoderHost.stop();
            compare(prefs.exploreFreqMhz, "851.0125");
        }
        // The same detection from its history row's menu.
        function test_explore_here_from_a_history_row() {
            searchStub.history = [detection];
            enterState(0);
            wait(0);
            var more = findChild(screen(), "searchHistoryActions");
            verify(more !== null && more.visible);
            compare(more.accessibleName, "Actions for 463.5521 MHz");
            more.activate();
            activateMenuItem("searchActionExplore");
            compare(decoderHost.sessionState, 1);
            compare(app.sessionSystem.name, "Exploring");
            searchStub.history = [];
        }
        // A USB permission wait keeps the request whole: granted, it starts
        // Explore here, which still leaves Explore's remembered frequency alone.
        function test_explore_here_through_a_usb_permission_grant() {
            testContext.setDongleStatus(false, 4, "No permission for RTL-SDR");
            findChild(app, "homeScreen").signalSearch();
            screen().sourceType = "usb";
            screen().exploreDetection(detection);
            verify(app.awaitingUsbAccess);
            verify(app.pendingStart && !app.pendingStart.signalSearch);
            compare(app.pendingStart.name, "Exploring");
            testContext.setDongleStatus(true, 0, "");
            compare(decoderHost.sessionState, 1);
            compare(app.sessionSystem.name, "Exploring");
            verify(app.attemptedSource.remember === false);
            testContext.setLifecyclePhase(2);
            app.lastExploreFreqMhz = "463.600000";
            decoderHost.stop();
            compare(prefs.exploreFreqMhz, "851.0125");
        }
        // A search start waiting for USB access is abandoned by the handoff: the
        // grant arriving afterwards starts no search.
        function test_late_usb_grant_after_the_handoff_starts_no_search() {
            testContext.setDongleStatus(false, 4, "No permission for RTL-SDR");
            findChild(app, "homeScreen").signalSearch();
            screen().sourceType = "usb";
            wait(0);
            findChild(screen(), "searchStartPause").activate();
            verify(app.awaitingUsbAccess && app.pendingStart.signalSearch);
            var cancels = searchStub.cancels;
            screen().sourceType = "rtltcp";
            screen().exploreDetection(detection);
            verify(searchStub.cancels > cancels);
            compare(decoderHost.sessionState, 1, "an rtl_tcp Explore here starts at once");
            verify(!app.awaitingUsbAccess);
            testContext.setDongleStatus(true, 0, "");
            compare(decoderHost.sessionState, 1);
            compare(app.sessionSystem.name, "Exploring");
            verify(!app.sessionSystem.signalSearch);
            verify(!app.signalSearchOpen);
        }

        function addWarehouse() {
            verify(savedSystems.add({name: "Warehouse", sourceType: "rtltcp", host: "127.0.0.1", port: 1234,
                freqMhz: "463.5500"}));
            return savedSystems.get(savedSystems.count - 1).uid;
        }
        // Start a saved system it matched, from a running search: the search
        // stops, then the saved system starts as saved.
        function test_start_saved_system_from_every_state_data() {
            return [{tag: "idle", state: 0}, {tag: "starting", state: 1}, {tag: "running", state: 2}, {tag: "stopping", state: 3},
                {tag: "failed", state: 4}];
        }
        function test_start_saved_system_from_every_state(data) {
            var uid = addWarehouse();
            enterState(data.state);
            screen().startSavedSystem(uid);
            if (data.state === 3) {
                testContext.setDelayedStop(false);
                testContext.setLifecyclePhase(0);
            }
            tryCompare(decoderHost, "sessionState", 1);
            verify(!app.signalSearchOpen);
            compare(app.sessionSystem.uid, uid);
            verify(!app.sessionSystem.signalSearch);
            verify(!app.exploring);
        }
        // An avoided system is refused visibly: before anything stops, on the
        // search screen; and if it became avoided while the search stopped, on
        // Home, where Retry asks again and Edit opens that system.
        function test_avoided_saved_system_is_refused_visibly() {
            var uid = addWarehouse();
            savedSystems.setAvoidSite(savedSystems.rowForUid(uid), true);
            enterState(2);
            var stops = decoderHost.stopCalls();
            screen().startSavedSystem(uid);
            compare(decoderHost.stopCalls(), stops, "nothing stops for a refused start");
            verify(app.signalSearchOpen, "the search stays");
            var refusal = "“Warehouse” is avoided. Remove the avoid in its site list to start it.";
            compare(findChild(screen(), "searchMessage").text, refusal);
            compare(app.startError, "", "the refusal is the screen's, not a start failure for Home");
            // What the search says afterwards shows too, after the refusal.
            searchStub.status = Object.assign({}, searchStub.status, {message: "The avoid list is full"});
            compare(findChild(screen(), "searchMessage").text, refusal + "\nThe avoid list is full");
            searchStub.status = Object.assign({}, searchStub.status, {message: ""});
            searchStub.error = "The decoder did not accept this action.";
            compare(findChild(screen(), "searchMessage").text, refusal + "\nThe decoder did not accept this action.");
            searchStub.error = "";
            // Avoided while the search stops.
            savedSystems.setAvoidSite(savedSystems.rowForUid(uid), false);
            testContext.setDelayedStop(true);
            screen().startSavedSystem(uid);
            compare(decoderHost.sessionState, 3);
            savedSystems.setAvoidSite(savedSystems.rowForUid(uid), true);
            testContext.setDelayedStop(false);
            testContext.setLifecyclePhase(0);
            wait(10);
            compare(decoderHost.sessionState, 0, "nothing starts");
            verify(!app.signalSearchOpen);
            compare(app.startError, refusal);
            verify(app.showFailure, "the refusal is shown");
            compare(app.attemptedSource.kind, "saved");
            compare(app.attemptedSource.uid, uid);
            // Retry is the saved system again, refused again; never the search.
            var arms = searchStub.arms;
            app.retrySource();
            wait(10);
            compare(decoderHost.sessionState, 0);
            compare(searchStub.arms, arms);
            verify(!app.signalSearchOpen);
            compare(app.startError, refusal);
            app.editFailedSource();
            verify(app.wizardOpen, "Edit opens the refused system");
            app.wizardOpen = false;
        }
        // Handing off from a running search keeps the search screen as the root
        // while it stops and says so; stray input does not drop the handoff.
        function test_running_handoff_keeps_the_search_screen_while_stopping() {
            enterState(2);
            testContext.setDelayedStop(true);
            var roots = [];
            var record = function () { roots.push({state: decoderHost.sessionState, root: Ui.Navigation.rootSurfaces[0]}); };
            Ui.Navigation.rootSurfacesChanged.connect(record);
            try {
                screen().exploreDetection(detection);
                compare(decoderHost.sessionState, 3);
                verify(app.leavingSearch);
                compare(Ui.Navigation.rootSurfaces[0], screen(), "the search screen is the root while it stops");
                compare(findChild(screen(), "searchIdentification").text, "Stopping the search…");
                verify(!findChild(screen(), "searchStop").enabled && !findChild(screen(), "searchStartPause").enabled, "the footer takes no input");
                verify(!findChild(screen(), "searchMore").enabled && !findChild(screen(), "searchHold").enabled, "nor do the Now actions");
                verify(!findChild(screen(), "searchBack").enabled);
                // A tap (the site chooser cancels a waiting restart on any input) leaves it.
                mouseClick(screen(), screen().width / 2, screen().height / 2);
                verify(app.pendingRestart !== null && app.leavingSearch);
                compare(Ui.Navigation.rootSurfaces[0], screen());
                testContext.setDelayedStop(false);
                testContext.setLifecyclePhase(0);
                tryCompare(decoderHost, "sessionState", 1);
            } finally {
                Ui.Navigation.rootSurfacesChanged.disconnect(record);
            }
            compare(app.sessionSystem.name, "Exploring");
            verify(!app.leavingSearch && !app.signalSearchOpen);
            // Nothing else was the root while the search stopped: not its monitor, not Home.
            for (var entry of roots) {
                if (entry.state === 3)
                    compare(entry.root, screen(), "the root while stopping");
            }
        }
        // A stop that never finishes cannot lock the screen: Back cancels the
        // handoff, the search screen stays with its controls, and reaching Idle
        // starts nothing. The next Back follows the session's usual policy.
        function test_back_cancels_a_stalled_handoff_data() {
            return [{tag: "back", key: false}, {tag: "escape", key: true}];
        }
        function test_back_cancels_a_stalled_handoff(data) {
            enterState(2);
            testContext.setDelayedStop(true);
            var system = app.sessionSystem;
            screen().exploreDetection(detection);
            compare(decoderHost.sessionState, 3);
            verify(app.leavingSearch);
            verify(!findChild(screen(), "searchStop").enabled);
            if (data.key)
                keyClick(Qt.Key_Escape);
            else
                app.requestBack();
            verify(!app.leavingSearch, "Back cancels the handoff");
            compare(app.pendingRestart, null);
            verify(app.signalSearchOpen);
            compare(Ui.Navigation.rootSurfaces[0], screen(), "the search screen stays the root");
            verify(findChild(screen(), "searchStop").enabled && findChild(screen(), "searchBack").enabled, "its controls are back");
            verify(findChild(screen(), "searchScroll").enabled);
            verify(findChild(screen(), "searchIdentification").text !== "Stopping the search…");
            testContext.setDelayedStop(false);
            testContext.setLifecyclePhase(0);
            wait(10);
            compare(decoderHost.sessionState, 0, "nothing starts");
            compare(app.sessionSystem, system);
        }
        // Escape on a running search screen is Main's Back, whichever control
        // has focus; after a cancelled handoff the next Escape is too.
        function test_escape_on_the_running_screen_is_back() {
            enterState(2);
            var hold = findChild(screen(), "searchHold");
            hold.forceActiveFocus();
            verify(hold.activeFocus);
            var calls = decoderHost.backgroundCalls();
            keyClick(Qt.Key_Escape);
            compare(decoderHost.backgroundCalls(), calls + 1, "Back's session policy ran");
            testContext.setLifecyclePhase(2);
            app.signalSearchOpen = true;
            testContext.setDelayedStop(true);
            screen().exploreDetection(detection);
            verify(app.leavingSearch);
            keyClick(Qt.Key_Escape);
            verify(!app.leavingSearch && app.pendingRestart === null, "the first Escape cancels the handoff");
            calls = decoderHost.backgroundCalls();
            keyClick(Qt.Key_Escape);
            compare(decoderHost.backgroundCalls(), calls + 1, "the second Escape is Back again");
            testContext.setDelayedStop(false);
        }
        // A start request that cannot run while the search stops (the start a
        // USB attachment would make, a site restart) leaves the handoff alone.
        function test_refused_starts_keep_a_stopping_handoff() {
            var uid = addWarehouse();
            enterState(2);
            testContext.setDelayedStop(true);
            screen().exploreDetection(detection);
            compare(decoderHost.sessionState, 3);
            app.startSystem(savedSystems.rowForUid(uid));
            app.restartSite(uid);
            verify(app.leavingSearch && app.pendingRestart !== null, "the handoff stands");
            testContext.setDelayedStop(false);
            testContext.setLifecyclePhase(0);
            tryCompare(decoderHost, "sessionState", 1);
            compare(app.sessionSystem.name, "Exploring");
        }
        // A search that fails to stop hands off nothing: the screen closes onto
        // Home's failure card and the flag clears.
        function test_failed_stop_ends_the_handoff() {
            enterState(2);
            testContext.setDelayedStop(true);
            screen().startSavedSystem(addWarehouse());
            verify(app.leavingSearch);
            testContext.setLifecyclePhase(4);
            verify(!app.leavingSearch);
            compare(app.pendingRestart, null);
            verify(!app.signalSearchOpen);
            testContext.setDelayedStop(false);
        }
    }
}
