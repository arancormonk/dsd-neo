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

    // The refresh entry point lives in the imports library, not on the screen
    // above, so it needs the library loaded to be reachable at all.
    Loader {
        id: importsLoader

        anchors.fill: parent
        active: false
        source: uiDir + "/ImportsScreen.qml"
    }

    // The stored-credentials rows live in Settings; the key row's gate is what
    // the last case below pins.
    Loader {
        id: settingsLoader

        anchors.fill: parent
        active: false
        source: uiDir + "/SettingsScreen.qml"
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
            testContext.setRadioReference("buildHasAppKey", false)
            testContext.setRadioReference("credentialsReady", false)
            testContext.setRadioReference("conventional", false)
            testContext.setRadioReference("busy", false)
            testContext.setRadioReference("sites", [])
            testContext.setRadioReference("systems", [])
            testContext.setRadioReference("countries", [])
            testContext.setRadioReference("states", [])
            testContext.setRadioReference("counties", [])
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

        // While a request runs, the overlay goes up and the actions that would
        // start another one go inert. The gate is on those buttons and not on a
        // container: `enabled` is hierarchical, so binding it on an ancestor
        // fights the buttons' own `enabled` bindings and Qt reports a binding
        // loop — on device only, never in this offscreen suite. That shipped
        // once; this case is here so the gate cannot quietly move back up.
        function test_05_a_running_request_makes_the_actions_inert() {
            testContext.setRadioReference("hasAppKey", true)
            testContext.setRadioReference("credentialsReady", true)
            tc.screen.zipText = "52401"

            var find = findChild(tc.screen, "radioReferenceZipGo")
            verify(find !== null, "the zip Find button is missing")
            tryVerify(function () { return find.enabled },
                      2000, "Find was inert with a zip typed and nothing running")

            testContext.setRadioReference("busy", true)
            var overlay = findChild(tc.screen, "radioReferenceBusyOverlay")
            verify(overlay !== null, "the busy overlay is missing")
            tryVerify(function () { return overlay.visible },
                      2000, "no busy overlay while a request was running")
            verify(!find.enabled, "Find could start a second request while one was running")

            // The container itself must NOT carry the gate — that is the shape
            // that loops.
            var sourcePanel = findChild(tc.screen, "radioReferenceSourcePanel")
            verify(sourcePanel.enabled, "the busy gate moved back onto a container")

            var cancel = findChild(tc.screen, "radioReferenceCancelButton")
            verify(cancel !== null, "the cancel button is missing")
            verify(cancel.enabled, "the cancel button was inert, leaving no way out of a slow request")

            testContext.setRadioReference("busy", false)
            tryVerify(function () { return find.enabled },
                      2000, "Find stayed inert after the request finished")
            tc.screen.zipText = ""
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

        // The three browse levels are one component now, so a break in it breaks
        // country, state and county at once — and the highlight is the only
        // thing on the sheet that says which one you are already on.
        function test_07_a_browse_sheet_lists_its_level_and_reports_the_choice() {
            testContext.setRadioReference("hasAppKey", true)
            testContext.setRadioReference("credentialsReady", true)
            testContext.setRadioReference("countries", [
                                              { "coid": 1, "name": "United States", "code": "US" },
                                              { "coid": 2, "name": "Canada", "code": "CA" }
                                          ])

            var sheet = findChild(tc.screen, "radioReferenceCountrySheet")
            verify(sheet !== null, "the country sheet is missing")
            compare(sheet.rowCount, 2, "the country sheet did not take the country list")
            // The screen defaults to the United States, so that is the row the
            // sheet must come up pointing at.
            compare(sheet.selectedId, 1, "the sheet did not follow the chosen country")

            sheet.chosen({ "coid": 2, "name": "Canada", "code": "CA" })
            compare(tc.screen.browseCoid, 2, "choosing a country did not move the selection")
            compare(sheet.selectedId, 2, "the highlight did not follow the new choice")
            verify(!sheet.visible, "the country sheet stayed up after a choice")

            // Empty is the pre-arrival state, which is what the notice reports.
            testContext.setRadioReference("counties", [])
            var county = findChild(tc.screen, "radioReferenceCountySheet")
            verify(county !== null, "the county sheet is missing")
            compare(county.rowCount, 0, "an unfetched level did not read as empty")
        }

        // A build that bakes the application key in never asks for one: the
        // field would sit under the password box looking like a second secret,
        // and anything typed there becomes a stored override that breaks the
        // working key.
        function test_08_a_keyed_build_never_asks_for_an_application_key() {
            testContext.setRadioReference("buildHasAppKey", true)

            var credentials = findChild(tc.screen, "radioReferenceCredentials")
            tryVerify(function () { return credentials.visible },
                      2000, "the credentials gate is missing for a keyed build")

            var appKey = findChild(tc.screen, "radioReferenceAppKeyField")
            verify(!appKey.visible, "a build with a baked key still asked for one")

            var username = findChild(tc.screen, "radioReferenceUsernameField")
            verify(username.visible, "the username field went missing with the key hidden")
            var password = findChild(tc.screen, "radioReferencePasswordField")
            verify(password.visible, "the password field went missing with the key hidden")
        }

        // The screen is a drill-down: find/results, or the loaded system —
        // never both. A loaded system swaps the search controls for a back row,
        // and closing it (simulated here by the model answering closeSystem()
        // with cleared readings) puts the results list back.
        function test_09_a_loaded_system_swaps_the_find_stage_for_a_back_row() {
            testContext.setRadioReference("credentialsReady", true)
            testContext.setRadioReference("systems", [
                                              { "sid": 6673, "name": "SARA Network", "city": "" },
                                              { "sid": 8734, "name": "ISICS", "city": "" }
                                          ])

            var sourcePanel = findChild(tc.screen, "radioReferenceSourcePanel")
            var systemList = findChild(tc.screen, "radioReferenceSystemList")
            var backRow = findChild(tc.screen, "radioReferenceBackToResults")
            verify(backRow !== null, "the back row is missing")
            tryVerify(function () { return systemList.visible },
                      2000, "the results list did not appear")
            verify(sourcePanel.visible, "the find panel went missing on the results stage")
            verify(!backRow.visible, "the back row appeared with nothing to go back from")

            testContext.setRadioReference("systemDetails", {
                                              "sid": 6673,
                                              "name": "SARA Network",
                                              "typeDescr": "Project 25",
                                              "flavorDescr": "Phase II"
                                          })
            tryVerify(function () { return !sourcePanel.visible },
                      2000, "the find panel stayed up over a loaded system")
            verify(!systemList.visible, "the results list stayed up over a loaded system")
            verify(backRow.visible, "a loaded system offered no way back")

            // The model keeps the results list across closeSystem(); with the
            // system gone the screen must land back on it.
            testContext.setRadioReference("systemDetails", {})
            testContext.setRadioReference("sites", [])
            tryVerify(function () { return systemList.visible },
                      2000, "closing the system did not bring the results back")
            verify(sourcePanel.visible, "closing the system did not bring the find panel back")
            verify(!backRow.visible, "the back row outlived the system it went back from")
        }

        // One trunked site is not a choice: the screen answers it, so the
        // preview and the Import button light up without a tap on the only row.
        // A conventional single repeater stays a question — "which repeaters
        // can I hear" has a real no-selection answer.
        function test_10_a_single_trunked_site_selects_itself() {
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
                                                "freqMhz": "851.0125", "colorCode": "" }
                                          ])
            tryVerify(function () { return tc.screen.selectedSites.length === 1 },
                      2000, "the only site of a trunked system was not selected")
            compare(tc.screen.selectedSites[0], 0, "the wrong site index was selected")

            // Two sites are a real choice, so nothing is answered for the user.
            testContext.setRadioReference("sites", [
                                              { "siteNumber": 1, "descr": "Alpha", "freqCount": 11,
                                                "controlFreqMhz": "851.0125", "simulcast": true,
                                                "freqMhz": "851.0125", "colorCode": "" },
                                              { "siteNumber": 2, "descr": "Bravo", "freqCount": 13,
                                                "controlFreqMhz": "852.5", "simulcast": false,
                                                "freqMhz": "852.5", "colorCode": "" }
                                          ])
            tryVerify(function () { return tc.screen.selectedSites.length === 0 },
                      2000, "a two-site system had a site answered for the user")

            testContext.setRadioReference("conventional", true)
            testContext.setRadioReference("sites", [
                                              { "siteNumber": 310011, "descr": "Zeta", "freqCount": 1,
                                                "controlFreqMhz": "", "simulcast": false,
                                                "freqMhz": "444.525", "colorCode": "1" }
                                          ])
            tryVerify(function () { return tc.screen.selectedSites.length === 0 },
                      2000, "a lone conventional repeater was selected uninvited")
        }

        // In a keyed build the user has no application key to fix, so an auth
        // failure must not send them hunting for one.
        function test_11_auth_wording_matches_what_this_build_asks_for() {
            var notice = findChild(tc.screen, "radioReferenceNotice")
            testContext.setRadioReference("errorText", "Invalid Username or Password")
            testContext.setRadioReference("errorIsAuth", true)

            testContext.setRadioReference("buildHasAppKey", true)
            tryVerify(function () { return notice.visible && notice.text.indexOf("application key") < 0 },
                      2000, "a keyed build blamed an application key the user cannot fix: " + notice.text)
            verify(notice.text.indexOf("username or password") >= 0,
                   "a keyed build's auth failure did not name the two real culprits: " + notice.text)

            testContext.setRadioReference("buildHasAppKey", false)
            tryVerify(function () { return notice.text.indexOf("application key") >= 0 },
                      2000, "a keyless build's auth failure stopped naming the key: " + notice.text)
        }

        // credentialsReady only says the fields are filled. When the account
        // check comes back rejected, the form must reopen — the password lives
        // nowhere but this screen, so a hidden form would leave no way to
        // retype it short of restarting the app.
        function test_12_a_rejected_account_reopens_the_credentials_form() {
            testContext.setRadioReference("credentialsReady", true)

            var credentials = findChild(tc.screen, "radioReferenceCredentials")
            tryVerify(function () { return !credentials.visible },
                      2000, "the form stayed up with the fields filled and no failure")

            testContext.setRadioReference("errorText", "Invalid Username or Password")
            testContext.setRadioReference("errorIsAuth", true)
            tryVerify(function () { return credentials.visible },
                      2000, "a rejected password left nowhere to retype it")

            testContext.setRadioReference("errorIsAuth", false)
            testContext.setRadioReference("errorIsSubscription", true)
            tryVerify(function () { return credentials.visible },
                      2000, "an expired subscription left nowhere to switch accounts")

            testContext.setRadioReference("errorIsSubscription", false)
            testContext.setRadioReference("errorText", "")
            tryVerify(function () { return !credentials.visible },
                      2000, "the form stayed up after the failure was resolved")
        }

    }

    // "Refresh from RadioReference" is offered on a row this app generated and on
    // nothing else — a picked file has no system to re-fetch. The gate reads the
    // row's own provenance through importedFiles.get(), because the delegate
    // declares only the six roles it renders and a role it never asked for comes
    // back `undefined`.
    TestCase {
        id: refreshCase

        name: "RadioReferenceRefreshEntryPoint"
        when: windowShown

        property var screen: null

        function initTestCase() {
            testContext.setRadioReference("available", true)
            importsLoader.active = true
            refreshCase.screen = importsLoader.item
            verify(refreshCase.screen !== null, "ImportsScreen.qml failed to load")

            // A generated row and a picked one, so the gate has both to answer.
            var generated = testContext.writeFixtureCsv(
                "rr_group.csv", "TG,Mode,Name\n1001,A,Dispatch\n")
            verify(generated.length > 0, "could not write the generated fixture")
            var adopted = importedFiles.importGeneratedFile(
                generated, "SARA group.csv", "group",
                { "origin": "radioreference", "rrSid": 6673,
                  "rrSiteIds": "16863", "rrKind": "group" })
            verify(adopted.ok, "the generated fixture did not import")

            var picked = testContext.writeFixtureCsv("picked.csv", "TG,Mode,Name\n2001,A,Works\n")
            verify(picked.length > 0, "could not write the picked fixture")
            verify(importedFiles.importFile(picked, "picked.csv", "group").ok,
                   "the picked fixture did not import")
        }

        function cleanupTestCase() {
            // The library is one persistent model shared by every case in this
            // suite; rows left behind would hand the next file a fixture it
            // never asked for.
            while (importedFiles.count > 0) {
                importedFiles.remove(0)
            }
            testContext.setRadioReference("available", false)
        }

        function test_01_refresh_is_offered_only_for_a_generated_row() {
            var sheet = findChild(refreshCase.screen, "importsActionSheet")
            verify(sheet !== null, "the action sheet is missing")
            var refresh = findChild(refreshCase.screen, "refreshFromRadioReferenceButton")
            verify(refresh !== null, "the refresh action is missing")

            refreshCase.screen.actionRow = 0
            sheet.visible = true
            tryVerify(function () { return refresh.visible },
                      2000, "a RadioReference-generated row was not offered a refresh")

            refreshCase.screen.actionRow = 1
            tryVerify(function () { return !refresh.visible },
                      2000, "a picked file was offered a refresh it cannot serve")

            sheet.visible = false
            refreshCase.screen.actionRow = -1
        }
    }

    // The Settings application-key row exists for builds that need a key from
    // the user. In a keyed build it hides — it sat right under Username reading
    // as a password box, and anything typed there becomes a stored override
    // that outranks the working baked key — except while an override IS stored:
    // the field is the only place to see and clear one.
    TestCase {
        id: settingsCase

        name: "SettingsRadioReferenceKeyRow"
        when: windowShown

        property var screen: null

        function initTestCase() {
            testContext.setRadioReference("available", true)
            settingsLoader.active = true
            settingsCase.screen = settingsLoader.item
            verify(settingsCase.screen !== null, "SettingsScreen.qml failed to load")
        }

        function cleanupTestCase() {
            testContext.setRadioReference("available", false)
            testContext.setRadioReference("buildHasAppKey", false)
            testContext.setPrefs("rrAppKey", "")
        }

        function test_01_the_key_row_follows_what_this_build_needs() {
            var row = findChild(settingsCase.screen, "settingsRrAppKeyRow")
            verify(row !== null, "the application-key row is missing")

            // A keyless build needs the user's key, so the row is offered.
            testContext.setRadioReference("buildHasAppKey", false)
            testContext.setPrefs("rrAppKey", "")
            tryVerify(function () { return row.visible },
                      2000, "a keyless build hid the application-key row")

            // A keyed build asks for nothing.
            testContext.setRadioReference("buildHasAppKey", true)
            tryVerify(function () { return !row.visible },
                      2000, "a keyed build still offered the application-key row")

            // ...unless an override is stored, which must stay visible to stay
            // clearable — and clearing it re-hides the row.
            testContext.setPrefs("rrAppKey", "OVERRIDE_KEY_1a2")
            tryVerify(function () { return row.visible },
                      2000, "a stored override was invisible and uneditable")
            testContext.setPrefs("rrAppKey", "")
            tryVerify(function () { return !row.visible },
                      2000, "clearing the override did not re-hide the row")
        }
    }
}
