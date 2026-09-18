// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

Item {
    width: 420
    height: 900

    Loader {
        id: appLoader

        source: uiDir + "/Main.qml"
    }

    TestCase {
        function init() {
            testContext.useLifecycleHost(true);
            while (scanLists.count)
                scanLists.remove(0);
            prefs.lastStartedKind = "";
            prefs.lastStartedUid = "";
            scanLists.add({
                "name": "Scan",
                "sourceType": "rtltcp",
                "host": "127.0.0.1",
                "port": 1234,
                "entries": [
                    {
                        "kind": "freq",
                        "name": "Simplex",
                        "protocol": "p25",
                        "freqMhz": "851.5",
                        "enabled": true
                    }
                ]
            });
        }

        function cleanup() {
            testContext.setScanTestUsb(false, false);
            testContext.useLifecycleHost(false);
            while (scanLists.count)
                scanLists.remove(0);
        }

        function visualChild(item, name) {
            if (typeof name === "function" ? name(item) : item.objectName === name)
                return item;

            var kids = item.contentItem ? [item.contentItem] : item.children || [];
            for (var i = 0; i < kids.length; ++i) {
                var found = visualChild(kids[i], name);
                if (found)
                    return found;
            }
            return null;
        }

        function test_readiness_updates_without_count_change() {
            var card = visualChild(appLoader.item, "scanListCard");
            verify(card !== null);
            var oldOnboarding = prefs.onboardingDone;
            prefs.onboardingDone = true;
            var count = scanLists.count;
            var manualEntries = scanLists.get(0).entries;
            scanLists.update(0, {isDraft: true});
            tryCompare(card, "isDraft", true);
            verify(!findChild(card, "scanListPlay").enabled);
            var source = testContext.writeFixtureCsv("ready-targets.csv",
                "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes\none,p25-conventional,851500000,,,,\ntwo,p25-conventional,852500000,,,,\n");
            var result = importedFiles.importFile(source, "Ready targets.csv", "trunkTargets");
            verify(result.ok);
            try {
                scanLists.update(0, {isDraft: false, targetSource: "csv", targetsCsvPath: result.path});
                compare(scanLists.count, count);
                tryCompare(card, "isDraft", false);
                tryCompare(card, "entryCount", 2);
                tryCompare(findChild(card, "scanListPlay"), "enabled", true);
                scanLists.update(0, {targetsCsvPath: ""});
                tryCompare(card, "entryCount", 0);
                scanLists.update(0, {targetSource: "entries", entries: manualEntries});
                tryCompare(card, "entryCount", 1);
                scanLists.update(0, {entries: []});
                tryCompare(card, "entryCount", 0);
                compare(scanLists.count, count);
            } finally {
                prefs.onboardingDone = oldOnboarding;
                importedFiles.remove(importedFiles.rowForPath(result.path));
            }
        }

        function test_csv_draft_without_target_path() {
            failOnWarning(/HomeScreen.qml.*Unable to assign/);
            scanLists.add({name: "CSV draft", sourceType: "usb", targetSource: "csv", isDraft: true});
            var card = visualChild(appLoader.item, function (entry) {
                return entry.objectName === "scanListCard" && entry.listName === "CSV draft";
            });
            verify(card !== null);
            compare(card.entryCount, 0);
            verify(card.isDraft);
            verify(!findChild(card, "scanListPlay").enabled);
        }

        function test_add_system_routes_data() {
            return [{tag: "unavailable", available: false, choice: ""},
                {tag: "manual", available: true, choice: "manual"},
                {tag: "RadioReference Back", available: true, choice: "import", back: true},
                {tag: "RadioReference", available: true, choice: "import"}];
        }
        function test_add_system_routes(data) {
            var app = appLoader.item;
            var oldOnboarding = prefs.onboardingDone;
            prefs.onboardingDone = true;
            testContext.setRadioReference("available", data.available);
            var home = findChild(app, "homeScreen");
            var menu = findChild(home, "addSystemMenu");
            try {
                verify(menu !== null);
                findChild(home, "addSystemButton").clicked();
                compare(menu.visible, data.available);
                compare(app.wizardOpen, !data.available);
                if (data.available) {
                    compare(menu.actions.length, 2);
                    compare(menu.actions[0].text, "Set up manually");
                    compare(menu.actions[1].text, "Import from RadioReference");
                    menu.activate(data.choice === "manual" ? 0 : 1);
                }
                compare(app.wizardOpen, data.choice !== "import");
                compare(app.radioReferenceOpen, data.choice === "import");
                compare(app.awaitingUsbAccess, false);
                var wizard = findChild(app, "wizardScreen");
                compare(wizard.sourceType, "usb");
                if (data.choice === "import") {
                    if (data.back) {
                        verify(Ui.Navigation.back(app));
                        verify(!app.radioReferenceOpen);
                        verify(!app.wizardOpen);
                        compare(app.currentTab, 0);
                        tryCompare(home, "enabled", true);
                        compare(Ui.Navigation.modals.length, 0);
                        return;
                    }
                    findChild(app, "radioReferenceScreen").imported({
                        name: "County import", freqMhz: "852.5", decodeFlag: "-f1", trunking: true,
                        chanCsvPath: "/tmp/channels.csv", groupCsvPath: "/tmp/groups.csv"
                    });
                    verify(!app.radioReferenceOpen);
                    verify(app.wizardOpen);
                    compare(wizard.nameText, "County import");
                    compare(wizard.freqText, "852.5");
                }
            } finally {
                if (menu) menu.visible = false;
                app.radioReferenceOpen = false;
                app.wizardOpen = false;
                prefs.onboardingDone = oldOnboarding;
                testContext.setRadioReference("available", false);
            }
        }

        function test_add_scan_list_opens_clean_editor() {
            var app = appLoader.item;
            findChild(app, "homeScreen").addScanList();
            var editor = findChild(app, "scanListScreen");
            verify(app.scanListOpen);
            compare(editor.editUid, "");
            compare(editor.fingerprint(), editor.initialDraft);
            compare(Ui.Navigation.modals.length, 0);
            editor.requestClose();
            verify(!app.scanListOpen);
        }

        function test_single_site_has_listen_and_management() {
            var row = savedSystems.count;
            var oldOnboarding = prefs.onboardingDone;
            try {
                prefs.onboardingDone = true;
                Ui.Theme.fontScale = 1.6;
                savedSystems.add({
                    name: "Grouped system",
                    sourceType: "usb",
                    freqMhz: "851.5",
                    rrSid: 12,
                    rrSiteId: 100
                });
                var card = visualChild(appLoader.item, function (item) {
                    return item.name === "Grouped system" && item.sourceType === "usb";
                });
                verify(card !== null);
                var sites = findChild(card, "homeSiteChooserButton");
                var play = findChild(card, "savedSystemPlay");
                var title = findChild(card, "savedSystemTitle");
                tryCompare(sites, "visible", true);
                compare(play.visible, true);
                verify(!play.featured);
                var more = findChild(card, "savedSystemManageButton");
                compare(more.accessibleName, "More options for Grouped system");
                verify(more.y + more.height <= play.y, "menu is above Listen");
                var scanCard = visualChild(appLoader.item, "scanListCard");
                var scanMore = findChild(scanCard, "scanListManageButton");
                compare(scanMore.accessibleName, "More options for Scan");
                compare(scanMore.y, more.y);
                compare(scanCard.width - scanMore.x, card.width - more.x);
                compare(more.x + more.width / 2, play.x + play.width / 2);
                var scanPlay = findChild(scanCard, "scanListPlay");
                compare(scanMore.x + scanMore.width / 2, scanPlay.x + scanPlay.width / 2);
                verify(scanMore.y + scanMore.height <= findChild(scanCard, "scanListPlay").y);
                verify(sites.x + sites.width <= play.x, "site management and Listen do not overlap");
                var start = title.mapToItem(card, 0, 0);
                verify(start.x + title.width <= sites.x, "title and site action do not overlap");
                verify(sites.y >= 0 && sites.y + sites.height <= card.height, "site action fits card");
            } finally {
                savedSystems.remove(row);
                prefs.onboardingDone = oldOnboarding;
                Ui.Theme.resetFontScale();
            }
        }

        function test_scan_target_uses_names_including_reattachment() {
            var app = appLoader.item;
            var previousSystem = app.sessionSystem;
            var row = savedSystems.count;
            try {
                var list = scanLists.get(0);
                var entry = list.entries[0];
                testContext.setMetric("scanTargetCount", 1);
                testContext.setMetric("scanTargetOrdinal", 1);
                testContext.setMetric("scanTargetId", entry.uid);
                app.sessionSystem = list;
                var monitor = visualChild(app, "monitorScreen");
                tryCompare(monitor, "scanTargetName", "Simplex");
                entry.name = "";
                app.sessionSystem = {
                    entries: [entry]
                };
                tryCompare(monitor, "scanTargetName", "851.5 MHz");
                savedSystems.add({
                    name: "Benton Simulcast",
                    sourceType: "usb",
                    freqMhz: "769.76875"
                });
                app.sessionSystem = {
                    entries: [
                        {
                            uid: entry.uid,
                            kind: "system",
                            systemUid: savedSystems.get(row).uid
                        }
                    ]
                };
                tryCompare(monitor, "scanTargetName", "Benton Simulcast");
                // A recreated Activity can recover the label from the last confirmed list.
                prefs.lastStartedKind = "scan";
                prefs.lastStartedUid = list.uid;
                app.sessionSystem = null;
                tryCompare(monitor, "scanTargetName", "Simplex");
                testContext.setMetric("scanTargetId", "unknown-uid");
                tryCompare(monitor, "scanTargetName", "Target 1");
                verify(visualChild(monitor, "scanTargetHeader").text.indexOf("unknown-uid") < 0);
            } finally {
                savedSystems.remove(row);
                app.sessionSystem = previousSystem;
                testContext.setMetric("scanTargetId", "");
                testContext.setMetric("scanTargetOrdinal", 0);
                testContext.setMetric("scanTargetCount", 0);
            }
        }

        function test_home_card_and_confirmed_start() {
            tryVerify(function () {
                return visualChild(appLoader.item, "scanListCard") !== null;
            });
            appLoader.item.startScanList(0);
            compare(prefs.lastStartedUid, "");
            compare(scanLists.get(0).lastHeard, 0);
            testContext.emitSessionInitialized();
            compare(prefs.lastStartedKind, "scan");
            compare(prefs.lastStartedUid, scanLists.get(0).uid);
            verify(scanLists.get(0).lastHeard > 0);
        }

        function test_csv_list_play_and_label() {
            var source = testContext.writeFixtureCsv("home-targets.csv",
                "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes\nCSV target,p25-conventional,851500000,,,,\n");
            var result = importedFiles.importFile(source, "Home targets.csv", "trunkTargets");
            verify(result.ok, result.detail || "Import failed");
            var app = appLoader.item;
            try {
                scanLists.update(0, {targetSource: "csv", targetsCsvPath: result.path});
                app.startScanList(0);
                compare(prefs.lastStartedUid, "");
                testContext.emitSessionInitialized();
                compare(prefs.lastStartedUid, scanLists.get(0).uid);
                testContext.setMetric("scanTargetId", "CSV target");
                compare(app.scanTargetLabel(), "CSV target");
                app.sessionSystem = null;
                compare(app.scanTargetLabel(), "CSV target");
                var card = visualChild(app, "scanListCard");
                verify(card !== null);
                compare(card.entryCount, 1);
            } finally {
                testContext.setMetric("scanTargetId", "");
                decoderHost.stop();
                importedFiles.remove(importedFiles.rowForPath(result.path));
            }
        }

        // WP-S2: signal routing reuses permission handling and initialization recency.
        function test_auto_start_scan_request() {
            scanLists.update(0, {
                "sourceType": "usb"
            });
            testContext.setScanTestUsb(true, false);
            uiController.requestAutoStart("scan", scanLists.get(0).uid);
            compare(appLoader.item.awaitingUsbAccess, true);
            compare(uiController.autoStartBlocked, true);
            compare(prefs.lastStartedUid, "");
            testContext.setScanTestUsb(true, true);
            testContext.emitSessionInitialized();
            compare(prefs.lastStartedKind, "scan");
            compare(prefs.lastStartedUid, scanLists.get(0).uid);
        }

        function test_auto_start_overlay_gate() {
            var overlays = ["wizardOpen", "scanListOpen", "exploreSetupOpen", "diagnosticsOpen", "importsOpen", "radioReferenceOpen", "spectrumOpen", "talkgroupsOpen"];
            for (var i = 0; i < overlays.length; ++i) {
                appLoader.item[overlays[i]] = true;
                compare(uiController.autoStartBlocked, true);
                appLoader.item[overlays[i]] = false;
            }
            compare(uiController.autoStartBlocked, false);
        }

        function test_auto_start_site_chooser() {
            var onboardingDone = prefs.onboardingDone;
            prefs.onboardingDone = true;
            var enabled = prefs.autoStartOnAttach;
            var row = savedSystems.count;
            savedSystems.add({
                name: "Site",
                sourceType: "usb",
                freqMhz: "851.5",
                rrSid: 12,
                rrSiteId: 100
            });
            var chooser = findChild(appLoader.item, "siteChooserSheet");
            try {
                prefs.autoStartOnAttach = true;
                prefs.lastStartedKind = "saved";
                prefs.lastStartedUid = savedSystems.get(row).uid;
                testContext.setScanTestUsb(true, true);
                chooser.openFor(row);
                testContext.emitLocalDeviceAttached();
                compare(decoderHost.sessionState, 0);
                chooser.visible = false;
                wait(300);
                compare(decoderHost.sessionState, 0);
                testContext.emitLocalDeviceAttached();
                compare(decoderHost.sessionState, 1);
            } finally {
                chooser.visible = false;
                prefs.autoStartOnAttach = enabled;
                prefs.onboardingDone = onboardingDone;
                savedSystems.remove(row);
            }
        }

        function test_auto_start_card_sheet_data() {
            return [
                {
                    tag: "saved management menu",
                    kind: "saved"
                },
                {
                    tag: "scan list management menu",
                    kind: "scan"
                }
            ];
        }

        function test_auto_start_card_sheet(data) {
            var onboardingDone = prefs.onboardingDone;
            var enabled = prefs.autoStartOnAttach;
            var targetModel = data.kind === "saved" ? savedSystems : scanLists;
            var row = data.kind === "saved" ? savedSystems.count : 0;
            if (data.kind === "saved")
                savedSystems.add({
                    "name": "Manage attached system",
                    "sourceType": "usb",
                    "freqMhz": "851.5"
                });
            else
                scanLists.update(row, {
                    "sourceType": "usb"
                });
            var uid = targetModel.get(row).uid;
            var sheet = null;
            try {
                prefs.onboardingDone = true;
                prefs.autoStartOnAttach = true;
                prefs.lastStartedKind = data.kind;
                prefs.lastStartedUid = uid;
                testContext.setScanTestUsb(true, true);
                appLoader.item.currentTab = 0;
                compare(decoderHost.sessionState, 0);
                tryCompare(uiController, "autoStartBlocked", false);
                var card = visualChild(appLoader.item, data.kind === "scan" ? "scanListCard" : function (item) {
                    return item.name === "Manage attached system" && item.sourceType === "usb";
                });
                verify(card !== null);
                wait(200); // Let the shell's onboarding transition finish before the gesture.
                mousePress(card, card.width / 4, card.height / 2);
                wait(1000);
                mouseRelease(card, card.width / 4, card.height / 2);
                sheet = visualChild(appLoader.item, function (item) {
                    return data.kind === "saved" ? item.systemName === "Manage attached system" : item.listUid === uid && item.objectName === "scanListManageSheet";
                });
                verify(sheet !== null);
                tryCompare(sheet, "visible", true);
                testContext.emitLocalDeviceAttached();
                compare(decoderHost.sessionState, 0, "attachment must be consumed while a card sheet is open");
                compare(uiController.autoStartBlocked, true);
                compare(targetModel.getByUid(uid).lastHeard, 0);
                var cancel = visualChild(sheet, function (item) {
                    return item.text === "Cancel" && typeof item.clicked === "function";
                });
                verify(cancel !== null);
                mouseClick(cancel, cancel.width / 2, cancel.height / 2);
                tryCompare(sheet, "visible", false);
                tryCompare(uiController, "autoStartBlocked", false);
                wait(300); // A dismissed sheet must not retry the consumed attachment.
                compare(decoderHost.sessionState, 0);
                compare(targetModel.getByUid(uid).lastHeard, 0);
                // Positive control: a fresh attachment after dismissal is still actionable.
                testContext.emitLocalDeviceAttached();
                compare(decoderHost.sessionState, 1);
            } finally {
                if (sheet)
                    sheet.visible = false;
                appLoader.item.scanListOpen = false;
                prefs.autoStartOnAttach = enabled;
                prefs.onboardingDone = onboardingDone;
                if (data.kind === "saved")
                    savedSystems.remove(savedSystems.rowForUid(uid));
            }
        }

        function test_auto_start_saved_request() {
            var row = savedSystems.count;
            savedSystems.add({
                "name": "Attached saved system",
                "sourceType": "usb",
                "freqMhz": "851.5"
            });
            var uid = savedSystems.get(row).uid;
            try {
                testContext.setScanTestUsb(true, true);
                uiController.requestAutoStart("saved", uid);
                compare(prefs.lastStartedUid, "");
                testContext.emitSessionInitialized();
                compare(prefs.lastStartedKind, "saved");
                compare(prefs.lastStartedUid, uid);
            } finally {
                savedSystems.remove(savedSystems.rowForUid(uid));
            }
        }

        function test_auto_start_failure_banner() {
            testContext.setScanTestAcceptStart(false);
            uiController.requestAutoStart("scan", scanLists.get(0).uid);
            verify(appLoader.item.startError.length > 0);
            verify(appLoader.item.showFailure);
            testContext.emitSessionInitialized();
            compare(prefs.lastStartedUid, "");
        }

        function test_auto_start_deleted_target() {
            uiController.requestAutoStart("scan", "deleted");
            compare(prefs.lastStartedUid, "");
            compare(scanLists.get(0).lastHeard, 0);
        }

        function test_usb_grant_resumes_scan_list() {
            scanLists.update(0, {
                "sourceType": "usb"
            });
            testContext.setScanTestUsb(true, false);
            appLoader.item.startScanList(0);
            compare(appLoader.item.awaitingUsbAccess, true);
            compare(prefs.lastStartedUid, "");
            testContext.setScanTestUsb(true, true);
            compare(appLoader.item.awaitingUsbAccess, false);
            compare(appLoader.item.sessionScanList, true);
            testContext.emitSessionInitialized();
            compare(prefs.lastStartedKind, "scan");
            compare(prefs.lastStartedUid, scanLists.get(0).uid);
        }

        function test_refused_host_start_does_not_touch() {
            testContext.setScanTestAcceptStart(false);
            appLoader.item.startScanList(0);
            testContext.emitSessionInitialized();
            compare(scanLists.get(0).lastHeard, 0);
            compare(prefs.lastStartedUid, "");
        }

        function test_recency_follows_uid_after_reorder() {
            scanLists.add(scanLists.get(0));
            var uid = scanLists.get(1).uid;
            appLoader.item.startScanList(1);
            scanLists.remove(0);
            testContext.emitSessionInitialized();
            compare(prefs.lastStartedUid, uid);
            verify(scanLists.getByUid(uid).lastHeard > 0);
        }

        function test_failed_start_does_not_touch() {
            appLoader.item.startScanList(0);
            decoderHost.stop();
            // INT-4: an unconfirmed stop has no initialization edge.
            compare(scanLists.get(0).lastHeard, 0);
            compare(prefs.lastStartedUid, "");
        }

        function test_late_initialization_after_idle_updates_recency() {
            appLoader.item.startScanList(0);
            decoderHost.stop();
            // Preserve INT-4: a delayed initialization confirmation is still authoritative.
            testContext.emitSessionInitialized();
            compare(prefs.lastStartedKind, "scan");
            compare(prefs.lastStartedUid, scanLists.get(0).uid);
            verify(scanLists.get(0).lastHeard > 0);
        }

        function test_running_fallback_follows_scan_uid() {
            scanLists.add(scanLists.get(0));
            var uid = scanLists.get(1).uid;
            appLoader.item.startScanList(1);
            compare(appLoader.item.sessionRow, -1);
            scanLists.remove(0);
            testContext.setLifecyclePhase(2);
            compare(prefs.lastStartedKind, "scan");
            compare(prefs.lastStartedUid, uid);
            verify(scanLists.getByUid(uid).lastHeard > 0);
        }

        name: "HomeScanLists"
        when: windowShown
    }
}
