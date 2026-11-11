// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// Issue #646 D6: which Listen tab shows after a flow, through Main. A saved system selects Systems and a saved scan
// list (or draft) selects Scan lists, for the next time Home shows; a screen still on top stays there. A failed save,
// a cancel, a removal, a pin, a refusal or a failure keeps the tab. Also through Main: the failure card on every tab and
// taken down by a save of what it names (B10), a failed start from a sheet opened on a deep card keeping that card in
// view (B3 precedence), and Signal search opening from the Explore tab.
Item {
    id: root

    width: 420
    height: 900

    Loader {
        id: appLoader

        source: uiDir + "/Main.qml"
    }

    // What the scan editor said, in order.
    property var editorSignals: []
    Connections {
        target: tc.editor
        function onSaved() {
            root.editorSignals.push("saved");
        }
        function onClosed() {
            root.editorSignals.push("closed");
        }
    }

    // A detection's receiver, as SignalSearchModel::systemForHit() gives it.
    QtObject {
        id: searchStub
        property var status: ({phase: 0, owned: false, session: "0", hit: {}})
        property var sessionAvoids: []
        property var ranges: []
        property var history: []
        property var avoids: []
        property string error: ""
        property string storeMessage: ""
        function control(action, session, hit) { return true; }
        function arm() { return ""; }
        function cancelArm() {}
        function removeAvoid(frequency) {}
        function avoidFrequency(session, frequency) { return true; }
        function avoidPermanently(frequency) { return true; }
        function systemForHit(hit, source) {
            return Object.assign({}, source, {freqMhz: hit.freqMHz, decodeFlag: "-fA", gainDb: 21, trunking: false,
                searchUnresolved: false});
        }
    }

    TestCase {
        id: tc

        name: "ListenTabsFlows"
        when: windowShown && appLoader.status === Loader.Ready

        property var app: appLoader.item
        readonly property var home: app ? findChild(app, "homeScreen") : null
        readonly property var editor: app ? findChild(app, "scanListScreen") : null
        readonly property var wizard: app ? findChild(app, "wizardScreen") : null
        readonly property string draftText: "This scan list is a draft. Edit and save it before listening."
        readonly property var tabs: ["systems", "scanLists", "explore"]
        property var saved: ({})

        function init() {
            testContext.useLifecycleHost(true);
            testContext.resetPins();
            testContext.resetListenSection();
            testContext.setScanTestUsb(false, false);
            testContext.setScanTestAcceptStart(true);
            tc.saved = {onboardingDone: prefs.onboardingDone, autoStartOnAttach: prefs.autoStartOnAttach};
            prefs.onboardingDone = true;
            prefs.autoStartOnAttach = false;
            prefs.lastStartedKind = "";
            prefs.lastStartedUid = "";
            clearSources();
            app.currentTab = 0;
            app.startError = "";
            app.attemptedSource = null;
            app.dismissedFailure = "";
            home.showSection("systems");
            home.resetScrollMemory();
            root.editorSignals = [];
        }

        function cleanup() {
            for (var modal of Ui.Navigation.modals.slice())
                modal.visible = false;
            for (var store of ["saved_systems.json", "scan_lists.json"])
                testContext.blockPath(testContext.storePath(store), false);
            app.wizardOpen = false;
            app.scanListOpen = false;
            app.signalSearchOpen = false;
            findChild(app, "signalSearchScreen").searchModel = null;
            decoderHost.stop();
            testContext.setLifecyclePhase(0);
            testContext.setLifecycleFailure("");
            app.startError = "";
            app.attemptedSource = null;
            app.dismissedFailure = "";
            app.currentTab = 0;
            testContext.setScanTestUsb(false, false);
            testContext.setScanTestAcceptStart(true);
            prefs.onboardingDone = tc.saved.onboardingDone;
            prefs.autoStartOnAttach = tc.saved.autoStartOnAttach;
            prefs.lastStartedKind = "";
            prefs.lastStartedUid = "";
            clearSources();
            home.showSection("systems");
            home.resetScrollMemory();
            testContext.resetPins();
            testContext.resetListenSection();
            testContext.useLifecycleHost(false);
            root.forceActiveFocus();
        }

        function clearSources() {
            while (savedSystems.count)
                savedSystems.remove(0);
            while (scanLists.count)
                scanLists.remove(0);
        }
        function addSystem(name) {
            verify(savedSystems.add({name: name, sourceType: "rtltcp", host: "127.0.0.1", port: 1234,
                freqMhz: "851.5"}), "could not add " + name);
            return savedSystems.get(savedSystems.count - 1).uid;
        }
        function addList(name, fields) {
            verify(scanLists.add(Object.assign({name: name, sourceType: "rtltcp", host: "127.0.0.1", port: 1234,
                entries: [{kind: "freq", name: "Simplex", protocol: "p25", freqMhz: "851.5", enabled: true}]},
                fields || {})), "could not add " + name);
            return scanLists.get(scanLists.count - 1).uid;
        }
        function block(store, blocked) {
            verify(testContext.blockPath(testContext.storePath(store), blocked), "could not change " + store);
        }
        function visibleNamed(node, name, found) {
            found = found || [];
            for (var child of node.children) {
                if (child.objectName === name && child.visible)
                    found.push(child);
                visibleNamed(child, name, found);
            }
            return found;
        }
        // Home's section Column for tab @a tab (listenSystemsSection, listenScanListsSection, listenExploreSection).
        function sectionOf(tab) {
            return findChild(home, "listen" + tab.charAt(0).toUpperCase() + tab.substring(1) + "Section");
        }
        // Home is on screen, on tab @a id: its section the only one shown and its tab the current one.
        function compareShown(id, why) {
            tryCompare(home, "visible", true);
            compare(home.section, id, why);
            for (var tab of tc.tabs) {
                var section = sectionOf(tab);
                compare(section.visible, tab === id, why + ": only " + id + "'s section is shown");
                compare(findChild(home, "listenTab_" + tab).active, tab === id, why + ": " + id + "'s tab is current");
            }
        }
        function inView(item, body) {
            var y = item.mapToItem(body.contentItem, 0, 0).y;
            return y >= body.contentY - 0.5 && y + item.height <= body.contentY + body.height + 0.5;
        }
        function activateMain() {
            app.requestActivate();
            tryCompare(app, "active", true);
        }
        // Tab (at most @a limit times) until the item named @a name in @a scope has the keyboard focus.
        function tabTo(scope, name, limit) {
            for (var i = 0; i < limit; ++i) {
                var focused = app.activeFocusItem;
                if (focused && focused.objectName.indexOf(name) === 0 && Ui.Navigation.contains(scope, focused))
                    return focused;
                keyClick(Qt.Key_Tab);
            }
            fail("no Tab stop named " + name);
            return null;
        }

        // A saved system selects Systems for the next time Home shows, from any tab and either bottom tab, add or edit.
        function test_wizard_save_shows_systems_data() {
            var rows = [];
            for (var from of ["scanLists", "explore"]) {
                for (var kind of ["add", "edit"]) {
                    rows.push({tag: kind + " from " + from, from: from, kind: kind, bottomTab: 0},
                        {tag: kind + " from " + from + ", History shown", from: from, kind: kind, bottomTab: 1});
                }
            }
            return rows;
        }
        function test_wizard_save_shows_systems(data) {
            var uid = addSystem("County");
            home.showSection(data.from);
            app.currentTab = data.bottomTab;
            if (data.kind === "add")
                home.addSystem();
            else
                home.editSystem(savedSystems.rowForUid(uid));
            verify(app.wizardOpen);
            tryCompare(home, "visible", false);
            wizard.nameText = data.kind === "add" ? "Added" : "Renamed";
            wizard.commit();
            verify(!app.wizardOpen, "the wizard saved and closed");
            compare(savedSystems.count, data.kind === "add" ? 2 : 1);
            compare(app.currentTab, 0);
            compareShown("systems", "a saved system shows on Systems");
            compare(prefs.listenSection, "systems", "and that is the tab remembered");
        }

        // Save as system from Signal search: the search stays on top; Listen shows Systems once it closes.
        function test_save_detection_keeps_signal_search_on_top() {
            home.showSection("explore");
            app.openSignalSearch();
            var search = findChild(app, "signalSearchScreen");
            search.searchModel = searchStub;
            verify(app.signalSearchOpen);
            search.saveDetection({id: "42", session: "17", freqMHz: "463.552100", frequency: 463552100, power: -32.1,
                ended: false, confirmed: false, matches: []});
            verify(app.wizardOpen);
            wizard.nameText = "Found";
            wizard.commit();
            verify(!app.wizardOpen);
            compare(savedSystems.count, 1);
            verify(app.signalSearchOpen, "Signal search is still on top");
            verify(search.visible);
            verify(!home.visible, "Home is still under it");
            compare(home.section, "systems", "Systems is selected for when Home shows");
            search.closed();
            verify(!app.signalSearchOpen);
            compareShown("systems", "Listen shows Systems after Signal search closes");
            tryVerify(function () {
                return visibleNamed(home, "savedSystemPlay").length === 1;
            }, 1000, "the saved system is listed");
        }

        // Update from RadioReference signed out, from Listen or from the site chooser, opens the account screen
        // over Listen (the chooser makes way for it), and the tab stays.
        function test_update_signed_out_opens_account_data() {
            return [{tag: "Listen", from: "home"}, {tag: "site chooser", from: "chooser"}];
        }
        function test_update_signed_out_opens_account(data) {
            var chooser = findChild(app, "siteChooserSheet");
            try {
                testContext.setRrPlacesSignedIn(false);
                verify(savedSystems.add({name: "County", sourceType: "rtltcp", host: "127.0.0.1", port: 1234,
                    freqMhz: "851.5", rrSid: 12, rrSiteId: 100}));
                if (data.from === "home") {
                    findChild(home, "savedSystemsControls").updateAllFromRadioReference();
                } else {
                    app.openSiteChooser(0);
                    tryCompare(chooser, "visible", true);
                    chooser.updateFromRadioReference();
                    verify(!chooser.visible);
                }
                verify(app.radioReferenceAccountOpen, "the account screen opens");
                compare(home.section, "systems");
            } finally {
                app.radioReferenceAccountOpen = false;
                chooser.visible = false;
                testContext.resetRrPlaces();
            }
        }

        // Edit saved system over a session: the monitor stays on top; Listen shows Systems once the session ends.
        function test_session_edit_keeps_the_monitor_on_top() {
            var uid = addSystem("County");
            home.showSection("explore");
            app.startSystem(savedSystems.rowForUid(uid));
            verify(app.monitorMode, "the session is on screen");
            compare(app.sessionRow, savedSystems.rowForUid(uid));
            findChild(app, "monitorScreen").openSessionMenu();
            var menu = findChild(app, "sessionMenu");
            verify(menu.visible);
            findChild(menu, "sessionMenuEdit").activate();
            verify(app.wizardOpen);
            wizard.nameText = "Renamed";
            wizard.commit();
            verify(!app.wizardOpen);
            verify(app.monitorMode && findChild(app, "monitorScreen").visible, "the monitor is still on top");
            verify(!home.visible);
            compare(home.section, "systems", "Systems is selected for when Home shows");
            decoderHost.stop();
            verify(!app.monitorMode);
            compareShown("systems", "Listen shows Systems after the session");
        }

        // A failed wizard save keeps the wizard open and the tab; so does a cancel.
        function test_wizard_failure_and_cancel_keep_the_tab_data() {
            return [{tag: "failed add", how: "fail", kind: "add"}, {tag: "failed edit", how: "fail", kind: "edit"},
                {tag: "cancel add", how: "cancel", kind: "add"}, {tag: "cancel edit", how: "cancel", kind: "edit"}];
        }
        function test_wizard_failure_and_cancel_keep_the_tab(data) {
            var uid = addSystem("County");
            home.showSection("explore");
            if (data.kind === "add")
                home.addSystem();
            else
                home.editSystem(savedSystems.rowForUid(uid));
            verify(app.wizardOpen);
            if (data.how === "fail") {
                wizard.nameText = "Not saved";
                block("saved_systems.json", true);
                wizard.commit();
                block("saved_systems.json", false);
                verify(app.wizardOpen, "a failed save keeps the wizard open");
                compare(savedSystems.get(0).name, "County");
                compare(home.section, "explore", "a failed save selects nothing");
                app.wizardOpen = false;
            } else {
                wizard.requestBack();
                verify(!app.wizardOpen);
            }
            compare(savedSystems.count, 1);
            compareShown("explore", "the tab is kept");
        }

        // A saved scan list or draft says `saved`, then `closed`, and shows on Scan lists when Home shows.
        function test_scan_editor_save_shows_scan_lists_data() {
            var rows = [];
            for (var from of ["systems", "explore"]) {
                for (var how of ["save", "saveDraft"]) {
                    rows.push({tag: how + " new from " + from, from: from, how: how, edit: false},
                        {tag: how + " edit from " + from, from: from, how: how, edit: true});
                }
            }
            return rows;
        }
        function test_scan_editor_save_shows_scan_lists(data) {
            var uid = addList("County lists");
            home.showSection(data.from);
            if (data.edit) {
                home.editScanList(scanLists.rowForUid(uid));
            } else {
                home.addScanList();
                editor.draft = Object.assign({}, editor.draft, {name: "New list", sourceType: "rtltcp",
                    host: "127.0.0.1", port: 1234});
                editor.addFrequency("Simplex", "p25", "851.5");
            }
            verify(app.scanListOpen);
            tryCompare(home, "visible", false);
            root.editorSignals = [];
            if (data.how === "save")
                editor.save();
            else
                editor.saveDraft();
            compare(root.editorSignals.join(), "saved,closed", editor.validationText);
            verify(!app.scanListOpen);
            compare(scanLists.count, data.edit ? 1 : 2);
            compare(scanLists.get(scanLists.count - 1).isDraft, data.how === "saveDraft");
            compareShown("scanLists", "a saved list shows on Scan lists");
            compare(prefs.listenSection, "scanLists");
        }

        // A save that does not persist (the store refuses it, or the list does not validate), a cancel and a removal
        // say nothing but `closed` (or nothing at all) and keep the tab.
        function test_scan_editor_without_a_save_keeps_the_tab_data() {
            return [{tag: "save refused by storage", how: "save", fail: "storage"},
                {tag: "draft refused by storage", how: "saveDraft", fail: "storage"},
                {tag: "save of an unnamed list", how: "save", fail: "name"},
                {tag: "draft with a bad tuner setting", how: "saveDraft", fail: "port"},
                {tag: "list removed meanwhile", how: "save", fail: "removed"},
                {tag: "cancel unchanged", how: "cancel"},
                {tag: "cancel discarding changes", how: "discard"},
                {tag: "remove", how: "remove"}];
        }
        function test_scan_editor_without_a_save_keeps_the_tab(data) {
            var uid = addList("County lists");
            home.showSection("explore");
            home.editScanList(scanLists.rowForUid(uid));
            verify(app.scanListOpen);
            root.editorSignals = [];
            if (data.how === "save" || data.how === "saveDraft") {
                if (data.fail === "storage")
                    block("scan_lists.json", true);
                else if (data.fail === "name")
                    editor.setDraft("name", "");
                else if (data.fail === "port")
                    editor.portText = "70000";
                else
                    verify(scanLists.remove(scanLists.rowForUid(uid)));
                editor[data.how]();
                if (data.fail === "storage")
                    block("scan_lists.json", false);
                compare(root.editorSignals.join(), "", "nothing saved, nothing closed");
                verify(app.scanListOpen, "the editor stays open");
                verify(editor.validationText.length > 0, "and says why");
                compare(home.section, "explore", "nothing is selected");
                app.scanListOpen = false;
            } else if (data.how === "cancel") {
                editor.requestClose();
                compare(root.editorSignals.join(), "closed");
            } else if (data.how === "discard") {
                editor.setDraft("name", "Changed");
                editor.requestClose();
                var discard = findChild(editor, "scanListDiscardDialog");
                verify(discard.visible);
                findChild(discard, "discardDialogDiscard").clicked();
                compare(root.editorSignals.join(), "closed");
                compare(scanLists.get(0).name, "County lists");
            } else {
                findChild(editor, "scanRemoveList").clicked();
                findChild(findChild(editor, "scanListRemoveDialog"), "scanRemoveConfirm").clicked();
                compare(root.editorSignals.join(), "closed");
                compare(scanLists.count, 0);
            }
            verify(!app.scanListOpen);
            compareShown("explore", "the tab is kept");
        }

        // Removing a source from Listen, pinning and unpinning keep the tab.
        function test_removal_and_pins_keep_the_tab_data() {
            return [{tag: "remove a system", tab: "systems"}, {tag: "remove a scan list", tab: "scanLists"},
                {tag: "pin and unpin a system", tab: "systems", pin: "system"},
                {tag: "pin and unpin a scan list", tab: "scanLists", pin: "scan"},
                {tag: "pin and unpin Signal search", tab: "explore", pin: "search"},
                {tag: "pin and unpin Explore", tab: "explore", pin: "explore"}];
        }
        function test_removal_and_pins_keep_the_tab(data) {
            var system = addSystem("County");
            var list = addList("County lists");
            home.showSection(data.tab);
            if (!data.pin) {
                var confirm = data.tab === "systems" ? "removeSystemConfirm" : "removeScanListConfirm";
                if (data.tab === "systems")
                    findChild(home, "savedSystemManageSheet").openFor(0, "County");
                else
                    findChild(home, "scanListManageSheet").openFor(0);
                var menu = data.tab === "systems" ? findChild(home, "savedSystemManageSheet")
                    : findChild(home, "scanListManageSheet");
                menu.activate(menu.actions.map(function (action) { return action.key; }).indexOf("remove"));
                var dialog = findChild(home, confirm);
                verify(dialog.visible, confirm + " asks first");
                findChild(dialog, "confirmActionButton").clicked();
                compare(data.tab === "systems" ? savedSystems.count : scanLists.count, 0);
            } else {
                var uid = data.pin === "system" ? system : data.pin === "scan" ? list : "";
                home.togglePin(data.pin, uid, false);
                verify(listenPins.isPinned(data.pin, uid), "pinned");
                compareShown(data.tab, "a pin keeps the tab");
                home.togglePin(data.pin, uid, true);
                verify(!listenPins.isPinned(data.pin, uid), "unpinned");
            }
            compareShown(data.tab, "the tab is kept");
        }

        // A refusal and a failure keep the tab, and the failure card shows at the top of whichever tab is shown.
        function test_failure_card_on_every_tab_data() {
            var rows = [];
            for (var tab of ["systems", "scanLists", "explore"])
                rows.push({tag: "refusal on " + tab, tab: tab, how: "refusal"},
                    {tag: "host failure on " + tab, tab: tab, how: "failure"});
            return rows;
        }
        function test_failure_card_on_every_tab(data) {
            for (var i = 0; i < 10; ++i)
                addSystem("Filler " + i);
            for (i = 0; i < 6; ++i)
                addList("Filler list " + i);
            var draft = addList("Draft", {isDraft: true});
            home.showSection(data.tab);
            var body = findChild(home, "dongleScrollBody");
            waitForRendering(home);
            if (data.how === "refusal") {
                app.startPin("scan", draft);
                compare(app.startError, tc.draftText);
            } else {
                testContext.setLifecycleFailure("Connection refused");
            }
            verify(app.showFailure);
            var card = findChild(home, "homeFailureCard");
            compareShown(data.tab, "a " + data.how + " keeps the tab");
            verify(card.visible, "the failure card shows on " + data.tab);
            verify(waitForPolish(home));
            waitForRendering(home);
            verify(card.height > 0);
            verify(inView(card, body), "at the top of the body, in view");
            for (var tab of tc.tabs) {
                home.showSection(tab);
                var section = sectionOf(tab);
                verify(card.visible, "the failure card shows on " + tab);
                tryVerify(function () {
                    return section.height > 0 && card.mapToItem(body.contentItem, 0, 0).y + card.height
                        <= section.mapToItem(body.contentItem, 0, 0).y;
                }, 2000, "above " + tab + "'s section");
            }
        }

        // B10: a successful save of the scan list or saved system the failure card names takes the card down on every
        // tab, as its X does. A card naming another list or system stays, and so does the card after a failed save.
        function test_saving_the_named_source_clears_its_failure_data() {
            var rows = [];
            for (var target of ["scan", "saved"]) {
                var how = target === "scan" ? "draft refusal" : "host failure";
                rows.push({tag: how + ", that one saved", target: target, save: "same", cleared: true},
                    {tag: how + ", another saved", target: target, save: "other", cleared: false},
                    {tag: how + ", save failed", target: target, save: "fail", cleared: false});
            }
            return rows;
        }
        function test_saving_the_named_source_clears_its_failure(data) {
            var scan = data.target === "scan";
            var named = scan ? addList("Draft", {isDraft: true}) : addSystem("County");
            var other = scan ? addList("Other lists") : addSystem("Other");
            if (scan) {
                home.showSection("scanLists");
                app.startPin("scan", named);
                compare(app.startError, tc.draftText);
            } else {
                app.startSystem(savedSystems.rowForUid(named));
                verify(app.monitorMode);
                testContext.setLifecycleFailure("Connection refused");
                decoderHost.stop();
                verify(!app.monitorMode);
            }
            verify(app.showFailure, "the card shows");
            compare(app.attemptedSource.uid, named, "and names that " + (scan ? "list" : "system"));
            var uid = data.save === "other" ? other : named;
            var store = scan ? "scan_lists.json" : "saved_systems.json";
            if (scan)
                home.editScanList(scanLists.rowForUid(uid));
            else
                home.editSystem(savedSystems.rowForUid(uid));
            verify(scan ? app.scanListOpen : app.wizardOpen);
            if (data.save === "fail")
                block(store, true);
            if (scan) {
                editor.save();
            } else {
                wizard.nameText = "Renamed";
                wizard.commit();
            }
            if (data.save === "fail") {
                block(store, false);
                verify(scan ? app.scanListOpen : app.wizardOpen, "a failed save stays in the editor");
                app.scanListOpen = false;
                app.wizardOpen = false;
            } else {
                verify(!app.scanListOpen && !app.wizardOpen, "saved and closed");
            }
            compare(app.showFailure, !data.cleared, data.cleared ? "the card is taken down" : "the card stays");
            var card = findChild(home, "homeFailureCard");
            for (var tab of tc.tabs) {
                home.showSection(tab);
                compareShown(tab, "on " + tab);
                compare(card.visible, !data.cleared, data.cleared ? "the card is gone from " + tab
                    : "the card stays on " + tab);
            }
        }

        // §4 B3 (1): "Scan with other lists…" from the keyboard on a deep card, then a failed start (brokered, where a
        // failure scrolls Listen to its top). The closing sheet hands the focus back to the deep card before the start,
        // which leaves a focus reveal pending, and again a turn later (ModalSheet), after the failure was revealed; the
        // failure card stays in view through every deferred callback and layout pass.
        function test_failed_start_from_a_deep_card_stays_in_view() {
            for (var i = 0; i < 12; ++i)
                addList("List " + i);
            testContext.setScanTestUsb(true, true);
            testContext.setScanTestAcceptStart(false);
            home.showSection("scanLists");
            var body = findChild(home, "dongleScrollBody");
            tryCompare(home, "visible", true);
            verify(waitForPolish(home));
            waitForRendering(home);
            activateMain();
            var mores = visibleNamed(findChild(home, "listenScanListsSection"), "scanListManageButton");
            compare(mores.length, 12);
            var more = mores[mores.length - 1];
            body.contentY = body.originY + body.contentHeight - body.height;
            verify(body.contentY > body.originY + 300, "the body scrolled to the last card");
            more.forceActiveFocus();
            verify(waitForPolish(home));
            waitForRendering(home);
            verify(more.activeFocus && inView(more, body), "the last card's More has the keyboard");

            keyClick(Qt.Key_Return);
            var menu = findChild(home, "scanListManageSheet");
            verify(menu.visible, "Return opens the card's menu");
            tabTo(menu, "scanWithOtherListsButton", 8);
            keyClick(Qt.Key_Return);
            var sheet = findChild(home, "scanTogetherSheet");
            tryCompare(sheet, "visible", true);
            tabTo(sheet, "scanTogetherRow_", 8);
            keyClick(Qt.Key_Space);
            compare(sheet.chosen.length, 1, "Space switches a list on");
            tabTo(sheet, "scanTogetherStart", 16);
            keyClick(Qt.Key_Return);
            verify(!sheet.visible, "Return starts the lists");

            verify(app.showFailure, "the start failed");
            compare(decoderHost.sessionState, 0);
            var card = findChild(home, "homeFailureCard");
            verify(card.visible);
            for (i = 0; i < 3; ++i) {
                verify(waitForPolish(home));
                waitForRendering(home);
            }
            verify(more.activeFocus, "the sheet handed the focus back to the deep card");
            verify(!inView(more, body), "which is out of view now");
            fuzzyCompare(body.contentY, body.originY, 0.5, "the body is at its top");
            verify(inView(card, body), "the failure card is in view after every deferred callback");
            fuzzyCompare(home.sectionScroll.scanLists, body.originY, 0.5, "and the top is where Scan lists was left");
        }

        // Signal search opens from the Explore tab, by a tap and from the keyboard.
        function test_signal_search_from_explore_data() {
            return [{tag: "tap", key: 0}, {tag: "Return", key: Qt.Key_Return},
                {tag: "keypad Enter", key: Qt.Key_Enter, modifiers: Qt.KeypadModifier}, {tag: "Space", key: Qt.Key_Space}];
        }
        function test_signal_search_from_explore(data) {
            home.showSection("explore");
            var button = findChild(home, "homeSignalSearch");
            tryCompare(home, "visible", true);
            verify(waitForPolish(home));
            waitForRendering(home);
            verify(button.visible && button.enabled, "Signal search is on the Explore tab");
            if (data.key === 0) {
                mouseClick(button);
            } else {
                activateMain();
                button.forceActiveFocus();
                verify(button.activeFocus);
                keyClick(data.key, data.modifiers || Qt.NoModifier);
            }
            verify(app.signalSearchOpen, "Signal search opened");
            verify(findChild(app, "signalSearchScreen").visible);
            compare(home.section, "explore", "and Explore stays the tab");
            app.signalSearchOpen = false;
            compareShown("explore", "Listen is back on Explore");
        }
    }
}
