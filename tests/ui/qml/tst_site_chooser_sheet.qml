// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui
import "../../../src/ui/qt/qml/Util.js" as Util
Item {
    id: root
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
            sheet.currentSiteUid = ""
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
            north = findChild(sheet, "siteChoice0")
            south = findChild(sheet, "siteChoice1")
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
        function test_location_result_ranks_without_starting() {
            var action = findChild(sheet, "nearestSiteButton")
            verify(!action.enabled)
            sheet.useLocation()
            var requestId = sheet.locationId
            verify(requestId > 0)
            verify(findChild(sheet, "siteLocationButton").text.indexOf("Finding") >= 0)
            decoderHost.locationResult(requestId, true, 41.01, -91, 25, Date.now(), false, "", "", "")
            compare(sheet.locationId, 0)
            compare(sheet.rows[0], 1)
            compare(sheet.nearest, 1)
            verify(findChild(sheet, "nearestSiteSummary").text.indexOf("South") >= 0)
            verify(sheet.locationUpdated)
            compare(starts.count, 0)
            compare(switches.count, 0)
            verify(action.enabled)
            action.clicked()
            compare(starts.count, 1)
            compare(starts.signalArguments[0][0], 1)
        }
        function test_running_nearest_uses_restart() {
            sheet.sessionState = 2
            sheet.currentSiteUid = savedSystems.get(0).uid
            prefs.setLocationFix(41.01, -91, Date.now(), 25)
            var action = findChild(sheet, "nearestSiteButton")
            verify(action.enabled)
            compare(action.text, "Stop and switch to nearest")
            action.clicked()
            compare(starts.count, 0)
            compare(switches.count, 1)
            compare(switches.signalArguments[0][0], savedSystems.get(1).uid)
        }
        function test_current_nearest_and_pending_fix_cannot_restart() {
            sheet.sessionState = 2
            sheet.currentSiteUid = savedSystems.get(1).uid
            prefs.setLocationFix(41.01, -91, Date.now(), 25)
            var action = findChild(sheet, "nearestSiteButton")
            verify(!action.enabled)
            compare(action.text, "Listening to nearest site")
            sheet.chooseNearest()
            compare(switches.count, 0)
            sheet.currentSiteUid = savedSystems.get(0).uid
            verify(action.enabled)
            sheet.useLocation()
            verify(!action.enabled)
            sheet.chooseNearest()
            compare(switches.count, 0)
        }
        function test_missing_and_avoided_sites_are_explained() {
            savedSystems.add({name:"Missing",rrSid:12,rrSiteId:30})
            prefs.setLocationFix(41.01, -91, Date.now(), 25)
            compare(sheet.rows.join(","), "1,0,2")
            verify(findChild(sheet, "sitePositionNotice2").visible)
            sheet.setAvoid(1, true)
            compare(sheet.nearest, 0)
            compare(sheet.rows.join(","), "0,2,1")
            sheet.setAvoid(0, true)
            verify(!findChild(sheet, "nearestSiteButton").enabled)
            verify(findChild(sheet, "nearestSiteSummary").text.indexOf("No site can be ranked") >= 0)
        }
        function test_failed_refresh_labels_saved_location_and_ignores_late_results() {
            prefs.setLocationFix(42, -91, Date.now(), 100)
            sheet.useLocation()
            var retired = sheet.locationId
            sheet.useLocation()
            var current = sheet.locationId
            decoderHost.locationResult(retired, true, 41, -91, 25, Date.now(), false, "", "", "")
            compare(sheet.locationId, current)
            compare(sheet.nearest, 0)
            decoderHost.locationResult(current, false, 0, 0, 0, 0, false, "", "", "denied")
            compare(sheet.locationId, 0)
            verify(!sheet.locationUpdated)
            verify(sheet.notice.indexOf("saved location") >= 0)
            sheet.useLocation()
            current = sheet.locationId
            sheet.visible = false
            decoderHost.locationResult(current, true, 41, -91, 25, Date.now(), false, "", "", "")
            compare(sheet.nearest, 0)
        }
        function test_interrupted_permission_prompt_returns_to_idle() {
            sheet.useLocation()
            var interrupted = sheet.locationId
            verify(interrupted !== 0)
            decoderHost.locationCancelled(interrupted + 1)
            compare(sheet.locationId, interrupted)
            decoderHost.locationCancelled(interrupted)
            compare(sheet.locationId, 0)
            compare(sheet.notice, "")
            verify(!sheet.locationUpdated)
            compare(findChild(sheet, "siteLocationNotice").text, "Use your location to find the nearest saved site in this system.")
            verify(findChild(sheet, "siteLocationButton").enabled)
        }
        function test_accuracy_and_saved_fix_feedback() {
            prefs.setLocationFix(41.5, -91, Date.now(), 2000)
            verify(sheet.uncertainNearest)
            verify(findChild(sheet, "siteLocationAccuracy").text.indexOf("may change order") >= 0)
            verify(findChild(sheet, "siteLocationNotice").text.indexOf("saved location") >= 0)
            prefs.metricUnits = true
            verify(sheet.accuracyText.indexOf("2.0\u00a0km") >= 0)
            prefs.setLocationFix(41, -91, Date.now(), 25)
            verify(!sheet.uncertainNearest)
            verify(sheet.accuracyText.indexOf("25\u00a0m") >= 0)
        }
        function test_nearest_summary_wraps_data() {
            return [{tag:"phone",width:360,scale:1}, {tag:"large text",width:360,scale:1.6}, {tag:"tablet",width:800,scale:1}]
        }
        function test_nearest_summary_wraps(data) {
            var width = root.width
            var scale = Ui.Theme.fontScale
            try {
                root.width = data.width
                Ui.Theme.fontScale = data.scale
                sheet.sessionState = 2
                savedSystems.update(1, {siteName:"South County Regional Public Safety Simulcast <b>Site</b>"})
                prefs.setLocationFix(41.01, -91, Date.now(), 25)
                waitForRendering(sheet)
                var summary = findChild(sheet, "nearestSiteSummary")
                compare(summary.textFormat, Text.PlainText)
                verify(summary.paintedWidth <= summary.width + 0.5)
                verify(summary.height >= summary.paintedHeight - 0.5)
                var action = findChild(sheet, "nearestSiteButton")
                verify(action.height >= 44)
                verify(findChild(sheet, "siteTitle1").visible)
                verify(!findChild(sheet, "siteChoice1").visible)
            } finally {
                root.width = width
                Ui.Theme.fontScale = scale
            }
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
