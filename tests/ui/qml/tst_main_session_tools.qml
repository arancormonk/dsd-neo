// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

Item {
    width: 420
    height: 900
    Loader {
        id: loader
        source: uiDir + "/Main.qml"
    }
    QtObject {
        id: searchStub
        property var status: ({phase: 4, owned: true, session: "17", hit: {id: "1", freqMHz: "450.125000", ended: false}})
        property var sessionAvoids: []
        property var ranges: []
        property var history: []
        property var avoids: []
        property string error: ""
        property int lastAction: -1
        property string armError: ""
        property int cancels: 0
        function control(action, session, hit) { lastAction = action; }
        function arm() { return armError; }
        function cancelArm() { cancels++; }
        function systemForHit(hit, source) {
            return Object.assign({}, source, {freqMhz: hit.freqMHz, decodeFlag: "-fA", gainDb: 21, trunking: false,
                searchUnresolved: false});
        }
        function defaultRange() { return {name: "Range", startMHz: "450.000000", endMHz: "460.000000", enabled: true,
            stepNum: 12500, stepDen: 1, bandwidth: 16000, am: false, digital: 1, gainSet: false, gainAuto: true,
            gain: 0, sensitivity: 15, minimumSet: false, minimum: -60, squelchMode: 2, squelchMargin: 10,
            squelchLevel: -60, probe: 2, visit: 30, airspyGainSet: false, airspy: {}}; }
    }
    TestCase {
        name: "MainSessionTools"
        when: windowShown
        readonly property var app: loader.item
        property bool originalOnboarding: false
        function init() {
            // A recreated Activity used to re-enter accessibility and slot-height
            // bindings while the monitor's tier groups settled their parents.
            failOnWarning(/Binding loop detected/);
            verify(app !== null);
            testContext.useLifecycleHost(true);
            testContext.setLifecycleFailure("");
            testContext.setLifecyclePhase(2);
            originalOnboarding = prefs.onboardingDone;
            prefs.onboardingDone = true;
            app.currentTab = 0;
            app.sessionDestination = "";
            app.importsOpen = false;
            app.dismissedFailure = "";
            testContext.setMetric("radioInput", true);
            testContext.resetCommands();
            app.requestActivate();
            tryCompare(app, "active", true);
        }
        function cleanup() {
            testContext.setDelayedStop(false);
            app.cancelPendingRestart();
            app.signalSearchOpen = false;
            var search = findChild(app, "signalSearchScreen");
            search.searchModel = null;
            // A case may leave the receiver a session restored; the next starts on the default.
            search.sourceType = "usb";
            testContext.setSignalSearchModel(null);
            testContext.setMetric("radioSource", {});
            searchStub.ranges = [];
            searchStub.armError = "";
            searchStub.cancels = 0;
            searchStub.status = {phase: 4, owned: true, session: "17", hit: {id: "1", freqMHz: "450.125000", ended: false}};
            app.awaitingUsbAccess = false;
            app.pendingStart = null;
            testContext.setDongleStatus(false, 0, "");
            testContext.setScanTestUsb(false, false);
            app.importsOpen = false;
            app.diagnosticsOpen = false;
            app.licensesOpen = false;
            app.radioReferenceAccountOpen = false;
            app.sessionDestination = "";
            app.spectrumOpen = false;
            // Every case starts without a waterfall, independent of test order.
            var screen = findChild(app, "spectrumScreen");
            if (screen) screen.parent.active = false;
            var radio = findChild(app, "radioSheet");
            if (radio) radio.visible = false;
            findChild(app, "sessionMenu").visible = false;
            testContext.setLifecyclePhase(0);
            testContext.setLifecycleFailure("");
            app.dismissedFailure = "";
            prefs.onboardingDone = originalOnboarding;
            historyView.filterText = "";
            callHistory.clearAll();
            testContext.setMetric("leadSlot", 0);
            testContext.setMetric("slot1CallState", 0);
            testContext.useLifecycleHost(false);
        }
        function choose(key) {
            findChild(app, "monitorScreen").openSessionMenu();
            var menu = findChild(app, "sessionMenu");
            verify(menu.visible);
            var row = findChild(menu, "sessionMenu" + key);
            verify(row !== null && row.visible, key + " must remain in overflow");
            row.activate();
            verify(!menu.visible);
            wait(0); // Settle queued navigation focus before typing or opening another layer.
        }
        function test_signal_search_controls_are_navigation_roots() {
            var screen = findChild(app, "signalSearchScreen");
            screen.searchModel = searchStub;
            app.signalSearchOpen = true;
            wait(0);
            for (var name of ["searchStartPause", "searchStop", "searchHold", "searchBack"]) {
                var button = findChild(screen, name);
                verify(button.visible && button.enabled, name);
                verify(Ui.Navigation.allows(button), name + " must be reachable during search");
                verify(!button.Accessible.ignored, name + " must remain accessible");
            }
            findChild(screen, "searchHold").activate();
            compare(searchStub.lastAction, 3);
            findChild(screen, "searchStartPause").activate();
            compare(searchStub.lastAction, 1);
            verify(!Ui.Navigation.back(app), "Active search delegates Back to Main's session policy");
        }
        // Main binds the search screen's failure to its own failure text. Start
        // must leave that binding in place, or a failure reported afterwards (a
        // refused connection, USB access) never reaches the search screen, which
        // covers Home's banner.
        function test_signal_search_shows_failures_after_start_data() {
            return [{tag: "host", usb: false, message: "rtl_tcp: connection refused"},
                {tag: "usb-access", usb: true, message: "No permission for RTL-SDR"}];
        }
        function test_signal_search_shows_failures_after_start(data) {
            testContext.setLifecyclePhase(0);
            if (data.usb)
                testContext.setDongleStatus(false, 4, data.message);
            var screen = findChild(app, "signalSearchScreen");
            searchStub.ranges = [{name: "Local UHF", startMHz: "450.000000", endMHz: "460.000000", enabled: true}];
            screen.searchModel = searchStub;
            app.signalSearchOpen = true;
            wait(0);
            findChild(screen, "searchStartPause").activate();
            compare(app.awaitingUsbAccess, data.usb);
            if (!data.usb)
                testContext.setLifecycleFailure(data.message);
            var message = findChild(screen, "searchMessage");
            verify(message !== null);
            tryCompare(message, "text", data.message);
            verify(message.visible, "the failure must be shown on the search screen");
        }
        // A Start waiting for Android USB access belongs to the search that asked
        // for it. Leaving the search (its Back, the system Back) or a later Start
        // it refuses abandons that start: the grant would otherwise start a plain
        // decoder session behind Home, the search's arm having been cancelled.
        // The grant still finishes the start the search is waiting for.
        function test_abandoned_search_drops_its_pending_usb_start_data() {
            return [{tag: "search-back", abandoned: true}, {tag: "system-back", abandoned: true},
                {tag: "refused-start", abandoned: true}, {tag: "granted", abandoned: false}];
        }
        function test_abandoned_search_drops_its_pending_usb_start(data) {
            testContext.setLifecyclePhase(0);
            testContext.setDongleStatus(false, 4, "No permission for RTL-SDR");
            var screen = findChild(app, "signalSearchScreen");
            searchStub.ranges = [{name: "Local UHF", startMHz: "450.000000", endMHz: "460.000000", enabled: true}];
            screen.searchModel = searchStub;
            screen.sourceType = "usb";
            app.signalSearchOpen = true;
            wait(0);
            findChild(screen, "searchStartPause").activate();
            verify(app.awaitingUsbAccess);
            verify(app.pendingStart && app.pendingStart.signalSearch, "the search's start waits for USB access");
            if (data.tag === "search-back") {
                findChild(screen, "searchBack").activate();
                verify(!app.signalSearchOpen);
            } else if (data.tag === "system-back") {
                app.requestBack();
                verify(!app.signalSearchOpen);
            } else if (data.tag === "refused-start") {
                searchStub.armError = "Enable at least one range.";
                findChild(screen, "searchStartPause").activate();
                verify(app.signalSearchOpen);
                compare(findChild(screen, "searchMessage").text, "Enable at least one range.");
            }
            compare(app.awaitingUsbAccess, !data.abandoned);
            compare(app.pendingStart === null, data.abandoned);
            compare(searchStub.cancels > 0, data.abandoned, "an abandoned start leaves nothing armed");
            testContext.setDongleStatus(true, 0, "");
            compare(decoderHost.sessionState, data.abandoned ? 0 : 1);
            compare(app.monitorMode, !data.abandoned);
            verify(!app.awaitingUsbAccess);
            compare(app.pendingStart, null);
        }
        // An ordinary start already waiting for USB access when Signal search is
        // opened and closed again from Home is not the search's to abandon.
        function test_closing_search_keeps_an_ordinary_pending_usb_start() {
            testContext.setLifecyclePhase(0);
            testContext.setDongleStatus(false, 4, "No permission for RTL-SDR");
            app.startWithMap(app.exploreSystem("usb", "", 1234, "451.000000"), -1, false);
            verify(app.awaitingUsbAccess);
            var screen = findChild(app, "signalSearchScreen");
            screen.searchModel = searchStub;
            findChild(app, "homeScreen").signalSearch();
            verify(app.signalSearchOpen);
            wait(0);
            findChild(screen, "searchBack").activate();
            verify(!app.signalSearchOpen);
            verify(app.awaitingUsbAccess, "the ordinary start is still waiting");
            verify(app.pendingStart && !app.pendingStart.signalSearch);
            testContext.setDongleStatus(true, 0, "");
            compare(decoderHost.sessionState, 1, "the grant finishes the ordinary start");
            verify(app.monitorMode);
        }
        // The same ordinary start granted while Signal search is still open runs
        // no search: the screen closes, so the session is not taken for one (its
        // Pause would do nothing, its Back would follow a search's policy).
        function test_ordinary_grant_closes_an_open_search() {
            testContext.setLifecyclePhase(0);
            testContext.setDongleStatus(false, 4, "No permission for RTL-SDR");
            app.startWithMap(app.exploreSystem("usb", "", 1234, "451.000000"), -1, false);
            verify(app.awaitingUsbAccess);
            var screen = findChild(app, "signalSearchScreen");
            searchStub.status = {phase: 0, owned: false, session: "0", hit: {}};
            screen.searchModel = searchStub;
            findChild(app, "homeScreen").signalSearch();
            verify(app.signalSearchOpen);
            wait(0);
            testContext.setDongleStatus(true, 0, "");
            compare(decoderHost.sessionState, 1, "the grant finishes the ordinary start");
            verify(!app.signalSearchOpen, "an ordinary session is not a search");
            verify(searchStub.cancels > 0, "nothing is left armed for the ordinary session");
            verify(app.monitorMode);
        }
        function findText(item, text) {
            if (item.text === text)
                return item;
            for (var child of item.children) {
                var found = findText(child, text);
                if (found)
                    return found;
            }
            return null;
        }
        // Android can recreate the QML over a search the decoder still owns. Main
        // then reopens the search screen from status.owned, never through Home's
        // onSignalSearch, so the screen takes its receiver from the session itself
        // (metrics.radioSource, copied from the decoder's options snapshot), the
        // first snapshot arriving before or after the screen reopens.
        function test_recreated_search_screen_restores_its_receiver_data() {
            var airspy = {sourceType: "airspy"};
            var rtltcp = {sourceType: "rtltcp", host: "10.0.0.5", port: 1235};
            return [{tag: "airspy-known", source: airspy, late: false}, {tag: "airspy-late", source: airspy, late: true},
                {tag: "rtltcp-known", source: rtltcp, late: false}, {tag: "rtltcp-late", source: rtltcp, late: true}];
        }
        function test_recreated_search_screen_restores_its_receiver(data) {
            searchStub.ranges = [searchStub.defaultRange()];
            searchStub.status = {phase: 6, owned: true, session: "17", hit: {}};
            testContext.setSignalSearchModel(searchStub);
            testContext.setMetric("radioSource", data.late ? {} : data.source);
            loader.active = false;
            wait(0);
            compare(decoderHost.sessionState, 2);
            loader.active = true;
            tryCompare(loader, "status", Loader.Ready);
            var screen = findChild(app, "signalSearchScreen");
            verify(app.signalSearchOpen, "an owned search reopens its screen");
            verify(screen.visible);
            if (data.late)
                testContext.setMetric("radioSource", data.source);
            compare(screen.sourceType, data.source.sourceType);
            if (data.source.sourceType === "rtltcp") {
                compare(screen.host, data.source.host);
                compare(screen.port, data.source.port);
            }
            // Paused, so ranges are editable: the editor offers this receiver's gain controls.
            var editor = findChild(screen, "signalSearchRangeEditor");
            editor.openFor(0, searchStub.defaultRange());
            verify(editor.visible);
            var airspyGains = findText(editor, "Use Airspy gain settings for this range");
            verify(airspyGains !== null);
            compare(airspyGains.visible, data.source.sourceType === "airspy");
            editor.visible = false;
            // Stopped from the screen, the search stays on it, as one Home opened
            // does, and starts again on the receiver it ran on.
            var cancels = searchStub.cancels;
            findChild(screen, "searchStop").activate();
            searchStub.status = {phase: 0, owned: false, session: "17", hit: {}};
            testContext.setMetric("radioSource", {});
            compare(decoderHost.sessionState, 0);
            verify(app.signalSearchOpen && screen.visible, "Stop leaves the search on its screen");
            verify(searchStub.cancels > cancels, "Stop leaves nothing armed");
            compare(screen.sourceType, data.source.sourceType);
            findChild(screen, "searchStartPause").activate();
            compare(decoderHost.sessionState, 1);
            verify(app.sessionSystem && app.sessionSystem.signalSearch);
            compare(app.sessionSystem.sourceType, data.source.sourceType);
            if (data.source.sourceType === "rtltcp") {
                compare(app.sessionSystem.host, data.source.host);
                compare(app.sessionSystem.port, data.source.port);
            }
        }
        // A handoff from a search screen Activity recreation reopened (its
        // signalSearchOpen still bound to status.owned): the search publishes
        // owned=false before the decoder finishes stopping, and the screen must
        // stay, saying it is stopping, not expose the Monitor. Back cancels the
        // handoff and the screen stays once the stop completes, as Stop leaves
        // it; carried through, the replacement starts once the decoder is idle.
        function test_recreated_search_screen_handoff_data() {
            return [{tag: "explore here, back", saved: false, back: true},
                {tag: "explore here, completes", saved: false, back: false},
                {tag: "saved system, back", saved: true, back: true}];
        }
        function test_recreated_search_screen_handoff(data) {
            var uid = "";
            if (data.saved) {
                verify(savedSystems.add({name: "Warehouse", sourceType: "rtltcp", host: "127.0.0.1", port: 1234,
                    freqMhz: "463.5500"}));
                uid = savedSystems.get(savedSystems.count - 1).uid;
            }
            searchStub.ranges = [searchStub.defaultRange()];
            searchStub.status = {phase: 4, owned: true, session: "17", hit: {id: "1", freqMHz: "450.125000", ended: false}};
            testContext.setSignalSearchModel(searchStub);
            testContext.setMetric("radioSource", {sourceType: "rtltcp", host: "10.0.0.5", port: 1235});
            loader.active = false;
            wait(0);
            loader.active = true;
            tryCompare(loader, "status", Loader.Ready);
            var screen = findChild(app, "signalSearchScreen");
            verify(app.signalSearchOpen && screen.visible, "an owned search reopens its screen");
            testContext.setDelayedStop(true);
            if (data.saved)
                screen.startSavedSystem(uid);
            else
                screen.exploreDetection(searchStub.status.hit);
            compare(decoderHost.sessionState, 3);
            verify(app.leavingSearch);
            // The search lets go of the tuner before the decoder is idle.
            searchStub.status = {phase: 0, owned: false, session: "17", hit: {}};
            testContext.setSignalSearchModel(searchStub);
            wait(0);
            verify(app.signalSearchOpen, "the screen stays while the search stops");
            compare(Ui.Navigation.rootSurfaces[0], screen, "the search screen, not the Monitor, is the root while it stops");
            compare(findChild(screen, "searchIdentification").text, "Stopping the search…");
            if (data.back) {
                app.requestBack();
                verify(!app.leavingSearch && app.pendingRestart === null);
                verify(app.signalSearchOpen);
                compare(Ui.Navigation.rootSurfaces[0], screen);
            }
            testContext.setDelayedStop(false);
            testContext.setLifecyclePhase(0);
            wait(0);
            if (data.back) {
                compare(decoderHost.sessionState, 0, "nothing starts after Back");
                verify(app.signalSearchOpen && screen.visible, "the stopped search stays on its screen");
                verify(findChild(screen, "searchStartPause").enabled, "ready to start again");
            } else {
                compare(decoderHost.sessionState, 1);
                verify(!app.signalSearchOpen);
                compare(app.sessionSystem.name, "Exploring");
                compare(app.sessionSystem.freqMhz, "450.125000");
            }
            while (savedSystems.count)
                savedSystems.remove(0);
        }
        function visibleTitles(item, title) {
            if (!item.visible) return 0;
            var count = item instanceof Text && item.text === title ? 1 : 0;
            for (var child of item.children)
                count += visibleTitles(child, title);
            return count;
        }
        function visibleFailureCard(item) {
            if (!item.visible) return null;
            if (item instanceof Ui.FailureCard) return item;
            for (var child of item.children) {
                var card = visibleFailureCard(child);
                if (card) return card;
            }
            return null;
        }
        function test_tools_back_data() {
            return [{tag: "history", key: "History"}, {tag: "settings", key: "Settings"}];
        }
        function test_tools_back(data) {
            choose(data.key);
            compare(app.sessionDestination, data.tag);
            compare(findChild(app, "sessionToolsTitle").text, data.key);
            compare(visibleTitles(findChild(app, "sessionTools"), data.key), 1);
            compare(uiController.autoStartBlocked, true);
            verify(!findChild(app, "monitorScreen").enabled);
            var back = findChild(app, "sessionToolsBack");
            compare(back.icon, "back");
            back.activate();
            compare(app.sessionDestination, "");
            verify(findChild(app, "monitorScreen").enabled);
            choose(data.key);
            back.forceActiveFocus();
            keyClick(Qt.Key_Escape);
            compare(app.sessionDestination, "");
            choose(data.key);
            app.requestBack();
            compare(app.sessionDestination, "");
            compare(app.currentTab, 0);
            compare(uiController.autoStartBlocked, false);
        }
        function test_service_stop_and_reattach_data() {
            return [{tag: "history", key: "History", tab: 1}, {tag: "settings", key: "Settings", tab: 2}];
        }
        function test_service_stop_and_reattach(data) {
            choose(data.key);
            // A stopping session still owns the screen; stale metrics must not
            // keep it there after lifecycle truth reaches Idle.
            testContext.setMetric("leadSlot", 1);
            testContext.setMetric("slot1CallState", 2);
            testContext.setLifecyclePhase(3);
            compare(app.sessionDestination, data.tag);
            testContext.setLifecyclePhase(0);
            compare(app.sessionDestination, "");
            compare(app.currentTab, data.tab);
            verify(!findChild(app, "monitorScreen").visible);
            verify(findChild(app, "idle" + data.key + "Screen").visible);
            compare(visibleTitles(findChild(app, "idle" + data.key + "Screen"), data.key), 1);
            verify(!findChild(app, "sessionTools").visible);
            compare(uiController.autoStartBlocked, false);
            testContext.setLifecyclePhase(2);
            compare(app.sessionDestination, "");
            verify(findChild(app, "monitorScreen").enabled);
            testContext.setMetric("leadSlot", 0);
            testContext.setMetric("slot1CallState", 0);
        }
        function test_failure_stop_data() {
            var rows = [];
            for (var key of ["History", "Settings"]) {
                for (var timing of ["before-stop", "after-stop", "dismissed"])
                    rows.push({tag: key + "-" + timing, key: key, timing: timing});
            }
            return rows;
        }
        function test_failure_stop(data) {
            choose(data.key);
            var message = "USB receiver disconnected";
            if (data.timing === "dismissed") app.dismissedFailure = message;
            if (data.timing !== "after-stop") testContext.setLifecycleFailure(message);
            testContext.setLifecyclePhase(0);
            if (data.timing === "after-stop") {
                compare(app.currentTab, data.key === "History" ? 1 : 2);
                wait(0);
                testContext.setLifecycleFailure(message);
            }
            compare(app.sessionDestination, "");
            verify(!findChild(app, "monitorScreen").visible);
            verify(!findChild(app, "sessionTools").visible);
            if (data.timing === "dismissed") {
                compare(app.currentTab, data.key === "History" ? 1 : 2);
                verify(!app.showFailure);
            } else {
                compare(app.currentTab, 0);
                verify(app.showFailure);
                var home = findChild(app, "homeScreen");
                verify(home.visible);
                var card = visibleFailureCard(home);
                verify(card !== null, "the failure explanation must be visible on Home");
                compare(card.message, app.failureMessage());
                verify(card.message.length > 0);
            }
        }
        function test_history_clear_without_duplicate_title() {
            callHistory.push("TODAY");
            choose("History");
            var screen = findChild(app, "sessionHistoryScreen");
            var clear = findChild(screen, "clearHistoryButton");
            verify(clear.visible && clear.enabled);
            compare(visibleTitles(findChild(app, "sessionTools"), "History"), 1);
            verify(clear.parent.height >= clear.height);
            compare(clear.x + clear.width, clear.parent.width);
            mouseClick(clear, clear.width / 2, clear.height / 2);
            var confirm = findChild(screen, "clearHistoryConfirm");
            verify(confirm.visible);
            app.requestBack();
            verify(!confirm.visible);
            compare(callHistory.count, 1);
        }
        // Issue #718: the diagnostics model takes rows only while its screen is open.
        function test_diagnostics_log_active_follows_screen() {
            compare(diagnosticsLog.active, false);
            app.diagnosticsOpen = true;
            verify(findChild(app, "diagnosticsScreen").visible);
            compare(diagnosticsLog.active, true);
            app.diagnosticsOpen = false;
            compare(diagnosticsLog.active, false);
        }
        function test_settings_subpage_escape_data() {
            return [
                {tag: "imports", signal: "openImports", property: "importsOpen", screen: "importsScreen"},
                {tag: "diagnostics", signal: "openDiagnostics", property: "diagnosticsOpen", screen: "diagnosticsScreen"},
                {tag: "licenses", signal: "openLicenses", property: "licensesOpen", screen: "licensesScreen"},
                {tag: "call-recordings", signal: "openCallRecordings", property: "callRecordingsOpen", screen: "callRecordingsScreen"},
                {tag: "account", signal: "openRadioReferenceAccount", property: "radioReferenceAccountOpen", screen: "radioReferenceAccountScreen"}
            ];
        }
        function test_settings_subpage_escape(data) {
            choose("Settings");
            var tools = findChild(app, "sessionTools");
            tryCompare(tools, "activeFocus", true);
            findChild(app, "sessionSettingsScreen")[data.signal]();
            verify(app[data.property]);
            verify(!tools.enabled);
            var subpage = findChild(app, data.screen);
            tryVerify(function () { return Ui.Navigation.contains(subpage, app.activeFocusItem); });
            keyClick(Qt.Key_Escape);
            compare(app[data.property], false);
            compare(app.sessionDestination, "settings");
            wait(0); // Let the layer's deferred focus restoration run.
            keyClick(Qt.Key_Escape);
            compare(app.sessionDestination, "");
            verify(findChild(app, "monitorScreen").enabled);
        }
        function test_failure_closes_settings_subpage_data() {
            var rows = [];
            for (var page of test_settings_subpage_escape_data()) {
                for (var timing of ["before-stop", "after-stop"])
                    rows.push({tag: page.tag + "-" + timing, page: page, timing: timing});
            }
            return rows;
        }
        function test_failure_closes_settings_subpage(data) {
            choose("Settings");
            findChild(app, "sessionSettingsScreen")[data.page.signal]();
            var subpage = findChild(app, data.page.screen);
            verify(app[data.page.property]);
            verify(subpage.visible && subpage.enabled);
            var message = "USB receiver disconnected";
            if (data.timing === "before-stop") testContext.setLifecycleFailure(message);
            testContext.setLifecyclePhase(0);
            if (data.timing === "after-stop") {
                compare(app.currentTab, 2);
                verify(subpage.visible, "a clean stop keeps the Settings subpage available");
                wait(0);
                testContext.setLifecycleFailure(message);
            }
            compare(app.currentTab, 0);
            compare(app.sessionDestination, "");
            compare(app[data.page.property], false, "failure must dismiss the covering subpage");
            verify(!subpage.visible);
            verify(!findChild(app, "sessionTools").visible);
            verify(!findChild(app, "monitorScreen").visible);
            var home = findChild(app, "homeScreen");
            verify(home.visible);
            verify(Ui.Navigation.allows(home), "the closed subpage must release navigation");
            verify(app.showFailure);
            var card = visibleFailureCard(home);
            verify(card !== null, "the failure explanation must be visible on Home");
            compare(card.message, app.failureMessage());
            verify(card.message.length > 0);
        }
        function test_settings_imports_back_and_stop() {
            choose("Settings");
            findChild(app, "sessionSettingsScreen").openImports();
            verify(findChild(app, "importsScreen").enabled);
            app.requestBack();
            compare(app.importsOpen, false);
            compare(app.sessionDestination, "settings");
            findChild(app, "sessionSettingsScreen").openImports();
            testContext.setLifecyclePhase(0);
            compare(app.currentTab, 2);
            compare(app.sessionDestination, "");
            verify(findChild(app, "importsScreen").enabled);
            app.requestBack();
            verify(findChild(app, "idleSettingsScreen").visible);
        }
        function test_radio_without_spectrum_and_shared_reopen() {
            compare(findChild(app, "spectrumScreen"), null);
            choose("Radio");
            var radio = findChild(app, "radioSheet");
            verify(radio !== null && radio.visible);
            verify(!findChild(app, "monitorScreen").enabled);
            compare(findChild(app, "spectrumScreen"), null);
            compare(spectrum.active, false);
            findChild(radio, "radioGainUp").activate();
            compare(radio.gainDb, metrics.tunerGainDb + 1);
            app.requestBack();
            verify(!radio.visible);
            verify(findChild(app, "monitorScreen").enabled);
            choose("Spectrum");
            var screen = findChild(app, "spectrumScreen");
            verify(screen !== null);
            findChild(screen, "spectrumRadioButton").activate();
            verify(radio.visible);
            verify(screen.sheetOpen);
            compare(radio.gainDb, metrics.tunerGainDb);
            testContext.setLifecyclePhase(0);
            verify(!radio.visible);
            verify(!screen.visible);
        }
        function test_recreated_activity_reattaches_without_a_stale_tool() {
            choose("History");
            loader.active = false;
            wait(0);
            compare(decoderHost.sessionState, 2);
            loader.active = true;
            tryCompare(loader, "status", Loader.Ready);
            verify(app.monitorMode);
            compare(app.sessionDestination, "");
            verify(findChild(app, "monitorScreen").enabled);
            choose("Settings");
            testContext.setLifecyclePhase(0);
            compare(app.currentTab, 2);
            compare(app.sessionDestination, "");
        }
        // Issue #671: while the operator holds the tuner a saved system's scan had
        // (paused by a frequency entered anywhere), the session is not that system:
        // the monitor names it Exploring and the menu offers no edit that would
        // push CSVs into it. Resume scan gives both back.
        function heldTuner(held) {
            testContext.setMetrics({operatorTuned: held, scanKind: 1, scanRotationActive: !held, scanPaused: held,
                                    scanResumable: held, tunerControlled: !held, scannerMode: !held});
        }
        function menuOffersEdit() {
            findChild(app, "monitorScreen").openSessionMenu();
            var menu = findChild(app, "sessionMenu");
            verify(menu.visible);
            var row = findChild(menu, "sessionMenuEdit");
            var offered = row !== null && row.visible;
            menu.visible = false;
            return offered;
        }
        function test_a_held_tuner_detaches_the_saved_system_until_resume() {
            var title = function () { return findChild(findChild(app, "monitorScreen"), "monitorHeaderTitle").text; };
            testContext.setLifecyclePhase(0);
            savedSystems.add({name: "Scanning site", sourceType: "rtltcp", host: "127.0.0.1", port: 1234, freqMhz: "851.5"});
            var row = savedSystems.count - 1;
            try {
                app.startSystem(row);
                testContext.setLifecyclePhase(2);
                testContext.setMetric("optionsKnown", true);
                heldTuner(false);
                compare(app.sessionRow, row);
                verify(menuOffersEdit(), "the running saved system offered no edit");
                compare(title(), "Scanning site");
                heldTuner(true);
                compare(app.sessionRow, -1);
                verify(!menuOffersEdit(), "the menu offered to edit a system the session left");
                compare(title(), "Exploring");
                compare(app.sessionSavedSystemUid, "");
                verify(!app.exploring, "a saved system's pause took taps");
                heldTuner(false);
                compare(app.sessionRow, row);
                verify(menuOffersEdit(), "the resumed saved system offered no edit");
                compare(title(), "Scanning site");
                compare(app.sessionSavedSystemUid, savedSystems.get(row).uid);
            } finally {
                testContext.setMetrics({operatorTuned: false, scanKind: 0, scanRotationActive: false, scanPaused: false,
                                        scanResumable: false, tunerControlled: false, scannerMode: false,
                                        optionsKnown: false});
                savedSystems.remove(row);
            }
        }
        // A recreated Activity finds the tuner held: no session map came with it,
        // so it names the session Exploring until the resume, then by the label
        // the history kept.
        function test_a_reattached_session_names_a_held_tuner() {
            var title = function () { return findChild(findChild(app, "monitorScreen"), "monitorHeaderTitle").text; };
            callHistory.sessionLabel = "Scan";
            loader.active = false;
            wait(0);
            loader.active = true;
            tryCompare(loader, "status", Loader.Ready);
            try {
                verify(app.monitorMode);
                compare(title(), "Scan");
                testContext.setMetric("optionsKnown", true);
                heldTuner(true);
                compare(title(), "Exploring");
                heldTuner(false);
                compare(title(), "Scan");
            } finally {
                testContext.setMetrics({operatorTuned: false, scanKind: 0, scanRotationActive: false, scanPaused: false,
                                        scanResumable: false, tunerControlled: false, scannerMode: false,
                                        optionsKnown: false});
                callHistory.sessionLabel = "Test Site";
            }
        }
        // A recreated Activity finds a scan list running: it takes the session's
        // identity from the start the host last accepted (the scan list and its UID),
        // so the list behaves as it did before -- its targets are named, a pause
        // makes the receiver the operator's to tune by hand with Resume scan, and
        // the resume brings the list back.
        function reattach() {
            loader.active = false;
            wait(0);
            loader.active = true;
            tryCompare(loader, "status", Loader.Ready);
            verify(app.monitorMode);
        }
        // ---- Issue #697: one session over several scan lists ----

        function addScanList(name, entryName, freq) {
            scanLists.add({name: name, sourceType: "rtltcp", host: "127.0.0.1", port: 1234,
                entries: [{kind: "freq", name: entryName, protocol: "p25", freqMhz: freq, enabled: true}]});
            return scanLists.get(scanLists.count - 1);
        }
        function removeScanLists(uids) {
            for (var uid of uids) {
                var row = scanLists.rowForUid(uid);
                if (row >= 0)
                    scanLists.remove(row);
            }
        }
        function trunkScanPaths(args) {
            var paths = [];
            for (var i = 0; i + 1 < args.length; ++i) {
                if (args[i] === "--trunk-scan")
                    paths.push(args[i + 1].substring(args[i + 1].lastIndexOf("/") + 1));
            }
            return paths;
        }
        function scanMetrics(entryUid, activeList) {
            testContext.setMetrics({optionsKnown: true, scanKind: 2, scanRotationActive: true, scanPaused: false,
                                    scanResumable: false, operatorTuned: false, tunerControlled: true,
                                    scanTargetId: entryUid, scanTargetOrdinal: activeList, scanTargetCount: 3,
                                    scanListCount: 3, scanActiveList: activeList, scanActiveListOff: false,
                                    scanRotationOrdinal: activeList, scanRotationCount: 3});
        }
        function clearScanMetrics() {
            testContext.setMetrics({operatorTuned: false, scanKind: 0, scanRotationActive: false, scanPaused: false,
                                    scanResumable: false, tunerControlled: false, optionsKnown: false,
                                    scanTargetId: "", scanTargetOrdinal: 0, scanTargetCount: 0, scanListCount: 0,
                                    scanActiveList: 0, scanRotationOrdinal: 0, scanRotationCount: 0});
            testContext.resetScanLists();
        }

        // The bundle a start was accepted as is the session's identity: recorded with the accepted start and at the
        // initialization edge, carried by Retry, read back by a recreated Activity, and named in the header and by
        // the session's channel names. A start of one list clears it.
        function test_a_scan_bundle_keeps_its_identity() {
            var title = function () { return findChild(findChild(app, "monitorScreen"), "monitorHeaderTitle").text; };
            var header = function () { return findChild(findChild(app, "monitorScreen"), "scanTargetHeader").text; };
            var security = addScanList("Security", "Gate", "851.0125");
            var hospital = addScanList("Hospital", "ER desk", "852.0125");
            var parks = addScanList("Parks", "Ranger", "853.0125");
            var kind = prefs.acceptedStartKind;
            var uid = prefs.acceptedStartUid;
            try {
                testContext.setLifecyclePhase(0);
                prefs.lastStartedCompanions = [];
                app.startScanBundle(scanLists.rowForUid(security.uid), [hospital.uid, parks.uid]);
                // Issue #697: each list's CSV is named after the list, in its own folder, so the decoder names it so.
                compare(trunkScanPaths(testContext.lastStartArgs()).join(), "Security.csv,Hospital.csv,Parks.csv");
                verify(testContext.lastStartArgs().indexOf(testContext.scanListCsvPath(hospital.uid, "Hospital")) > 0);
                // Accepted: the bundle is recorded with the start, the initialization edge records it as started.
                compare(prefs.acceptedStartKind, "scan");
                compare(prefs.acceptedStartUid, security.uid);
                compare(prefs.acceptedStartCompanions.join(), [hospital.uid, parks.uid].join());
                compare(prefs.lastStartedCompanions.length, 0);
                testContext.emitSessionInitialized();
                compare(prefs.lastStartedUid, security.uid);
                compare(prefs.lastStartedCompanions.join(), [hospital.uid, parks.uid].join());
                compare(app.sessionLists.map(function (l) { return l.name; }).join(), "Security,Hospital,Parks");
                testContext.setLifecyclePhase(2);
                // Names: the header says which list the target on air is from, and every list names its channels.
                var er = hospital.entries[0].uid;
                scanMetrics(er, 2);
                compare(app.scanTargetLabel(), "ER desk");
                tryCompare(findChild(app, "monitorScreen"), "scanListName", "Hospital");
                compare(header(), "SCANNING · 2/3 · ER desk · Hospital");
                // The title names the session's own list and how many more.
                tryVerify(function () { return title() === "Security + 2 lists"; }, 5000, title());
                compare(metrics.channelNames[security.entries[0].uid], "Gate");
                compare(metrics.channelNames[parks.entries[0].uid], "Ranger");
                // Session options offers the lists while the scan has more than one.
                choose("ScanLists");
                tryCompare(findChild(app, "scanListsSheet"), "visible", true);
                findChild(app, "scanListsSheet").visible = false;
                // Recreated: the bundle comes back from the accepted start.
                prefs.acceptedStartKind = "scan";
                reattach();
                tryVerify(function () { return title() === "Security + 2 lists"; }, 5000, title());
                compare(app.sessionStartCompanions.join(), [hospital.uid, parks.uid].join());
                compare(app.sessionLists.map(function (l) { return l.name; }).join(), "Security,Hospital,Parks");
                scanMetrics(parks.entries[0].uid, 3);
                compare(app.scanTargetLabel(), "Ranger");
                tryCompare(findChild(app, "monitorScreen"), "scanListName", "Parks");
                // Retry starts the same bundle again.
                clearScanMetrics();
                testContext.setLifecycleFailure("Connection refused");
                testContext.setLifecyclePhase(4);
                app.attemptedSource = {kind: "scan", uid: security.uid, companions: [hospital.uid, parks.uid],
                    system: security, remember: true};
                app.retrySource();
                tryCompare(decoderHost, "sessionState", 1);
                compare(trunkScanPaths(testContext.lastStartArgs()).length, 3);
                compare(prefs.acceptedStartCompanions.join(), [hospital.uid, parks.uid].join());
                // A start of the one list clears it.
                testContext.setLifecycleFailure("");
                testContext.setLifecyclePhase(0);
                app.startScanList(scanLists.rowForUid(security.uid));
                compare(trunkScanPaths(testContext.lastStartArgs()).length, 1);
                compare(prefs.acceptedStartCompanions.length, 0);
                testContext.emitSessionInitialized();
                compare(prefs.lastStartedCompanions.length, 0);
                compare(app.sessionLists.length, 1);
                // A list gone since the bundle was chosen is left out; the rest start.
                testContext.setLifecyclePhase(0);
                var gone = parks.uid;
                removeScanLists([gone]);
                app.startScanBundle(scanLists.rowForUid(security.uid), [hospital.uid, gone]);
                compare(app.startError, "");
                compare(trunkScanPaths(testContext.lastStartArgs()).join(), "Security.csv,Hospital.csv");
                compare(prefs.acceptedStartCompanions.join(), hospital.uid);
            } finally {
                clearScanMetrics();
                testContext.setLifecycleFailure("");
                testContext.setLifecyclePhase(2);
                app.startError = "";
                prefs.acceptedStartKind = kind;
                prefs.acceptedStartUid = uid;
                prefs.acceptedStartCompanions = [];
                prefs.lastStartedCompanions = [];
                removeScanLists([security.uid, hospital.uid, parks.uid]);
            }
        }

        // Issue #697: the paths a bundle takes that a person cannot see: a USB permission wait (the lists as saved at
        // the grant, by uid), an auto-start with its companions, Retry after a start the host refused, and a removed
        // list leaving the remembered sessions and an auto-start that names it.
        function test_a_scan_bundle_through_waits_retries_and_removals() {
            var addUsb = function (name, entryName, freq) {
                scanLists.add({name: name, sourceType: "usb",
                    entries: [{kind: "freq", name: entryName, protocol: "p25", freqMhz: freq, enabled: true}]});
                return scanLists.get(scanLists.count - 1);
            };
            var security = addUsb("Security", "Gate", "851.0125");
            var hospital = addUsb("Hospital", "ER desk", "852.0125");
            var parks = addUsb("Parks", "Ranger", "853.0125");
            var kind = prefs.acceptedStartKind;
            var uid = prefs.acceptedStartUid;
            try {
                testContext.setLifecyclePhase(0);
                // 1. A brokered receiver not ready yet: the start waits with the companions' uids.
                testContext.setScanTestUsb(true, false);
                app.startScanBundle(scanLists.rowForUid(security.uid), [hospital.uid, parks.uid]);
                verify(app.awaitingUsbAccess);
                compare(app.pendingStartCompanions.join(), [hospital.uid, parks.uid].join());
                // A companion renamed during the dialog starts as it is saved then.
                scanLists.update(scanLists.rowForUid(parks.uid), {name: "City parks"});
                testContext.setScanTestUsb(true, true);
                tryCompare(decoderHost, "sessionState", 1);
                compare(trunkScanPaths(testContext.lastStartArgs()).join(), "Security.csv,Hospital.csv,City parks.csv");
                compare(app.pendingStartCompanions.length, 0);
                decoderHost.stop();
                tryCompare(decoderHost, "sessionState", 0);
                // 2. An auto-start hands Main the companions it was last started with.
                uiController.requestAutoStart("scan", security.uid, [hospital.uid, parks.uid]);
                tryCompare(decoderHost, "sessionState", 1);
                compare(trunkScanPaths(testContext.lastStartArgs()).length, 3);
                compare(prefs.acceptedStartCompanions.join(), [hospital.uid, parks.uid].join());
                decoderHost.stop();
                tryCompare(decoderHost, "sessionState", 0);
                // 3. The host refuses the start: Retry starts the same bundle, from the attempt the start recorded.
                testContext.setScanTestAcceptStart(false);
                app.startScanBundle(scanLists.rowForUid(security.uid), [parks.uid]);
                compare(decoderHost.sessionState, 0);
                compare(app.attemptedSource.companions.join(), parks.uid);
                testContext.setScanTestAcceptStart(true);
                app.retrySource();
                tryCompare(decoderHost, "sessionState", 1);
                compare(trunkScanPaths(testContext.lastStartArgs()).join(), "Security.csv,City parks.csv");
                testContext.emitSessionInitialized();
                compare(prefs.lastStartedCompanions.join(), parks.uid);
                decoderHost.stop();
                tryCompare(decoderHost, "sessionState", 0);
                // 4. Removing a companion takes it out of the next start's record (the accepted start's keeps its
                // places; every start writes it anew), and an auto-start that still names it starts the rest.
                prefs.acceptedStartCompanions = [hospital.uid, parks.uid];
                prefs.lastStartedCompanions = [parks.uid, hospital.uid];
                removeScanLists([parks.uid]);
                compare(prefs.acceptedStartCompanions.join(), [hospital.uid, parks.uid].join());
                compare(prefs.lastStartedCompanions.join(), hospital.uid);
                app.startError = "";
                uiController.requestAutoStart("scan", security.uid, [parks.uid, hospital.uid]);
                tryCompare(decoderHost, "sessionState", 1);
                compare(app.startError, "");
                compare(trunkScanPaths(testContext.lastStartArgs()).join(), "Security.csv,Hospital.csv");
            } finally {
                testContext.setScanTestAcceptStart(true);
                testContext.setScanTestUsb(false, false);
                decoderHost.stop();
                app.awaitingUsbAccess = false;
                app.pendingStart = null;
                testContext.setLifecycleFailure("");
                testContext.setLifecyclePhase(2);
                app.startError = "";
                prefs.acceptedStartKind = kind;
                prefs.acceptedStartUid = uid;
                prefs.acceptedStartCompanions = [];
                prefs.lastStartedCompanions = [];
                removeScanLists([security.uid, hospital.uid, parks.uid]);
            }
        }

        // ---- Issue #644: Listen with its settings ----

        // An Explore session on @p freqMhz, running, the receiver tuned there.
        function exploreOn(freqMhz) {
            testContext.setLifecyclePhase(0);
            app.startWithMap(app.exploreSystem("rtltcp", "127.0.0.1", 1234, freqMhz), -1, false);
            testContext.setLifecyclePhase(2);
            testContext.setMetrics({centerFreqHz: Math.round(Number(freqMhz) * 1e6), channelBandwidthHz: 12500});
            verify(app.exploring);
        }
        function endExplore() {
            testContext.setMetrics({centerFreqHz: 851000000, channelBandwidthHz: 12500, scanKind: 0,
                                    scanRotationActive: false, scanHold: false, scanEntryHold: false,
                                    scanTargetId: "", scanTargetOrdinal: 0, scanTargetCount: 0, optionsKnown: false});
            testContext.setDelayedStop(false);
            testContext.setScanTestAcceptStart(true);
            testContext.setScanTestUsb(false, false);
            app.cancelPendingRestart();
            app.awaitingUsbAccess = false;
            app.pendingStart = null;
            app.startError = "";
            findChild(app, "listenMenu").visible = false;
        }
        // The value the argv gives --trunk-scan-hold, or "" with none.
        function heldTarget(args) {
            var at = args.indexOf("--trunk-scan-hold");
            return at < 0 ? "" : args[at + 1];
        }
        function listenButton() {
            return findChild(findChild(app, "monitorScreen"), "monitorListenButton");
        }

        // One scan-list row on the tuned channel: the Monitor names it and offers to listen with its settings, read
        // out with the row's name; the action stops the Explore session and starts the list held on that row, for
        // that start only. Nothing keeps the hold: not the attempt Retry repeats, nor the remembered start.
        function test_listen_with_its_settings_starts_the_list_held_on_the_row() {
            var hospital = addScanList("Hospital", "ER desk", "852.0125");
            var desk = hospital.entries[0].uid;
            var kind = prefs.acceptedStartKind;
            var uid = prefs.acceptedStartUid;
            try {
                exploreOn("852.012500");
                var button = listenButton();
                tryVerify(function () { return button.visible; });
                compare(button.text, "Listen with its settings");
                compare(button.Accessible.name, "Listen to Hospital / ER desk with its settings");
                compare(findChild(findChild(app, "monitorScreen"), "monitorListenNames").text, "Hospital / ER desk");
                // Off the channel, or out of Explore, there is nothing to offer.
                testContext.setMetric("centerFreqHz", 853500000);
                tryVerify(function () { return !button.visible; });
                testContext.setMetric("centerFreqHz", 852012500);
                tryVerify(function () { return button.visible; });

                var stops = decoderHost.stopCalls();
                button.activate();
                verify(decoderHost.stopCalls() > stops, "the Explore session was not stopped");
                tryCompare(decoderHost, "sessionState", 1);
                compare(heldTarget(testContext.lastStartArgs()), desk);
                compare(prefs.acceptedStartKind, "scan");
                compare(prefs.acceptedStartUid, hospital.uid);
                verify(app.sessionScanList && !app.exploring);
                testContext.emitSessionInitialized();
                compare(prefs.lastStartedUid, hospital.uid);
                // Consumed: Retry after a failure, and the list's next start, scan it as ever.
                testContext.setLifecycleFailure("Connection refused");
                testContext.setLifecyclePhase(4);
                app.retrySource();
                tryCompare(decoderHost, "sessionState", 1);
                verify(testContext.lastStartArgs().indexOf("--trunk-scan") >= 0);
                compare(heldTarget(testContext.lastStartArgs()), "");
                testContext.setLifecycleFailure("");
                testContext.setLifecyclePhase(0);
                app.startScanList(scanLists.rowForUid(hospital.uid));
                compare(heldTarget(testContext.lastStartArgs()), "");
            } finally {
                endExplore();
                testContext.setLifecycleFailure("");
                testContext.setLifecyclePhase(2);
                prefs.acceptedStartKind = kind;
                prefs.acceptedStartUid = uid;
                removeScanLists([hospital.uid]);
            }
        }

        // The hold is carried through a stop the host takes its time over and a USB permission wait, and dropped by a
        // cancel, a dismissed wait, a start the host refuses, and another start in its place.
        function test_listen_with_its_settings_is_one_shot() {
            var hospital = addScanList("Hospital", "ER desk", "852.0125");
            scanLists.add({name: "Gate", sourceType: "usb",
                entries: [{kind: "freq", name: "Front", protocol: "p25", freqMhz: "852.5125", enabled: true}]});
            var gate = scanLists.get(scanLists.count - 1);
            var desk = hospital.entries[0].uid;
            var front = gate.entries[0].uid;
            try {
                // 1. A slow stop: the start waits for Idle, then holds.
                exploreOn("852.012500");
                testContext.setDelayedStop(true);
                listenButton().activate();
                compare(decoderHost.sessionState, 3);
                verify(app.pendingRestart && app.pendingRestart.holdTarget === desk);
                testContext.setLifecyclePhase(0);
                tryCompare(decoderHost, "sessionState", 1);
                compare(heldTarget(testContext.lastStartArgs()), desk);
                // 2. Cancelled while it stops (Back): nothing starts.
                exploreOn("852.012500");
                listenButton().activate();
                app.requestBack();
                verify(!app.pendingRestart);
                testContext.setLifecyclePhase(0);
                wait(0);
                compare(decoderHost.sessionState, 0);
                // 3. Another start in its place takes no hold.
                exploreOn("852.012500");
                listenButton().activate();
                app.startScanList(scanLists.rowForUid(hospital.uid));
                verify(!app.pendingRestart);
                compare(heldTarget(testContext.lastStartArgs()), "");
                // 4. A USB permission wait carries it to the grant.
                testContext.setDelayedStop(false);
                exploreOn("852.512500");
                testContext.setScanTestUsb(true, false);
                listenButton().activate();
                tryVerify(function () { return app.awaitingUsbAccess; });
                compare(app.pendingStartHold, front);
                testContext.setScanTestUsb(true, true);
                tryCompare(decoderHost, "sessionState", 1);
                compare(heldTarget(testContext.lastStartArgs()), front);
                compare(app.pendingStartHold, "");
                // 5. A dismissed wait drops it: the list's next start holds nothing.
                exploreOn("852.512500");
                testContext.setScanTestUsb(true, false);
                listenButton().activate();
                tryVerify(function () { return app.awaitingUsbAccess; });
                app.dismissedFailure = app.failureText.length > 0 ? app.failureText : "USB";
                verify(!app.awaitingUsbAccess);
                testContext.setScanTestUsb(true, true);
                compare(decoderHost.sessionState, 0);
                app.dismissedFailure = "";
                app.startScanList(scanLists.rowForUid(gate.uid));
                tryCompare(decoderHost, "sessionState", 1);
                compare(heldTarget(testContext.lastStartArgs()), "");
                // 6. A start the host refuses drops it: Retry starts the list unheld.
                testContext.setScanTestUsb(false, false);
                exploreOn("852.012500");
                testContext.setScanTestAcceptStart(false);
                listenButton().activate();
                tryVerify(function () { return heldTarget(testContext.lastStartArgs()) === desk; });
                compare(decoderHost.sessionState, 0);
                testContext.setScanTestAcceptStart(true);
                app.retrySource();
                tryCompare(decoderHost, "sessionState", 1);
                compare(heldTarget(testContext.lastStartArgs()), "");
            } finally {
                endExplore();
                testContext.setLifecyclePhase(2);
                removeScanLists([hospital.uid, gate.uid]);
            }
        }

        // Several on the channel: the action opens the menu of them, scan-list rows by name (read out as what they
        // do) and a saved system as "Start “Name”", an avoided one disabled with the reason; the pick starts it. One
        // saved system alone is offered as "Start “Name”" directly, with no hold, from the Monitor and the Spectrum.
        function test_listen_with_its_settings_offers_every_match() {
            var hospital = addScanList("Hospital", "ER desk", "852.0125");
            var security = addScanList("Security", "Gate", "852.0125");
            savedSystems.add({name: "County", sourceType: "rtltcp", host: "127.0.0.1", port: 1234,
                freqMhz: "852.0125"});
            var county = savedSystems.get(savedSystems.count - 1).uid;
            savedSystems.add({name: "Shop", sourceType: "rtltcp", host: "127.0.0.1", port: 1234, freqMhz: "852.0125",
                avoidSite: true});
            var shop = savedSystems.get(savedSystems.count - 1).uid;
            try {
                exploreOn("852.012500");
                var button = listenButton();
                tryVerify(function () { return button.visible; });
                compare(button.text, "Listen with its settings");
                compare(button.Accessible.name, "Listen with its settings: 4 to choose from");
                button.activate();
                var menu = findChild(app, "listenMenu");
                tryVerify(function () { return menu.visible; });
                compare(menu.title, "Listen with its settings");
                compare(menu.actions.map(function (a) { return a.text; }).join("|"),
                        "Hospital / ER desk|Security / Gate|Start “County”|Start “Shop”");
                var first = findChild(menu, "actionMenuAction0");
                compare(first.Accessible.name, "Listen to Hospital / ER desk with its settings");
                var avoided = findChild(menu, "actionMenuAction3");
                verify(!avoided.enabled);
                compare(avoided.Accessible.name,
                        "Start “Shop”. Avoided; remove the avoid in its site list to start it");
                findChild(menu, "actionMenuAction1").activate();
                verify(!menu.visible);
                tryCompare(decoderHost, "sessionState", 1);
                compare(prefs.acceptedStartUid, security.uid);
                compare(heldTarget(testContext.lastStartArgs()), security.entries[0].uid);
                // The lists gone, the one saved system that can start: its own name, no menu, no hold.
                removeScanLists([hospital.uid, security.uid]);
                savedSystems.remove(savedSystems.rowForUid(shop));
                exploreOn("852.012500");
                tryCompare(button, "text", "Start “County”");
                compare(button.Accessible.name, "Start “County”");
                verify(!findChild(findChild(app, "monitorScreen"), "monitorListenNames").visible);
                // The Spectrum offers the same action under its tuning controls.
                findChild(app, "spectrumLoader").active = true;
                app.spectrumOpen = true;
                var spectrum = findChild(app, "spectrumScreen");
                var spectrumButton = findChild(spectrum, "spectrumListenWithSettings");
                tryVerify(function () { return spectrumButton.visible; });
                compare(spectrumButton.text, "Start “County”");
                spectrumButton.activate();
                tryCompare(decoderHost, "sessionState", 1);
                compare(prefs.acceptedStartKind, "saved");
                compare(prefs.acceptedStartUid, county);
                compare(heldTarget(testContext.lastStartArgs()), "");
            } finally {
                endExplore();
                app.spectrumOpen = false;
                testContext.setLifecyclePhase(2);
                removeScanLists([hospital.uid, security.uid]);
                for (var uid of [county, shop]) {
                    var row = savedSystems.rowForUid(uid);
                    if (row >= 0)
                        savedSystems.remove(row);
                }
            }
        }

        // The only match on the channel is a saved system that cannot start (its site avoided): the Monitor's and the
        // Spectrum's button say why to a screen reader and open the menu, whose one row is disabled with the reason.
        // Nothing starts.
        function test_listen_shows_a_lone_disabled_action_with_its_reason() {
            savedSystems.add({name: "Shop", sourceType: "rtltcp", host: "127.0.0.1", port: 1234, freqMhz: "852.0125",
                avoidSite: true});
            var shop = savedSystems.get(savedSystems.count - 1).uid;
            var reason = "Avoided; remove the avoid in its site list to start it";
            try {
                exploreOn("852.012500");
                var button = listenButton();
                tryVerify(function () { return button.visible; });
                compare(button.text, "Start “Shop”");
                compare(button.Accessible.name, "Start “Shop”. " + reason);
                var stops = decoderHost.stopCalls();
                button.activate();
                var menu = findChild(app, "listenMenu");
                tryVerify(function () { return menu.visible; });
                var row = findChild(menu, "actionMenuAction0");
                verify(!row.enabled);
                compare(row.Accessible.name, "Start “Shop”. " + reason);
                row.activate();
                compare(decoderHost.stopCalls(), stops);
                verify(!app.pendingRestart);
                menu.visible = false;
                // The Spectrum's button does the same.
                findChild(app, "spectrumLoader").active = true;
                app.spectrumOpen = true;
                var spectrumButton = findChild(findChild(app, "spectrumScreen"), "spectrumListenWithSettings");
                tryVerify(function () { return spectrumButton.visible; });
                compare(spectrumButton.Accessible.name, "Start “Shop”. " + reason);
                spectrumButton.activate();
                tryVerify(function () { return menu.visible; });
                compare(decoderHost.stopCalls(), stops);
            } finally {
                endExplore();
                app.spectrumOpen = false;
                testContext.setLifecyclePhase(2);
                var at = savedSystems.rowForUid(shop);
                if (at >= 0)
                    savedSystems.remove(at);
            }
        }

        // A scan list's entry for a saved system is a target of that list too: on the system's channel, Explore
        // offers to listen with the list held on that entry, beside starting the saved system itself.
        function test_listen_offers_a_list_held_on_its_saved_system_entry() {
            savedSystems.add({name: "County", sourceType: "rtltcp", host: "127.0.0.1", port: 1234,
                freqMhz: "852.0125", decodeFlag: "-fs"});
            var county = savedSystems.get(savedSystems.count - 1).uid;
            scanLists.add({name: "Weekend", sourceType: "rtltcp", host: "127.0.0.1", port: 1234,
                entries: [{kind: "system", systemUid: county, enabled: true}]});
            var weekend = scanLists.get(scanLists.count - 1);
            try {
                exploreOn("852.012500");
                var button = listenButton();
                tryVerify(function () { return button.visible; });
                compare(button.Accessible.name, "Listen with its settings: 2 to choose from");
                button.activate();
                var menu = findChild(app, "listenMenu");
                tryVerify(function () { return menu.visible; });
                compare(menu.actions.map(function (a) { return a.text; }).join("|"),
                        "Weekend / County|Start “County”");
                compare(findChild(menu, "actionMenuAction0").Accessible.name,
                        "Listen to Weekend / County with its settings");
                findChild(menu, "actionMenuAction0").activate();
                tryCompare(decoderHost, "sessionState", 1);
                compare(prefs.acceptedStartKind, "scan");
                compare(prefs.acceptedStartUid, weekend.uid);
                compare(heldTarget(testContext.lastStartArgs()), weekend.entries[0].uid);
            } finally {
                endExplore();
                testContext.setLifecyclePhase(2);
                removeScanLists([weekend.uid]);
                var row = savedSystems.rowForUid(county);
                if (row >= 0)
                    savedSystems.remove(row);
            }
        }

        // A paused scan-list session reaches its own list's saved-system entry in place, as it reaches any of its own
        // rows: the entry's tune at the system's frequency, with no stop and no restart.
        function test_a_paused_scan_list_reaches_its_saved_system_entry_in_place() {
            savedSystems.add({name: "County", sourceType: "rtltcp", host: "127.0.0.1", port: 1234,
                freqMhz: "852.0125", decodeFlag: "-fs"});
            var county = savedSystems.get(savedSystems.count - 1).uid;
            scanLists.add({name: "Weekend", sourceType: "rtltcp", host: "127.0.0.1", port: 1234,
                entries: [{kind: "freq", name: "Gate", protocol: "p25", freqMhz: "851.0125", enabled: true},
                          {kind: "system", systemUid: county, enabled: true}]});
            var weekend = scanLists.get(scanLists.count - 1);
            try {
                testContext.setLifecyclePhase(0);
                app.startScanBundle(scanLists.rowForUid(weekend.uid), []);
                testContext.setLifecyclePhase(2);
                testContext.setMetrics({optionsKnown: true, operatorTuned: true, scanKind: 2,
                                        scanRotationActive: false, scanPaused: true, scanResumable: true,
                                        tunerControlled: false, centerFreqHz: 852014000, channelBandwidthHz: 12500});
                tryVerify(function () { return app.operatorHolds && app.exploring; });
                var button = listenButton();
                tryVerify(function () { return button.visible; });
                compare(button.text, "Listen with its settings");
                compare(button.Accessible.name, "Listen to Weekend / County with its settings");
                testContext.resetCommands();
                var stops = decoderHost.stopCalls();
                button.activate();
                compare(testContext.tuneHzCalls(), 1);
                compare(testContext.lastTuneHz(), 852012500);
                compare(decoderHost.stopCalls(), stops);
                verify(!app.pendingRestart, "the paused session was set to restart");
            } finally {
                endExplore();
                testContext.setMetrics({operatorTuned: false, scanPaused: false, scanResumable: false,
                                        tunerControlled: false});
                testContext.setLifecyclePhase(2);
                removeScanLists([weekend.uid]);
                var row = savedSystems.rowForUid(county);
                if (row >= 0)
                    savedSystems.remove(row);
            }
        }

        // A scan-list session the operator paused keeps its lists: a row of one of them is reached in place by the
        // entry's tune at the row's own frequency (the scan resumes there and holds it), with no stop and no restart;
        // a row of a list the session does not scan, or a saved system, would end the session and is not offered.
        function test_a_paused_scan_list_reaches_its_own_rows_in_place() {
            var security = addScanList("Security", "Gate", "851.0125");
            var hospital = addScanList("Hospital", "ER desk", "852.0125");
            var parks = addScanList("Parks", "Ranger", "853.0125");
            savedSystems.add({name: "County", sourceType: "rtltcp", host: "127.0.0.1", port: 1234,
                freqMhz: "853.0125"});
            var county = savedSystems.get(savedSystems.count - 1).uid;
            try {
                testContext.setLifecyclePhase(0);
                app.startScanBundle(scanLists.rowForUid(security.uid), [hospital.uid]);
                testContext.setLifecyclePhase(2);
                // Paused by a tap onto the companion list's row.
                testContext.setMetrics({optionsKnown: true, operatorTuned: true, scanKind: 2,
                                        scanRotationActive: false, scanPaused: true, scanResumable: true,
                                        tunerControlled: false, centerFreqHz: 852014000, channelBandwidthHz: 12500});
                tryVerify(function () { return app.operatorHolds && app.exploring; });
                var button = listenButton();
                tryVerify(function () { return button.visible; });
                compare(button.text, "Listen with its settings");
                compare(button.Accessible.name, "Listen to Hospital / ER desk with its settings");
                testContext.resetCommands();
                var stops = decoderHost.stopCalls();
                button.activate();
                compare(testContext.tuneHzCalls(), 1);
                compare(testContext.lastTuneHz(), 852012500);
                compare(decoderHost.stopCalls(), stops);
                verify(!app.pendingRestart, "the paused session was set to restart");
                compare(app.sessionStartCompanions.join(), hospital.uid);
                // A list the session does not scan, and a saved system, on the channel: nothing to offer.
                testContext.setMetric("centerFreqHz", 853012500);
                tryVerify(function () { return !button.visible; });
                compare(app.listenActions.length, 0);
            } finally {
                endExplore();
                testContext.setMetrics({operatorTuned: false, scanPaused: false, scanResumable: false,
                                        tunerControlled: false});
                testContext.setLifecyclePhase(2);
                removeScanLists([security.uid, hospital.uid, parks.uid]);
                var row = savedSystems.rowForUid(county);
                if (row >= 0)
                    savedSystems.remove(row);
            }
        }

        // A paused scan-list session with @p security and @p hospital (its companion), paused on @p hz.
        function pauseBundleOn(security, hospital, hz) {
            testContext.setLifecyclePhase(0);
            app.startScanBundle(scanLists.rowForUid(security.uid), [hospital.uid]);
            testContext.setLifecyclePhase(2);
            testContext.setMetrics({optionsKnown: true, operatorTuned: true, scanKind: 2,
                                    scanRotationActive: false, scanPaused: true, scanResumable: true,
                                    tunerControlled: false, centerFreqHz: hz, channelBandwidthHz: 12500});
            tryVerify(function () { return app.operatorHolds && app.exploring; });
        }
        function endPausedBundle(uids) {
            endExplore();
            testContext.resetScanLists();
            testContext.setScanAvoids([]);
            testContext.setMetrics({operatorTuned: false, scanPaused: false, scanResumable: false,
                                    tunerControlled: false, scanAvoidCount: 0});
            testContext.setLifecyclePhase(2);
            removeScanLists(uids);
        }

        // A paused scan-list session reaches in place only a row its session would take now: not one on a list
        // switched off, nor one avoided. Either offers nothing.
        function test_a_paused_scan_list_offers_only_rows_its_session_takes() {
            var security = addScanList("Security", "Gate", "851.0125");
            var hospital = addScanList("Hospital", "ER desk", "852.0125");
            var desk = hospital.entries[0].uid;
            try {
                pauseBundleOn(security, hospital, 852012500);
                testContext.setScanLists({paused: true, lists: [{name: "Security", on: true, targets: 1},
                                                                {name: "Hospital", on: false, targets: 1}]});
                compare(app.listenActions.length, 0, "a list switched off");
                testContext.setScanLists({paused: true, lists: [{name: "Security", on: true, targets: 1},
                                                                {name: "Hospital", on: true, targets: 1}]});
                compare(app.listenActions.length, 1, "the list switched back on");
                testContext.setScanAvoids([{key: "trunk:7:" + desk, label: "ER desk", targetId: desk}]);
                testContext.setMetric("scanAvoidCount", 1);
                compare(app.listenActions.length, 0, "an avoided target");
                testContext.setScanAvoids([]);
                testContext.setMetric("scanAvoidCount", 0);
                compare(app.listenActions.length, 1, "the target put back");
            } finally {
                endPausedBundle([security.uid, hospital.uid]);
            }
        }

        // Two of the session's targets on one frequency, an NXDN48 row and a P25 row, paused 4 kHz above it: the
        // tuned frequency is that channel, since the P25 row's 12.5 kHz covers it, but the in-place Listen sends only
        // the frequency, and the scan takes the first row on it it would take, in list order. The action names that
        // row: NXDN48 when its list comes first, P25 when the order is the other way round.
        function test_a_paused_scan_list_names_the_row_its_scan_takes_data() {
            return [{tag: "NXDN48 first", first: "nxdn48", second: "p25", named: "Security / Gate", label: "NXDN48"},
                    {tag: "P25 first", first: "p25", second: "nxdn48", named: "Security / Gate", label: "P25"}];
        }
        function test_a_paused_scan_list_names_the_row_its_scan_takes(data) {
            scanLists.add({name: "Security", sourceType: "rtltcp", host: "127.0.0.1", port: 1234,
                entries: [{kind: "freq", name: "Gate", protocol: data.first, freqMhz: "851.0125", enabled: true}]});
            var security = scanLists.get(scanLists.count - 1);
            scanLists.add({name: "Hospital", sourceType: "rtltcp", host: "127.0.0.1", port: 1234,
                entries: [{kind: "freq", name: "Lobby", protocol: data.second, freqMhz: "851.0125", enabled: true}]});
            var hospital = scanLists.get(scanLists.count - 1);
            try {
                pauseBundleOn(security, hospital, 851016500);
                compare(app.listenActions.length, 1);
                compare(app.listenActions[0].hz, 851012500);
                compare(app.listenActions[0].name, data.named);
                compare(app.listenActions[0].protocolLabel, data.label);
                compare(app.listenActions[0].accessibleName, "Listen to " + data.named + " with its settings");
            } finally {
                endPausedBundle([security.uid, hospital.uid]);
            }
        }

        // The session's own targets are the ones it started with, taken when its start was accepted: deleting its
        // saved lists while it runs (which removes their generated target files), then pausing it, changes nothing the
        // paused session reaches in place.
        function test_a_paused_scan_list_keeps_its_targets_when_its_lists_are_deleted() {
            var security = addScanList("Security", "Gate", "851.0125");
            var hospital = addScanList("Hospital", "ER desk", "852.0125");
            try {
                testContext.setLifecyclePhase(0);
                app.startScanBundle(scanLists.rowForUid(security.uid), [hospital.uid]);
                testContext.setLifecyclePhase(2);
                removeScanLists([hospital.uid, security.uid]);
                testContext.setMetrics({optionsKnown: true, operatorTuned: true, scanKind: 2,
                                        scanRotationActive: false, scanPaused: true, scanResumable: true,
                                        tunerControlled: false, centerFreqHz: 851012500, channelBandwidthHz: 12500});
                tryVerify(function () { return app.operatorHolds && app.exploring; });
                compare(app.listenActions.length, 1, "the session's own row");
                compare(app.listenActions[0].key, "tune");
                compare(app.listenActions[0].hz, 851012500);
                compare(app.listenActions[0].name, "Security / Gate");
                testContext.setMetric("centerFreqHz", 852012500);
                compare(app.listenActions.length, 1, "the companion's row");
                compare(app.listenActions[0].name, "Hospital / ER desk");
            } finally {
                endPausedBundle([security.uid, hospital.uid]);
            }
        }

        // A recreated Activity takes the running session's own targets up as its start was accepted, not from the saved
        // lists' files: lists deleted while the session runs (their generated target files with them) still leave
        // their rows reached in place once the session is paused.
        function test_a_reattached_scan_list_keeps_its_targets_when_its_lists_are_deleted() {
            var security = addScanList("Security", "Gate", "851.0125");
            var hospital = addScanList("Hospital", "ER desk", "852.0125");
            var kind = prefs.acceptedStartKind;
            var uid = prefs.acceptedStartUid;
            try {
                testContext.setLifecyclePhase(0);
                app.startScanBundle(scanLists.rowForUid(security.uid), [hospital.uid]);
                testContext.setLifecyclePhase(2);
                removeScanLists([hospital.uid, security.uid]);
                reattach();
                compare(app.sessionStartKind, "scan");
                testContext.setMetrics({optionsKnown: true, operatorTuned: true, scanKind: 2,
                                        scanRotationActive: false, scanPaused: true, scanResumable: true,
                                        tunerControlled: false, centerFreqHz: 851012500, channelBandwidthHz: 12500});
                tryVerify(function () { return app.operatorHolds && app.exploring; });
                compare(app.listenActions.length, 1, "the session's own row");
                compare(app.listenActions[0].key, "tune");
                compare(app.listenActions[0].hz, 851012500);
                compare(app.listenActions[0].name, "Security / Gate");
                testContext.setMetric("centerFreqHz", 852012500);
                compare(app.listenActions.length, 1, "the companion's row");
                compare(app.listenActions[0].name, "Hospital / ER desk");
            } finally {
                endPausedBundle([security.uid, hospital.uid]);
                prefs.acceptedStartKind = kind;
                prefs.acceptedStartUid = uid;
                prefs.acceptedStartCompanions = [];
            }
        }

        // A paused scan-list session reaches its rows as it started them: a row saved on another frequency since is
        // still reached where the session runs it, under the name it started with, and not at the frequency saved.
        function test_a_paused_scan_list_reaches_rows_as_its_session_started_them() {
            var security = addScanList("Security", "Gate", "851.0125");
            var hospital = addScanList("Hospital", "ER desk", "852.0125");
            try {
                pauseBundleOn(security, hospital, 851012500);
                var edited = scanLists.get(scanLists.rowForUid(security.uid));
                edited.entries[0].freqMhz = "853.0125";
                verify(scanLists.update(scanLists.rowForUid(security.uid), edited));
                compare(app.listenActions.length, 1, "the frequency the session runs");
                compare(app.listenActions[0].key, "tune");
                compare(app.listenActions[0].hz, 851012500);
                compare(app.listenActions[0].name, "Security / Gate");
                testContext.setMetric("centerFreqHz", 853012500);
                compare(app.listenActions.length, 0, "the frequency saved since");
            } finally {
                endPausedBundle([security.uid, hospital.uid]);
            }
        }

        // "Explore from here" on a scan list marks the session exploring before the decoder reports the operator's
        // tune: in that moment its own rows are still reached in place, and nothing that restarts it is offered.
        function test_a_released_scan_list_offers_no_restart_before_the_tune_reads_back() {
            var security = addScanList("Security", "Gate", "851.0125");
            var parks = addScanList("Parks", "Ranger", "853.0125");
            try {
                testContext.setLifecyclePhase(0);
                app.startScanBundle(scanLists.rowForUid(security.uid), []);
                testContext.setLifecyclePhase(2);
                testContext.setMetrics({optionsKnown: true, operatorTuned: false, scanKind: 2,
                                        scanRotationActive: true, scanPaused: false, tunerControlled: false,
                                        centerFreqHz: 851012500, channelBandwidthHz: 12500});
                app.exploring = true;
                verify(!app.operatorHolds);
                verify(app.sessionScanList);
                compare(app.listenActions.length, 1);
                compare(app.listenActions[0].key, "tune");
                testContext.setMetric("centerFreqHz", 853012500);
                compare(app.listenActions.length, 0);
            } finally {
                endExplore();
                testContext.setMetrics({operatorTuned: false, scanPaused: false, scanResumable: false,
                                        scanRotationActive: false, tunerControlled: false});
                testContext.setLifecyclePhase(2);
                removeScanLists([security.uid, parks.uid]);
            }
        }

        // Two rows of one name on the channel (one list's entries for two protocols) say their protocols in the
        // menu and to a screen reader, so they never read the same; a name of its own stays plain.
        function test_listen_names_two_rows_of_one_name_by_protocol() {
            scanLists.add({name: "Hospital", sourceType: "rtltcp", host: "127.0.0.1", port: 1234,
                entries: [{kind: "freq", name: "Gate", protocol: "p25", freqMhz: "852.0125", enabled: true},
                          {kind: "freq", name: "Gate", protocol: "dmr", freqMhz: "852.0125", enabled: true},
                          {kind: "freq", name: "Lobby", protocol: "nxdn48", freqMhz: "852.0125", enabled: true}]});
            var hospital = scanLists.get(scanLists.count - 1);
            try {
                exploreOn("852.012500");
                var button = listenButton();
                tryVerify(function () { return button.visible; });
                compare(findChild(findChild(app, "monitorScreen"), "monitorListenNames").text,
                        "Hospital / Gate / Hospital / Lobby");
                button.activate();
                var menu = findChild(app, "listenMenu");
                tryVerify(function () { return menu.visible; });
                compare(menu.actions.map(function (a) { return a.text; }).join("|"),
                        "Hospital / Gate (P25)|Hospital / Gate (DMR)|Hospital / Lobby");
                compare(findChild(menu, "actionMenuAction1").Accessible.name,
                        "Listen to Hospital / Gate (DMR) with its settings");
                compare(findChild(menu, "actionMenuAction2").Accessible.name,
                        "Listen to Hospital / Lobby with its settings");
            } finally {
                endExplore();
                testContext.setLifecyclePhase(2);
                removeScanLists([hospital.uid]);
            }
        }

        // The held start on air: while its park is still tuning the scan runs unheld, and the Hold button and the
        // header say so; once the park lands the hold is the entry's, shown held, and the ordinary Hold toggle
        // releases it.
        function test_an_entry_hold_shows_held_and_the_hold_toggle_releases_it() {
            var hospital = addScanList("Hospital", "ER desk", "852.0125");
            var desk = hospital.entries[0].uid;
            var monitor = findChild(app, "monitorScreen");
            var header = function () { return findChild(monitor, "scanTargetHeader").text; };
            try {
                exploreOn("852.012500");
                listenButton().activate();
                tryCompare(decoderHost, "sessionState", 1);
                compare(heldTarget(testContext.lastStartArgs()), desk);
                testContext.setLifecyclePhase(2);
                testContext.setMetrics({optionsKnown: true, scanKind: 2, scanRotationActive: true, scanPaused: false,
                                        operatorTuned: false, tunerControlled: true, scanTargetId: desk,
                                        scanTargetOrdinal: 1, scanTargetCount: 1, scanHold: false,
                                        scanEntryHold: false});
                var hold = findChild(monitor, "scanHoldButton");
                tryVerify(function () { return hold.visible; });
                verify(!hold.checked, "a park still tuning showed held");
                compare(header(), "SCANNING · 1/1 · ER desk");
                verify(!listenButton().visible, "a scan list's session offered to listen to its own row");
                testContext.setMetrics({scanHold: true, scanEntryHold: true});
                tryVerify(function () { return hold.checked; });
                compare(header(), "SCANNING · 1/1 · HOLD · ER desk");
                testContext.resetCommands();
                hold.activate();
                compare(testContext.scanHoldCalls(), 1);
                testContext.setMetrics({scanHold: false, scanEntryHold: false});
                tryVerify(function () { return !hold.checked; });
                compare(header(), "SCANNING · 1/1 · ER desk");
            } finally {
                endExplore();
                testContext.setMetrics({operatorTuned: false, scanPaused: false, scanResumable: false,
                                        tunerControlled: false});
                testContext.setLifecyclePhase(2);
                removeScanLists([hospital.uid]);
            }
        }

        // A list removed on Home while its session runs keeps its place: the decoder still runs it as list 2, so after
        // a recreated Activity list 3 is still Parks, the title still counts two more lists, and the list removed is
        // named by the decoder. The next start forgets it; the running session's record does not.
        function test_a_removed_list_keeps_its_place_in_a_running_session() {
            var title = function () { return findChild(findChild(app, "monitorScreen"), "monitorHeaderTitle").text; };
            var security = addScanList("Security", "Gate", "851.0125");
            var hospital = addScanList("Hospital", "ER desk", "852.0125");
            var parks = addScanList("Parks", "Ranger", "853.0125");
            var kind = prefs.acceptedStartKind;
            var uid = prefs.acceptedStartUid;
            try {
                testContext.setLifecyclePhase(0);
                app.startScanBundle(scanLists.rowForUid(security.uid), [hospital.uid, parks.uid]);
                testContext.emitSessionInitialized();
                testContext.setLifecyclePhase(2);
                verify(decoderHost.sessionActive);
                testContext.setScanLists({session: 7, seq: 3, lists: [{name: "Security", targets: 1},
                    {name: "Hospital", targets: 1, onAir: true}, {name: "Parks", targets: 1}]});
                var er = hospital.entries[0].uid;
                scanMetrics(er, 2);
                removeScanLists([hospital.uid]);
                compare(prefs.acceptedStartCompanions.join(), [hospital.uid, parks.uid].join(),
                        "the running session's record was cut");
                compare(prefs.lastStartedCompanions.join(), parks.uid);
                prefs.acceptedStartKind = "scan";
                reattach();
                compare(app.sessionListName(1), "Security");
                compare(app.sessionListName(2), "");
                compare(app.sessionListName(3), "Parks");
                tryVerify(function () { return title() === "Security + 2 lists"; }, 5000, title());
                var monitor = findChild(app, "monitorScreen");
                tryCompare(monitor, "scanListName", "Hospital");
                scanMetrics(parks.entries[0].uid, 3);
                tryCompare(monitor, "scanListName", "Parks");
                compare(app.scanTargetLabel(), "Ranger");
            } finally {
                clearScanMetrics();
                testContext.setLifecyclePhase(0);
                testContext.setLifecyclePhase(2);
                prefs.acceptedStartKind = kind;
                prefs.acceptedStartUid = uid;
                prefs.acceptedStartCompanions = [];
                prefs.lastStartedCompanions = [];
                removeScanLists([security.uid, parks.uid]);
            }
        }

        // On Android the UI loads before the host is polled, so a recreated Activity reads the host idle while its
        // session in fact runs: the load's prune must not cut the accepted start's record then, or list 3 is named
        // after list 2's place once the host reads Running.
        function test_a_removed_list_keeps_its_place_when_the_host_reads_idle_at_load() {
            var title = function () { return findChild(findChild(app, "monitorScreen"), "monitorHeaderTitle").text; };
            var security = addScanList("Security", "Gate", "851.0125");
            var hospital = addScanList("Hospital", "ER desk", "852.0125");
            var parks = addScanList("Parks", "Ranger", "853.0125");
            var kind = prefs.acceptedStartKind;
            var uid = prefs.acceptedStartUid;
            try {
                prefs.acceptedStartKind = "scan";
                prefs.acceptedStartUid = security.uid;
                prefs.acceptedStartCompanions = [hospital.uid, parks.uid];
                removeScanLists([hospital.uid]);
                // The Activity comes back with the host not yet polled.
                testContext.setLifecyclePhase(0);
                loader.active = false;
                wait(0);
                loader.active = true;
                tryCompare(loader, "status", Loader.Ready);
                compare(prefs.acceptedStartCompanions.join(), [hospital.uid, parks.uid].join(),
                        "the load cut the running session's record");
                // The first poll finds the session running.
                testContext.setLifecyclePhase(2);
                tryVerify(function () { return app.sessionLists.length === 3; }, 5000);
                compare(app.sessionListName(2), "");
                compare(app.sessionListName(3), "Parks");
                tryVerify(function () { return title() === "Security + 2 lists"; }, 5000, title());
            } finally {
                testContext.setLifecyclePhase(2);
                prefs.acceptedStartKind = kind;
                prefs.acceptedStartUid = uid;
                prefs.acceptedStartCompanions = [];
                prefs.lastStartedCompanions = [];
                removeScanLists([security.uid, parks.uid]);
            }
        }

        // A list that cannot join, or two lists sharing a target id, start nothing; the refusal names the list.
        function test_a_scan_bundle_refused_starts_nothing() {
            var security = addScanList("Security", "Gate", "851.0125");
            scanLists.add({name: "Twin", sourceType: "rtltcp", host: "127.0.0.1", port: 1234,
                entries: [{uid: security.entries[0].uid, kind: "freq", name: "Gate", protocol: "p25", freqMhz: "854.0125",
                    enabled: true}]});
            var twin = scanLists.get(scanLists.count - 1);
            try {
                testContext.setLifecyclePhase(0);
                var before = testContext.lastStartArgs();
                app.startScanBundle(scanLists.rowForUid(security.uid), [twin.uid]);
                verify(app.startError.indexOf("Scan lists rejected: ") === 0, app.startError);
                compare(testContext.lastStartArgs(), before);
                compare(decoderHost.sessionState, 0);
            } finally {
                app.startError = "";
                testContext.setLifecyclePhase(2);
                removeScanLists([security.uid, twin.uid]);
            }
        }

        function test_a_reattached_scan_list_keeps_its_identity() {
            var title = function () { return findChild(findChild(app, "monitorScreen"), "monitorHeaderTitle").text; };
            scanLists.add({name: "County scan", sourceType: "rtltcp", host: "127.0.0.1", port: 1234,
                entries: [{kind: "freq", name: "Simplex", protocol: "p25", freqMhz: "851.5", enabled: true}]});
            var row = scanLists.count - 1;
            var list = scanLists.get(row);
            var entryUid = list.entries[0].uid;
            verify(entryUid && entryUid.length > 0, "the fixture's entry has no UID");
            var kind = prefs.acceptedStartKind;
            var uid = prefs.acceptedStartUid;
            prefs.acceptedStartKind = "scan";
            prefs.acceptedStartUid = list.uid;
            try {
                reattach();
                verify(app.sessionScanList, "the reattached session forgot it is a scan list");
                compare(title(), "County scan");
                testContext.setMetrics({optionsKnown: true, scanKind: 2, scanRotationActive: true, scanPaused: false,
                                        scanResumable: false, operatorTuned: false, tunerControlled: true,
                                        scanTargetId: entryUid, scanTargetOrdinal: 1, scanTargetCount: 1});
                compare(app.scanTargetLabel(), "Simplex");
                // Issue #671: a call named by its channel reads the entry's name, never its uid.
                compare(callHistory.channelNames()[entryUid], "Simplex");
                compare(metrics.channelNames[entryUid], "Simplex");
                // Go to (from any frontend) pauses it: the receiver is the operator's, by hand too.
                testContext.setMetrics({scanRotationActive: false, scanPaused: true, scanResumable: true,
                                        operatorTuned: true, tunerControlled: false, scanTargetId: "",
                                        scanTargetOrdinal: 0});
                compare(title(), "Exploring");
                verify(app.exploring, "a reattached paused scan list stayed view-only");
                compare(callHistory.channelNames()[entryUid], "Simplex", "the paused list's names went with the pause");
                choose("Spectrum");
                var spectrum = findChild(app, "spectrumScreen");
                verify(!spectrum.viewOnly, "the reattached paused list's spectrum is VIEW ONLY");
                verify(!findChild(spectrum, "spectrumExploreFromHere").visible, "it offers Explore from here");
                var resume = findChild(spectrum, "spectrumResumeScan");
                verify(resume.visible && resume.enabled, "it offers no Resume scan");
                resume.clicked();
                compare(testContext.resumeScanCalls(), 1);
                app.spectrumOpen = false;
                // The resume lands on the target it left.
                testContext.setMetrics({scanRotationActive: true, scanPaused: false, scanResumable: false,
                                        operatorTuned: false, tunerControlled: true, scanTargetId: entryUid,
                                        scanTargetOrdinal: 1});
                compare(title(), "County scan");
                verify(!app.exploring, "the resumed list stayed free to tap-tune");
                compare(app.scanTargetLabel(), "Simplex");
            } finally {
                testContext.setMetrics({operatorTuned: false, scanKind: 0, scanRotationActive: false, scanPaused: false,
                                        scanResumable: false, tunerControlled: false, optionsKnown: false,
                                        scanTargetId: "", scanTargetOrdinal: 0, scanTargetCount: 0});
                prefs.acceptedStartKind = kind;
                prefs.acceptedStartUid = uid;
                scanLists.remove(row);
            }
        }
        // The list was already paused when the Activity went: the recreated UI
        // holds the list for Resume scan before it names the session Exploring,
        // so the resume brings the list back, not History's label.
        function test_a_reattached_paused_scan_list_resumes_to_its_list() {
            var title = function () { return findChild(findChild(app, "monitorScreen"), "monitorHeaderTitle").text; };
            scanLists.add({name: "County scan", sourceType: "rtltcp", host: "127.0.0.1", port: 1234,
                entries: [{kind: "freq", name: "Simplex", protocol: "p25", freqMhz: "851.5", enabled: true}]});
            var row = scanLists.count - 1;
            var list = scanLists.get(row);
            var kind = prefs.acceptedStartKind;
            var uid = prefs.acceptedStartUid;
            prefs.acceptedStartKind = "scan";
            prefs.acceptedStartUid = list.uid;
            callHistory.sessionLabel = "Label from history";
            try {
                testContext.setMetrics({optionsKnown: true, scanKind: 2, scanRotationActive: false, scanPaused: true,
                                        scanResumable: true, operatorTuned: true, tunerControlled: false});
                reattach();
                compare(title(), "Exploring");
                verify(app.exploring, "the reattached paused list is not tunable by hand");
                verify(app.pausedScanSystem !== null && app.pausedScanSystem.name === "County scan",
                       "the hold did not keep the list");
                testContext.setMetrics({scanRotationActive: true, scanPaused: false, scanResumable: false,
                                        operatorTuned: false, tunerControlled: true});
                compare(title(), "County scan");
                verify(!app.exploring);
            } finally {
                testContext.setMetrics({operatorTuned: false, scanKind: 0, scanRotationActive: false, scanPaused: false,
                                        scanResumable: false, tunerControlled: false, optionsKnown: false});
                prefs.acceptedStartKind = kind;
                prefs.acceptedStartUid = uid;
                callHistory.sessionLabel = "Test Site";
                scanLists.remove(row);
            }
        }
        // A saved system reattached keeps its map and row, as a start in this UI
        // has them: its title, Edit saved system and the key sheet's system.
        function test_a_reattached_saved_system_keeps_its_map() {
            var title = function () { return findChild(findChild(app, "monitorScreen"), "monitorHeaderTitle").text; };
            savedSystems.add({name: "Reattached site", sourceType: "usb", freqMhz: "851.5"});
            var row = savedSystems.count - 1;
            var sys = savedSystems.get(row);
            var kind = prefs.acceptedStartKind;
            var uid = prefs.acceptedStartUid;
            prefs.acceptedStartKind = "saved";
            prefs.acceptedStartUid = sys.uid;
            callHistory.sessionLabel = "Label from history";
            try {
                reattach();
                compare(title(), "Reattached site");
                compare(app.sessionRow, row);
                compare(app.sessionSavedSystemUid, sys.uid);
                compare(findChild(findChild(app, "monitorScreen"), "keySheet").systemUid, sys.uid);
                verify(menuOffersEdit(), "the reattached saved system offered no edit");
                verify(!app.exploring, "a reattached saved system became tunable by hand");
            } finally {
                prefs.acceptedStartKind = kind;
                prefs.acceptedStartUid = uid;
                callHistory.sessionLabel = "Test Site";
                savedSystems.remove(row);
            }
        }
        // The Activity goes while start B is still starting, after start A was
        // recorded at its initialization edge: the recreated UI takes B, the
        // start the host accepted last, and leaves A's record to Home.
        function test_a_reattach_while_starting_takes_the_accepted_start() {
            var title = function () { return findChild(findChild(app, "monitorScreen"), "monitorHeaderTitle").text; };
            scanLists.add({name: "Scan A", sourceType: "rtltcp", host: "127.0.0.1", port: 1234,
                entries: [{kind: "freq", name: "Simplex", protocol: "p25", freqMhz: "851.5", enabled: true}]});
            var listRow = scanLists.count - 1;
            savedSystems.add({name: "System B", sourceType: "rtltcp", host: "127.0.0.1", port: 1234, freqMhz: "852.5"});
            var row = savedSystems.count - 1;
            var kind = prefs.lastStartedKind;
            var uid = prefs.lastStartedUid;
            prefs.lastStartedKind = "scan";
            prefs.lastStartedUid = scanLists.get(listRow).uid;
            try {
                testContext.setLifecyclePhase(0);
                app.startSystem(row);
                compare(decoderHost.sessionState, 1);
                compare(prefs.acceptedStartKind, "saved");
                compare(prefs.acceptedStartUid, savedSystems.get(row).uid);
                compare(prefs.lastStartedKind, "scan", "the start wrote Home's record before it initialized");
                reattach();
                testContext.setLifecyclePhase(2);
                verify(!app.sessionScanList, "the recreated UI took the last initialized start's list");
                compare(title(), "System B");
                compare(app.sessionRow, row);
            } finally {
                prefs.lastStartedKind = kind;
                prefs.lastStartedUid = uid;
                savedSystems.remove(row);
                scanLists.remove(listRow);
            }
        }
        // With no session map to name the receiver (an unknown session, or an
        // Explore session's first tune after a recreation), the hold's Explore map
        // takes the running input's own kind, so the pill and Save as system match
        // the receiver, never an assumed USB one.
        function test_a_held_tuner_keeps_the_running_receiver_data() {
            // The pill opens a receiver sheet only for a local receiver; rtl_tcp keeps an indicator.
            return [{tag: "rtl_tcp", source: {sourceType: "rtltcp", host: "10.0.0.2", port: 1235}, receiver: ""},
                    {tag: "airspy", source: {sourceType: "airspy"}, receiver: "airspy"},
                    {tag: "usb", source: {sourceType: "usb"}, receiver: "usb"}];
        }
        function test_a_held_tuner_keeps_the_running_receiver(data) {
            var kind = prefs.acceptedStartKind;
            var uid = prefs.acceptedStartUid;
            prefs.acceptedStartKind = "";
            prefs.acceptedStartUid = "";
            testContext.setScanTestUsb(true, true);
            try {
                reattach();
                compare(app.sessionSystem, null);
                testContext.setMetrics({optionsKnown: true, radioSource: data.source, operatorTuned: true});
                verify(app.sessionSystem !== null);
                compare(app.sessionSystem.name, "Exploring");
                compare(app.sessionSystem.sourceType, data.source.sourceType);
                if (data.source.host !== undefined) {
                    compare(app.sessionSystem.host, data.source.host);
                    compare(app.sessionSystem.port, data.source.port);
                }
                compare(findChild(app, "monitorScreen").sessionReceiver, data.receiver);
            } finally {
                testContext.setScanTestUsb(false, false);
                testContext.setMetrics({operatorTuned: false, optionsKnown: false, radioSource: {}});
                prefs.acceptedStartKind = kind;
                prefs.acceptedStartUid = uid;
            }
        }
        // The same for an Explore session: reattached, it is still free to tune.
        function test_a_reattached_explore_session_still_explores() {
            var kind = prefs.acceptedStartKind;
            var uid = prefs.acceptedStartUid;
            prefs.acceptedStartKind = "explore";
            prefs.acceptedStartUid = "";
            try {
                reattach();
                verify(app.exploring, "a reattached Explore session became view-only");
                verify(!app.sessionScanList);
                choose("Spectrum");
                verify(!findChild(app, "spectrumScreen").viewOnly);
                app.spectrumOpen = false;
            } finally {
                prefs.acceptedStartKind = kind;
                prefs.acceptedStartUid = uid;
            }
        }
        function test_actual_history_instances_share_search_across_stop() {
            var idle = findChild(findChild(app, "idleHistoryScreen"), "historySearch");
            var live = findChild(findChild(app, "sessionHistoryScreen"), "historySearch");
            historyView.filterText = "dispatch";
            choose("History");
            compare(live.text, "dispatch");
            live.input.forceActiveFocus();
            live.input.selectAll();
            keyClick(Qt.Key_F);
            tryCompare(historyView, "filterText", "f");
            compare(idle.text, "f");
            testContext.setLifecyclePhase(0);
            compare(app.currentTab, 1);
            compare(idle.text, "f");
            historyView.filterText = "";
            compare(live.text, "");
        }
        function test_history_pending_edit_handoff_data() {
            return [{tag: "session-to-idle", stop: true}, {tag: "idle-to-session", stop: false}];
        }
        function test_history_pending_edit_handoff(data) {
            var idle = findChild(findChild(app, "idleHistoryScreen"), "historySearch");
            var live = findChild(findChild(app, "sessionHistoryScreen"), "historySearch");
            historyView.filterText = "";
            if (data.stop) {
                choose("History");
            } else {
                testContext.setLifecyclePhase(0);
                app.currentTab = 1;
                wait(0);
            }
            var departing = data.stop ? live : idle;
            var arriving = data.stop ? idle : live;
            departing.input.forceActiveFocus();
            keyClick(Qt.Key_O);
            compare(departing.text, "o");
            wait(50);
            compare(historyView.filterText, "", "typing must remain debounced");
            if (data.stop) {
                testContext.setLifecyclePhase(0);
                compare(app.currentTab, 1);
                wait(0);
            } else {
                testContext.setLifecyclePhase(2);
                choose("History");
            }
            arriving.input.forceActiveFocus();
            arriving.input.selectAll();
            keyClick(Qt.Key_N);
            compare(arriving.text, "n");
            wait(100);
            verify(historyView.filterText !== "n", "the new editor must retain the typing debounce");
            tryCompare(historyView, "filterText", "n");
            compare(idle.text, "n");
            compare(live.text, "n");
            wait(300);
            compare(historyView.filterText, "n", "no departing timer may overwrite the newer query");
        }

        function test_non_radio_menu_and_compact_fallbacks() {
            testContext.setMetric("radioInput", false);
            var monitor = findChild(app, "monitorScreen");
            monitor.openSessionMenu();
            var menu = findChild(app, "sessionMenu");
            verify(findChild(menu, "sessionMenuRadio") === null);
            verify(findChild(menu, "sessionMenuSpectrum") === null);
            menu.cancel();
            testContext.setMetric("radioInput", true);
            app.height = 360;
            monitor.openSessionMenu();
            for (var key of ["Radio", "Spectrum", "Talkgroups", "History", "Settings", "Diagnostics"])
                verify(findChild(menu, "sessionMenu" + key) !== null);
            app.height = 900;
        }
    }
}
