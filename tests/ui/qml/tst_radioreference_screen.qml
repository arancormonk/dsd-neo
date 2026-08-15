// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest

// The RadioReference screen is a sequence of gates: no credentials, no search;
// no system, no site list; no site, no import. Each gate is a binding on a
// context-property reading, and a binding that reads a key the model does not
// publish evaluates to `undefined` rather than failing — so a gate that lost its
// reading stops gating silently and the screen offers an import that cannot
// work.
//
// `testContext.setRadioReference()` drives those readings, which is the only way
// to reach the later states from here: the suite registers `radioReference` as a
// map of readings with no invokables (see qml_test_context.h), so nothing in QML
// can call loadSystem() and have a system appear.
Item {
    id: root

    width: 420
    height: 900

    Loader {
        id: screenLoader

        anchors.fill: parent
        source: uiDir + "/RadioReferenceScreen.qml"
    }

    TestCase {
        id: tc

        name: "RadioReferenceScreen"
        when: windowShown

        property var screen: null

        function initTestCase() {
            tc.screen = screenLoader.item
            verify(tc.screen !== null, "RadioReferenceScreen.qml failed to load")
        }

        // Everything the cases below drive, back to what a fresh install shows.
        // The map is shared by the whole suite, so a case that left a system
        // loaded would hand the next one a screen it never set up.
        function init() {
            testContext.setRadioReference("hasAppKey", false)
            testContext.setRadioReference("credentialsReady", false)
            testContext.setRadioReference("conventional", false)
            testContext.setRadioReference("busy", false)
            testContext.setRadioReference("sites", [])
            testContext.setRadioReference("systems", [])
            testContext.setRadioReference("systemDetails", {})
            testContext.setRadioReference("talkgroupSummary", {})
            testContext.setRadioReference("errorText", "")
            testContext.setRadioReference("errorIsAuth", false)
            testContext.setRadioReference("errorIsSubscription", false)
            tc.screen.reset()
        }

        function test_01_a_fresh_install_asks_for_credentials_and_nothing_else() {
            var credentials = findChild(tc.screen, "radioReferenceCredentials")
            verify(credentials !== null, "the credentials panel is missing")
            tryVerify(function () { return credentials.visible },
                      2000, "a fresh install did not ask for credentials")

            var appKey = findChild(tc.screen, "radioReferenceAppKeyField")
            verify(appKey !== null, "the application-key field is missing")
            verify(appKey.visible, "a build with no application key did not ask for one")

            // Nothing further is offered until the account is answered: a search
            // would fail with an error the user cannot act on.
            var sourcePanel = findChild(tc.screen, "radioReferenceSourcePanel")
            verify(sourcePanel !== null, "the source panel is missing")
            verify(!sourcePanel.visible, "the search controls appeared before any credentials")

            var importButton = findChild(tc.screen, "radioReferenceImportButton")
            verify(importButton !== null, "the import button is missing")
            verify(!importButton.visible, "the import button appeared with no system loaded")
        }

        function test_02_answered_credentials_open_the_search_controls() {
            testContext.setRadioReference("hasAppKey", true)
            testContext.setRadioReference("credentialsReady", true)

            var credentials = findChild(tc.screen, "radioReferenceCredentials")
            tryVerify(function () { return !credentials.visible },
                      2000, "the credentials panel stayed up after they were answered")

            var sourcePanel = findChild(tc.screen, "radioReferenceSourcePanel")
            verify(sourcePanel.visible, "the search controls stayed hidden with credentials in hand")

            // The county cannot be browsed before a state is chosen — there is
            // no county list to ask for.
            var county = findChild(tc.screen, "radioReferenceCountyRow")
            verify(county !== null, "the county row is missing")
            verify(!county.enabled, "county browsing was offered before a state was chosen")
        }

        // A trunked system takes one site: a second tap moves the choice rather
        // than adding to it, because the generator uses only the first.
        function test_03_a_trunked_system_selects_one_site() {
            testContext.setRadioReference("hasAppKey", true)
            testContext.setRadioReference("credentialsReady", true)
            testContext.setRadioReference("systemDetails", {
                                              "sid": 6673,
                                              "name": "Test System",
                                              "typeDescr": "Project 25",
                                              "flavorDescr": "Phase II"
                                          })
            testContext.setRadioReference("sites", [
                                              { "siteNumber": 1, "descr": "Alpha", "freqCount": 11,
                                                "controlFreqMhz": "851.0125", "simulcast": true,
                                                "freqMhz": "851.0125", "colorCode": "" },
                                              { "siteNumber": 2, "descr": "Bravo", "freqCount": 13,
                                                "controlFreqMhz": "852.5", "simulcast": false,
                                                "freqMhz": "852.5", "colorCode": "" }
                                          ])

            var siteList = findChild(tc.screen, "radioReferenceSiteList")
            verify(siteList !== null, "the site list is missing")
            tryVerify(function () { return siteList.visible },
                      2000, "the site list stayed hidden for a loaded system")

            // The repeater count belongs to the conventional flow only; showing
            // it for a trunked system would promise a multi-select the generator
            // ignores.
            var count = findChild(tc.screen, "radioReferenceRepeaterCount")
            verify(count !== null, "the repeater count line is missing")
            verify(!count.visible, "a trunked system offered a repeater count")

            tc.screen.toggleSite(0)
            compare(tc.screen.selectedSites.length, 1)
            tc.screen.toggleSite(1)
            compare(tc.screen.selectedSites.length, 1, "a trunked system accepted a second site")
            compare(tc.screen.selectedSites[0], 1, "the second tap did not move the choice")

            // The simulcast toggle is pre-set from the chosen site's own record
            // and stays overridable. It is a P25 question, so it is offered here
            // and the EDACS one is not.
            var simulcast = findChild(tc.screen, "radioReferenceSimulcastRow")
            verify(simulcast !== null, "the simulcast row is missing")
            verify(simulcast.visible, "the simulcast toggle was hidden for a P25 system")
            compare(tc.screen.simulcast, false, "site 2 is not simulcast but the toggle claimed it was")
            tc.screen.toggleSite(0)
            compare(tc.screen.simulcast, true, "the toggle did not follow the site's own record")
            tc.screen.simulcastOverride = 0
            compare(tc.screen.simulcast, false, "the pre-set toggle could not be turned off")

            var esk = findChild(tc.screen, "radioReferenceEskRow")
            verify(esk !== null, "the ESK row is missing")
            verify(!esk.visible, "the EDACS-only ESK toggle appeared for a P25 system")
        }

        // A conventional networked system has no control channel: the unit of
        // choice is the repeater, and two or more of them make the scan list.
        function test_04_a_conventional_system_selects_several_repeaters() {
            testContext.setRadioReference("hasAppKey", true)
            testContext.setRadioReference("credentialsReady", true)
            testContext.setRadioReference("conventional", true)
            testContext.setRadioReference("systemDetails", {
                                              "sid": 9340,
                                              "name": "Users Group",
                                              "typeDescr": "DMR",
                                              "flavorDescr": "Conventional Networked"
                                          })
            testContext.setRadioReference("sites", [
                                              { "siteNumber": 310011, "descr": "Zeta", "freqCount": 1,
                                                "controlFreqMhz": "", "simulcast": false,
                                                "freqMhz": "444.525", "colorCode": "1" },
                                              { "siteNumber": 310012, "descr": "Alpha", "freqCount": 1,
                                                "controlFreqMhz": "", "simulcast": false,
                                                "freqMhz": "441.9875", "colorCode": "2" }
                                          ])

            var count = findChild(tc.screen, "radioReferenceRepeaterCount")
            tryVerify(function () { return count.visible },
                      2000, "a conventional system did not show the repeater count")
            verify(count.text.indexOf("26") >= 0,
                   "the repeater count does not name the 26-entry ceiling: " + count.text)

            tc.screen.toggleSite(0)
            tc.screen.toggleSite(1)
            compare(tc.screen.selectedSites.length, 2, "a conventional system refused a second repeater")
            tc.screen.toggleSite(0)
            compare(tc.screen.selectedSites.length, 1, "tapping a chosen repeater did not deselect it")

            // Database order is neither alphabetical nor geographic, so the list
            // is sorted by name — but every row keeps the index buildImportPlan()
            // takes, or the wrong repeater would be imported.
            compare(tc.screen.siteRows.length, 2)
            compare(tc.screen.siteRows[0].site.descr, "Alpha", "the repeater list is not sorted by name")
            compare(tc.screen.siteRows[0].index, 1, "sorting lost the row's index into sites()")
        }

        // Covering a layer is not what stops a tap reaching it — TapHandlers take
        // no exclusive grab — so the body says so itself while a request runs.
        function test_05_a_running_request_makes_the_screen_inert() {
            testContext.setRadioReference("hasAppKey", true)
            testContext.setRadioReference("credentialsReady", true)

            var sourcePanel = findChild(tc.screen, "radioReferenceSourcePanel")
            tryVerify(function () { return sourcePanel.enabled },
                      2000, "the screen was inert with nothing running")

            testContext.setRadioReference("busy", true)
            var overlay = findChild(tc.screen, "radioReferenceBusyOverlay")
            verify(overlay !== null, "the busy overlay is missing")
            tryVerify(function () { return overlay.visible },
                      2000, "no busy overlay while a request was running")
            verify(!sourcePanel.enabled, "a tap could still reach the screen under the busy overlay")

            var cancel = findChild(tc.screen, "radioReferenceCancelButton")
            verify(cancel !== null, "the cancel button is missing")
            verify(cancel.enabled, "the cancel button was inert, leaving no way out of a slow request")

            testContext.setRadioReference("busy", false)
            tryVerify(function () { return sourcePanel.enabled },
                      2000, "the screen stayed inert after the request finished")
        }

        // Auth and subscription failures are the two the user can actually act
        // on, and both need wording the server's own text does not give.
        function test_06_the_two_actionable_failures_get_their_own_wording() {
            var notice = findChild(tc.screen, "radioReferenceNotice")
            verify(notice !== null, "the notice line is missing")
            tryVerify(function () { return !notice.visible },
                      2000, "the notice line showed with nothing to report")

            testContext.setRadioReference("errorText", "Invalid Username or Password")
            testContext.setRadioReference("errorIsAuth", true)
            tryVerify(function () { return notice.visible },
                      2000, "an auth failure was not reported")
            verify(notice.text.indexOf("application key") >= 0,
                   "an auth failure did not name what to check: " + notice.text)

            testContext.setRadioReference("errorIsAuth", false)
            testContext.setRadioReference("errorIsSubscription", true)
            tryVerify(function () { return notice.text.indexOf("premium") >= 0 },
                      2000, "an expired subscription was not named as such: " + notice.text)
        }
    }
}
