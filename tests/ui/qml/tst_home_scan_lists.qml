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
