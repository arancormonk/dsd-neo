// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// Issue #801: pinned Listen shortcuts, and the one saved-system start gate every start passes through: card, site
// chooser, auto-start, Retry, a pin and a USB grant all refuse an avoided or removed site the same way, and a refusal
// replaces whatever start was waiting.
Item {
    id: root

    width: 420
    height: 900

    Loader {
        id: appLoader

        source: uiDir + "/Main.qml"
    }

    TestCase {
        id: tc

        name: "HomePinned"
        when: windowShown && appLoader.status === Loader.Ready

        property var app: appLoader.item
        readonly property string removedText: "This source was removed. Choose another source."
        readonly property string draftText: "This scan list is a draft. Edit and save it before listening."
        property var saved: ({})

        function avoidedText(name) {
            return "“" + name + "” is avoided. Remove the avoid in its site list to start it.";
        }

        function init() {
            testContext.useLifecycleHost(true);
            testContext.resetPins();
            testContext.setScanTestUsb(false, false);
            testContext.resetDeviceAccess();
            tc.saved = {
                onboardingDone: prefs.onboardingDone,
                autoStartOnAttach: prefs.autoStartOnAttach,
                exploreSourceType: prefs.exploreSourceType,
                exploreHost: prefs.exploreHost,
                explorePort: prefs.explorePort,
                exploreFreqMhz: prefs.exploreFreqMhz
            };
            prefs.onboardingDone = true;
            prefs.autoStartOnAttach = false;
            prefs.lastStartedKind = "";
            prefs.lastStartedUid = "";
            while (savedSystems.count)
                savedSystems.remove(0);
            while (scanLists.count)
                scanLists.remove(0);
            app.currentTab = 0;
            app.startError = "";
            app.attemptedSource = null;
            app.dismissedFailure = "";
        }

        function cleanup() {
            findChild(app, "siteChooserSheet").visible = false;
            app.signalSearchOpen = false;
            app.exploreSetupOpen = false;
            app.scanListOpen = false;
            decoderHost.stop();
            testContext.setLifecyclePhase(0);
            app.awaitingUsbAccess = false;
            app.pendingStart = null;
            app.pendingStartRow = -1;
            app.startError = "";
            app.attemptedSource = null;
            app.dismissedFailure = "";
            testContext.setScanTestUsb(false, false);
            testContext.setLocalDeviceRecord("usb", false, "");
            testContext.setScanTestAcceptStart(true);
            prefs.onboardingDone = tc.saved.onboardingDone;
            prefs.autoStartOnAttach = tc.saved.autoStartOnAttach;
            prefs.exploreSourceType = tc.saved.exploreSourceType;
            prefs.exploreHost = tc.saved.exploreHost;
            prefs.explorePort = tc.saved.explorePort;
            prefs.exploreFreqMhz = tc.saved.exploreFreqMhz;
            prefs.lastStartedKind = "";
            prefs.lastStartedUid = "";
            while (savedSystems.count)
                savedSystems.remove(0);
            while (scanLists.count)
                scanLists.remove(0);
            testContext.resetPins();
            testContext.useLifecycleHost(false);
        }

        function addSystem(name, sourceType, fields) {
            var system = Object.assign({name: name, sourceType: sourceType, freqMhz: "851.5"},
                sourceType === "rtltcp" ? {host: "127.0.0.1", port: 1234} : {}, fields || {});
            verify(savedSystems.add(system), "could not add " + name);
            return savedSystems.get(savedSystems.count - 1).uid;
        }
        // Two sites of one RadioReference system, North then South.
        function addGroup(sourceType) {
            return [addSystem("County", sourceType, {siteName: "North", rrSid: 12, rrSiteId: 100}),
                addSystem("County", sourceType, {siteName: "South", freqMhz: "852.5", rrSid: 12, rrSiteId: 101})];
        }
        function addList(name, fields) {
            verify(scanLists.add(Object.assign({name: name, sourceType: "rtltcp", host: "127.0.0.1", port: 1234,
                entries: [{kind: "freq", name: "Simplex", protocol: "p25", freqMhz: "851.5", enabled: true}]},
                fields || {})), "could not add " + name);
            return scanLists.get(scanLists.count - 1).uid;
        }
        function avoid(uid) {
            verify(savedSystems.setAvoidSite(savedSystems.rowForUid(uid), true));
        }
        function compareRefused(message, attemptUid) {
            compare(decoderHost.sessionState, 0, "nothing started");
            compare(app.startError, message);
            verify(app.showFailure, "the refusal is on screen");
            if (attemptUid === null)
                compare(app.attemptedSource, null, "a refusal of nothing names no attempt");
            else
                compare(app.attemptedSource.uid, attemptUid);
        }
        function sessionUid() {
            return app.sessionSystem ? app.sessionSystem.uid : "";
        }

        // B2: auto-start on attach starts the last-started site even after it was avoided.
        function test_auto_start_refuses_avoided_site() {
            var sites = addGroup("rtltcp");
            avoid(sites[1]);
            uiController.requestAutoStart("saved", sites[1]);
            compareRefused(avoidedText("County"), sites[1]);
            compare(app.attemptedSource.kind, "saved");
            // Read and dismissed, the same refusal is shown again the next time.
            app.dismissedFailure = app.failureText;
            verify(!app.showFailure);
            uiController.requestAutoStart("saved", sites[1]);
            compareRefused(avoidedText("County"), sites[1]);
        }

        // B6: a saved start that waits on USB access starts what is saved when access arrives, by uid.
        function test_usb_grant_revalidates_saved_start_data() {
            return [{tag: "removed"}, {tag: "avoided"}, {tag: "reindexed"}, {tag: "receiver edited"},
                {tag: "Airspy serial edited"}];
        }
        function test_usb_grant_revalidates_saved_start(data) {
            if (data.tag === "Airspy serial edited") {
                grantAfterSerialEdit();
                return;
            }
            var earlier = addSystem("Earlier", "usb");
            var sites = addGroup("usb");
            var north = sites[0];
            testContext.setScanTestUsb(true, false);
            app.startSystem(savedSystems.rowForUid(north));
            verify(app.awaitingUsbAccess, "the start waits on USB access");
            compare(decoderHost.sessionState, 0);
            if (data.tag === "removed")
                verify(savedSystems.remove(savedSystems.rowForUid(north)));
            else if (data.tag === "avoided")
                avoid(north);
            else if (data.tag === "reindexed")
                verify(savedSystems.remove(savedSystems.rowForUid(earlier)));
            else
                verify(savedSystems.update(savedSystems.rowForUid(north), {sourceType: "rtltcp", host: "127.0.0.1", port: 1234}));
            testContext.setScanTestUsb(true, true);
            verify(!app.awaitingUsbAccess);
            if (data.tag === "removed") {
                compareRefused(removedText, null);
                return;
            }
            if (data.tag === "avoided") {
                compareRefused(avoidedText("County"), north);
                return;
            }
            compare(decoderHost.sessionState, 1, "the saved system started");
            compare(sessionUid(), north);
            compare(app.sessionRow, savedSystems.rowForUid(north), "the session names the row the system is at now");
            compare(app.sessionSystem.sourceType, data.tag === "receiver edited" ? "rtltcp" : "usb");
        }

        // An Airspy start waits on access to its serial. The serial edited meanwhile is the one the grant asks for:
        // the fixture host reports no serial open, so the edited start waits again, for the new serial.
        function grantAfterSerialEdit() {
            var uid = addSystem("Airspy site", "airspy", {airspy: {serial: "AAAA1111"}});
            testContext.setLocalDeviceRecord("airspy", false, "");
            testContext.setScanTestUsb(true, false);
            app.startSystem(savedSystems.rowForUid(uid));
            verify(app.awaitingUsbAccess);
            compare(testContext.lastDeviceAccess().serial, "AAAA1111");
            verify(savedSystems.update(savedSystems.rowForUid(uid), {airspy: {serial: "BBBB2222"}}));
            testContext.setScanTestUsb(true, true);
            compare(testContext.lastDeviceAccess().serial, "BBBB2222", "the grant asks for the serial saved now");
            verify(app.awaitingUsbAccess);
            compare(app.pendingStart.airspy.serial, "BBBB2222");
            compare(app.attemptedSource.system.airspy.serial, "BBBB2222");
        }

        // [r2-3] A refused start replaces a start still waiting on USB access: a late grant starts nothing.
        function test_refusal_clears_pending_usb_start_data() {
            return [{tag: "card"}, {tag: "pin"}];
        }
        function test_refusal_clears_pending_usb_start(data) {
            var waiting = addSystem("Waiting", "usb");
            var sites = addGroup("rtltcp");
            avoid(sites[1]);
            testContext.setScanTestUsb(true, false);
            app.startSystem(savedSystems.rowForUid(waiting));
            verify(app.awaitingUsbAccess);
            if (data.tag === "card")
                app.startSystem(savedSystems.rowForUid(sites[1]));
            else
                app.startPin("site", sites[1]);
            compareRefused(avoidedText("County"), sites[1]);
            verify(!app.awaitingUsbAccess, "the earlier wait is over");
            compare(app.pendingStart, null);
            testContext.setScanTestUsb(true, true);
            compareRefused(avoidedText("County"), sites[1]);
            verify(sessionUid() !== waiting, "the waiting system did not start");
        }

        // [r2-5] A refusal is revealed with the body scrolled, on both hosts, also over a failure already showing.
        function test_refusal_revealed_when_scrolled_data() {
            return [{tag: "desktop", brokered: false, replacing: false},
                {tag: "desktop replacing a failure", brokered: false, replacing: true},
                {tag: "brokered", brokered: true, replacing: false},
                {tag: "brokered replacing a failure", brokered: true, replacing: true}];
        }
        function test_refusal_revealed_when_scrolled(data) {
            for (var i = 0; i < 12; ++i)
                addSystem("Filler " + i, "rtltcp");
            var sites = addGroup("rtltcp");
            avoid(sites[1]);
            testContext.setScanTestUsb(data.brokered, data.brokered);
            if (data.replacing)
                app.startError = "An earlier failure.";
            var home = findChild(app, "homeScreen");
            tryCompare(home, "visible", true);
            var body = findChild(home, "dongleScrollBody");
            waitForRendering(home);
            body.contentY = body.contentHeight - body.height;
            verify(body.contentY > body.originY + 100, "the body scrolled");
            app.startSystem(savedSystems.rowForUid(sites[1]));
            compareRefused(avoidedText("County"), sites[1]);
            compare(body.contentY, body.originY, "the failure card is in view");
        }

        // D7 and [r2-4]: a draft list is refused without a start; Edit source opens it, Retry starts it once saved.
        function test_draft_refusal_edit_and_retry() {
            var uid = addList("Draft", {sourceType: "usb", isDraft: true});
            compare(scanListStarter.draftRefusal, draftText);
            testContext.setScanTestUsb(true, false);
            app.startPin("scan", uid);
            compareRefused(draftText, uid);
            compare(app.attemptedSource.kind, "scan");
            verify(!app.awaitingUsbAccess, "a draft asks for no receiver");
            compare(testContext.lastDeviceAccess().count, 0);

            // Retry while it is still a draft refuses again, still without asking for the receiver.
            app.startError = "";
            app.retrySource();
            tryCompare(app, "startError", draftText);
            compare(decoderHost.sessionState, 0);
            compare(testContext.lastDeviceAccess().count, 0);

            app.editFailedSource();
            verify(app.scanListOpen, "Edit source opens the list");
            compare(findChild(app, "scanListScreen").editUid, uid);
            app.scanListOpen = false;

            verify(scanLists.update(scanLists.rowForUid(uid), {isDraft: false}));
            testContext.setScanTestUsb(true, true);
            app.retrySource();
            tryCompare(decoderHost, "sessionState", 1);
            verify(app.sessionScanList);
            compare(app.startError, "");
        }

        // §5: each kind does what its source does, resolved at the tap.
        function test_start_pin_each_kind_data() {
            return [{tag: "system"}, {tag: "site"}, {tag: "scan list alone"}, {tag: "signal search"},
                {tag: "explore"}, {tag: "explore not set up"}, {tag: "missing system"}, {tag: "missing site"},
                {tag: "missing scan list"}, {tag: "transitioning"}];
        }
        function test_start_pin_each_kind(data) {
            var solo = addSystem("Solo", "rtltcp");
            var sites = addGroup("rtltcp");
            var list = addList("Patrol");
            addList("Other");
            prefs.exploreSourceType = data.tag === "explore" ? "rtltcp" : "";
            prefs.exploreHost = "127.0.0.1";
            prefs.explorePort = 1234;
            prefs.exploreFreqMhz = "851.5";
            if (data.tag === "system") {
                app.startPin("system", solo);
                compare(decoderHost.sessionState, 1);
                compare(sessionUid(), solo);
            } else if (data.tag === "site") {
                app.startPin("site", sites[1]);
                compare(decoderHost.sessionState, 1);
                compare(sessionUid(), sites[1]);
            } else if (data.tag === "scan list alone") {
                app.startPin("scan", list);
                compare(decoderHost.sessionState, 1);
                verify(app.sessionScanList);
                compare(app.sessionStartUid, list);
                compare(app.sessionStartCompanions.length, 0, "D6: a pinned list starts alone");
                verify(testContext.lastStartArgs().indexOf("--trunk-scan") >= 0);
            } else if (data.tag === "signal search") {
                app.startPin("search", "");
                verify(app.signalSearchOpen);
                compare(decoderHost.sessionState, 0);
            } else if (data.tag === "explore") {
                app.startPin("explore", "");
                compare(decoderHost.sessionState, 1);
                verify(app.exploring);
            } else if (data.tag === "explore not set up") {
                app.startPin("explore", "");
                verify(app.exploreSetupOpen);
                compare(decoderHost.sessionState, 0);
            } else if (data.tag === "missing system" || data.tag === "missing site") {
                app.startPin(data.tag === "missing system" ? "system" : "site", "6f1c2a1e-0000-4000-8000-000000000000");
                compareRefused(removedText, null);
            } else if (data.tag === "missing scan list") {
                app.startPin("scan", "gone");
                compareRefused(removedText, null);
            } else {
                testContext.setLifecyclePhase(1);
                verify(decoderHost.transitioning);
                for (var kind of ["system", "site", "scan", "search", "explore"])
                    app.startPin(kind, kind === "scan" ? list : solo);
                compare(app.startError, "");
                verify(!app.signalSearchOpen);
                verify(!app.exploreSetupOpen);
                compare(app.attemptedSource, null, "nothing was attempted");
            }
        }

        // The scan gate has the saved gate's transition guard: while a session runs or changes state, neither a list
        // nor a refusal goes through, and nothing waiting is dropped.
        function test_scan_gate_waits_out_a_session_data() {
            return [{tag: "starting", phase: 1}, {tag: "running", phase: 2}, {tag: "stopping", phase: 3}];
        }
        function test_scan_gate_waits_out_a_session(data) {
            var ready = addList("Ready");
            var draft = addList("Draft", {isDraft: true});
            testContext.setLifecyclePhase(data.phase);
            app.pendingRestart = {kind: "scan", uid: ready};
            for (var uid of [ready, draft, "gone"])
                app.startScanByUid(uid, [], "");
            compare(decoderHost.sessionState, data.phase);
            compare(app.startError, "", "nothing was refused");
            compare(app.attemptedSource, null, "nothing was attempted");
            verify(app.pendingRestart !== null, "the waiting restart is kept");
            app.pendingRestart = null;
        }

        // D4: a pinned grouped system plays its one playable site, else opens the site chooser.
        function test_start_pin_grouped_playable_counts_data() {
            return [{tag: "none playable", avoided: [0, 1]}, {tag: "one playable", avoided: [1]},
                {tag: "two playable", avoided: []}];
        }
        function test_start_pin_grouped_playable_counts(data) {
            var sites = addGroup("rtltcp");
            for (var index of data.avoided)
                avoid(sites[index]);
            var chooser = findChild(app, "siteChooserSheet");
            // The pin names the avoided member: what plays is the group's playable site.
            app.startPin("system", sites[1]);
            if (data.avoided.length === 1) {
                compare(decoderHost.sessionState, 1);
                compare(sessionUid(), sites[0]);
                verify(!chooser.visible);
            } else {
                compare(decoderHost.sessionState, 0);
                verify(chooser.visible, "the site chooser opens");
                compare(chooser.groupUid, sites[1]);
                compare(app.startError, "");
            }
        }
    }
}
