// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
//
// GPS site roaming (#786) through Main: the lifecycle host, a location feed and
// a monotonic clock the case scripts. Three made-up sites of one system on one
// rtl_tcp receiver: Hill (A, running), Lake (B, about 55 km north) and Ridge
// (C, far). A fix near Lake, three of them over 20 s, makes the switch due.
import QtQuick
import QtTest
import DsdNeo 1.0

Item {
    width: 420
    height: 900

    Loader {
        id: loader
        source: uiDir + "/Main.qml"
    }

    TestCase {
        id: tc
        name: "SiteRoaming"
        when: windowShown

        property double ns: 1
        property var uids: []
        property bool onboardingWas: false
        readonly property var app: loader.item

        function init() {
            testContext.setLifecycleLocationSupported(true);
            testContext.useLifecycleHost(true, true);
            testContext.setDelayedStop(true);
            testContext.setRoamingNow(1000000);
            testContext.setUserStopGeneration(0);
            testContext.setForegroundEpoch(0);
            testContext.resetCommands();
            prefs.extraArgs = "";
            prefs.metricUnits = true;
            prefs.roamingEnabled = true;
            // Home shows (the notice is on it) only past onboarding.
            onboardingWas = prefs.onboardingDone;
            prefs.onboardingDone = true;
            testContext.setMetrics({trunkingEnabled: true, radioInput: true, replayInput: false, scanKind: 0,
                operatorTuned: false, heldTg: 0, callBusy: false, optionsKnown: true});
            while (savedSystems.count)
                savedSystems.remove(0);
            addSites("rtltcp", "-fp");
        }

        function cleanup() {
            findChild(app, "siteChooserSheet").visible = false;
            app.cancelPendingRestart("start");
            app.dismissedFailure = "";
            app.startError = "";
            siteRoaming.clearRecoveryNotice();
            prefs.roamingEnabled = false;
            prefs.extraArgs = "";
            prefs.onboardingDone = onboardingWas;
            testContext.setDelayedStop(false);
            testContext.setNextStartSuperseded(false);
            testContext.setRetireRoamAttemptWins(true);
            testContext.setLifecycleAcceptStart(true);
            testContext.setLifecycleFailure("");
            testContext.setUsbBrokered(false);
            testContext.useLifecycleHost(false);
            testContext.setLifecycleLocationSupported(false);
            testContext.setMetrics({trunkingEnabled: false, radioInput: true, replayInput: false, scanKind: 0,
                operatorTuned: false, heldTg: 0, callBusy: false, optionsKnown: false});
            while (savedSystems.count)
                savedSystems.remove(0);
        }

        // Hill, Lake and Ridge, each with its own channel map and talkgroups.
        function addSites(source, flag) {
            var names = ["Hill", "Lake", "Ridge"];
            var lats = [10.0, 10.5, 11.5];
            var ids = [];
            for (var i = 0; i < 3; ++i) {
                savedSystems.add({name: "Valley — " + names[i], siteName: names[i], rrSid: 7, rrSiteId: 70 + i,
                    trunking: true, sourceType: source, host: "127.0.0.1", port: 1234, decodeFlag: flag,
                    freqMhz: (851.0125 + i).toFixed(4), siteLat: lats[i], siteLon: 20.0, hasSitePos: true,
                    chanCsvPath: "/tmp/roam-" + names[i] + "-chan.csv", groupCsvPath: "/tmp/roam-" + names[i] + "-tg.csv"});
                ids.push(savedSystems.get(i).uid);
            }
            uids = ids;
        }

        function now() {
            return testContext.roamingNow();
        }

        // A new fix at @p lat, @p lon, @p after ms on, stamped then.
        function fix(lat, lon, after) {
            testContext.setRoamingNow(now() + (after || 0));
            ns += 1000;
            testContext.emitLocationStreamUpdate({generation: 1, state: "ok", hasFix: true, provider: "gps",
                measurementNs: ns, lat: lat, lon: lon, accuracyM: 10, fixAtMs: now()});
        }
        function nearB(after) {
            fix(10.49, 20.0, after);
        }
        function nearA(after) {
            fix(10.01, 20.0, after);
        }
        function tick(after) {
            testContext.setRoamingNow(now() + after);
            siteRoaming.tick();
        }
        function calls() {
            return testContext.roamingHostCalls();
        }
        function args() {
            return testContext.lastStartArgs();
        }
        function argAfter(flag) {
            var list = args();
            var at = list.indexOf(flag);
            return at >= 0 ? list[at + 1] : "";
        }

        // Saved site @p row running, past its initialization edge unless @p uninitialized.
        function startSite(row, uninitialized) {
            app.startSystem(row);
            compare(decoderHost.sessionState, 1, "the start was accepted");
            testContext.setLifecyclePhase(2);
            if (!uninitialized)
                testContext.emitSessionInitialized();
        }

        // Hill running, then three fixes near Lake over 20 s.
        function qualifyB() {
            startSite(0);
            nearB(0);
            nearB(10000);
            nearB(10000);
        }

        // Through the switch's stop to the destination's start, handed to the host.
        function toLakeStarting() {
            var stops = calls().switchStops;
            qualifyB();
            compare(calls().switchStops, stops + 1);
            testContext.setLifecyclePhase(0);
            tryCompare(decoderHost, "sessionState", 1);
            compare(argAfter("-C"), "/tmp/roam-Lake-chan.csv");
        }

        function roamNotice() {
            return findChild(app, "homeRoamNotice");
        }

        function test_01_arms_only_after_the_initialization_edge() {
            startSite(0, true);
            compare(siteRoaming.reason, SiteRoaming.SessionStarting);
            compare(calls().streamStarts, 0, "no location before the edge");
            nearB(0);
            nearB(10000);
            nearB(10000);
            compare(calls().switchStops, 0, "fixes before the edge count for nothing");
            testContext.emitSessionInitialized();
            compare(siteRoaming.state, SiteRoaming.Paused);
            compare(siteRoaming.statusText, "Waiting for a location fix");
            verify(calls().streamActive);
            compare(calls().streamStarts, 1);
            compare(siteRoaming.currentUid, uids[0]);
            verify(siteRoaming.groupKey.length > 0, "the group of the session");
        }

        function test_02_idle_and_pause_reasons() {
            compare(siteRoaming.state, SiteRoaming.Idle);
            compare(siteRoaming.statusText, "Roaming works while listening to a site of a multi-site system");
            startSite(0);
            nearA(0);
            compare(siteRoaming.state, SiteRoaming.Following);
            compare(siteRoaming.statusText, "Staying on this site until another is clearly closer");
            testContext.setMetric("heldTg", 5);
            compare(siteRoaming.statusText, "Paused while a talkgroup is held");
            verify(!calls().streamActive, "location stops while paused for a held talkgroup");
            testContext.setMetric("heldTg", 0);
            testContext.emitLocationStreamUpdate({generation: 2, state: "noPermission", hasFix: false});
            compare(siteRoaming.statusText, "Location permission is off");
            compare(siteRoaming.action, SiteRoaming.OpenAppSettings);
            siteRoaming.performAction();
            compare(calls().appSettingsOpens, 1);
            testContext.emitLocationStreamUpdate({generation: 2, state: "approximateOnly", hasFix: false});
            compare(siteRoaming.statusText, "Roaming needs precise location");
            testContext.emitLocationStreamUpdate({generation: 2, state: "locationOff", hasFix: false});
            compare(siteRoaming.statusText, "Location is off");
            siteRoaming.performAction();
            compare(calls().locationSettingsOpens, 1);
            testContext.setRoamingNow(now() + 1000);
            testContext.emitLocationStreamUpdate({generation: 2, state: "ok", hasFix: true, provider: "gps",
                measurementNs: ++ns, lat: 10.49, lon: 20.0, accuracyM: 5000, fixAtMs: now()});
            compare(siteRoaming.statusText, "Location is too imprecise right now");
            fix(30.0, 20.0, 1000);
            compare(siteRoaming.statusText, "No saved site within 100 km");
            prefs.roamingEnabled = false;
            compare(siteRoaming.state, SiteRoaming.Off);
            compare(siteRoaming.statusText, "");
        }

        function test_03_route_waits_for_the_call_then_starts_lake_with_its_own_files() {
            startSite(0);
            testContext.setMetric("callBusy", true);
            nearB(0);
            compare(siteRoaming.state, SiteRoaming.Candidate);
            compare(siteRoaming.statusText, "Lake is closer. Checking before switching");
            nearB(10000);
            nearB(10000);
            compare(siteRoaming.state, SiteRoaming.WaitingForCall);
            compare(siteRoaming.statusText, "Waiting for this call to end before switching to Lake (up to 90 seconds)");
            compare(calls().switchStops, 0, "no switch while the call is followed");
            var userStops = decoderHost.stopCalls();
            testContext.setMetric("callBusy", false);
            compare(calls().switchStops, 1, "the call ended: the switch stop");
            compare(decoderHost.stopCalls(), userStops, "a switch stop, never the user's");
            compare(app.pendingRestart.kind, "roam");
            compare(siteRoaming.statusText, "Switching to Lake…");
            testContext.setLifecyclePhase(0);
            tryCompare(decoderHost, "sessionState", 1);
            compare(argAfter("-C"), "/tmp/roam-Lake-chan.csv");
            compare(argAfter("-G"), "/tmp/roam-Lake-tg.csv");
            compare(testContext.lastStartStopExpectation(), 0, "the start carries the generation it was decided under");
            compare(app.pendingRestart, null);
            compare(app.sessionStartUid, uids[1]);
            testContext.setLifecyclePhase(2);
            testContext.emitSessionInitialized();
            verify(!siteRoaming.transactionActive);
            compare(siteRoaming.currentUid, uids[1]);
            compare(prefs.lastStartedUid, uids[1], "Lake's start is recorded as any start is");
            verify(!roamNotice().visible);
        }

        // DMR sites: the channel map is the LCN map, and Lake's goes, never Hill's.
        function test_04_dmr_pair_starts_with_the_destination_lcn_map() {
            while (savedSystems.count)
                savedSystems.remove(0);
            addSites("rtltcp", "-fs");
            toLakeStarting();
            compare(args().indexOf("/tmp/roam-Hill-chan.csv"), -1);
            compare(args().indexOf("-fs") >= 0, true);
        }

        function test_05_input_during_the_stop_leaves_the_roam() {
            qualifyB();
            compare(decoderHost.sessionState, 3);
            mouseClick(app.contentItem, 10, 10);
            keyClick(Qt.Key_Escape);
            compare(app.pendingRestart.kind, "roam", "a tap and Back leave the roam's restart");
            testContext.setLifecyclePhase(0);
            tryCompare(decoderHost, "sessionState", 1);
            compare(argAfter("-C"), "/tmp/roam-Lake-chan.csv");
        }

        function test_06_back_closing_the_chooser_and_a_dismissed_sheet_during_a_delayed_stop() {
            qualifyB();
            var chooser = findChild(app, "siteChooserSheet");
            chooser.openFor(0);
            verify(chooser.visible);
            app.requestBack(); // the system Back
            verify(!chooser.visible, "Back closed the chooser");
            chooser.openFor(0);
            chooser.requestDismiss();
            verify(!chooser.visible);
            compare(app.pendingRestart.kind, "roam", "neither cancelled the roam");
            testContext.setLifecyclePhase(0);
            tryCompare(decoderHost, "sessionState", 1);
            compare(argAfter("-C"), "/tmp/roam-Lake-chan.csv");
        }

        // A user stop moves the generation; each leaves the session stopped.
        function test_07a_stop_generation_before_qualification() {
            startSite(0);
            testContext.setUserStopGeneration(1);
            nearB(0);
            nearB(10000);
            nearB(10000);
            compare(calls().switchStops, 0);
            compare(siteRoaming.reason, SiteRoaming.NoSession);
            decoderHost.stop();
            testContext.setLifecyclePhase(0);
            wait(20);
            compare(decoderHost.sessionState, 0);
            compare(app.pendingRestart, null);
        }

        function test_07b_stop_generation_during_stopping_with_no_edge() {
            qualifyB();
            testContext.setUserStopGeneration(1);
            testContext.setLifecyclePhase(0);
            wait(20);
            compare(decoderHost.sessionState, 0);
            verify(!siteRoaming.transactionActive);
            verify(!roamNotice().visible, "the user's stop: no notice");
            compare(app.startError, "");
        }

        function test_07c_superseded_after_the_last_read() {
            qualifyB();
            testContext.setNextStartSuperseded(true);
            testContext.setLifecyclePhase(0);
            wait(20);
            compare(decoderHost.sessionState, 0);
            verify(!app.showFailure, "no failure card");
            compare(app.startError, "");
            verify(!roamNotice().visible);
            verify(!siteRoaming.transactionActive);
        }

        function test_07d_superseded_during_configuration() {
            toLakeStarting();
            testContext.supersedeStart();
            compare(decoderHost.sessionState, 0);
            verify(!app.showFailure);
            verify(!roamNotice().visible);
            verify(!siteRoaming.transactionActive);
        }

        function test_08_disable_during_stopping_leaves_listening_stopped_with_the_notice() {
            qualifyB();
            prefs.roamingEnabled = false;
            compare(app.pendingRestart, null);
            testContext.setLifecyclePhase(0);
            wait(20);
            compare(decoderHost.sessionState, 0);
            verify(roamNotice().visible);
            compare(siteRoaming.recoveryNotice, "Roaming did not switch to Lake. Listening stopped.");
            var listen = findChild(app, "homeRoamNoticeListen");
            compare(listen.text, "Listen to Hill");
            listen.clicked();
            compare(decoderHost.sessionState, 1);
            compare(argAfter("-C"), "/tmp/roam-Hill-chan.csv");
            verify(!roamNotice().visible, "the start clears the notice");
        }

        function test_09_manual_pick_before_the_roam_starts_supersedes_it_silently() {
            qualifyB();
            var chooser = findChild(app, "siteChooserSheet");
            testContext.setLifecyclePhase(0); // the roam's start is queued
            chooser.openFor(0);
            chooser.choose(2);
            compare(decoderHost.sessionState, 1);
            compare(argAfter("-C"), "/tmp/roam-Ridge-chan.csv");
            wait(20);
            compare(argAfter("-C"), "/tmp/roam-Ridge-chan.csv", "the roam's start never ran");
            verify(!siteRoaming.transactionActive);
            verify(!roamNotice().visible);
            testContext.setLifecyclePhase(2);
            testContext.emitSessionInitialized();
            // Nothing ran when it was picked: a new start, which roams as any other.
            compare(siteRoaming.state, SiteRoaming.Paused);
            compare(siteRoaming.reason, SiteRoaming.NoFix);
        }

        // D3: a switch the user makes from a session roaming follows holds roaming on the site chosen.
        function test_09b_stop_and_switch_from_a_roaming_session_holds() {
            startSite(0);
            var chooser = findChild(app, "siteChooserSheet");
            chooser.openFor(0);
            chooser.stopAndSwitch(1);
            testContext.setLifecyclePhase(0);
            tryCompare(decoderHost, "sessionState", 1);
            compare(argAfter("-C"), "/tmp/roam-Lake-chan.csv");
            testContext.setLifecyclePhase(2);
            testContext.emitSessionInitialized();
            compare(siteRoaming.state, SiteRoaming.Holding);
            compare(siteRoaming.statusText, "Holding the site you chose");
            compare(siteRoaming.action, SiteRoaming.ResumeRoaming);
            siteRoaming.performAction();
            verify(siteRoaming.state !== SiteRoaming.Holding, "Resume roaming");
        }

        // A switch the user gave up (a tap during its stop) holds nothing on a later start of that site.
        function test_09c_a_switch_given_up_holds_nothing_later() {
            startSite(0);
            var chooser = findChild(app, "siteChooserSheet");
            chooser.openFor(0);
            chooser.stopAndSwitch(1);
            compare(decoderHost.sessionState, 3);
            mouseClick(app.contentItem, 10, 10);
            compare(app.pendingRestart, null, "the tap gave the switch up");
            testContext.setLifecyclePhase(0);
            wait(20);
            compare(decoderHost.sessionState, 0);
            app.startSystem(1);
            testContext.setLifecyclePhase(2);
            testContext.emitSessionInitialized();
            verify(siteRoaming.state !== SiteRoaming.Holding);
        }

        // A switch whose start is refused (the site was avoided meanwhile) holds nothing on a later start.
        function test_09d_a_refused_switch_holds_nothing_later() {
            startSite(0);
            var chooser = findChild(app, "siteChooserSheet");
            chooser.openFor(0);
            chooser.stopAndSwitch(1);
            chooser.visible = false;
            savedSystems.setAvoidSite(1, true);
            testContext.setLifecyclePhase(0);
            tryVerify(function () { return app.startError.length > 0; });
            savedSystems.setAvoidSite(1, false);
            app.startError = "";
            app.startSystem(1);
            testContext.setLifecyclePhase(2);
            testContext.emitSessionInitialized();
            verify(siteRoaming.state !== SiteRoaming.Holding);
        }

        // A switch whose start the build refuses (a bad PPM) holds nothing on a later start either.
        function test_09f_a_switch_the_build_refuses_holds_nothing_later() {
            verify(savedSystems.update(1, {ppm: "x"}));
            startSite(0);
            var chooser = findChild(app, "siteChooserSheet");
            chooser.openFor(0);
            chooser.stopAndSwitch(1);
            testContext.setLifecyclePhase(0);
            tryVerify(function () { return app.startError.length > 0; });
            verify(savedSystems.update(1, {ppm: ""}));
            app.startError = "";
            app.startSystem(1);
            testContext.setLifecyclePhase(2);
            testContext.emitSessionInitialized();
            verify(siteRoaming.state !== SiteRoaming.Holding);
        }

        // A session the window took up without starting it never arms, and says so.
        function test_09e_an_adopted_session_never_arms() {
            app.sessionStartKind = "";
            prefs.acceptedStartKind = "saved";
            prefs.acceptedStartUid = uids[0];
            testContext.setLifecyclePhase(2);
            compare(app.sessionStartUid, uids[0], "Main took the session up");
            compare(siteRoaming.reason, SiteRoaming.SessionAdopted);
            compare(siteRoaming.statusText, "Start listening again to use GPS roaming");
            verify(!calls().streamActive);
            nearB(0);
            nearB(10000);
            nearB(10000);
            compare(calls().switchStops, 0);
            testContext.setLifecyclePhase(0);
            compare(siteRoaming.reason, SiteRoaming.NoSession);
        }

        function test_10_failed_during_the_stop_starts_nothing() {
            qualifyB();
            testContext.setLifecycleFailure("Connection refused");
            testContext.setLifecyclePhase(4);
            compare(app.pendingRestart, null);
            verify(!siteRoaming.transactionActive);
            testContext.setLifecycleFailure("");
            testContext.setLifecyclePhase(0);
            wait(20);
            compare(decoderHost.sessionState, 0);
        }

        function test_11_operator_hold_held_talkgroup_or_a_scan_before_the_stop() {
            var changes = [["operatorTuned", true], ["heldTg", 9], ["scanKind", 2]];
            for (var i = 0; i < changes.length; ++i) {
                testContext.setMetric("callBusy", true);
                startSite(0);
                nearB(0);
                nearB(10000);
                nearB(10000);
                compare(siteRoaming.state, SiteRoaming.WaitingForCall);
                testContext.setMetric(changes[i][0], changes[i][1]);
                verify(!siteRoaming.transactionActive, changes[i][0] + " cancels the switch");
                testContext.setMetric("callBusy", false);
                compare(calls().switchStops, 0, changes[i][0] + ": no stop");
                testContext.setMetric(changes[i][0], changes[i][0] === "operatorTuned" ? false : 0);
                testContext.setDelayedStop(false);
                decoderHost.stop();
                testContext.setDelayedStop(true);
            }
        }

        function test_12_usb_gate_carries_the_roam() {
            while (savedSystems.count)
                savedSystems.remove(0);
            addSites("usb", "-fp");
            testContext.setDongleStatus(true, 0, "");
            qualifyB();
            testContext.setDongleStatus(false, 0, "No RTL-SDR attached");
            testContext.setLifecyclePhase(0);
            tryVerify(function () { return app.awaitingUsbAccess; });
            compare(app.pendingStartRoam.roamId > 0, true);
            verify(siteRoaming.transactionActive);
            testContext.setDongleStatus(true, 0, "");
            compare(decoderHost.sessionState, 1);
            compare(argAfter("-C"), "/tmp/roam-Lake-chan.csv");
            compare(testContext.lastStartStopExpectation(), 0);
            compare(app.pendingStartRoam, null);
        }

        function test_13_usb_denied_refuses_the_roam() {
            while (savedSystems.count)
                savedSystems.remove(0);
            addSites("usb", "-fp");
            testContext.setDongleStatus(true, 0, "");
            qualifyB();
            testContext.setDongleStatus(false, 0, "No RTL-SDR attached");
            testContext.setLifecyclePhase(0);
            tryVerify(function () { return app.awaitingUsbAccess; });
            testContext.setDongleStatus(false, 4, "USB permission denied");
            verify(!siteRoaming.transactionActive);
            compare(siteRoaming.statusText, "Could not switch to Lake");
            compare(app.failureText, "USB permission denied", "the banner says why");
            testContext.setDongleStatus(true, 0, "");
            compare(decoderHost.sessionState, 0, "a later grant starts nothing");
        }

        function test_14_disable_or_manual_pick_during_the_usb_wait() {
            for (var pick = 0; pick < 2; ++pick) {
                while (savedSystems.count)
                    savedSystems.remove(0);
                addSites("usb", "-fp");
                testContext.setDongleStatus(true, 0, "");
                qualifyB();
                testContext.setDongleStatus(false, 0, "No RTL-SDR attached");
                testContext.setLifecyclePhase(0);
                tryVerify(function () { return app.awaitingUsbAccess; });
                if (pick) {
                    findChild(app, "siteChooserSheet").openFor(0);
                    findChild(app, "siteChooserSheet").choose(2);
                    verify(!siteRoaming.transactionActive, "the pick ends the roam");
                    compare(app.pendingStartRoam, null);
                    compare(app.pendingStart.uid, uids[2], "the pick waits for USB itself");
                    verify(!roamNotice().visible, "silently");
                    findChild(app, "siteChooserSheet").visible = false;
                } else {
                    prefs.roamingEnabled = false;
                    verify(!siteRoaming.transactionActive);
                    verify(!app.awaitingUsbAccess, "the wait is dropped");
                    verify(roamNotice().visible, "listening stopped: the notice");
                    testContext.setDongleStatus(true, 0, "");
                    compare(decoderHost.sessionState, 0, "the grant starts nothing");
                    prefs.roamingEnabled = true;
                }
                app.dropPendingStarts("start");
                siteRoaming.clearRecoveryNotice();
            }
        }

        function test_15_argv_error_host_refusal_and_failed_pause_roaming() {
            // A start the build refuses.
            verify(savedSystems.update(1, {ppm: "x"}));
            qualifyB();
            testContext.setLifecyclePhase(0);
            tryVerify(function () { return app.startError.length > 0; });
            compare(decoderHost.sessionState, 0);
            compare(siteRoaming.statusText, "Could not switch to Lake");
            verify(app.showFailure, "the usual failure card");
            app.startError = "";
            savedSystems.update(1, {ppm: ""});
            // A start the host refuses.
            qualifyB();
            testContext.setLifecycleAcceptStart(false);
            testContext.setLifecyclePhase(0);
            tryVerify(function () { return app.startError.length > 0; });
            compare(siteRoaming.statusText, "Could not switch to Lake");
            testContext.setLifecycleAcceptStart(true);
            app.startError = "";
            // A start that fails.
            toLakeStarting();
            testContext.setLifecycleFailure("Connection refused");
            testContext.setLifecyclePhase(4);
            verify(!siteRoaming.transactionActive);
            compare(siteRoaming.statusText, "Could not switch to Lake");
            verify(app.showFailure);
            verify(!roamNotice().visible, "a failure card, not the notice");
        }

        function test_16_foreground_loss_during_the_dwell_drops_the_candidate() {
            startSite(0);
            nearB(0);
            nearB(10000);
            compare(siteRoaming.state, SiteRoaming.Candidate);
            testContext.setForegroundEpoch(1);
            tick(500);
            compare(siteRoaming.statusText, "Waiting for a location fix");
            compare(calls().streamStarts, 2, "the feed starts again on return");
            nearB(9500);
            nearB(10000);
            compare(calls().switchStops, 0, "the evidence from before is gone");
        }

        // The app left the foreground and came back while roaming was armed:
        // Qt asks for the feed again once per epoch move, whenever its tick
        // sees it (the platform keeps a start asked for while hidden until the
        // app is visible), and never stops it in between.
        function test_16b_each_foreground_loss_asks_for_the_feed_again() {
            startSite(0);
            nearA(0);
            compare(siteRoaming.state, SiteRoaming.Following);
            var starts = calls().streamStarts;
            var stops = calls().streamStops;
            testContext.setForegroundEpoch(1);
            tick(500);
            compare(calls().streamStarts, starts + 1, "asked for again at the epoch move");
            compare(calls().streamStops, stops, "not stopped by Qt");
            compare(siteRoaming.statusText, "Waiting for a location fix");
            tick(1000);
            tick(1000);
            compare(calls().streamStarts, starts + 1, "once per epoch, not on every tick");
            nearA(1000);
            compare(siteRoaming.state, SiteRoaming.Following, "the feed asked for again counts");
            testContext.setForegroundEpoch(2);
            tick(500);
            compare(calls().streamStarts, starts + 2, "and again after the next return");
            compare(calls().streamStops, stops);
        }

        function test_17_foreground_loss_while_waiting_for_the_call() {
            startSite(0);
            testContext.setMetric("callBusy", true);
            nearB(0);
            nearB(10000);
            nearB(10000);
            testContext.setForegroundEpoch(1);
            tick(500);
            verify(!siteRoaming.transactionActive);
            testContext.setMetric("callBusy", false);
            compare(calls().switchStops, 0);
            compare(decoderHost.sessionState, 2, "Hill keeps running");
            verify(!roamNotice().visible);
        }

        function test_18_foreground_loss_during_the_stop() {
            qualifyB();
            testContext.setForegroundEpoch(1);
            tick(500);
            verify(!siteRoaming.transactionActive);
            testContext.setLifecyclePhase(0);
            wait(20);
            compare(decoderHost.sessionState, 0, "no start");
            verify(roamNotice().visible, "Home's notice");
        }

        function test_19_queued_start_before_the_first_resumed_tick() {
            qualifyB();
            testContext.setLifecyclePhase(0); // finishPendingRestart is queued
            testContext.setForegroundEpoch(1); // the app left the foreground meanwhile
            wait(20);
            compare(decoderHost.sessionState, 0, "the start's checks read the epoch now");
            verify(roamNotice().visible);
        }

        // Undelivered and configuring are the host's to tell apart; either way its attempt is retired.
        function test_20_disable_while_the_start_is_handed_over_retires_it() {
            toLakeStarting();
            prefs.roamingEnabled = false;
            compare(calls().retirements, 1);
            compare(decoderHost.sessionState, 0, "retired: listening stopped");
            verify(roamNotice().visible);
            compare(siteRoaming.recoveryNotice, "Roaming did not switch to Lake. Listening stopped.");
        }

        function test_21_disable_once_the_start_committed() {
            toLakeStarting();
            testContext.setRetireRoamAttemptWins(false);
            prefs.roamingEnabled = false;
            compare(calls().retirements, 1);
            compare(decoderHost.sessionState, 1, "Lake runs");
            verify(!siteRoaming.transactionActive);
            verify(!roamNotice().visible, "no notice");
            compare(siteRoaming.state, SiteRoaming.Off);
        }

        // A restore (#580) replaces the systems a roam names: it ends the roam, silently, in its stop or its start.
        function test_21b_restore_during_the_stop_or_the_start() {
            qualifyB();
            app.dropPendingStarts("restore");
            verify(!siteRoaming.transactionActive);
            compare(app.pendingRestart, null);
            testContext.setLifecyclePhase(0);
            wait(20);
            compare(decoderHost.sessionState, 0, "during the stop: nothing starts");
            verify(!roamNotice().visible);
            toLakeStarting();
            var retired = calls().retirements;
            app.dropPendingStarts("restore");
            compare(calls().retirements, retired + 1);
            compare(decoderHost.sessionState, 0, "during the start: retired, listening stopped");
            verify(!siteRoaming.transactionActive);
            verify(!roamNotice().visible, "the restore's own action: no notice");
        }

        function test_22_manual_pick_once_the_destination_runs() {
            toLakeStarting();
            testContext.setLifecyclePhase(2);
            testContext.emitSessionInitialized();
            verify(!siteRoaming.transactionActive);
            var chooser = findChild(app, "siteChooserSheet");
            chooser.openFor(1);
            chooser.stopAndSwitch(2);
            compare(app.pendingRestart.uid, uids[2], "a manual switch as any other");
            testContext.setLifecyclePhase(0);
            tryCompare(decoderHost, "sessionState", 1);
            compare(argAfter("-C"), "/tmp/roam-Ridge-chan.csv");
            verify(!roamNotice().visible);
            testContext.setLifecyclePhase(2);
            testContext.emitSessionInitialized();
            compare(siteRoaming.state, SiteRoaming.Holding, "picked from the roaming session Lake: held");
        }

        function test_23_destination_that_exits_without_initializing() {
            toLakeStarting();
            testContext.setLifecyclePhase(0); // a successful exit, never initialized, not failed
            verify(!siteRoaming.transactionActive);
            compare(siteRoaming.statusText, "Could not switch to Lake");
            verify(roamNotice().visible);
            compare(findChild(app, "homeRoamNoticeListen").text, "Listen to Hill");
        }

        // An operator command blocks roaming until the decoder shows it, however long that takes.
        function test_24_hold_talkgroup_delayed_past_a_qualified_roam() {
            startSite(0);
            testContext.setMetric("callBusy", true);
            nearB(0);
            nearB(10000);
            nearB(10000);
            compare(siteRoaming.state, SiteRoaming.WaitingForCall);
            verify(commands.holdTalkgroup(5));
            verify(!siteRoaming.transactionActive, "the hold revokes the switch");
            testContext.setMetric("callBusy", false);
            tick(1000);
            compare(calls().switchStops, 0, "no stop at the next evaluation");
            for (var i = 0; i < 5; ++i)
                nearB(10000);
            compare(calls().switchStops, 0, "nor after a full dwell");
            compare(siteRoaming.statusText, "Waiting for your last change to take effect");
            verify(commands.holdTalkgroup(0));
            nearB(10000);
            nearB(10000);
            compare(calls().switchStops, 0);
            nearB(10000);
            compare(calls().switchStops, 1, "a release supersedes the hold that never applied");
        }

        function test_25_hold_survives_roaming_off_and_on() {
            startSite(0);
            nearB(0);
            verify(commands.holdTalkgroup(5));
            prefs.roamingEnabled = false;
            prefs.roamingEnabled = true;
            for (var i = 0; i < 4; ++i)
                nearB(10000);
            compare(calls().switchStops, 0);
            compare(siteRoaming.statusText, "Waiting for your last change to take effect");
            testContext.setMetric("heldTg", 5);
            compare(siteRoaming.statusText, "Paused while a talkgroup is held");
        }

        function test_26_manual_tune_and_explore_from_here_delayed() {
            var kinds = ["tune", "explore"];
            for (var k = 0; k < kinds.length; ++k) {
                startSite(0);
                nearB(0);
                if (kinds[k] === "tune")
                    verify(commands.manualTuneHz(852000000));
                else
                    verify(commands.releaseTuner());
                tick(1000);
                for (var i = 0; i < 6; ++i)
                    nearB(10000);
                compare(calls().switchStops, 0, kinds[k] + ": no stop past two dwells");
                compare(siteRoaming.reason, SiteRoaming.PendingChange);
                testContext.setMetric("operatorTuned", true);
                compare(siteRoaming.reason, SiteRoaming.OperatorTune, kinds[k] + ": applied, it pauses");
                testContext.setMetric("operatorTuned", false);
                testContext.setDelayedStop(false);
                decoderHost.stop();
                testContext.setDelayedStop(true);
            }
        }

        function test_27_a_sibling_with_extra_options_is_never_chosen() {
            savedSystems.update(1, {extraArgs: "-v 3"});
            startSite(0);
            for (var i = 0; i < 5; ++i)
                nearB(10000);
            compare(calls().switchStops, 0);
            compare(siteRoaming.state, SiteRoaming.Following, "Hill is the nearest roaming may use");
        }

        function test_28_extra_options_in_settings_idle_roaming_and_never_stop_hill() {
            startSite(0);
            nearB(0);
            nearB(10000);
            prefs.extraArgs = "-v 3";
            compare(siteRoaming.statusText, "GPS roaming is off while extra options are set in Settings");
            nearB(10000);
            nearB(10000);
            compare(calls().switchStops, 0);
            compare(decoderHost.sessionState, 2);
        }

        function test_29_the_same_input_option_on_both_rows_never_arms() {
            savedSystems.update(0, {extraArgs: "-i rtl:0:851.0125"});
            savedSystems.update(1, {extraArgs: "-i rtl:0:851.0125"});
            startSite(0);
            verify(siteRoaming.state === SiteRoaming.Idle, "never armed");
            verify(!calls().streamActive);
            for (var i = 0; i < 4; ++i)
                nearB(10000);
            compare(calls().switchStops, 0);
        }

        function test_30_auto_start_is_blocked_during_a_transaction() {
            startSite(0);
            testContext.setMetric("callBusy", true);
            nearB(0);
            nearB(10000);
            verify(!uiController.autoStartBlocked);
            nearB(10000);
            verify(siteRoaming.transactionActive);
            verify(uiController.autoStartBlocked, "waiting for the call");
            testContext.setMetric("callBusy", false);
            verify(uiController.autoStartBlocked, "stopping for the switch");
            testContext.setLifecyclePhase(0);
            tryCompare(decoderHost, "sessionState", 1);
            verify(uiController.autoStartBlocked, "starting the destination");
            testContext.setLifecyclePhase(2);
            testContext.emitSessionInitialized();
            verify(!uiController.autoStartBlocked);
        }
    }
}
