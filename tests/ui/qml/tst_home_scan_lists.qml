// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest

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
            while (scanLists.count)scanLists.remove(0)
            prefs.lastStartedKind = "";
            prefs.lastStartedUid = "";
            scanLists.add({
                "name": "Scan",
                "sourceType": "rtltcp",
                "host": "127.0.0.1",
                "port": 1234,
                "entries": [{
                    "kind": "freq",
                    "name": "Simplex",
                    "protocol": "p25",
                    "freqMhz": "851.5",
                    "enabled": true
                }]
            });
        }

        function cleanup() {
            testContext.setScanTestUsb(false, false);
            testContext.useLifecycleHost(false);
            while (scanLists.count)scanLists.remove(0)
        }

        function visualChild(item, name) {
            if (item.objectName === name)
                return item;

            var kids = item.contentItem ? [item.contentItem] : item.children || [];
            for (var i = 0; i < kids.length; ++i) {
                var found = visualChild(kids[i], name);
                if (found)
                    return found;

            }
            return null;
        }

        function test_home_card_and_confirmed_start() {
            tryVerify(function() {
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

        // WP-S2: signal routing reuses permission handling and initialization recency.
        function test_auto_start_scan_request() {
            scanLists.update(0, { "sourceType": "usb" });
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
            var overlays = ["wizardOpen", "scanListOpen", "exploreSetupOpen", "diagnosticsOpen",
                            "importsOpen", "radioReferenceOpen", "spectrumOpen", "talkgroupsOpen"];
            for (var i = 0; i < overlays.length; ++i) {
                appLoader.item[overlays[i]] = true;
                compare(uiController.autoStartBlocked, true);
                appLoader.item[overlays[i]] = false;
            }
            compare(uiController.autoStartBlocked, false);
        }

        function test_auto_start_saved_request() {
            var row = savedSystems.count;
            savedSystems.add({ "name": "Attached saved system", "sourceType": "usb", "freqMhz": "851.5" });
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
