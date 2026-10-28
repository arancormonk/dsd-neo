// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest

// #647 through Main's own bindings: a session its USB receiver ended leaves a
// notice on Listen, and Reconnect resumes that same session through the USB
// gate (D2a). Also the pill through a failed Airspy start (B1), the reveal on
// a brokered host (B5), Dismiss before a deferred restart (B13), and which
// attachments may start a session (D3a).
Item {
    width: 420
    height: 900

    Loader {
        id: loader
        source: uiDir + "/Main.qml"
    }

    TestCase {
        name: "MainReceiverNotice"
        when: windowShown

        readonly property var app: loader.item
        property bool originalOnboarding: false
        property bool originalAutoStart: false

        function home() {
            return findChild(app, "homeScreen");
        }
        function card() {
            return findChild(app, "homeFailureCard");
        }
        // Once the card's buttons are laid out: before its Flow is polished they overlap.
        function tap(button) {
            verify(waitForPolish(app.contentItem));
            waitForRendering(button);
            mouseClick(button, button.width / 2, button.height / 2);
        }
        function receiverName(source) {
            return source === "airspy" ? "Airspy" : "RTL-SDR";
        }
        // The receiver connected and ready, as Android reports it once access is granted.
        function connect(source) {
            testContext.setLocalDeviceRecord(source, false, source);
            testContext.setDongleStatus(true, 0, "Ready: " + receiverName(source));
        }
        // Unplugged: nothing connected, the record naming the receiver that went.
        function unplug(source) {
            testContext.setLocalDeviceRecord("", false, source);
            testContext.setDongleStatus(false, 3, receiverName(source) + " detached");
        }
        function addSaved(name, source) {
            savedSystems.add({name: name, sourceType: source, host: "127.0.0.1", port: 1234, freqMhz: "851.5"});
            return savedSystems.count - 1;
        }
        function addScanList(name, source) {
            scanLists.add({name: name, sourceType: source, entries: [
                {kind: "freq", name: "Simplex", protocol: "p25", freqMhz: "851.5", enabled: true}]});
            return scanLists.count - 1;
        }
        // Start @p kind ("saved", "scan" or "explore") on @p source; returns its uid ("" for Explore).
        function startTarget(kind, source) {
            testContext.setAttachedDeviceSource(source);
            connect(source);
            var uid = "";
            if (kind === "saved") {
                var row = addSaved("County P25", source);
                uid = savedSystems.get(row).uid;
                app.startSystem(row);
            } else if (kind === "scan") {
                var list = addScanList("Night scan", source);
                uid = scanLists.get(list).uid;
                app.startScanList(list);
            } else {
                prefs.exploreSourceType = source;
                app.startExploring();
            }
            compare(decoderHost.sessionState, 1, kind + " started");
            return uid;
        }
        // The session's receiver ends it: the host reports the ending while
        // the session winds down, and Listen says nothing until it is over.
        function endByReceiver(state, source, phase) {
            testContext.setReceiverHealth(state, source);
            unplug(source);
            verify(!app.showFailure, "no notice while the session is still on screen");
            testContext.setLifecyclePhase(phase === undefined ? 0 : phase);
        }
        // A stream failure ends the run Failed, with the run's own error text, as Android reports it.
        readonly property string streamFailure: "Decoder run failed (1); the receiver stopped sending samples"
        function failByReceiver(source) {
            testContext.setLifecycleFailureKind(7);
            testContext.setLifecycleFailure(streamFailure);
            endByReceiver("failed", source, 4);
        }
        // Long enough for a card that would come back to have come back.
        function verifyNoCard(why) {
            wait(100);
            verify(!card().visible, why);
            verify(!app.showFailure, why);
        }
        function sessionName(kind) {
            return kind === "saved" ? "County P25" : kind === "scan" ? "Night scan" : "Explore";
        }
        function verifySameSession(kind, uid) {
            tryCompare(decoderHost, "sessionState", 1);
            if (kind === "explore") {
                verify(app.exploring);
            } else {
                compare(app.sessionScanList, kind === "scan");
                compare(app.sessionSystem.uid, uid);
            }
        }

        // The brokered capability is CONSTANT: it is set before the host is published.
        function useHost(brokered) {
            testContext.useLifecycleHost(false);
            testContext.setUsbBrokered(brokered);
            testContext.useLifecycleHost(true);
        }

        function init() {
            verify(app !== null);
            // The notice's bindings and the gate's handlers run in every case: any warning is a defect.
            failOnWarning(/./);
            useHost(true);
            testContext.setLifecycleFailure("");
            testContext.setLifecycleFailureKind(0);
            testContext.setLifecyclePhase(0);
            testContext.setDelayedStop(false);
            testContext.setReceiverHealth("ok", "");
            testContext.resetDeviceAccess();
            originalOnboarding = prefs.onboardingDone;
            originalAutoStart = prefs.autoStartOnAttach;
            prefs.onboardingDone = true;
            prefs.autoStartOnAttach = false;
            prefs.exploreFreqMhz = "851.0125";
            while (savedSystems.count)
                savedSystems.remove(0);
            while (scanLists.count)
                scanLists.remove(0);
            app.currentTab = 0;
            app.dismissedFailure = "";
            app.startError = "";
            app.attemptedSource = null;
            app.requestActivate();
            tryCompare(app, "active", true);
        }
        function cleanup() {
            // A tap that went astray must not leave the next case behind an editor.
            verify(!app.wizardOpen && !app.scanListOpen && !app.exploreSetupOpen, "no editor opened");
            app.cancelPendingRestart();
            app.pendingStart = null;
            app.awaitingUsbAccess = false;
            app.signalSearchOpen = false;
            app.attemptedSource = null;
            app.startError = "";
            app.dismissedFailure = "";
            testContext.setReceiverHealth("ok", "");
            testContext.setLocalDeviceRecord("usb", false, "");
            testContext.setDongleStatus(false, 0, "");
            testContext.setUsbBrokered(false);
            testContext.setAttachedDeviceSource("");
            testContext.setDeviceRequestPends(false);
            testContext.setLifecycleFailureKind(0);
            testContext.setLifecycleFailure("");
            testContext.setLifecyclePhase(0);
            while (savedSystems.count)
                savedSystems.remove(0);
            while (scanLists.count)
                scanLists.remove(0);
            prefs.lastStartedKind = "";
            prefs.lastStartedUid = "";
            prefs.exploreSourceType = "";
            prefs.exploreFreqMhz = "";
            prefs.onboardingDone = originalOnboarding;
            prefs.autoStartOnAttach = originalAutoStart;
            testContext.useLifecycleHost(false);
        }

        function test_notice_names_what_ended_data() {
            return [
                {tag: "saved system, unplugged", kind: "saved", source: "usb", state: "lost",
                    text: "RTL-SDR was unplugged, so County P25 stopped. Plug it back in, then tap Reconnect."},
                {tag: "scan list, unplugged", kind: "scan", source: "usb", state: "lost",
                    text: "RTL-SDR was unplugged, so Night scan stopped. Plug it back in, then tap Reconnect."},
                {tag: "Explore, unplugged", kind: "explore", source: "usb", state: "lost",
                    text: "RTL-SDR was unplugged, so Explore stopped. Plug it back in, then tap Reconnect."},
                {tag: "Airspy saved system, stream failed", kind: "saved", source: "airspy", state: "failed",
                    text: "Airspy stopped sending samples, so County P25 stopped. Tap Reconnect to start again."},
                {tag: "saved system, stream failed", kind: "saved", source: "usb", state: "failed",
                    text: "RTL-SDR stopped sending samples, so County P25 stopped. Tap Reconnect to reset it and start again."},
                {tag: "Explore, stream failed", kind: "explore", source: "usb", state: "failed",
                    text: "RTL-SDR stopped sending samples, so Explore stopped. Tap Reconnect to reset it and start again."},
                {tag: "Airspy scan list, unplugged", kind: "scan", source: "airspy", state: "lost",
                    text: "Airspy was unplugged, so Night scan stopped. Plug it back in, then tap Reconnect."}
            ];
        }
        function test_notice_names_what_ended(data) {
            startTarget(data.kind, data.source);
            endByReceiver(data.state, data.source);
            tryCompare(card(), "visible", true);
            compare(card().message, data.text);
            var retry = findChild(card(), "failureRetry");
            compare(retry.text, "Reconnect");
            verify(retry.enabled);
            // One instruction at a time: the notice takes the details panel's
            // place, whose own Retry only asks for access.
            compare(findChild(home(), "dongleDetailsPanel").visible, false);
            verify(findChild(home(), "dongleStatusPill").visible);
            compare(findChild(home(), "dongleStatusLabel").text, receiverName(data.source).toUpperCase() + " LOST");
        }

        function test_reconnect_resumes_the_same_session_data() {
            return [
                {tag: "saved system", kind: "saved", source: "usb"},
                {tag: "scan list", kind: "scan", source: "usb"},
                {tag: "Explore", kind: "explore", source: "usb"},
                {tag: "Airspy saved system", kind: "saved", source: "airspy"}
            ];
        }
        function test_reconnect_resumes_the_same_session(data) {
            var uid = startTarget(data.kind, data.source);
            endByReceiver("lost", data.source);
            tryCompare(card(), "visible", true);
            var retry = findChild(card(), "failureRetry");
            tap(retry);
            // Through the USB gate, for the session's own receiver.
            tryCompare(app, "awaitingUsbAccess", true);
            var access = testContext.lastDeviceAccess();
            compare(access.count, 1);
            compare(access.source, data.source);
            compare(decoderHost.sessionState, 0, "nothing starts before access lands");
            compare(app.receiverEnded, false, "the access request acknowledged the ending");
            testContext.setDongleStatus(false, 0, "Waiting for permission");
            compare(card().message, "Waiting for permission");
            connect(data.source);
            verifySameSession(data.kind, uid);
        }

        function test_reconnect_after_a_failed_session() {
            // A stream failure leaves the session Failed: Reconnect acknowledges
            // it first, then runs the same gate.
            var uid = startTarget("saved", "usb");
            failByReceiver("usb");
            tryCompare(card(), "visible", true);
            compare(card().message, "RTL-SDR stopped sending samples, so County P25 stopped. Tap Reconnect to reset it and start again.");
            tap(findChild(card(), "failureRetry"));
            tryCompare(app, "awaitingUsbAccess", true);
            connect("usb");
            verifySameSession("saved", uid);
        }

        function test_permission_denial_shows_the_access_text() {
            startTarget("saved", "usb");
            failByReceiver("usb");
            tryCompare(card(), "visible", true);
            tap(findChild(card(), "failureRetry"));
            tryCompare(app, "awaitingUsbAccess", true);
            testContext.setDongleStatus(false, 4, "USB permission denied");
            // The answer to this request, not the ended session's stream failure.
            compare(card().message, "USB permission denied");
            compare(findChild(card(), "failureRetry").text, "Retry");
            wait(50);
            compare(decoderHost.sessionState, 0);
        }

        function test_dismiss_before_a_deferred_restart() {
            // B13: Retry queues the restart for the decoder's next idle turn;
            // Dismiss in between abandons it.
            var row = addSaved("Server", "rtltcp");
            app.startSystem(row);
            compare(decoderHost.sessionState, 1);
            testContext.setLifecycleFailure("Connection refused");
            testContext.setLifecyclePhase(4);
            tryCompare(card(), "visible", true);
            var stops = decoderHost.stopCalls();
            app.retrySource();
            verify(app.pendingRestart !== null, "the restart is deferred");
            card().dismiss();
            compare(app.pendingRestart, null);
            wait(100);
            compare(decoderHost.sessionState, 0, "a dismissed failure restarts nothing");
            compare(decoderHost.stopCalls(), stops + 1);
            verify(!card().visible);
        }

        function test_dismissed_notice_leaves_the_panel() {
            startTarget("saved", "usb");
            endByReceiver("lost", "usb");
            tryCompare(card(), "visible", true);
            var panel = findChild(home(), "dongleDetailsPanel");
            compare(panel.visible, false);
            card().dismiss();
            tryCompare(card(), "visible", false);
            tryCompare(panel, "visible", true);
            compare(findChild(home(), "dongleRetry").text, "Retry");
            compare(decoderHost.sessionState, 0);
        }

        // One ended session, one explanation: acknowledging the notice in any
        // way retires the session's Failed record with it, so no second,
        // generic card for the same session comes back.
        function test_failed_session_dismissed_then_panel_retry() {
            startTarget("saved", "usb");
            failByReceiver("usb");
            tryCompare(card(), "visible", true);
            card().dismiss();
            var retry = findChild(home(), "dongleRetry");
            tryCompare(retry, "visible", true);
            tap(retry);
            compare(testContext.dongleRetryRequests() > 0, true);
            compare(decoderHost.sessionState, 0, "the failure was acknowledged");
            verifyNoCard("the dismissed notice does not come back as a Retry card");
        }

        function test_failed_session_pill_tap_with_the_notice_up() {
            startTarget("saved", "usb");
            failByReceiver("usb");
            tryCompare(card(), "visible", true);
            tap(findChild(home(), "dongleStatusPill"));
            compare(decoderHost.sessionState, 0, "the failure was acknowledged");
            verifyNoCard("the notice gives way to the pill, not to a Retry card");
        }

        function test_recreated_failed_reconnect_leaves_no_dead_retry() {
            testContext.setLifecycleFailureKind(7);
            testContext.setLifecycleFailure(streamFailure);
            testContext.setLifecyclePhase(4);
            testContext.setReceiverHealth("failed", "usb");
            compare(app.attemptedSource, null);
            tryCompare(card(), "visible", true);
            compare(card().message, "RTL-SDR stopped sending samples, so the session stopped. Tap Reconnect to reset it, then start it again.");
            tap(findChild(card(), "failureRetry"));
            compare(testContext.lastDeviceAccess().count, 1);
            compare(decoderHost.sessionState, 0);
            verifyNoCard("no card whose Retry has nothing to retry");
        }

        function test_recreated_reconnect_only_asks_for_access() {
            // After an Activity recreation nothing names the session the
            // service ran: Reconnect asks for that receiver, and the notice
            // says to start it afterwards.
            unplug("airspy");
            testContext.setReceiverHealth("lost", "airspy");
            compare(app.attemptedSource, null);
            tryCompare(card(), "visible", true);
            compare(card().message, "Airspy was unplugged, so the session stopped. Plug it back in, tap Reconnect, then start it again.");
            var retry = findChild(card(), "failureRetry");
            compare(retry.text, "Reconnect");
            verify(retry.enabled);
            tap(retry);
            var access = testContext.lastDeviceAccess();
            compare(access.count, 1);
            compare(access.source, "airspy");
            compare(app.pendingRestart, null);
            compare(app.awaitingUsbAccess, false);
            connect("airspy");
            wait(100);
            compare(decoderHost.sessionState, 0, "access only: nothing starts");
        }

        function test_recreated_stream_failure_wording() {
            testContext.setReceiverHealth("failed", "usb");
            tryCompare(card(), "visible", true);
            compare(card().message, "RTL-SDR stopped sending samples, so the session stopped. Tap Reconnect to reset it, then start it again.");
        }

        // #782: only an RTL-SDR is reset on its next open; an Airspy's notice
        // promises no reset, and none blames the cable.
        function test_airspy_stream_failure_promises_no_reset() {
            testContext.setReceiverHealth("failed", "airspy");
            tryCompare(card(), "visible", true);
            compare(card().message, "Airspy stopped sending samples, so the session stopped. Tap Reconnect, then start it again.");
        }

        function test_airspy_signal_search_stream_failure() {
            connect("airspy");
            app.attemptedSource = {kind: "explore", uid: "", remember: false,
                system: {name: "Signal search", sourceType: "airspy", signalSearch: true}};
            home().signalSearch();
            verify(app.signalSearchOpen);
            testContext.setReceiverHealth("failed", "airspy");
            tryCompare(findChild(findChild(app, "signalSearchScreen"), "searchMessage"), "text",
                "Airspy stopped sending samples, so Signal search stopped. Start the search again.");
        }

        function test_signal_search_says_to_start_the_search_again() {
            connect("usb");
            app.attemptedSource = {kind: "explore", uid: "", remember: false,
                system: {name: "Signal search", sourceType: "usb", signalSearch: true}};
            home().signalSearch();
            verify(app.signalSearchOpen);
            testContext.setReceiverHealth("lost", "usb");
            var search = findChild(app, "signalSearchScreen");
            tryCompare(findChild(search, "searchMessage"), "text",
                "RTL-SDR was unplugged, so Signal search stopped. Plug it back in, then start the search again.");
            testContext.setReceiverHealth("failed", "usb");
            tryCompare(findChild(search, "searchMessage"), "text",
                "RTL-SDR stopped sending samples, so Signal search stopped. Start the search again to reset it.");
            // Closed, the search's ending is Listen's notice, with Reconnect.
            app.signalSearchOpen = false;
            testContext.setReceiverHealth("lost", "usb");
            tryCompare(card(), "visible", true);
            compare(card().message, "RTL-SDR was unplugged, so Signal search stopped. Plug it back in, then tap Reconnect.");
        }

        function test_airspy_failure_keeps_the_pill() {
            // B1 through Main's binding: a failed Airspy start keeps the pill.
            connect("airspy");
            var row = addSaved("Airspy system", "airspy");
            testContext.setScanTestAcceptStart(false);
            app.startSystem(row);
            tryCompare(card(), "visible", true);
            var pill = findChild(home(), "dongleStatusPill");
            verify(pill.visible);
            var top = pill.mapToItem(app.contentItem, 0, 0);
            verify(top.y >= 0 && top.y + pill.height <= app.height, "the pill is on screen");
            compare(findChild(home(), "dongleDetailsPanel").visible, false);
        }

        // Re-review N1: a start the host refuses over a run on screen with
        // nothing to publish (an entry point no guard covers) leaves that run
        // as it was. When the run then fails, the card names the run, not the
        // refused target, and Reconnect resumes the run.
        function test_refused_start_leaves_the_run_on_screen() {
            var uid = startTarget("saved", "usb");
            var other = addSaved("City DMR", "usb");
            testContext.setScanTestAcceptStart(false);
            app.startWithMap(savedSystems.get(other), other, false);
            compare(decoderHost.sessionState, 1, "the run on screen is still starting");
            compare(app.startError, "");
            compare(app.attemptedSource.uid, uid);
            verify(!app.showFailure, "no card over the run on screen");
            failByReceiver("usb");
            tryCompare(card(), "visible", true);
            compare(card().message, "RTL-SDR stopped sending samples, so County P25 stopped. Tap Reconnect to reset it and start again.");
            testContext.setScanTestAcceptStart(true);
            tap(findChild(card(), "failureRetry"));
            tryCompare(app, "awaitingUsbAccess", true);
            connect("usb");
            verifySameSession("saved", uid);
        }

        // #782: a failed, refused or undelivered start says what happened.
        // The host publishes the failure kind (kinds 8 and up are the
        // attempt's own); the card words it, never the generic line.
        function test_start_failure_says_what_happened_data() {
            return [
                {tag: "RTL-SDR open failed", source: "usb", kind: 7, error: "Decoder run failed (1); USB open/claim error -6",
                    text: "The receiver could not be opened or stopped receiving. Check its USB connection and settings."},
                {tag: "Airspy open failed", source: "airspy", kind: 7, error: "Decoder run failed (1)",
                    text: "The receiver could not be opened or stopped receiving. Check its USB connection and settings."},
                {tag: "refused, previous session stopping", source: "usb", kind: 8, error: "A previous session is still stopping",
                    text: "The previous session is still stopping. Tap Retry to start this one once it has stopped."},
                {tag: "not delivered", source: "usb", kind: 9, error: "The decoder service did not respond to the start",
                    text: "The decoder did not respond in time, so the session was not started. Tap Retry, or open Details if this keeps happening."}
            ];
        }
        function test_start_failure_says_what_happened(data) {
            startTarget("saved", data.source);
            testContext.setLifecycleFailureKind(data.kind);
            testContext.setLifecycleFailure(data.error);
            testContext.setLifecyclePhase(4);
            tryCompare(card(), "visible", true);
            compare(card().message, data.text);
            var retry = findChild(card(), "failureRetry");
            compare(retry.text, "Retry");
            verify(retry.enabled);
        }

        // #782: a start that could not take its USB receiver before the
        // session began, as the service reports it (kinds 10-12, with its own
        // text) and the broker then reads it (not ready, and why), for each
        // kind of start Listen makes.
        function receiverTakeFailure(kind, source) {
            var service = {10: "The USB receiver needs permission again", 11: "The USB receiver could not be reopened", 12: "The USB receiver is no longer connected"};
            var broker = {10: 4, 11: 2, 12: 3};
            testContext.setDongleStatus(false, broker[kind], receiverName(source) + " not ready");
            testContext.setLifecycleFailureKind(kind);
            testContext.setLifecycleFailure(service[kind]);
            testContext.setLifecyclePhase(4);
        }
        function test_receiver_take_failure_data() {
            var rows = [];
            var texts = {
                10: "%1 needs USB permission again, so the session was not started. Tap Retry, then allow access when asked.",
                11: "%1 could not be reopened, so the session was not started. Tap Retry, or unplug it and plug it back in if this keeps happening.",
                12: "%1 is no longer connected, so the session was not started. Plug it back in, then tap Retry."
            };
            for (var kind = 10; kind <= 12; ++kind) {
                for (var target of ["saved", "scan", "explore"]) {
                    for (var source of ["usb", "airspy"])
                        rows.push({tag: target + " " + source + " kind " + kind, target: target, source: source, kind: kind,
                            text: texts[kind].replace("%1", receiverName(source))});
                }
            }
            return rows;
        }
        function test_receiver_take_failure(data) {
            startTarget(data.target, data.source);
            receiverTakeFailure(data.kind, data.source);
            tryCompare(card(), "visible", true);
            compare(card().message, data.text);
            var retry = findChild(card(), "failureRetry");
            compare(retry.text, "Retry");
            verify(retry.enabled);
        }
        function test_receiver_take_failure_in_signal_search_data() {
            return [
                {tag: "permission", kind: 10, text: "RTL-SDR needs USB permission again, so Signal search was not started. Start the search again, then allow access when asked."},
                {tag: "reopen failed", kind: 11, text: "RTL-SDR could not be reopened, so Signal search was not started. Start the search again, or unplug it and plug it back in if this keeps happening."},
                {tag: "gone", kind: 12, text: "RTL-SDR is no longer connected, so Signal search was not started. Plug it back in, then start the search again."}
            ];
        }
        function test_receiver_take_failure_in_signal_search(data) {
            connect("usb");
            app.attemptedSource = {kind: "explore", uid: "", remember: false,
                system: {name: "Signal search", sourceType: "usb", signalSearch: true}};
            home().signalSearch();
            verify(app.signalSearchOpen);
            receiverTakeFailure(data.kind, "usb");
            var search = findChild(app, "signalSearchScreen");
            tryCompare(findChild(search, "searchMessage"), "text", data.text);
            // Closed, the failure is Listen's card, worded for its Retry.
            app.signalSearchOpen = false;
            tryCompare(card(), "visible", true);
            verify(card().message.indexOf("Tap Retry") >= 0 || card().message.indexOf("tap Retry") >= 0, card().message);
        }

        // #782: a start refused while the previous session stops (8) or never answered (9) is worded for Signal
        // search's screen too, which has no Retry or Details, never as the host's own text.
        function test_attempt_failure_in_signal_search_data() {
            return [
                {tag: "previous stopping", kind: 8, host: "A previous session is still stopping",
                    text: "The previous session is still stopping, so Signal search was not started. Start the search again once it has stopped."},
                {tag: "undelivered", kind: 9, host: "The decoder service did not respond to the start",
                    text: "The decoder did not respond in time, so Signal search was not started. Start the search again, or open Diagnostics in Settings if this keeps happening."}
            ];
        }
        function test_attempt_failure_in_signal_search(data) {
            connect("usb");
            app.attemptedSource = {kind: "explore", uid: "", remember: false,
                system: {name: "Signal search", sourceType: "usb", signalSearch: true}};
            home().signalSearch();
            verify(app.signalSearchOpen);
            testContext.setLifecycleFailureKind(data.kind);
            testContext.setLifecycleFailure(data.host);
            testContext.setLifecyclePhase(4);
            var search = findChild(app, "signalSearchScreen");
            tryCompare(findChild(search, "searchMessage"), "text", data.text);
            app.signalSearchOpen = false;
            tryCompare(card(), "visible", true);
            verify(card().message.indexOf("Tap Retry") >= 0, card().message);
        }

        function test_dismiss_gives_up_the_attempt() {
            startTarget("saved", "usb");
            testContext.setLifecycleFailureKind(9);
            testContext.setLifecycleFailure("The decoder service did not respond to the start");
            testContext.setLifecyclePhase(4);
            tryCompare(card(), "visible", true);
            var dismissals = decoderHost.dismissCalls();
            card().dismiss();
            compare(decoderHost.dismissCalls(), dismissals + 1, "the host hears of the dismissal");
            tryCompare(card(), "visible", false);
        }

        function test_failure_reveals_the_top_data() {
            return [
                {tag: "brokered", brokered: true, source: "usb"},
                {tag: "desktop", brokered: false, source: "rtltcp"}
            ];
        }
        function test_failure_reveals_the_top(data) {
            // B5: on a brokered host a failure scrolls Home back to where it is
            // reported; the desktop keeps the user's place.
            if (data.brokered)
                connect("usb");
            else
                useHost(false);
            for (var i = 0; i < 14; ++i)
                addSaved("System " + i, data.source);
            var body = findChild(home(), "dongleScrollBody");
            verify(waitForPolish(home()));
            tryVerify(function () { return body.contentHeight > body.height + 200; });
            var end = body.originY + body.contentHeight - body.height;
            body.contentY = end;
            testContext.setScanTestAcceptStart(false);
            app.startSystem(0);
            tryCompare(card(), "visible", true);
            if (data.brokered)
                compare(body.contentY, body.originY);
            else
                compare(body.contentY, end);
        }

        function test_auto_start_waits_out_a_pending_request() {
            var row = addSaved("County P25", "usb");
            prefs.lastStartedKind = "saved";
            prefs.lastStartedUid = savedSystems.get(row).uid;
            prefs.autoStartOnAttach = true;
            connect("usb");
            tryCompare(uiController, "autoStartBlocked", false);
            // A permission prompt of its own is up: the attachment is consumed.
            testContext.setLocalDeviceRecord("usb", true, "usb");
            compare(uiController.autoStartBlocked, true);
            testContext.emitLocalDeviceAttached("usb");
            testContext.setLocalDeviceRecord("usb", false, "usb");
            tryCompare(uiController, "autoStartBlocked", false);
            wait(100);
            compare(decoderHost.sessionState, 0, "a suppressed attachment is not retried");
            testContext.emitLocalDeviceAttached("usb");
            compare(decoderHost.sessionState, 1);
        }

        function test_auto_start_follows_the_attached_kind_data() {
            return [
                {tag: "Airspy target, RTL-SDR then Airspy", target: "airspy", order: ["usb", "airspy"]},
                {tag: "RTL-SDR target, Airspy then RTL-SDR", target: "usb", order: ["airspy", "usb"]}
            ];
        }
        function test_auto_start_follows_the_attached_kind(data) {
            var row = addScanList("Night scan", data.target);
            prefs.lastStartedKind = "scan";
            prefs.lastStartedUid = scanLists.get(row).uid;
            prefs.autoStartOnAttach = true;
            testContext.setAttachedDeviceSource("both");
            connect(data.target);
            tryCompare(uiController, "autoStartBlocked", false);
            testContext.emitLocalDeviceAttached(data.order[0]);
            compare(decoderHost.sessionState, 0, "another receiver's attachment starts nothing");
            testContext.emitLocalDeviceAttached(data.order[1]);
            compare(decoderHost.sessionState, 1);
            verify(app.sessionScanList);
        }
    }
}
