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
        // Main's Connections require a QObject host during construction.
        testContext.useLifecycleHost(true, true, true);
        loader.active = true;
    }
    TestCase {
        name: "ModalInput"
        when: windowShown && loader.status === Loader.Ready
        property var app: loader.item

        function item(name) {
            var value = findChild(app, name);
            verify(value !== null, name + " exists");
            return value;
        }
        function init() {
            testContext.useLifecycleHost(true, true, true);
            while (savedSystems.count)
                savedSystems.remove(0);
            for (var i = 0; i < 2; ++i)
                savedSystems.add({
                    name: "Site " + i,
                    sourceType: "rtltcp",
                    host: "127.0.0.1",
                    port: 1234,
                    freqMhz: "851.5",
                    rrSid: 12,
                    rrSiteId: 100 + i
                });
            app.startSystem(0);
            app.scanWarnings = "";
            app.talkgroupsOpen = false;
            testContext.setMetric("siteLine", "P25 · NAC 293");
            testContext.setMetric("siteProtocol", "P25");
            testContext.setMetric("uiMessage", "");
            testContext.resetCommands();
            tryCompare(decoderHost, "sessionState", 2);
            tryCompare(item("monitorScreen"), "opacity", 1);
            tryCompare(item("monitorScreen"), "enabled", true);
            waitForRendering(item("monitorScreen"));
        }
        function cleanup() {
            for (var name of ["siteSheet", "networkSheet", "siteChooserSheet", "talkgroupEditSheet"])
                item(name).visible = false;
            app.scanWarnings = "";
            app.talkgroupsOpen = false;
            app.spectrumOpen = false;
            testContext.setMetric("uiMessage", "");
            testContext.useLifecycleHost(false);
            while (savedSystems.count)
                savedSystems.remove(0);
        }
        function test_monitor_scrim_data() {
            return [
                {
                    tag: "site-stop",
                    sheet: "siteSheet",
                    target: "stopListeningButton"
                },
                {
                    tag: "network-stop",
                    sheet: "networkSheet",
                    target: "stopListeningButton"
                },
                {
                    tag: "site-spectrum",
                    sheet: "siteSheet",
                    target: "openSpectrumButton"
                },
                {
                    tag: "network-spectrum",
                    sheet: "networkSheet",
                    target: "openSpectrumButton"
                }
            ];
        }
        function test_monitor_scrim(data) {
            var sheet = item(data.sheet);
            var target = item(data.target);
            sheet.visible = true;
            waitForRendering(sheet);
            var point = target.mapToItem(sheet, target.width / 2, target.height / 2);
            verify(!sheet.hitsPanel(point.x, point.y), "tap is on the scrim over the live control");
            mouseClick(target, target.width / 2, target.height / 2);
            tryCompare(sheet, "visible", false);
            compare(decoderHost.sessionState, 2, "dismissing a sheet must keep listening");
            verify(!app.spectrumOpen, "dismissing a sheet must not navigate");
        }
        function test_notification_explanation_covers_monitor() {
            var explanation = item("notificationExplanation");
            explanation.visible = true;
            waitForRendering(explanation);
            var stop = item("stopListeningButton");
            var point = stop.mapToItem(explanation, stop.width / 2, stop.height / 2);
            verify(!explanation.hitsPanel(point.x, point.y));
            mouseClick(stop, stop.width / 2, stop.height / 2);
            tryCompare(explanation, "visible", false);
            compare(decoderHost.sessionState, 2);
        }
        function test_keyboard_keeps_wizard_action_visible() {
            testContext.useLifecycleHost(true, true, false);
            var wizard = item("wizardScreen");
            wizard.openForAdd(true);
            app.wizardOpen = true;
            testContext.setKeyboardBoundary(500);
            var next = item("wizardContinue");
            tryVerify(function () {
                return next.mapToItem(null, 0, next.height).y <= 500;
            });
            verify(next.height >= 48);
            testContext.setKeyboardBoundary(-1);
            app.wizardOpen = false;
        }
        function test_navigation_reflows_after_resize() {
            testContext.useLifecycleHost(true, true, false);
            app.width = 1200;
            app.height = 700;
            var nav = item("primaryNavigation");
            var home = item("homeScreen");
            tryCompare(nav, "vertical", true);
            verify(nav.width < 200 && home.width > 900);
            app.width = 420;
            app.height = 900;
            tryCompare(nav, "vertical", false);
            verify(nav.height < 150 && home.height > 700);
            verify(nav.y > 700);
        }
        function test_network_back_returns_to_site() {
            testContext.setMetric("siteProtocol", "P25");
            var site = item("siteSheet");
            var network = item("networkSheet");
            site.visible = true;
            mouseClick(item("siteNetworkButton"));
            verify(site.visible && network.visible);
            verify(Ui.Navigation.back(app));
            verify(site.visible && !network.visible);
            verify(Ui.Navigation.back(app));
            verify(!site.visible && decoderHost.sessionState === 2);
            testContext.setMetric("siteProtocol", "");
        }
        function test_warning_over_stop() {
            app.scanWarnings = "Source aliases were replaced";
            var stop = item("stopListeningButton");
            waitForRendering(stop);
            mouseClick(stop, stop.width / 2, stop.height / 2);
            tryCompare(app, "scanWarnings", "");
            compare(decoderHost.sessionState, 2, "dismissing a scan warning must keep listening");
            mouseClick(stop, stop.width / 2, stop.height / 2);
            compare(decoderHost.sessionState, 0, "Stop remains usable after dismissal");
        }
        function test_sites_button_under_monitor_sheets_data() {
            return [
                {
                    tag: "site",
                    sheet: "siteSheet"
                },
                {
                    tag: "network",
                    sheet: "networkSheet"
                }
            ];
        }
        function test_sites_button_under_monitor_sheets(data) {
            var sites = item("runningSiteChooserButton");
            tryCompare(sites, "visible", true);
            var sheet = item(data.sheet);
            sheet.visible = true;
            waitForRendering(sheet);
            mouseClick(sites, sites.width / 2, sites.height / 2);
            verify(!item("siteChooserSheet").visible, "Monitor sheets cover the separate Sites control");
            tryCompare(sheet, "visible", false);
        }
        function openEdit() {
            app.talkgroupsOpen = true;
            tryCompare(item("talkgroupsScreen"), "opacity", 1);
            verify(testContext.clearTalkgroups());
            verify(testContext.pushTalkgroup(1001, "A", "Dispatch", "FIRE"));
            var grid = item("talkgroupGrid");
            tryCompare(grid, "count", 1);
            tryVerify(function () {
                return grid.itemAtIndex(0) !== null;
            });
            var sheet = item("talkgroupEditSheet");
            sheet.openRow(1001, 1001, "Dispatch", true, true, 25, false, talkgroups.policyContext, talkgroups.policyGeneration);
            return sheet;
        }
        function test_edit_scrim_data() {
            return [
                {
                    tag: "card",
                    target: "card"
                },
                {
                    tag: "listen-all",
                    target: "listenAllButton"
                },
                {
                    tag: "block-all",
                    target: "noTuneAllButton"
                }
            ];
        }
        function test_edit_scrim(data) {
            var sheet = openEdit();
            // Use the production keyboard constraint to leave the card/bulk row on the scrim.
            sheet.keyboardTop = 140;
            var target = data.target === "card" ? findChild(item("talkgroupGrid").itemAtIndex(0), "talkgroupListeningSwitch") : item(data.target);
            waitForRendering(sheet);
            var point = target.mapToItem(sheet, target.width / 2, target.height / 2);
            verify(!sheet.hitsPanel(point.x, point.y));
            mouseClick(target, target.width / 2, target.height / 2);
            tryCompare(sheet, "visible", false);
            compare(testContext.talkgroupListenCalls(), 0);
            compare(testContext.allTalkgroupsListenCalls(), 0);
            mouseClick(target, target.width / 2, target.height / 2);
            if (data.target !== "card") {
                var confirmation = item("talkgroupBulkConfirm");
                tryCompare(confirmation, "visible", true);
                var apply = item("confirmTalkgroupBulk");
                mouseClick(apply, apply.width / 2, apply.height / 2);
            }
            tryVerify(function () {
                return testContext.talkgroupListenCalls() + testContext.allTalkgroupsListenCalls() === 1;
            });
        }
        function test_rejection_after_accepted_edit_stays_visible() {
            var sheet = openEdit();
            sheet.keyboardTop = sheet.height;
            item("talkgroupName").text = "Updated";
            sheet.saveRow();
            verify(!sheet.visible, "queue accepted the edit");
            var reason = "Talkgroup list changed: stale edit rejected";
            testContext.setMetric("uiMessage", reason);
            var toast = item("talkgroupCommandToast");
            tryCompare(toast, "visible", true);
            compare(toast.text, reason);
        }
    }
}
