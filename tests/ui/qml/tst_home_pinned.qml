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
            findChild(app, "homeScreen").showSection("systems");
            findChild(app, "homeScreen").resetScrollMemory();
        }

        function cleanup() {
            for (var modal of Ui.Navigation.modals.slice())
                modal.visible = false;
            findChild(app, "siteChooserSheet").visible = false;
            findChild(app, "pinnedSheet").visible = false;
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
            testContext.setTerminalReason(0);
            app.completionMessage = "";
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

        // Auto-start of a last-started scan list that became a draft goes through the scan gate: refused as its card
        // refuses it, before any USB request.
        function test_auto_start_refuses_draft_list() {
            var other = addList("Other");
            var uid = addList("Draft", {sourceType: "usb", isDraft: true});
            testContext.setScanTestUsb(true, false);
            uiController.requestAutoStart("scan", uid, [other]);
            compareRefused(draftText, uid);
            compare(app.attemptedSource.kind, "scan");
            compare(app.attemptedSource.companions, [other], "Retry starts the list with what it was started with");
            verify(!app.awaitingUsbAccess, "a draft asks for no receiver");
            compare(testContext.lastDeviceAccess().count, 0);
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
        // Issue #646 B3: when the refusal lands, either a tab's place is still owed or a keyboard focus reveal is still
        // pending; revealTop() drops either.
        function test_refusal_revealed_when_scrolled_data() {
            var rows = [];
            for (var pending of ["restore", "reveal"]) {
                rows.push({tag: "desktop, " + pending + " pending", brokered: false, replacing: false, pending: pending},
                    {tag: "desktop replacing a failure, " + pending + " pending", brokered: false, replacing: true, pending: pending},
                    {tag: "brokered, " + pending + " pending", brokered: true, replacing: false, pending: pending},
                    {tag: "brokered replacing a failure, " + pending + " pending", brokered: true, replacing: true, pending: pending});
            }
            return rows;
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
            var plays = [];
            (function look(node) {
                for (var child of node.children) {
                    if (child.objectName === "savedSystemPlay" && child.visible)
                        plays.push(child);
                    look(child);
                }
            })(findChild(home, "listenSystemsSection"));
            if (data.pending === "restore") {
                home.showSection("explore");
                home.showSection("systems");
                verify(home.restorePending, "Systems' place is owed");
            } else {
                // A Play out of view both here and at the top: its reveal would scroll the failure card away.
                var away = plays.filter(function (play) {
                    var y = play.mapToItem(body.contentItem, 0, 0).y;
                    return y + play.height < body.contentY - 100 && y > body.originY + body.height + 100;
                });
                verify(away.length > 0, "a Play out of view at both places");
                // The keyboard is in Main's window, on Home (outside the body, so nothing is revealed yet).
                app.requestActivate();
                tryCompare(app, "active", true);
                tryVerify(function () {
                    home.forceActiveFocus();
                    return home.activeFocus;
                }, 2000, "Main's window takes the keyboard");
                var at = body.contentY;
                away[0].forceActiveFocus();
                compare(body.contentY, at, "the reveal has not run yet");
                verify(!home.restorePending && away[0].activeFocus, "a focus reveal is pending");
            }
            app.startSystem(savedSystems.rowForUid(sites[1]));
            compareRefused(avoidedText("County"), sites[1]);
            compare(body.contentY, body.originY, "the failure card is in view");
            verify(waitForPolish(home));
            waitForRendering(home);
            compare(body.contentY, body.originY, "and stays in view once the deferred scrolls have run");
            // A focus handed back afterwards, as a sheet does to its opener, scrolls nothing either.
            plays[plays.length - 2].forceActiveFocus();
            verify(waitForPolish(home));
            waitForRendering(home);
            compare(body.contentY, body.originY, "a later focus hand-back leaves the failure card in view");
            verify(findChild(home, "homeFailureCard").visible);
            root.forceActiveFocus();
        }

        // A refused pin start is the newest attempt: a finished replay's card goes, so its Play again cannot retry the
        // refused target in place of the recording.
        function test_refusal_replaces_replay_completion_data() {
            return [{tag: "avoided site"}, {tag: "draft list"}, {tag: "removed target"}];
        }
        function test_refusal_replaces_replay_completion(data) {
            var recording = testContext.writeFixtureCsv("pinned-replay.wav", "RIFF");
            var replay = addSystem("Replay", "file", {filePath: recording});
            var sites = addGroup("rtltcp");
            avoid(sites[1]);
            var draft = addList("Draft", {isDraft: true});
            app.startSystem(savedSystems.rowForUid(replay));
            compare(decoderHost.sessionState, 1, "the replay started");
            testContext.setTerminalReason(1);
            decoderHost.stop();
            compare(app.completionMessage, "Replay completed: pinned-replay.wav");
            var home = findChild(app, "homeScreen");
            tryCompare(home, "visible", true);
            var card = findChild(home, "homeCompletionCard");
            verify(card.visible, "the replay's card shows");
            verify(findChild(card, "homeReplayAgain").visible);

            var refusal = data.tag === "avoided site" ? avoidedText("County")
                : data.tag === "draft list" ? draftText : removedText;
            if (data.tag === "avoided site")
                app.startPin("site", sites[1]);
            else if (data.tag === "draft list")
                app.startPin("scan", draft);
            else
                app.startPin("system", "6f1c2a1e-0000-4000-8000-000000000000");
            compareRefused(refusal, data.tag === "avoided site" ? sites[1] : data.tag === "draft list" ? draft : null);
            compare(app.completionMessage, "", "the replay's completion is gone");
            verify(!card.visible, "no Play again for the refused target");
            var failure = findChild(home, "homeFailureCard");
            verify(failure.visible);
            compare(failure.message, refusal);
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

        // §6: a pin on Home's strip goes to Main's startPin(), which starts it as its card would.
        function stripPin(key) {
            var strip = findChild(app, "pinnedStrip");
            for (var child of strip.children) {
                if (child.objectName === "pinnedStripPin" && child.pinKey === key && child.opacity > 0)
                    return child;
            }
            return null;
        }
        function test_strip_pin_starts_through_main() {
            var solo = addSystem("Solo", "rtltcp");
            var list = addList("Patrol");
            compare(listenPins.pin("scan", list), "");
            compare(listenPins.pin("system", solo), "");
            var home = findChild(app, "homeScreen");
            tryCompare(home, "visible", true);
            waitForRendering(home);
            var button = stripPin("system:" + solo);
            verify(button !== null, "the pin is on the strip");
            mouseClick(button);
            compare(decoderHost.sessionState, 1);
            compare(sessionUid(), solo);
            compare(prefs.lastStartedUid, "", "a pin records nothing until the session initializes, as a card");
        }

        // D9: a start from the Pinned sheet closes the sheet first, so the site chooser it may open is on top.
        function test_sheet_start_closes_first() {
            var sites = addGroup("rtltcp");
            compare(listenPins.pin("system", sites[0]), "");
            var home = findChild(app, "homeScreen");
            tryCompare(home, "visible", true);
            waitForRendering(home);
            mouseClick(findChild(home, "pinnedStripMore"));
            var sheet = findChild(app, "pinnedSheet");
            tryCompare(sheet, "visible", true);
            verify(home.managementSheetOpen);
            waitForRendering(sheet);
            verify(findChild(sheet, "pinnedSheetHint").text.startsWith("You can pin up to 6. "),
                   "the hint names the cap the full refusal names");
            mouseClick(findChild(sheet, "pinnedSheetStart"));
            verify(!sheet.visible, "the sheet closed");
            var chooser = findChild(app, "siteChooserSheet");
            verify(chooser.visible, "two playable sites: the chooser opens");
            compare(chooser.groupUid, sites[0]);
            compare(decoderHost.sessionState, 0);
        }

        // Task 6: Pin to Listen in each source. Home's controls, found by what they are.
        function homeItem(name) {
            var found = findChild(findChild(app, "homeScreen"), name);
            verify(found !== null, name + " exists");
            return found;
        }
        function visualChild(root, predicate) {
            if (predicate(root))
                return root;
            var children = root.contentItem ? [root.contentItem] : root.children || [];
            for (var child of children) {
                var found = visualChild(child, predicate);
                if (found)
                    return found;
            }
            return null;
        }
        function showHome() {
            var home = findChild(app, "homeScreen");
            tryCompare(home, "visible", true);
            verify(waitForPolish(app.contentItem));
            waitForRendering(home);
            return home;
        }
        // Issue #646: taps Home's Listen tab @p id, as a finger does, and waits for its section.
        function showTab(id) {
            var home = showHome();
            var tab = homeItem("listenTab_" + id);
            mouseClick(tab, tab.width / 2, tab.height / 2);
            compare(home.section, id);
            verify(waitForPolish(app.contentItem));
            waitForRendering(home);
        }
        // Scrolls Home's body to @p control and taps it.
        function tapOnHome(control) {
            var scroll = homeItem("dongleScrollBody");
            var top = control.mapToItem(scroll.contentItem, 0, 0).y;
            scroll.contentY = Math.max(scroll.originY, Math.min(top - 8, scroll.originY + scroll.contentHeight - scroll.height));
            verify(waitForPolish(app.contentItem));
            waitForRendering(control);
            mouseClick(control, control.width / 2, control.height / 2);
        }
        function cardMore(name) {
            var more = visualChild(findChild(app, "homeScreen"), function (entry) {
                return entry.icon === "more" && entry.accessibleName === "More options for " + name && entry.visible;
            });
            verify(more !== null, "More for " + name);
            return more;
        }
        function keys(menu) {
            return menu.actions.map(function (action) { return action.key; }).join();
        }
        function openMenu(opener, menuName) {
            tapOnHome(opener);
            var menu = homeItem(menuName);
            tryCompare(menu, "visible", true);
            return menu;
        }
        // The Pin row of an open menu, checked for what it says, then tapped.
        function tapPin(menu, objectName, text, accessibleName) {
            var pin = findChild(menu, objectName);
            verify(pin !== null, objectName);
            compare(pin.text, text);
            compare(pin.accessibleName, accessibleName);
            verify(pin.enabled);
            verify(waitForPolish(app.contentItem));
            waitForRendering(pin);
            mouseClick(pin, pin.width / 2, pin.height / 2);
            tryCompare(menu, "visible", false);
        }
        function comparePinned(keys, why) {
            compare(listenPins.entries.map(function (entry) { return entry.key; }).join(), keys, why);
            compare(decoderHost.sessionState, 0, "pinning starts nothing");
            compare(prefs.lastStartedKind, "", "pinning records no start");
            compare(prefs.lastStartedUid, "");
        }

        // D4: a system card's menu pins the system, through any site of its group, and unpins it again.
        function test_pin_from_system_menu() {
            var solo = addSystem("Solo", "rtltcp");
            var sites = addGroup("rtltcp");
            showHome();
            var menu = openMenu(cardMore("Solo"), "savedSystemManageSheet");
            compare(keys(menu), "edit,pin,remove");
            tapPin(menu, "pinSavedSystemButton", "Pin to Listen", "Pin Solo to Listen");
            comparePinned("system:" + solo);
            verify(findChild(app, "pinnedStrip").shown, "the strip shows the pin");

            menu = openMenu(cardMore("County"), "savedSystemManageSheet");
            compare(keys(menu), "edit,sites,pin,rrUpdate,remove");
            tapPin(menu, "pinSavedSystemButton", "Pin to Listen", "Pin County to Listen");
            comparePinned("system:" + solo + ",system:" + sites[0], "the group's card pins its lead");
            verify(listenPins.isPinned("system", sites[1]), "every site of the group shows the card pinned");

            menu = openMenu(cardMore("County"), "savedSystemManageSheet");
            tapPin(menu, "pinSavedSystemButton", "Unpin from Listen", "Unpin County from Listen");
            comparePinned("system:" + solo);
            menu = openMenu(cardMore("Solo"), "savedSystemManageSheet");
            tapPin(menu, "pinSavedSystemButton", "Unpin from Listen", "Unpin Solo from Listen");
            comparePinned("");
            verify(!findChild(app, "pinnedStrip").shown);
        }

        function test_pin_from_scan_menu() {
            var list = addList("Patrol");
            showTab("scanLists");
            var menu = openMenu(homeItem("scanListManageButton"), "scanListManageSheet");
            compare(keys(menu), "edit,pin,remove");
            tapPin(menu, "pinScanListButton", "Pin to Listen", "Pin Patrol to Listen");
            comparePinned("scan:" + list);
            var other = addList("Other");
            menu = openMenu(homeItem("scanListManageButton"), "scanListManageSheet");
            compare(keys(menu), "edit,together,pin,remove");
            tapPin(menu, "pinScanListButton", "Unpin from Listen", "Unpin Patrol from Listen");
            comparePinned("");
            verify(!listenPins.isPinned("scan", other));
        }

        // B4: one system saved for USB and for rtl_tcp is two cards of two sites each, and a pin through one card's
        // menu pins only that card.
        function test_receivers_are_separate_cards() {
            var usb = addGroup("usb");
            var tcp = addGroup("rtltcp");
            var home = showHome();
            var cardOf = function (uid) {
                return visualChild(home, function (entry) {
                    // The card's entry names the group by its lead site.
                    return entry.objectName === "savedSystemManageButton" && entry.visible
                        && entry.parent.parent.leadUid === uid;
                });
            };
            var siteButtons = [];
            visualChild(home, function (entry) {
                if (entry.objectName === "homeSiteChooserButton" && entry.visible)
                    siteButtons.push(entry.text);
                return false;
            });
            compare(siteButtons, ["2 sites", "2 sites"], "a card per receiver, each with its own sites");
            verify(cardOf(usb[0]) !== null && cardOf(tcp[0]) !== null, "both receivers' cards");
            var menu = openMenu(cardOf(tcp[0]), "savedSystemManageSheet");
            tapPin(menu, "pinSavedSystemButton", "Pin to Listen", "Pin County to Listen");
            comparePinned("system:" + tcp[0]);
            verify(listenPins.isPinned("system", tcp[1]), "the rtl_tcp card is pinned through either site");
            verify(!listenPins.isPinned("system", usb[0]) && !listenPins.isPinned("system", usb[1]),
                "the USB card is not");
            menu = openMenu(cardOf(usb[0]), "savedSystemManageSheet");
            compare(findChild(menu, "pinSavedSystemButton").text, "Pin to Listen", "the USB card offers its own pin");
        }

        // Review focus 2: a pinned site edited out of its group is its own card, whose menu unpins it.
        function test_standalone_site_card_unpins() {
            var sites = addGroup("rtltcp");
            compare(listenPins.pin("site", sites[1]), "");
            verify(savedSystems.update(savedSystems.rowForUid(sites[1]), {freqMhz: "853.5"}));
            compare(savedSystems.siblingRows(savedSystems.rowForUid(sites[1])).length, 0, "the site left its group");
            showHome();
            var more = visualChild(findChild(app, "homeScreen"), function (entry) {
                return entry.objectName === "savedSystemManageButton" && entry.visible
                    && entry.parent.parent.leadUid === sites[1];
            });
            verify(more !== null, "the standalone card's More");
            var menu = openMenu(more, "savedSystemManageSheet");
            compare(keys(menu), "edit,pin,remove");
            tapPin(menu, "pinSavedSystemButton", "Unpin from Listen", "Unpin County from Listen");
            comparePinned("", "the site's pin is gone");
        }

        // D5: Explore's More and Signal search's More each open a menu that pins it; Edit connection opens setup.
        function test_pin_explore_and_search_menus() {
            prefs.exploreSourceType = "rtltcp";
            showTab("explore");
            var searchMore = homeItem("signalSearchManageButton");
            compare(searchMore.accessibleName, "More options for Signal search");
            var menu = openMenu(searchMore, "signalSearchManageSheet");
            compare(menu.title, "Signal search");
            compare(keys(menu), "pin");
            tapPin(menu, "pinSignalSearchButton", "Pin to Listen", "Pin Signal search to Listen");
            comparePinned("search:");
            verify(!app.signalSearchOpen, "the menu's More opened nothing else");

            var exploreMore = homeItem("exploreManageButton");
            compare(exploreMore.accessibleName, "More options for Explore");
            menu = openMenu(exploreMore, "exploreManageSheet");
            compare(keys(menu), "edit,pin");
            compare(findChild(menu, "editExploreConnectionButton").text, "Edit connection");
            tapPin(menu, "pinExploreButton", "Pin to Listen", "Pin Explore to Listen");
            comparePinned("search:,explore:");
            verify(!app.exploring, "More never starts Explore");

            menu = openMenu(searchMore, "signalSearchManageSheet");
            tapPin(menu, "pinSignalSearchButton", "Unpin from Listen", "Unpin Signal search from Listen");
            menu = openMenu(exploreMore, "exploreManageSheet");
            tapPin(menu, "pinExploreButton", "Unpin from Listen", "Unpin Explore from Listen");
            comparePinned("");

            menu = openMenu(exploreMore, "exploreManageSheet");
            var edit = findChild(menu, "editExploreConnectionButton");
            waitForRendering(edit);
            mouseClick(edit, edit.width / 2, edit.height / 2);
            tryCompare(menu, "visible", false);
            verify(app.exploreSetupOpen, "Edit connection opens Explore's setup");
            compare(decoderHost.sessionState, 0);
        }

        // D3: six stored, every Pin to Listen is disabled with the reason, in each menu and the site chooser; Unpin
        // stays.
        function test_full_disables_pin_with_reason() {
            var reason = "Pinned is full (6). Unpin one first.";
            addSystem("Solo", "rtltcp");
            var sites = addGroup("rtltcp");
            var list = addList("Patrol");
            for (var i = 0; i < 5; ++i)
                compare(listenPins.pin("scan", addList("List " + i)), "");
            compare(listenPins.pin("scan", list), "");
            verify(listenPins.full);
            showHome();
            var menu;
            var pin;
            var sources = [{tab: "systems", opener: function () { return cardMore("Solo"); }, menu: "savedSystemManageSheet", pin: "pinSavedSystemButton"},
                {tab: "systems", opener: function () { return cardMore("County"); }, menu: "savedSystemManageSheet", pin: "pinSavedSystemButton"},
                {tab: "explore", opener: function () { return homeItem("signalSearchManageButton"); }, menu: "signalSearchManageSheet", pin: "pinSignalSearchButton"},
                {tab: "explore", opener: function () { return homeItem("exploreManageButton"); }, menu: "exploreManageSheet", pin: "pinExploreButton"}];
            for (var source of sources) {
                if (findChild(app, "homeScreen").section !== source.tab)
                    showTab(source.tab);
                menu = openMenu(source.opener(), source.menu);
                pin = findChild(menu, source.pin);
                compare(pin.text, "Pin to Listen", source.menu);
                verify(!pin.enabled, source.pin + " is disabled while full");
                verify(pin.accessibleName.indexOf(reason) > 0, "its name carries the reason: " + pin.accessibleName);
                var index = keys(menu).split(",").indexOf("pin");
                var description = findChild(menu, "actionMenuDescription" + index);
                verify(description.visible);
                compare(description.text, reason);
                verify(waitForPolish(app.contentItem));
                waitForRendering(pin);
                mouseClick(pin, pin.width / 2, pin.height / 2);
                verify(menu.visible, source.pin + ": a disabled Pin does nothing");
                menu.cancel();
                tryCompare(menu, "visible", false);
            }
            // A pinned list still offers Unpin while full.
            var scanMenu = homeItem("scanListManageSheet");
            scanMenu.openFor(scanLists.rowForUid(list));
            pin = findChild(scanMenu, "pinScanListButton");
            compare(pin.text, "Unpin from Listen");
            verify(pin.enabled);
            scanMenu.cancel();

            // The site chooser: each site's Pin disabled, the reason on a line under it, until a pin goes.
            var chooser = findChild(app, "siteChooserSheet");
            chooser.openFor(savedSystems.rowForUid(sites[0]));
            tryCompare(chooser, "visible", true);
            var sitePin = findChild(chooser, "sitePin" + savedSystems.rowForUid(sites[1]));
            var line = findChild(chooser, "sitePinReason" + savedSystems.rowForUid(sites[1]));
            verify(!sitePin.enabled);
            compare(sitePin.text, "Pin to Listen");
            verify(line.visible);
            compare(line.text, reason);
            listenPins.unpin("scan", list);
            tryCompare(sitePin, "enabled", true);
            verify(!line.visible, "the reason goes with the full row");
        }

        // Before #801 a key a handler did not know fell through to Remove; now it does nothing.
        function test_unknown_menu_key_does_nothing() {
            addSystem("Solo", "rtltcp");
            addList("Patrol");
            showHome();
            for (var name of ["savedSystemManageSheet", "scanListManageSheet", "exploreManageSheet", "signalSearchManageSheet"]) {
                var menu = homeItem(name);
                if (name === "savedSystemManageSheet")
                    menu.openFor(0, "Solo");
                else if (name === "scanListManageSheet")
                    menu.openFor(0);
                else
                    menu.openFor();
                tryCompare(menu, "visible", true);
                menu.actions = menu.actions.concat([{text: "Favorite", key: "favorite"}]);
                menu.activate(menu.actions.length - 1);
                verify(!menu.visible);
                verify(!findChild(app, "removeSystemConfirm").visible, name + ": no removal asked");
                verify(!findChild(app, "removeScanListConfirm").visible, name + ": no removal asked");
                verify(!findChild(app, "homeScreen").managementSheetOpen, name + ": nothing opened");
                verify(!app.wizardOpen && !app.scanListOpen && !app.exploreSetupOpen && !app.signalSearchOpen);
                compare(listenPins.count, 0, name + ": nothing pinned");
                compare(savedSystems.count, 1);
                compare(scanLists.count, 1);
            }
        }

        // The new menus hold Home and auto-start as every Home sheet does.
        function test_new_menus_block_background() {
            prefs.exploreSourceType = "rtltcp";
            // Issue #646: on the Explore tab, so the face is on screen and its block below is not a given.
            showTab("explore");
            var home = showHome();
            var shownFace = homeItem("exploreStart");
            verify(shownFace.visible && shownFace.enabled && shownFace.activeFocusOnTab, "the face is live before a menu opens");
            for (var name of ["exploreManageSheet", "signalSearchManageSheet"]) {
                var sheet = homeItem(name);
                sheet.openFor();
                tryCompare(sheet, "visible", true);
                verify(home.managementSheetOpen, name);
                tryCompare(uiController, "autoStartBlocked", true);
                verify(!homeItem("dongleScrollBody").enabled, name + " disables the body");
                var face = homeItem("exploreStart");
                face.forceActiveFocus();
                verify(!face.activeFocus, name + ": the covered face takes no focus");
                face.activate();
                testContext.pressAccessible(face);
                compare(decoderHost.sessionState, 0, name + ": nothing under it starts");
                verify(!app.exploring);
                sheet.visible = false;
                tryCompare(uiController, "autoStartBlocked", false);
                verify(!home.managementSheetOpen);
            }
        }

        // A menu row does what it said when it was offered, never the opposite: pins that change behind an open menu
        // (only a test can, the menus being modal) leave a "Pin" pinning and an "Unpin" unpinning.
        function test_menu_does_what_it_offered_data() {
            return [{tag: "system", menu: "savedSystemManageSheet", kind: "system"},
                {tag: "scan list", menu: "scanListManageSheet", kind: "scan"},
                {tag: "Explore", menu: "exploreManageSheet", kind: "explore"},
                {tag: "Signal search", menu: "signalSearchManageSheet", kind: "search"}];
        }
        function test_menu_does_what_it_offered(data) {
            var uid = data.kind === "system" ? addSystem("Solo", "rtltcp") : data.kind === "scan" ? addList("Patrol") : "";
            showHome();
            var menu = homeItem(data.menu);
            var open = function () {
                if (data.kind === "system")
                    menu.openFor(0, "Solo");
                else if (data.kind === "scan")
                    menu.openFor(0);
                else
                    menu.openFor();
                tryCompare(menu, "visible", true);
                return keys(menu).split(",").indexOf("pin");
            };
            var index = open();
            compare(menu.actions[index].text, "Pin to Listen");
            compare(listenPins.pin(data.kind, uid), "", "pinned behind the open menu");
            menu.activate(index);
            verify(listenPins.isPinned(data.kind, uid), "the offered Pin did not unpin");
            compare(listenPins.count, 1);

            index = open();
            compare(menu.actions[index].text, "Unpin from Listen");
            listenPins.unpin(data.kind, uid);
            menu.activate(index);
            verify(!listenPins.isPinned(data.kind, uid), "the offered Unpin did not pin");
            compare(listenPins.count, 0);
        }
    }
}
