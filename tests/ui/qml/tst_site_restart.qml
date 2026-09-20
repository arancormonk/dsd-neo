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
            findChild(loader.item, "siteChooserSheet").visible = false
            loader.item.cancelPendingRestart()
            testContext.setDelayedStop(false)
            testContext.useLifecycleHost(false)
            while (savedSystems.count) savedSystems.remove(0)
        }
        function visualNamed(item, name) {
            if (!item) return null
            if (item.objectName === name) return item
            var children = item.children || []
            for (var i = 0; i < children.length; ++i) {
                var found = visualNamed(children[i], name)
                if (found) return found
            }
            return item.contentItem ? visualNamed(item.contentItem, name) : null
        }
        function clickChooserEntry(name) {
            var button = null
            tryVerify(function() { button = visualNamed(loader.item.contentItem, name); return button !== null })
            var chooser = findChild(loader.item, "siteChooserSheet")
            verify(button !== null && chooser !== null)
            tryCompare(button, "visible", true)
            tryCompare(button, "enabled", true)
            waitForRendering(button)
            verify(button.width >= 44 && button.height >= 44, "Site entry must have a touch target")
            mouseClick(button, button.width / 2, button.height / 2)
            tryCompare(chooser, "visible", true)
            compare(chooser.groupUid, savedSystems.get(0).uid)
        }
        function test_retry_saved_after_failure() {
            testContext.setLifecyclePhase(4)
            decoderHost.requestLocalDeviceAccess()
            loader.item.startSystem(0)
            compare(decoderHost.sessionState, 1)
            testContext.setLifecyclePhase(2)
            compare(loader.item.sessionSystem.uid, savedSystems.get(0).uid)
        }
        function test_failed_chooser_recovery() {
            testContext.setDelayedStop(false)
            testContext.setLifecyclePhase(4)
            var chooser = findChild(loader.item, "siteChooserSheet")
            chooser.openFor(0)
            var recover = findChild(chooser, "siteRecoverButton")
            verify(recover !== null)
            verify(recover.visible)
            recover.clicked()
            compare(decoderHost.sessionState, 0)
            chooser.choose(1)
            compare(decoderHost.sessionState, 1)
            compare(loader.item.sessionSystem.uid, savedSystems.get(1).uid)
        }
        function test_nearby_back_chooser_reopen() {
            testContext.setLifecyclePhase(0)
            loader.item.radioReferenceOpen = true
            radioReference.lookupNearby()
            verify(radioReference.busy)
            findChild(loader.item, "radioReferenceScreen").closed()
            verify(!radioReference.busy)
            var chooser = findChild(loader.item, "siteChooserSheet")
            chooser.openFor(0)
            chooser.useLocation()
            var first = chooser.locationId
            verify(first > 0)
            // A new D3 owner explicitly retires the chooser, even before dismissal.
            loader.item.radioReferenceOpen = true
            radioReference.lookupNearby()
            compare(chooser.locationId, 0)
            verify(radioReference.busy)
            loader.item.radioReferenceOpen = false
            verify(!radioReference.busy)
            chooser.useLocation()
            verify(chooser.locationId > first)
            chooser.visible = false
            compare(chooser.locationId, 0)
        }
        function test_home_entry_opens_chooser() {
            testContext.setDelayedStop(false)
            decoderHost.stop()
            var onboardingDone = prefs.onboardingDone
            prefs.onboardingDone = true
            try {
                clickChooserEntry("homeSiteChooserButton")
            } finally {
                prefs.onboardingDone = onboardingDone
            }
        }
        function test_running_entry_opens_chooser() {
            clickChooserEntry("runningSiteChooserButton")
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
