// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest
import DsdNeo 1.0
import "../../../src/ui/qt/qml" as Ui
import "../../../src/ui/qt/qml/Util.js" as Util
Item {
    id: root
    width: 420; height: 900
    Ui.SiteChooserSheet { id: sheet }
    SignalSpy { id: starts; target: sheet; signalName: "startSite" }
    SignalSpy { id: switches; target: sheet; signalName: "restartSite" }
    SignalSpy { id: accountOpened; target: sheet; signalName: "openRadioReferenceAccount" }
    TestCase {
        name: "SiteChooserSheet"
        when: windowShown
        function init() {
            testContext.setRadioReference("available", true)
            testContext.useLifecycleHost(true)
            testContext.resetPins()
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
            testContext.setRadioReference("available", false)
            sheet.visible = false
            testContext.resetRrPlaces()
            accountOpened.clear()
            prefs.metricUnits = false
            prefs.setLocationFix(0, 0, 0)
            testContext.useLifecycleHost(false)
            while (savedSystems.count) savedSystems.remove(0)
            testContext.resetPins()
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
            compare(north.text, "North · 69.0 mi")
            compare(south.text, "South · 0.0 mi")
            compare(sheet.nearest, 1)
            prefs.metricUnits = true
            compare(north.text, "North · 111.1 km")
            compare(south.text, "South · 0.0 km")
            compare(sheet.nearest, 1)
            prefs.metricUnits = false
            compare(north.text, "North · 69.0 mi")

            savedSystems.update(0, {hasSitePos: false})
            north = findChild(sheet, "siteChoice0")
            compare(north.text, "North")
            prefs.setLocationFix(0, 0, 0)
            south = findChild(sheet, "siteChoice1")
            compare(south.text, "South")
            compare(sheet.nearest, -1)
        }
        // The ellipsoid, not the sphere, ranks the sites: from (0, 30) the
        // sphere puts (0, 30.896) first, 99.631 km against 100.076 km; on
        // WGS84 (0.9, 30) is nearer, 99.517 km against 99.742 km.
        function test_wgs84_orders_the_nearer_site() {
            savedSystems.update(0, {siteLat: 0, siteLon: 30.896})
            savedSystems.update(1, {siteLat: 0.9, siteLon: 30})
            prefs.metricUnits = true
            prefs.setLocationFix(0, 30, Date.now(), 10)
            compare(sheet.rows.join(","), "1,0")
            compare(sheet.nearest, 1)
            compare(sheet.rowModel[0].row, 1)
            verify(Math.abs(sheet.rowModel[0].distance - 99.516930425) < 1e-6)
            verify(Math.abs(sheet.rowModel[1].distance - 99.742263751) < 1e-6)
            compare(sheet.nearestDistance, sheet.rowModel[0].distance)
            compare(findChild(sheet, "siteChoice1").text, "South · 99.5 km")
            compare(findChild(sheet, "siteChoice0").text, "North · 99.7 km")
            verify(findChild(sheet, "nearestSiteSummary").text.indexOf("South") >= 0)
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
        // The same system saved for another receiver is another group: the chooser lists this receiver's sites
        // once, and its nearest is one of them.
        function test_other_receiver_is_another_group() {
            savedSystems.add({name:"North", rrSid:12, rrSiteId:16863, siteName:"North", siteLat:41.1, siteLon:-91, hasSitePos:true, sourceType:"rtltcp", host:"127.0.0.1", port:1234})
            sheet.refresh()
            compare(sheet.rows, [0, 1])
            prefs.setLocationFix(41.1, -91, Date.now(), 100)
            compare(sheet.nearest, 1)
            sheet.openFor(2)
            compare(sheet.rows, [2])
            compare(sheet.nearest, 2)
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
        // Issue #801 B7: an avoid the store refuses is not shown as made; the row says it failed. The rows are
        // rebuilt on every change, so each step finds them again.
        function clickAvoid(row, blocked) {
            var toggle = findChild(sheet, "siteAvoid" + row)
            verify(toggle !== null)
            waitForRendering(sheet)
            var store = testContext.storePath("saved_systems.json")
            if (blocked)
                verify(testContext.blockPath(store, true), "could not block the store")
            try {
                mouseClick(toggle, toggle.width / 2, toggle.height / 2)
            } finally {
                if (blocked)
                    verify(testContext.blockPath(store, false), "could not unblock the store")
            }
        }
        function test_avoid_toggle_failure_shown() {
            clickAvoid(0, true)
            verify(!savedSystems.get(0).avoidSite)
            compare(findChild(sheet, "siteAvoid0").checked, false, "the toggle shows what is saved")
            var error = findChild(sheet, "siteAvoidError0")
            verify(error.visible)
            compare(error.text, "Could not avoid this site. Try again.")
            compare(error.textFormat, Text.PlainText)
            verify(!findChild(sheet, "siteAvoidError1").visible, "only the row that failed says so")

            clickAvoid(0, false)
            verify(savedSystems.get(0).avoidSite)
            compare(findChild(sheet, "siteAvoid0").checked, true)
            verify(!findChild(sheet, "siteAvoidError0").visible, "a saved change clears the line")

            clickAvoid(0, true)
            verify(savedSystems.get(0).avoidSite)
            compare(findChild(sheet, "siteAvoid0").checked, true)
            error = findChild(sheet, "siteAvoidError0")
            verify(error.visible)
            compare(error.text, "Could not stop avoiding this site. Try again.")

            // Reopened, the sheet starts clean.
            sheet.openFor(0)
            verify(!findChild(sheet, "siteAvoidError0").visible)
        }
        // Issue #801 D4: each site pins on its own from here. The button says what it will do, flips as it is
        // pressed and keeps the chooser open; pinning starts nothing, and is offered while a session runs too.
        function test_pin_from_site_chooser() {
            var north = savedSystems.get(0).uid
            var south = savedSystems.get(1).uid
            var pin = findChild(sheet, "sitePin0")
            verify(pin !== null)
            compare(pin.text, "Pin to Listen")
            compare(pin.accessibleName, "Pin North · North to Listen")
            verify(pin.enabled)
            verify(!findChild(sheet, "sitePinReason0").visible)
            waitForRendering(sheet)
            mouseClick(pin, pin.width / 2, pin.height / 2)
            verify(sheet.visible, "pinning keeps the chooser open")
            verify(listenPins.isPinned("site", north))
            verify(!listenPins.isPinned("site", south), "only the pressed site")
            verify(!listenPins.isPinned("system", north), "a site, not its system")
            compare(pin.text, "Unpin from Listen", "the same button flips")
            compare(pin.accessibleName, "Unpin North · North from Listen")
            compare(findChild(sheet, "sitePin1").text, "Pin to Listen")
            compare(starts.count, 0)
            compare(switches.count, 0)
            mouseClick(pin, pin.width / 2, pin.height / 2)
            compare(listenPins.count, 0)
            compare(pin.text, "Pin to Listen")
            sheet.sessionState = 2
            verify(pin.visible && pin.enabled, "offered while a session runs")
        }
        // Without RadioReference in this build there is no update to offer: no button (so no Tab stop), and the
        // note does not send the user to one.
        function test_no_coordinates_without_radioreference() {
            testContext.setRadioReference("available", false)
            savedSystems.update(1, {hasSitePos: false})
            sheet.openFor(0)
            var note = findChild(sheet, "sitePositionNotice1")
            verify(note.visible)
            compare(note.text, "No coordinates.")
            var update = findChild(sheet, "siteUpdateFromRr")
            verify(!update.visible, "no Update button")
            testContext.setRadioReference("available", true)
            verify(update.visible, "offered again where RadioReference is built in")
            compare(note.text, "No coordinates. Use Update from RadioReference to check for new location data.")
        }
        // A site with no coordinates points at Update from RadioReference (the menu's, and a button here), not at
        // a re-import that would add a second copy of the site.
        function test_no_coordinates_offers_update() {
            verify(!findChild(sheet, "siteUpdateFromRr").visible, "every site placed: nothing to offer")
            savedSystems.update(1, {hasSitePos: false})
            sheet.openFor(0)
            var note = findChild(sheet, "sitePositionNotice1")
            verify(note.visible)
            compare(note.text, "No coordinates. Use Update from RadioReference to check for new location data.")
            var update = findChild(sheet, "siteUpdateFromRr")
            verify(update.visible && update.enabled)
            compare(update.text, "Update from RadioReference")

            testContext.setRrPlacesSignedIn(false)
            update.clicked()
            verify(!sheet.visible, "signed out: the sheet makes way for the account screen")
            compare(accountOpened.count, 1)
            testContext.setRrPlacesSignedIn(true)

            sheet.openFor(0)
            update.clicked()
            compare(testContext.rrPlacesUpdateUids(), [savedSystems.get(0).uid, savedSystems.get(1).uid])
            verify(!update.enabled, "one update at a time")
            var progress = findChild(sheet, "siteUpdateProgress")
            verify(progress.visible)
            compare(progress.text, "Updating 1 of 2")
            testContext.finishRrPlacesUpdate(2, 0)
            verify(!progress.visible)
            var summary = findChild(sheet, "siteUpdateSummary")
            verify(summary.visible)
            compare(summary.text, "Updated 2 systems.")
            verify(sheet.visible)
        }
        // An update started elsewhere (a Home card) runs: Update here is disabled, reads as such, and a call asks the
        // service nothing, so it neither takes the other update's summary as its own nor sends the user to the
        // account screen. Once that update ends, Update starts this one.
        function test_update_refused_while_another_runs() {
            savedSystems.update(1, {hasSitePos: false})
            sheet.openFor(0)
            var update = findChild(sheet, "siteUpdateFromRr")
            verify(update.visible && update.enabled)
            verify(rrPlaces.updateFromRadioReference(["elsewhere"]), "an update from a card")
            verify(!update.enabled, "disabled while it runs")
            verify(update.Accessible.ignored === false, "still read, as disabled")
            compare(update.Accessible.name, "Update from RadioReference")
            verify(findChild(sheet, "siteUpdateProgress").visible, "the progress line says why")
            sheet.updateFromRadioReference()
            compare(testContext.rrPlacesUpdateCalls(), 1, "the service is not asked")
            verify(sheet.visible, "the sheet stays")
            compare(accountOpened.count, 0, "not the account screen")
            verify(!sheet.rrUpdateStarted, "the running update is not this sheet's")
            testContext.finishRrPlacesUpdate(1, 0)
            verify(!findChild(sheet, "siteUpdateSummary").visible, "nor is its summary")
            verify(update.enabled)
            update.clicked()
            compare(testContext.rrPlacesUpdateUids(), [savedSystems.get(0).uid, savedSystems.get(1).uid])
            verify(sheet.rrUpdateStarted)
        }
        // An update started elsewhere (a Home card) saves this group's sites while the keyboard is on one of them
        // here: each save rebuilds the rows, and the keyboard stays on the same control of that site's row, wherever
        // the row moves. A site that goes leaves it on the first site.
        function test_refresh_from_elsewhere_keeps_the_keyboard() {
            prefs.setLocationFix(41, -91, Date.now(), 10)
            sheet.openFor(0)
            compare(sheet.rows, [1, 0], "South is nearer")
            var southUid = savedSystems.get(1).uid
            var avoid = findChild(sheet, "siteAvoid1")
            avoid.forceActiveFocus()
            verify(avoid.activeFocus)
            verify(rrPlaces.updateFromRadioReference(["elsewhere"]), "an update from a card")

            // A save moves North nearer: the rows are rebuilt and reordered.
            savedSystems.update(0, {siteLat: 41, siteLon: -91})
            compare(sheet.rows, [0, 1])
            tryVerify(function () { return findChild(sheet, "siteAvoid1").activeFocus }, 1000,
                      "South's Avoid keeps the keyboard")
            verify(findChild(sheet, "siteAvoid1") !== avoid, "a rebuilt row")

            var pin = findChild(sheet, "sitePin1")
            pin.forceActiveFocus()
            savedSystems.update(1, {siteName: "South Renamed"})
            testContext.finishRrPlacesUpdate(2, 0)
            tryVerify(function () { return findChild(sheet, "sitePin1").activeFocus }, 1000,
                      "two rebuilds in one turn: South's Pin keeps it")
            compare(savedSystems.get(1).uid, southUid)

            // South leaves the group: the first site takes the keyboard.
            savedSystems.remove(1)
            compare(sheet.rows, [0])
            tryVerify(function () { return findChild(sheet, "siteChoice0").activeFocus }, 1000,
                      "the first site has it")
            verify(sheet.visible)
        }
        // Update from the keyboard: it disables while the update runs, so the keyboard goes to the first site. When a
        // saved site reorders the rows (rebuilt, so the focused row goes), it stays with that site, wherever its row
        // lands, through rebuilds and the update's end in one turn.
        function test_update_keeps_the_keyboard() {
            prefs.setLocationFix(41, -91, Date.now(), 10)
            savedSystems.update(1, {hasSitePos: false})
            sheet.openFor(0)
            var update = findChild(sheet, "siteUpdateFromRr")
            var firstSite = function () { return findChild(sheet, "siteChoice" + sheet.rows[0]) }
            compare(sheet.rows, [0, 1])
            update.forceActiveFocus()
            verify(update.activeFocus)
            keyClick(Qt.Key_Space)
            verify(rrPlaces.updating)
            tryVerify(function () { return firstSite().activeFocus }, 1000, "the first site has the keyboard")
            testContext.finishRrPlacesUpdate(0, 1)
            verify(update.enabled)
            verify(firstSite().activeFocus, "a site that stays keeps it")

            update.forceActiveFocus()
            keyClick(Qt.Key_Space)
            tryVerify(function () { return firstSite().activeFocus }, 1000)
            // South gains a position nearer than North: the rows reorder, and Update is no longer offered.
            savedSystems.update(1, {hasSitePos: true, siteLat: 41, siteLon: -91})
            compare(sheet.rows, [1, 0])
            verify(!update.visible)
            var north = function () { return findChild(sheet, "siteChoice0") }
            tryVerify(function () { return north().activeFocus }, 1000, "North keeps it, now second")
            testContext.finishRrPlacesUpdate(1, 0)
            verify(north().activeFocus)

            // The rows rebuild and the update ends in one turn (no event in between): the keyboard, lost with the
            // focused row, goes back to that site's row.
            savedSystems.update(0, {hasSitePos: false})
            update.forceActiveFocus()
            keyClick(Qt.Key_Space)
            tryVerify(function () { return firstSite().activeFocus }, 1000)
            compare(sheet.rows, [1, 0])
            var south = findChild(sheet, "siteChoice1")
            verify(south.activeFocus)
            savedSystems.update(1, {hasSitePos: false})
            compare(sheet.rows, [0, 1], "rebuilt: the focused row went")
            testContext.finishRrPlacesUpdate(0, 1)
            tryVerify(function () { return findChild(sheet, "siteChoice1").activeFocus }, 1000, "South has it again")
            verify(findChild(sheet, "siteChoice1") !== south)
            verify(update.enabled)
            verify(sheet.visible)
        }
        // #786 GPS roaming. Three made-up sites of one system on one rtl_tcp receiver, as tst_site_roaming.qml has
        // them: Hill (lat 10.0), Lake (10.5) and Ridge (11.5), all at lon 20.0. Roaming runs over the lifecycle host
        // with a location feed, on the clock the case sets.
        property double ns: 1
        property var valley: []
        function addValley() {
            var names = ["Hill", "Lake", "Ridge"]
            var lats = [10.0, 10.5, 11.5]
            var uids = []
            for (var i = 0; i < 3; ++i) {
                savedSystems.add({name: "Valley — " + names[i], siteName: names[i], rrSid: 7, rrSiteId: 70 + i,
                    trunking: true, sourceType: "rtltcp", host: "127.0.0.1", port: 1234, decodeFlag: "-fp",
                    freqMhz: (851.0125 + i).toFixed(4), siteLat: lats[i], siteLon: 20.0, hasSitePos: true})
                uids.push(savedSystems.get(savedSystems.count - 1).uid)
            }
            valley = uids
        }
        // Roaming on, over a host with a location feed (a fresh executor), metric, and the Valley sites.
        function roamingOn() {
            testContext.setLifecycleLocationSupported(true)
            testContext.useLifecycleHost(true)
            testContext.setRoamingNow(1000000)
            prefs.metricUnits = true
            prefs.extraArgs = ""
            prefs.roamingEnabled = true
            addValley()
        }
        // Saved site @p uid running past its initialization edge, as Main reports it.
        function runSite(uid, busy) {
            testContext.setLifecyclePhase(1)
            siteRoaming.noteSessionStart("saved", uid, 0)
            testContext.setLifecyclePhase(2)
            siteRoaming.facts = {savedUid: uid, trunking: true, radioInput: true, replayInput: false, scanKind: 0,
                operatorTuned: false, heldTg: 0, callBusy: busy === true, optionsKnown: true}
            siteRoaming.noteSessionInitialized()
            sheet.sessionState = 2
            sheet.currentSiteUid = uid
        }
        // A new roaming fix at @p lat (lon 20.0), @p after ms on.
        function roamFix(lat, after, accuracy) {
            testContext.setRoamingNow(testContext.roamingNow() + (after || 0))
            ns += 1000
            testContext.emitLocationStreamUpdate({generation: 1, state: "ok", hasFix: true, provider: "gps",
                measurementNs: ns, lat: lat, lon: 20.0, accuracyM: accuracy || 10, fixAtMs: testContext.roamingNow()})
        }
        function roamingOff() {
            prefs.roamingEnabled = false
            prefs.metricUnits = false
            testContext.setLifecyclePhase(0)
            testContext.setLifecycleLocationSupported(false)
            testContext.useLifecycleHost(true)
        }
        function valleyRow(index) {
            return savedSystems.rowForUid(valley[index])
        }
        function line(name) {
            var item = findChild(sheet, name)
            verify(item !== null, name)
            return item
        }

        // Off, the switch says what it would do; it switches on only where the card has two sites roaming can use,
        // and always off. Switching on asks for precise location; without a location feed the switch is not offered.
        function test_roaming_toggle_and_disclosure() {
            verify(!line("siteRoamingBlock").visible, "no location feed: not offered")
            roamingOn()
            prefs.roamingEnabled = false
            sheet.openFor(valleyRow(0))
            var toggle = line("siteRoamingToggle")
            try {
                verify(line("siteRoamingBlock").visible)
                compare(toggle.text, "GPS roaming")
                verify(!toggle.checked)
                verify(toggle.enabled)
                compare(line("siteRoamingDisclosure").text, "Switch to the nearest saved site of the system you are listening to as you move. Uses precise location while DSD-neo is on screen.")
                verify(!line("siteRoamingUnavailable").visible)
                verify(!line("siteRoamingContext").visible && !line("siteRoamingState").visible, "off: no status")
                waitForRendering(sheet)
                mouseClick(toggle, toggle.width / 2, toggle.height / 2)
                verify(siteRoaming.enabled, "switched on")
                compare(testContext.roamingHostCalls().permissionRequests.length, 1, "precise location asked for once")
                verify(toggle.checked)
                compare(line("siteRoamingContext").text, "Roaming works while listening to a site of a multi-site system")
                mouseClick(toggle, toggle.width / 2, toggle.height / 2)
                verify(!siteRoaming.enabled && !toggle.checked, "switched off")

                // Extra options in Settings keep roaming off for every site: the switch says why.
                prefs.extraArgs = "-Z"
                verify(!toggle.enabled, "extra options in Settings: cannot switch on")
                compare(line("siteRoamingUnavailable").text, "GPS roaming is off while extra options are set in Settings")
                compare(toggle.Accessible.description, "GPS roaming is off while extra options are set in Settings",
                        "a screen reader hears why on the switch")
                prefs.extraArgs = ""
                verify(toggle.enabled)
                compare(toggle.Accessible.description, "")

                // North and South are no sites roaming can use (no trunking, no receiver it can tune).
                sheet.openFor(0)
                verify(!toggle.enabled, "fewer than two roamable sites: cannot switch on")
                compare(line("siteRoamingUnavailable").text, "Needs two or more sites in this system that GPS roaming can use: trunking on, a receiver it can tune, and no extra options.")
                compare(toggle.Accessible.description, line("siteRoamingUnavailable").text)
                verify(line("siteRoamingUnavailable").visible)
                prefs.roamingEnabled = true
                verify(toggle.enabled && toggle.checked, "but always switchable off")
                verify(!line("siteRoamingUnavailable").visible)
            } finally {
                roamingOff()
            }
        }

        // The card of the session roaming follows: the site it runs and how far, then what roaming does, with the
        // action a pause or hold needs. A state change is read out once; a distance moving is not, and the distance
        // stays out of what a screen reader hears.
        function test_roaming_status_for_this_system() {
            roamingOn()
            try {
                runSite(valley[0])
                sheet.openFor(valleyRow(0))
                var context = line("siteRoamingContext")
                var state = line("siteRoamingState")
                var action = line("siteRoamingAction")
                compare(context.text, "Listening to Hill")
                compare(state.text, "Waiting for a location fix")
                verify(!action.visible)
                var speech = findChild(sheet, "siteRoamingSpeech")
                var heard = speech.announcements
                roamFix(10.01, 1000)
                compare(context.text, "Listening to Hill · 1.1\u00a0km")
                compare(context.Accessible.name, "Listening to Hill", "the distance is not spoken")
                compare(state.text, "Staying on this site until another is clearly closer")
                tryCompare(speech, "announcements", heard + 1)
                compare(speech.announced, "Staying on this site until another is clearly closer")
                roamFix(10.02, 1000)
                compare(context.text, "Listening to Hill · 2.2\u00a0km", "the distance moves in place")
                wait(0)
                compare(speech.announcements, heard + 1, "a distance alone is not read out")
                roamFix(10.49, 1000)
                compare(state.text, "Lake is closer. Checking before switching")
                tryCompare(speech, "announcements", heard + 2)

                testContext.emitLocationStreamUpdate({generation: 2, state: "noPermission", hasFix: false})
                compare(state.text, "Location permission is off")
                compare(state.color, Ui.Theme.alert, "a pause the user must fix is in the alert colour")
                verify(action.visible)
                compare(action.text, "Open app settings")
                waitForRendering(sheet)
                mouseClick(action, action.width / 2, action.height / 2)
                compare(testContext.roamingHostCalls().appSettingsOpens, 1)
                testContext.emitLocationStreamUpdate({generation: 2, state: "approximateOnly", hasFix: false})
                compare(state.text, "Roaming needs precise location")
                compare(action.text, "Open app settings")
                testContext.emitLocationStreamUpdate({generation: 2, state: "locationOff", hasFix: false})
                compare(state.text, "Location is off")
                compare(action.text, "Turn on location")
                action.clicked()
                compare(testContext.roamingHostCalls().locationSettingsOpens, 1)
                verify(sheet.visible, "the actions keep the chooser open")

                // Lake chosen from Hill's session: roaming holds it until Resume roaming.
                siteRoaming.noteSiteChoice(valley[1])
                testContext.setLifecyclePhase(0)
                runSite(valley[1])
                compare(context.text, "Listening to Lake")
                compare(state.text, "Holding the site you chose")
                compare(state.color, Ui.Theme.textPrimary)
                compare(action.text, "Resume roaming")
                action.clicked()
                verify(siteRoaming.state !== SiteRoaming.Holding, "resumed")
                verify(state.text !== "Holding the site you chose")
            } finally {
                roamingOff()
            }
        }

        // Another system's card says which system roaming follows, with no state line; with nothing roaming can
        // follow it gives the reason.
        function test_roaming_other_system() {
            roamingOn()
            try {
                sheet.openFor(0)
                compare(line("siteRoamingContext").text, "Roaming works while listening to a site of a multi-site system")
                verify(!line("siteRoamingState").visible)
                runSite(valley[0])
                roamFix(10.01, 1000)
                sheet.openFor(0)
                compare(line("siteRoamingContext").text, "GPS roaming follows Valley, the system you are listening to")
                verify(!line("siteRoamingState").visible, "no state line for another system")
                verify(!line("siteRoamingAction").visible)
            } finally {
                roamingOff()
            }
        }

        // While roaming supplies fixes the rows keep the order they opened in; only each row's distance moves, the
        // rows are not made again, and the keyboard stays where it is. Use my location takes the order of now.
        function test_frozen_order_with_live_distances() {
            roamingOn()
            try {
                runSite(valley[0])
                roamFix(10.0, 1000)
                sheet.openFor(valleyRow(0))
                compare(sheet.rows, [valleyRow(0), valleyRow(1), valleyRow(2)], "nearest first when it opens")
                var hill = line("siteTitle" + valleyRow(0))
                var lake = line("siteTitle" + valleyRow(1))
                var ridge = line("siteTitle" + valleyRow(2))
                var avoid = line("siteAvoid" + valleyRow(1))
                avoid.forceActiveFocus()
                verify(avoid.activeFocus)
                for (var step = 1; step <= 5; ++step)
                    roamFix(10.0 + step * 0.3, 1000)
                // At 11.5 Ridge is nearest now, Hill the farthest.
                compare(sheet.nearest, valleyRow(2))
                compare(sheet.rows, [valleyRow(0), valleyRow(1), valleyRow(2)], "the order is kept")
                verify(line("siteTitle" + valleyRow(0)) === hill && line("siteTitle" + valleyRow(1)) === lake
                       && line("siteTitle" + valleyRow(2)) === ridge, "no row made again")
                compare(ridge.text, "Ridge · 0.0 km", "each distance moves in place")
                compare(hill.text, "Hill · 165.9 km")
                compare(hill.Accessible.name, "Hill", "a moving distance is not spoken")
                verify(line("siteAvoid" + valleyRow(1)) === avoid && avoid.activeFocus, "the keyboard stays on Lake's Avoid")
                compare(line("siteLocationNotice").text, "Distances follow your location while GPS roaming runs.")
                var summary = line("nearestSiteSummary")
                compare(summary.text, "Nearest site: Ridge · 0.0\u00a0km", "the nearest is the nearest now")
                compare(summary.Accessible.name, "Nearest site: Ridge", "its moving distance is not spoken")
                var accuracy = line("siteLocationAccuracy")
                roamFix(11.5, 1000, 12)
                verify(accuracy.text.indexOf("about 12\u00a0m") >= 0, accuracy.text)
                verify(accuracy.Accessible.name.indexOf("about 25\u00a0m") >= 0, "spoken to a band: " + accuracy.Accessible.name)
                var spoken = accuracy.Accessible.name
                roamFix(11.5, 1000, 14)
                verify(accuracy.text.indexOf("about 14\u00a0m") >= 0, accuracy.text)
                compare(accuracy.Accessible.name, spoken, "a move within the band is not spoken")

                sheet.useLocation()
                decoderHost.locationResult(sheet.locationId, true, 11.5, 20.0, 10, Date.now(), false, "", "", "")
                compare(sheet.rows, [valleyRow(2), valleyRow(1), valleyRow(0)], "asked for: the order of now")
            } finally {
                roamingOff()
            }
        }

        // A sheet opened before any fix shows the rows unranked; the first fix ranks them once, and from then on the
        // order is kept like any other.
        function test_first_fix_ranks_an_unranked_order() {
            roamingOn()
            try {
                runSite(valley[0])
                sheet.openFor(valleyRow(0))
                compare(sheet.rows, [valleyRow(0), valleyRow(1), valleyRow(2)], "no fix: row order")
                roamFix(11.5, 1000)
                compare(sheet.rows, [valleyRow(2), valleyRow(1), valleyRow(0)], "the first fix ranks them")
                roamFix(10.0, 1000)
                compare(sheet.rows, [valleyRow(2), valleyRow(1), valleyRow(0)], "then they are kept")
                compare(line("siteTitle" + valleyRow(2)).text, "Ridge · 165.9 km")
            } finally {
                roamingOff()
            }
        }

        // A sibling with options of its own is never a roaming destination: while roaming is on, its row says so.
        function test_roaming_extra_options_sibling() {
            roamingOn()
            try {
                savedSystems.update(valleyRow(1), {extraArgs: "-Z"})
                sheet.openFor(valleyRow(0))
                var note = line("siteRoamingExtraArgs" + valleyRow(1))
                verify(note.visible)
                compare(note.text, "Not used by GPS roaming: this site has extra options")
                verify(!line("siteRoamingExtraArgs" + valleyRow(0)).visible)
                prefs.roamingEnabled = false
                verify(!note.visible, "only while roaming is on")
            } finally {
                roamingOff()
            }
        }
        function test_avoid_toggle() {
            sheet.setAvoid(1, true)
            verify(savedSystems.get(1).avoidSite)
            sheet.choose(1); compare(starts.count, 0)
            sheet.setAvoid(1, false); sheet.choose(1); compare(starts.count, 1)
        }
    }
}
