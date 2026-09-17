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
            verify(waitForPolish(button));
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
            app.sessionDestination = "";
            app.currentTab = 0;
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
                return entry.icon === "more" && entry.accessibleName === "Edit County radio" && entry.visible;
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
                return entry.text === "Remove this system" && typeof entry.clicked === "function";
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
        function test_auto_start_consumed_data() {
            return [{tag: "Home", kind: "home"}, {tag: "Remove", kind: "remove"},
                {tag: "History", kind: "history"}, {tag: "Licenses", kind: "licenses"},
                {tag: "Account", kind: "account"}];
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
            else if (data.kind === "history")
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
