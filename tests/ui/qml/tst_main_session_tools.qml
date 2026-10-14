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
        function test_settings_subpage_escape_data() {
            return [
                {tag: "imports", signal: "openImports", property: "importsOpen", screen: "importsScreen"},
                {tag: "diagnostics", signal: "openDiagnostics", property: "diagnosticsOpen", screen: "diagnosticsScreen"},
                {tag: "licenses", signal: "openLicenses", property: "licensesOpen", screen: "licensesScreen"},
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
