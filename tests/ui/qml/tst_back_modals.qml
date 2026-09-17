// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

Item {
    width: 420
    height: 900

    Loader {
        id: loader
        active: false
        source: uiDir + "/Main.qml"
    }
    Component.onCompleted: {
        testContext.useLifecycleHost(true);
        loader.active = true;
    }

    SignalSpy {
        id: historyCleared
        target: callHistory
        signalName: "modelReset"
    }
    SignalSpy {
        id: confirmed
        signalName: "confirmed"
    }
    SignalSpy {
        id: cancelled
        signalName: "cancelled"
    }

    TestCase {
        name: "BackModals"
        when: windowShown && loader.status === Loader.Ready
        property var app: loader.item
        property bool oldOnboarding: false
        property bool oldAutoStart: false
        property bool oldBackgroundListening: false

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
        function item(name, root) {
            var found = findChild(root || app, name);
            verify(found !== null, name + " exists");
            return found;
        }
        function tap(button) {
            verify(waitForPolish(app.contentItem));
            waitForRendering(button);
            mouseClick(button, button.width / 2, button.height / 2);
        }
        function init() {
            testContext.useLifecycleHost(true);
            oldOnboarding = prefs.onboardingDone;
            oldAutoStart = prefs.autoStartOnAttach;
            oldBackgroundListening = prefs.backgroundListening;
            prefs.onboardingDone = true;
            prefs.autoStartOnAttach = false;
            prefs.backgroundListening = false;
            app.currentTab = 0;
            app.sessionDestination = "";
            app.startError = "";
            while (savedSystems.count)
                savedSystems.remove(0);
            savedSystems.add({name: "County radio", sourceType: "usb", freqMhz: "851.5"});
            callHistory.clearAll();
            callHistory.push("TODAY");
            confirmed.target = null;
            cancelled.target = null;
            historyCleared.clear();
            app.requestActivate();
            tryCompare(app, "active", true);
            tryCompare(item("homeScreen"), "enabled", true);
            item("dongleScrollBody").contentY = 0;
            wait(200); // Wait for the idle shell's opacity transition.
        }
        function cleanup() {
            for (var modal of Ui.Navigation.modals.slice())
                modal.visible = false;
            var management = visualChild(item("homeScreen"), function (entry) {
                return entry.systemName === "County radio";
            });
            if (management)
                management.visible = false;
            app.wizardOpen = false;
            app.licensesOpen = false;
            app.radioReferenceAccountOpen = false;
            app.scanListOpen = false;
            app.spectrumOpen = false;
            app.talkgroupsOpen = false;
            app.diagnosticsOpen = false;
            testContext.setRadioReference("available", false);
            testContext.setMetric("radioInput", false);
            while (scanLists.count)
                scanLists.remove(0);
            app.sessionDestination = "";
            app.currentTab = 0;
            app.width = 420;
            app.height = 900;
            prefs.onboardingDone = oldOnboarding;
            prefs.autoStartOnAttach = oldAutoStart;
            prefs.backgroundListening = oldBackgroundListening;
            prefs.lastStartedKind = "";
            prefs.lastStartedUid = "";
            testContext.setScanTestUsb(false, false);
            testContext.useLifecycleHost(false);
            while (savedSystems.count)
                savedSystems.remove(0);
            callHistory.clearAll();
            confirmed.target = null;
            cancelled.target = null;
        }
        function openManage() {
            var opener = visualChild(item("homeScreen"), function (entry) {
                return entry.icon === "more" && entry.accessibleName === "More options for County radio" && entry.visible;
            });
            verify(opener !== null);
            opener.forceActiveFocus();
            tap(opener);
            var sheet = visualChild(item("homeScreen"), function (entry) {
                return entry.systemName === "County radio";
            });
            verify(sheet !== null);
            tryCompare(sheet, "visible", true);
            return {sheet: sheet, opener: opener};
        }
        function openRemove() {
            var management = openManage();
            var count = savedSystems.count;
            var remove = visualChild(management.sheet, function (entry) {
                return entry.text === "Remove…" && typeof entry.clicked === "function";
            });
            verify(remove !== null);
            tap(remove);
            compare(savedSystems.count, count, "Remove asks before changing saved systems");
            var sheet = item("removeSystemConfirm");
            tryCompare(sheet, "visible", true);
            verify(!management.sheet.visible);
            confirmed.target = sheet;
            cancelled.target = sheet;
            confirmed.clear();
            cancelled.clear();
            return {sheet: sheet, opener: management.opener};
        }
        function openHistory(session) {
            app.currentTab = 1;
            if (session) {
                app.startSystem(0);
                testContext.setLifecyclePhase(2);
                app.sessionDestination = "history";
            }
            var screen = item(session ? "sessionHistoryScreen" : "idleHistoryScreen");
            var opener = item("clearHistoryButton", screen);
            opener.forceActiveFocus();
            tap(opener);
            var sheet = item("clearHistoryConfirm", screen);
            tryCompare(sheet, "visible", true);
            confirmed.target = sheet;
            cancelled.target = sheet;
            confirmed.clear();
            cancelled.clear();
            return {sheet: sheet, opener: opener};
        }
        function dismiss(opened, method) {
            var tab = app.currentTab;
            var destination = app.sessionDestination;
            var stops = decoderHost.stopCalls();
            var backgrounds = decoderHost.backgroundCalls();
            if (method === "navigation")
                verify(Ui.Navigation.back(app), "Back consumes the modal");
            else if (method === "host")
                decoderHost.backRequested();
            else if (method === "escape")
                keyClick(Qt.Key_Escape);
            else if (method === "scrim")
                mouseClick(opened.sheet, 2, 2);
            else
                tap(item("confirmCancelButton", opened.sheet));
            tryCompare(opened.sheet, "visible", false);
            compare(app.currentTab, tab);
            compare(app.sessionDestination, destination);
            compare(decoderHost.stopCalls(), stops);
            compare(decoderHost.backgroundCalls(), backgrounds);
            tryCompare(opened.opener, "activeFocus", true);
        }
        function test_home_back_data() {
            return [{tag: "Navigation.back", method: "navigation"},
                {tag: "Android host Back", method: "host"},
                {tag: "desktop Escape", method: "escape"},
                {tag: "scrim", method: "scrim"}];
        }
        function test_home_back(data) {
            dismiss(openManage(), data.method);
            compare(savedSystems.count, 1);
        }
        function test_home_edit_data() {
            return [{tag: "unchanged", change: "none"},
                {tag: "row shifted", change: "shift"},
                {tag: "system removed", change: "remove"}];
        }
        function test_home_edit(data) {
            if (data.change === "shift") {
                savedSystems.update(0, {name: "Earlier system"});
                savedSystems.add({name: "County radio", sourceType: "usb", freqMhz: "851.5"});
            } else {
                savedSystems.add({name: "Keep this system", sourceType: "usb", freqMhz: "852.5"});
            }
            var uid = savedSystems.get(data.change === "shift" ? 1 : 0).uid;
            var opened = openManage();
            if (data.change !== "none")
                savedSystems.remove(0);
            tap(item("editSavedSystemButton", opened.sheet));
            tryCompare(opened.sheet, "visible", false);
            compare(Ui.Navigation.modals.length, 0);
            compare(app.wizardOpen, data.change !== "remove");
            if (app.wizardOpen) {
                var wizard = item("wizardScreen");
                compare(wizard.editRow, savedSystems.rowForUid(uid));
                compare(wizard.nameText, "County radio");
            }
        }
        function test_remove_cancel_data() {
            return [{tag: "Cancel", method: "cancel"},
                {tag: "Back", method: "navigation"},
                {tag: "Escape", method: "escape"},
                {tag: "scrim", method: "scrim"}];
        }
        function test_remove_cancel(data) {
            var opened = openRemove();
            compare(opened.sheet.title, "Remove County radio?");
            compare(opened.sheet.message, "This deletes the saved system and its settings. Imported files stay in the library.");
            compare(item("confirmActionButton", opened.sheet).text, "Remove system");
            dismiss(opened, data.method);
            compare(savedSystems.count, 1);
            compare(confirmed.count, 0);
            compare(cancelled.count, 1);
        }
        function test_remove_confirm_once() {
            savedSystems.add({name: "Keep this system", sourceType: "usb", freqMhz: "852.5"});
            var removedUid = savedSystems.get(0).uid;
            var keptUid = savedSystems.get(1).uid;
            var opened = openRemove();
            var button = item("confirmActionButton", opened.sheet);
            compare(button.border.color, Ui.Theme.magenta);
            tap(button);
            compare(savedSystems.count, 1);
            compare(savedSystems.rowForUid(removedUid), -1);
            compare(savedSystems.get(0).uid, keptUid);
            compare(confirmed.count, 1);
            // Even a duplicate activation delivered after dismissal is inert.
            button.clicked();
            compare(confirmed.count, 1);
            compare(savedSystems.count, 1);
        }
        function test_grouped_removal_names_site_and_keeps_sibling() {
            savedSystems.update(0, {rrSid: 12, rrSiteId: 100, siteName: "North"});
            savedSystems.add({name: "County radio", sourceType: "usb", freqMhz: "852.5",
                rrSid: 12, rrSiteId: 101, siteName: "South"});
            var removedUid = savedSystems.get(0).uid;
            var keptUid = savedSystems.get(1).uid;
            compare(savedSystems.siblingRows(0).length, 2);
            var opened = openRemove();
            compare(opened.sheet.title, "Remove site North of County radio?");
            verify(opened.sheet.message.indexOf("Other sites remain.") >= 0);
            verify(opened.sheet.message.indexOf("Imported files stay in the library.") >= 0);
            compare(item("confirmActionButton", opened.sheet).text, "Remove site");
            tap(item("confirmActionButton", opened.sheet));
            compare(savedSystems.count, 1);
            compare(savedSystems.rowForUid(removedUid), -1);
            compare(savedSystems.get(0).uid, keptUid);
        }
        function test_remove_resolves_original_uid() {
            savedSystems.add({name: "Keep this system", sourceType: "usb", freqMhz: "852.5"});
            var keptUid = savedSystems.get(1).uid;
            var opened = openRemove();
            savedSystems.remove(0);
            tap(item("confirmActionButton", opened.sheet));
            compare(savedSystems.count, 1);
            compare(savedSystems.get(0).uid, keptUid);
        }
        function test_history_back_data() {
            var rows = [];
            for (var session of [false, true]) {
                for (var method of ["navigation", "host", "escape", "cancel", "scrim"])
                    rows.push({tag: (session ? "session-" : "idle-") + method, session: session, method: method});
            }
            return rows;
        }
        function test_history_back(data) {
            var opened = openHistory(data.session);
            compare(opened.sheet.title, "Clear call history?");
            dismiss(opened, data.method);
            compare(callHistory.count, 1);
            compare(historyCleared.count, 0);
            compare(confirmed.count, 0);
            compare(cancelled.count, 1);
        }
        function test_history_confirm_restores_focus_data() {
            return [{tag: "idle", session: false}, {tag: "session", session: true}];
        }
        function test_history_confirm_restores_focus(data) {
            var opened = openHistory(data.session);
            var screen = item(data.session ? "sessionHistoryScreen" : "idleHistoryScreen");
            tap(item("confirmActionButton", opened.sheet));
            compare(callHistory.count, 0);
            verify(!opened.opener.visible);
            tryCompare(opened.sheet, "visible", false);
            tryVerify(function () {
                return app.activeFocusItem !== null && Ui.Navigation.contains(screen, app.activeFocusItem)
                    && Ui.Navigation.presented(app.activeFocusItem) && app.activeFocusItem.enabled;
            }, 1000, "clearing history keeps focus inside the History screen");
        }
        function test_history_confirm_once_data() {
            return [{tag: "idle", session: false}, {tag: "session", session: true}];
        }
        function test_history_confirm_once(data) {
            var opened = openHistory(data.session);
            var button = item("confirmActionButton", opened.sheet);
            compare(button.text, "Clear history");
            compare(button.border.color, Ui.Theme.magenta);
            tap(button);
            compare(callHistory.count, 0);
            compare(historyCleared.count, 1);
            compare(confirmed.count, 1);
            callHistory.push("TODAY");
            button.clicked();
            compare(callHistory.count, 1, "a late activation must not clear a newly logged call");
            compare(historyCleared.count, 1);
            compare(confirmed.count, 1);
        }
        function openScanMenu() {
            scanLists.add({name: "First list", sourceType: "usb"});
            scanLists.add({name: "Keep list", sourceType: "usb"});
            var menu = item("scanListManageSheet");
            var card = visualChild(item("homeScreen"), function (entry) {
                return entry.objectName === "scanListCard" && entry.listName === "First list";
            });
            verify(card !== null);
            var scroll = item("dongleScrollBody");
            verify(waitForPolish(app.contentItem));
            scroll.contentY = Math.min(card.mapToItem(scroll.contentItem, 0, 0).y, scroll.contentHeight - scroll.height);
            tap(item("scanListManageButton", card));
            tryCompare(menu, "visible", true);
            verify(!app.scanListOpen, "the more button opens the menu before the editor");
            return menu;
        }
        function test_home_menu_cancel_over_recent_activity_data() {
            return [{tag: "Add system", add: true}, {tag: "Saved system more", add: false}];
        }
        function test_home_menu_cancel_over_recent_activity(data) {
            // Leave a 1000 x 900 Home beside the desktop navigation rail.
            app.width = 1104;
            app.height = 900;
            for (var i = callHistory.count; i < 20; ++i)
                callHistory.push("TODAY");
            var home = item("homeScreen");
            verify(waitForPolish(app.contentItem));
            tryCompare(home, "width", 1000);
            tryCompare(home, "height", 900);
            verify(home.supportingPane);
            var recent = visualChild(home, function (entry) {
                return entry.model === callHistory && typeof entry.itemAt === "function";
            });
            verify(recent !== null);
            tryCompare(recent, "count", 20);
            var detail = item("homeRecentDetailSheet", home);
            verify(!detail.visible);
            compare(Ui.Navigation.modals.length, 0);

            var menu;
            if (data.add) {
                testContext.setRadioReference("available", true);
                tap(item("addSystemButton", home));
                menu = item("addSystemMenu", home);
            } else {
                menu = openManage().sheet;
            }
            tryCompare(menu, "visible", true);
            compare(Ui.Navigation.modals.length, 1);
            var cancel = item("actionMenuCancel", menu);
            verify(waitForPolish(app.contentItem));
            waitForRendering(cancel);
            // The right edge overlaps the supporting pane, beyond the cards.
            var x = cancel.width - 8;
            var y = cancel.height / 2;
            var point = cancel.mapToItem(recent.contentItem, x, y);
            var row = recent.itemAt(point.x, point.y);
            verify(row !== null && row.interactive, "Cancel overlaps an interactive history row");
            var rowPoint = cancel.mapToItem(row, x, y);
            mouseClick(cancel, x, y);
            tryCompare(menu, "visible", false);
            verify(!detail.visible, "Cancel must not open Activity details underneath");
            compare(Ui.Navigation.modals.length, 0);

            // The same history row remains usable once the menu is dismissed.
            mouseClick(row, rowPoint.x, rowPoint.y);
            tryCompare(detail, "visible", true);
            verify(Ui.Navigation.back(app));
            compare(Ui.Navigation.modals.length, 0);
        }
        function test_scan_remove_confirms_exact_uid_data() {
            return [{tag: "confirm", change: "none"}, {tag: "row shifted", change: "shift"},
                {tag: "already removed", change: "removed"}, {tag: "cancel", change: "cancel"}];
        }
        function test_scan_remove_confirms_exact_uid(data) {
            if (data.change === "shift")
                scanLists.add({name: "Earlier list", sourceType: "usb"});
            var menu = openScanMenu();
            var removedUid = scanLists.get(data.change === "shift" ? 1 : 0).uid;
            var keptUid = scanLists.get(data.change === "shift" ? 2 : 1).uid;
            var confirmation = item("removeScanListConfirm");
            tap(item("removeScanListButton", menu));
            verify(!menu.visible);
            tryCompare(confirmation, "visible", true);
            compare(scanLists.count, data.change === "shift" ? 3 : 2);
            compare(confirmation.title, "Remove First list?");
            compare(confirmation.confirmText, "Remove list");
            verify(item("homeScreen").managementSheetOpen);
            if (data.change === "cancel") {
                verify(Ui.Navigation.back(app));
                compare(scanLists.count, 2);
                return;
            }
            if (data.change === "shift" || data.change === "removed")
                scanLists.remove(0);
            var button = item("confirmActionButton", confirmation);
            tap(button);
            verify(!confirmation.visible);
            compare(scanLists.rowForUid(removedUid), -1);
            compare(scanLists.get(0).uid, keptUid);
            var count = scanLists.count;
            button.clicked();
            compare(scanLists.count, count);
        }
        function test_scan_edit_routes_to_editor() {
            var menu = openScanMenu();
            var uid = scanLists.get(0).uid;
            tap(item("editScanListButton", menu));
            verify(!menu.visible);
            verify(app.scanListOpen);
            compare(item("scanListScreen").editUid, uid);
        }
        function test_scan_removal_notice_data() {
            return [{tag: "OK"}, {tag: "Back"}, {tag: "Escape"}, {tag: "scrim"}];
        }
        function test_scan_removal_notice(data) {
            scanLists.add({name: "Keep list", sourceType: "usb"});
            var uid = scanLists.get(0).uid;
            var notice = item("scanListRemovalError");
            notice.open();
            compare(notice.title, "Scan list removal failed");
            compare(notice.message, "Could not remove the scan list.");
            verify(item("homeScreen").managementSheetOpen);
            verify(uiController.autoStartBlocked);
            var button = item("confirmActionButton", notice);
            compare(button.text, "OK");
            verify(!item("confirmCancelButton", notice).visible);
            confirmed.target = notice;
            confirmed.clear();
            if (data.tag === "OK")
                tap(button);
            else if (data.tag === "Back")
                verify(Ui.Navigation.back(app));
            else if (data.tag === "Escape")
                keyClick(Qt.Key_Escape);
            else
                mouseClick(notice, 2, 2);
            tryCompare(notice, "visible", false);
            compare(confirmed.count, data.tag === "OK" ? 1 : 0);
            button.clicked();
            compare(confirmed.count, data.tag === "OK" ? 1 : 0);
            compare(scanLists.count, 1);
            compare(scanLists.get(0).uid, uid);
            verify(!item("homeScreen").managementSheetOpen);
        }
        function test_grouped_menu_sites() {
            var menu = openManage().sheet;
            compare(menu.actions.length, 2);
            menu.cancel();
            savedSystems.update(0, {rrSid: 12, rrSiteId: 100});
            savedSystems.add({name: "County radio", sourceType: "usb", freqMhz: "852.5",
                rrSid: 12, rrSiteId: 101});
            menu = openManage().sheet;
            compare(menu.actions.length, 3);
            tap(item("sitesSavedSystemButton", menu));
            verify(!menu.visible);
            tryCompare(item("siteChooserSheet"), "visible", true);
        }
        function test_session_menu_routes_data() {
            var rows = [];
            for (var height of [900, 440]) {
                for (var text of ["Sites", "Spectrum", "Talkgroups", "History", "Settings", "Diagnostics", "Edit saved system", "Cancel"])
                    rows.push({tag: text + "-" + height, text: text, height: height});
            }
            return rows;
        }
        function test_session_menu_routes(data) {
            savedSystems.update(0, {rrSid: 12, rrSiteId: 100});
            savedSystems.add({name: "County radio", sourceType: "usb", freqMhz: "852.5", rrSid: 12, rrSiteId: 101});
            app.startSystem(0);
            testContext.setLifecyclePhase(2);
            testContext.setMetric("radioInput", true);
            app.height = data.height;
            var monitor = item("monitorScreen");
            if (data.height < 500) {
                tryCompare(monitor, "compactHeight", true);
                tryCompare(item("runningSiteChooserButton"), "visible", false);
                tryCompare(item("openSpectrumButton"), "visible", false);
            }
            var menu = item("sessionMenu");
            monitor.openSessionMenu();
            tryCompare(menu, "visible", true);
            compare(menu.actions.length, 7);
            var button = visualChild(menu, function (entry) {
                return entry.text === data.text && typeof entry.clicked === "function";
            });
            verify(button !== null);
            verify(waitForPolish(app.contentItem));
            var scroll = item("modalSheetScroll", menu);
            if (button.mapToItem(scroll, 0, button.height).y > scroll.height) {
                // Start on a destination row: its tap handler must let the sheet flick.
                var dragRow = item(menu.actions[3].objectName, menu);
                var previousY = scroll.contentY;
                mouseDrag(dragRow, dragRow.width / 2, dragRow.height / 2, 0, -190, Qt.LeftButton);
                tryVerify(function () { return scroll.contentY > previousY; });
                tryCompare(scroll, "moving", false);
                verify(menu.visible, "dragging must not activate a destination");
            }
            verify(button.mapToItem(scroll, 0, 0).y >= 0);
            verify(button.mapToItem(scroll, 0, button.height).y <= scroll.height);
            tap(button);
            verify(!menu.visible);
            compare(decoderHost.sessionState, 2);
            if (data.text === "Sites")
                verify(item("siteChooserSheet").visible);
            if (data.text === "Spectrum")
                verify(app.spectrumOpen);
            if (data.text === "Talkgroups")
                verify(app.talkgroupsOpen);
            if (data.text === "History")
                compare(app.sessionDestination, "history");
            if (data.text === "Settings")
                compare(app.sessionDestination, "settings");
            if (data.text === "Diagnostics")
                verify(app.diagnosticsOpen);
            if (data.text === "Edit saved system")
                verify(app.wizardOpen);
        }
        function test_session_menu_conditions_and_back() {
            var menu = item("sessionMenu");
            app.sessionRow = -1;
            testContext.setMetric("radioInput", false);
            menu.open();
            compare(menu.actions.length, 4);
            verify(Ui.Navigation.back(app));
            verify(!menu.visible);
        }
        function test_auto_start_consumed_data() {
            return [{tag: "Home", kind: "home"}, {tag: "Remove", kind: "remove"},
                {tag: "Scan menu", kind: "scan"}, {tag: "Scan remove", kind: "scanRemove"},
                {tag: "Add system", kind: "add"}, {tag: "History", kind: "history"},
                {tag: "Licenses", kind: "licenses"}, {tag: "Account", kind: "account"}];
        }
        function test_auto_start_consumed(data) {
            prefs.autoStartOnAttach = true;
            prefs.lastStartedKind = "saved";
            prefs.lastStartedUid = savedSystems.get(0).uid;
            testContext.setScanTestUsb(true, true);
            tryCompare(uiController, "autoStartBlocked", false);
            var sheet;
            if (data.kind === "home")
                sheet = openManage().sheet;
            else if (data.kind === "remove")
                sheet = openRemove().sheet;
            else if (data.kind === "scan" || data.kind === "scanRemove") {
                sheet = openScanMenu();
                if (data.kind === "scanRemove") {
                    tap(item("removeScanListButton", sheet));
                    sheet = item("removeScanListConfirm");
                }
            } else if (data.kind === "add") {
                testContext.setRadioReference("available", true);
                tap(item("addSystemButton"));
                sheet = item("addSystemMenu");
            } else if (data.kind === "history")
                sheet = openHistory(false).sheet;
            else {
                app.currentTab = 2;
                sheet = item(data.kind === "licenses" ? "licensesScreen" : "radioReferenceAccountScreen");
                if (data.kind === "licenses")
                    app.licensesOpen = true;
                else
                    app.radioReferenceAccountOpen = true;
            }
            tryCompare(uiController, "autoStartBlocked", true, 1000);
            testContext.emitLocalDeviceAttached();
            compare(decoderHost.sessionState, 0);
            verify(Ui.Navigation.back(app));
            tryCompare(sheet, "visible", false);
            tryCompare(uiController, "autoStartBlocked", false);
            wait(300);
            compare(decoderHost.sessionState, 0, "blocked attachment is consumed, never retried");
            compare(savedSystems.get(0).lastHeard, 0);
            testContext.emitLocalDeviceAttached();
            compare(decoderHost.sessionState, 1, "a fresh attachment can start after dismissal");
        }
    }
}
