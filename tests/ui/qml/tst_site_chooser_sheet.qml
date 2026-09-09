// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui
Item {
    width: 420; height: 900
    Ui.SiteChooserSheet { id: sheet }
    SignalSpy { id: starts; target: sheet; signalName: "startSite" }
    SignalSpy { id: switches; target: sheet; signalName: "restartSite" }
    TestCase {
        name: "SiteChooserSheet"
        when: windowShown
        function init() {
            testContext.useLifecycleHost(true)
            while (savedSystems.count) savedSystems.remove(0)
            savedSystems.add({name:"North", rrSid:12, rrSiteId:16863, siteName:"North", siteLat:42, siteLon:-91, hasSitePos:true})
            savedSystems.add({name:"South", rrSid:12, rrSiteId:48391, siteName:"South", siteLat:41, siteLon:-91, hasSitePos:true})
            sheet.sessionState = 0
            sheet.openFor(0)
            starts.clear(); switches.clear()
        }
        function cleanup() { sheet.visible = false; testContext.useLifecycleHost(false); while (savedSystems.count) savedSystems.remove(0) }
        function test_reopen_same_uid_after_reindexing() {
            // Place a one-site group immediately before another group, so a stale
            // row index would select the other group after removing an earlier row.
            savedSystems.add({name:"Chosen", rrSid:20, rrSiteId:200, siteName:"Chosen"})
            savedSystems.add({name:"Unrelated", rrSid:30, rrSiteId:300, siteName:"Unrelated"})
            var uid = savedSystems.get(2).uid
            sheet.openFor(2)
            compare(sheet.rows.length, 1)
            sheet.visible = false
            savedSystems.remove(0)
            sheet.openFor(savedSystems.rowForUid(uid))
            verify(sheet.visible)
            compare(sheet.groupUid, uid)
            compare(sheet.groupRow, 1)
            compare(sheet.rows.length, 1)
            compare(savedSystems.get(sheet.rows[0]).uid, uid)
            sheet.choose(sheet.rows[0])
            compare(starts.count, 1)
            compare(savedSystems.get(starts.signalArguments[0][0]).uid, uid)
        }
        function test_idle_starts() { sheet.choose(1); compare(starts.count, 1); compare(switches.count, 0) }
        function test_running_requires_restart() {
            sheet.sessionState = 2; sheet.choose(1)
            compare(starts.count, 0); compare(switches.count, 0)
            sheet.stopAndSwitch(1); compare(switches.count, 1)
        }
        function test_transitions_reject_actions() {
            for (var state of [1, 3, 4]) {
                sheet.sessionState = state; sheet.choose(1); sheet.stopAndSwitch(1)
            }
            compare(starts.count, 0); compare(switches.count, 0)
        }
        function test_nearest_skips_avoided() {
            prefs.setLocationFix(41.1, -91, Date.now(), 100)
            compare(sheet.nearest, 1)
            sheet.setAvoid(1, true)
            compare(sheet.nearest, 0)
            sheet.setAvoid(0, true)
            compare(sheet.nearest, -1)
        }
        function test_avoid_switch_click() {
            var toggle = findChild(sheet, "siteAvoid0")
            verify(toggle !== null)
            waitForRendering(sheet)
            mouseClick(toggle, toggle.width / 2, toggle.height / 2)
            verify(savedSystems.get(0).avoidSite)
        }
        function test_avoid_toggle() {
            sheet.setAvoid(1, true)
            verify(savedSystems.get(1).avoidSite)
            sheet.choose(1); compare(starts.count, 0)
            sheet.setAvoid(1, false); sheet.choose(1); compare(starts.count, 1)
        }
    }
}
