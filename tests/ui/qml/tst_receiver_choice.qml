// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui
import "../../../src/ui/qt/qml/Util.js" as Util

// Which radio Explore and Signal search start on: the one remembered, resolved
// against the receiver kind plugged in (decoderHost.attachedDeviceSource).
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
        function control(action, session, hit) {}
        function arm() { return ""; }
        function cancelArm() {}
    }
    TestCase {
        name: "ReceiverChoice"
        when: windowShown
        readonly property var app: loader.item
        property bool originalOnboarding: false
        function forget() {
            prefs.exploreSourceType = "";
            prefs.exploreHost = "";
            prefs.explorePort = 1234;
            prefs.exploreFreqMhz = "";
            prefs.searchSourceType = "";
            prefs.searchHost = "";
            prefs.searchPort = 1234;
        }
        function init() {
            verify(app !== null);
            testContext.useLifecycleHost(true);
            testContext.setLifecycleFailure("");
            testContext.setLifecyclePhase(0);
            testContext.setAttachedDeviceSource("");
            originalOnboarding = prefs.onboardingDone;
            prefs.onboardingDone = true;
            forget();
            app.currentTab = 0;
            app.dismissedFailure = "";
            app.requestActivate();
            tryCompare(app, "active", true);
        }
        function cleanup() {
            app.signalSearchOpen = false;
            app.exploreSetupOpen = false;
            app.pendingStart = null;
            app.awaitingUsbAccess = false;
            findChild(app, "signalSearchScreen").searchModel = null;
            testContext.setAttachedDeviceSource("");
            testContext.setLifecyclePhase(0);
            testContext.setLifecycleFailure("");
            forget();
            prefs.onboardingDone = originalOnboarding;
            testContext.useLifecycleHost(false);
        }

        function test_rule_data() {
            return [
                {tag: "usb-remembered-airspy-attached", remembered: "usb", attached: "airspy", want: "airspy"},
                {tag: "airspy-remembered-usb-attached", remembered: "airspy", attached: "usb", want: "usb"},
                {tag: "usb-remembered-usb-attached", remembered: "usb", attached: "usb", want: "usb"},
                {tag: "airspy-remembered-airspy-attached", remembered: "airspy", attached: "airspy", want: "airspy"},
                {tag: "usb-remembered-both-attached", remembered: "usb", attached: "both", want: "usb"},
                {tag: "airspy-remembered-both-attached", remembered: "airspy", attached: "both", want: "airspy"},
                {tag: "usb-remembered-nothing-attached", remembered: "usb", attached: "", want: "usb"},
                {tag: "airspy-remembered-nothing-attached", remembered: "airspy", attached: "", want: "airspy"},
                {tag: "rtltcp-remembered-airspy-attached", remembered: "rtltcp", attached: "airspy", want: "rtltcp"},
                {tag: "rtltcp-remembered-usb-attached", remembered: "rtltcp", attached: "usb", want: "rtltcp"},
                {tag: "nothing-remembered-airspy-attached", remembered: "", attached: "airspy", want: "airspy"},
                {tag: "nothing-remembered-both-attached", remembered: "", attached: "both", want: ""},
                {tag: "nothing-remembered-nothing-attached", remembered: "", attached: "", want: ""}
            ];
        }
        function test_rule(data) {
            compare(Util.effectiveSource(data.remembered, data.attached), data.want);
            testContext.setAttachedDeviceSource(data.attached);
            compare(app.effectiveSource(data.remembered), data.want, "Main resolves through the host's reading");
        }

        function test_desktop_host_sees_nothing_attached() {
            testContext.useLifecycleHost(false);
            compare(decoderHost.attachedDeviceSource, "");
        }

        // Home's Explore tap starts directly once a radio is remembered, so the
        // attached receiver has to be resolved on that path too, not only in setup.
        function test_home_launch_data() {
            return [
                {tag: "airspy-remembered", remembered: "airspy", attached: "", want: "airspy"},
                {tag: "usb-remembered-airspy-attached", remembered: "usb", attached: "airspy", want: "airspy"},
                {tag: "airspy-remembered-usb-attached", remembered: "airspy", attached: "usb", want: "usb"},
                {tag: "usb-remembered-both-attached", remembered: "usb", attached: "both", want: "usb"},
                {tag: "rtltcp-remembered-airspy-attached", remembered: "rtltcp", attached: "airspy", want: "rtltcp"}
            ];
        }
        function test_home_launch(data) {
            prefs.exploreSourceType = data.remembered;
            prefs.exploreHost = data.remembered === "rtltcp" ? "10.0.0.5" : "";
            prefs.explorePort = 1235;
            prefs.exploreFreqMhz = "851.0125";
            testContext.setAttachedDeviceSource(data.attached);
            var home = findChild(app, "homeScreen");
            var label = data.want === "rtltcp" ? "RTL-TCP" : data.want === "airspy" ? "Airspy" : "USB dongle";
            compare(findChild(home, "homeExploreSubtitle").text, label + " · from 851.0125 MHz",
                    "the card names the radio the tap starts on");
            home.explore();
            verify(!app.exploreSetupOpen, "a remembered radio starts without asking");
            verify(app.sessionSystem !== null);
            compare(app.sessionSystem.sourceType, data.want);
            compare(app.sessionSystem.freqMhz, "851.0125");
            if (data.want === "rtltcp") {
                compare(app.sessionSystem.host, "10.0.0.5");
                compare(app.sessionSystem.port, 1235);
            }
            compare(prefs.exploreSourceType, data.remembered, "resolving never rewrites what was remembered");
        }

        // Explore setup preselects the attached receiver before it takes its
        // baseline, so closing it straight away asks nothing.
        function test_setup_preselects_without_dirtying_data() {
            return [
                {tag: "usb-remembered-airspy-attached", remembered: "usb", attached: "airspy", want: "airspy"},
                {tag: "airspy-remembered", remembered: "airspy", attached: "", want: "airspy"},
                {tag: "rtltcp-remembered-usb-attached", remembered: "rtltcp", attached: "usb", want: "rtltcp"}
            ];
        }
        function test_setup_preselects_without_dirtying(data) {
            prefs.exploreSourceType = data.remembered;
            prefs.exploreFreqMhz = "851.0125";
            testContext.setAttachedDeviceSource(data.attached);
            findChild(app, "homeScreen").exploreSetup();
            verify(app.exploreSetupOpen);
            var setup = findChild(app, "exploreSetupScreen");
            compare(setup.sourceType, data.want);
            verify(findChild(setup, "exploreSource_" + data.want).selected);
            setup.requestClose();
            verify(!app.exploreSetupOpen, "an untouched setup closes without a discard prompt");
        }

        // First run: nothing remembered, so the tap asks, and the setup it opens
        // is filled the same way as every other opening.
        function test_first_run_data() {
            return [
                {tag: "airspy-attached", attached: "airspy", want: "airspy"},
                {tag: "usb-attached", attached: "usb", want: "usb"},
                {tag: "both-attached", attached: "both", want: "usb"},
                {tag: "nothing-attached", attached: "", want: "usb"}
            ];
        }
        function test_first_run(data) {
            testContext.setAttachedDeviceSource(data.attached);
            var setup = findChild(app, "exploreSetupScreen");
            setup.reset("rtltcp", "stale.example", 9999, "100.0000");
            findChild(app, "homeScreen").explore();
            verify(app.exploreSetupOpen, "nothing remembered: ask instead of guessing");
            compare(decoderHost.sessionState, 0, "nothing started");
            compare(setup.sourceType, data.want);
            compare(setup.hostText, "192.168.1.10", "the setup is reset, not left on an earlier draft");
            compare(setup.freqText, "855.0000");
            setup.requestClose();
            verify(!app.exploreSetupOpen, "the first-run setup starts clean");
        }

        // Signal search seeds from its own last radio, else Explore's, else USB,
        // and then resolves the attached receiver.
        function test_search_seed_data() {
            return [
                {tag: "nothing", attached: "", want: "usb", host: "192.168.1.10", port: 1234},
                {tag: "nothing-airspy-attached", attached: "airspy", want: "airspy", host: "192.168.1.10", port: 1234},
                {tag: "explore", explore: "airspy", attached: "", want: "airspy", host: "10.0.0.5", port: 1235},
                {tag: "explore-rtltcp", explore: "rtltcp", attached: "airspy", want: "rtltcp", host: "10.0.0.5", port: 1235},
                {tag: "search-over-explore", explore: "rtltcp", search: "airspy", attached: "", want: "airspy",
                    host: "10.0.0.9", port: 1240},
                {tag: "search-usb-airspy-attached", search: "usb", attached: "airspy", want: "airspy",
                    host: "10.0.0.9", port: 1240},
                {tag: "search-rtltcp", explore: "airspy", search: "rtltcp", attached: "usb", want: "rtltcp",
                    host: "10.0.0.9", port: 1240}
            ];
        }
        function test_search_seed(data) {
            if (data.explore) {
                prefs.exploreSourceType = data.explore;
                prefs.exploreHost = "10.0.0.5";
                prefs.explorePort = 1235;
            }
            if (data.search) {
                prefs.searchSourceType = data.search;
                prefs.searchHost = "10.0.0.9";
                prefs.searchPort = 1240;
            }
            testContext.setAttachedDeviceSource(data.attached);
            var screen = findChild(app, "signalSearchScreen");
            screen.sourceType = "rtltcp";
            findChild(app, "homeScreen").signalSearch();
            verify(app.signalSearchOpen);
            compare(screen.sourceType, data.want);
            compare(screen.host, data.host);
            compare(screen.port, data.port);
            // The receiver Home seeded, an attached one included, is not an edit:
            // Back closes without asking.
            screen.requestClose();
            verify(!app.signalSearchOpen, "the seeded receiver is not an edit");
        }

        // Start search remembers the radio it starts on in the search's keys, and
        // leaves Explore's answer alone.
        function test_start_search_remembers_its_radio() {
            prefs.exploreSourceType = "usb";
            prefs.exploreFreqMhz = "851.0125";
            var screen = findChild(app, "signalSearchScreen");
            screen.searchModel = searchStub;
            findChild(app, "homeScreen").signalSearch();
            wait(0);
            screen.sourceType = "airspy";
            findChild(screen, "searchStartPause").activate();
            compare(prefs.searchSourceType, "airspy");
            compare(app.sessionSystem.sourceType, "airspy");
            verify(app.sessionSystem.signalSearch);
            compare(prefs.exploreSourceType, "usb", "a search never moves Explore's radio");
            compare(prefs.exploreFreqMhz, "851.0125");
            testContext.setLifecyclePhase(0);
            app.signalSearchOpen = false;
            // Opened again, the screen comes back on the radio the search used.
            findChild(app, "homeScreen").signalSearch();
            compare(screen.sourceType, "airspy");
        }

        function test_start_search_remembers_an_rtltcp_endpoint() {
            var screen = findChild(app, "signalSearchScreen");
            screen.searchModel = searchStub;
            findChild(app, "homeScreen").signalSearch();
            wait(0);
            screen.sourceType = "rtltcp";
            screen.host = "10.0.0.7";
            findChild(screen, "searchStartPause").activate();
            compare(prefs.searchSourceType, "rtltcp");
            compare(prefs.searchHost, "10.0.0.7");
            compare(prefs.searchPort, 1234);
            compare(prefs.exploreSourceType, "", "Explore stays unconfigured");
        }
    }
}
