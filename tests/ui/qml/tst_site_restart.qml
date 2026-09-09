// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest
Item {
    width: 420; height: 900
    Loader { id: loader; source: uiDir + "/Main.qml" }
    TestCase {
        name: "SiteRestart"
        when: windowShown
        function init() {
            testContext.useLifecycleHost(true, true)
            testContext.setDelayedStop(true)
            while (savedSystems.count) savedSystems.remove(0)
            for (var i = 0; i < 2; i++)
                savedSystems.add({name:"Site " + i, sourceType:"rtltcp", host:"127.0.0.1", port:1234,
                                  freqMhz:"851.5", rrSid:12, rrSiteId:100+i})
            loader.item.startSystem(0)
            testContext.setLifecyclePhase(2)
        }
        function cleanup() {
            loader.item.cancelPendingRestart()
            testContext.setDelayedStop(false)
            testContext.useLifecycleHost(false)
            while (savedSystems.count) savedSystems.remove(0)
        }
        function test_waits_for_idle_and_resolves_uid() {
            var uid = savedSystems.get(1).uid
            loader.item.restartSite(uid)
            compare(decoderHost.sessionState, 3)
            wait(10)
            compare(loader.item.sessionSystem.uid, savedSystems.get(0).uid)
            savedSystems.remove(0)
            testContext.setLifecyclePhase(0)
            tryCompare(decoderHost, "sessionState", 1)
            compare(loader.item.sessionSystem.uid, uid)
            compare(loader.item.pendingRestart, null)
        }
        function test_deleted_uid_cannot_start_shifted_row() {
            loader.item.restartSite(savedSystems.get(1).uid)
            savedSystems.remove(1)
            testContext.setLifecyclePhase(0)
            wait(10)
            compare(decoderHost.sessionState, 0)
            compare(loader.item.pendingRestart, null)
        }
        function test_cancelled_by_keyboard() {
            loader.item.restartSite(savedSystems.get(1).uid)
            keyClick(Qt.Key_Escape)
            compare(loader.item.pendingRestart, null)
            testContext.setLifecyclePhase(0)
            wait(10)
            compare(decoderHost.sessionState, 0)
        }
        function test_failed_stop_cancels_restart() {
            loader.item.restartSite(savedSystems.get(1).uid)
            testContext.setLifecyclePhase(4)
            compare(loader.item.pendingRestart, null)
        }
    }
}
