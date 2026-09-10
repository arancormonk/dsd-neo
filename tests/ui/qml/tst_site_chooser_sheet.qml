// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui
import "../../../src/ui/qt/qml/Util.js" as Util
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
            prefs.metricUnits = false
            prefs.setLocationFix(0, 0, 0)
            while (savedSystems.count) savedSystems.remove(0)
            savedSystems.add({name:"North", rrSid:12, rrSiteId:16863, siteName:"North", siteLat:42, siteLon:-91, hasSitePos:true})
            savedSystems.add({name:"South", rrSid:12, rrSiteId:48391, siteName:"South", siteLat:41, siteLon:-91, hasSitePos:true})
            sheet.sessionState = 0
            sheet.openFor(0)
            starts.clear(); switches.clear()
        }
        function cleanup() {
            sheet.visible = false
            prefs.metricUnits = false
            prefs.setLocationFix(0, 0, 0)
            testContext.useLifecycleHost(false)
            while (savedSystems.count) savedSystems.remove(0)
        }
        function test_distance_units_update_live() {
            var north = findChild(sheet, "siteChoice0")
            var south = findChild(sheet, "siteChoice1")
            verify(north !== null && south !== null)
            compare(north.text, "North")
            compare(south.text, "South")

            prefs.setLocationFix(41, -91, Date.now(), 100)
            compare(north.text, "North · 69.1 mi")
            compare(south.text, "South · 0.0 mi")
            compare(sheet.nearest, 1)
            prefs.metricUnits = true
            compare(north.text, "North · 111.2 km")
            compare(south.text, "South · 0.0 km")
            compare(sheet.nearest, 1)
            prefs.metricUnits = false
            compare(north.text, "North · 69.1 mi")

            savedSystems.update(0, {hasSitePos: false})
            north = findChild(sheet, "siteChoice0")
            compare(north.text, "North")
            prefs.setLocationFix(0, 0, 0)
            south = findChild(sheet, "siteChoice1")
            compare(south.text, "South")
            compare(sheet.nearest, -1)
        }
        function test_distance_format_data() {
            return [
                {tag: "mile", km: 1.609344, imperial: "1.0 mi", metric: "1.6 km"},
                {tag: "rounding", km: 10, imperial: "6.2 mi", metric: "10.0 km"},
                {tag: "zero", km: 0, imperial: "0.0 mi", metric: "0.0 km"},
                {tag: "unavailable", km: -1, imperial: "", metric: ""},
                {tag: "nan", km: NaN, imperial: "", metric: ""},
                {tag: "infinite", km: Infinity, imperial: "", metric: ""},
                {tag: "missing", km: undefined, imperial: "", metric: ""},
                {tag: "null", km: null, imperial: "", metric: ""}
            ]
        }
        function test_distance_format(data) {
            compare(Util.fmtDistanceKm(data.km, false), data.imperial)
            compare(Util.fmtDistanceKm(data.km, true), data.metric)
        }
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
