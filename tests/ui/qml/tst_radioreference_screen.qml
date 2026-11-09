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
// property-map recorder with only lookupNearby (see qml_test_context.h), so QML
// cannot call loadSystem() and have a system appear.
Item {
    id: root

    width: 420
    height: 900

    FakeRadioReference {
        id: fakeAccount
    }

    SignalSpy {
        id: accountCommit
        signalName: "editingFinished"
    }

    Loader {
        id: screenLoader

        anchors.fill: parent
        source: uiDir + "/RadioReferenceScreen.qml"
    }

    SignalSpy {
        id: closedSpy
        target: screenLoader.item
        signalName: "closed"
    }

    // The refresh entry point lives in the imports library, not on the screen
    // above, so it needs the library loaded to be reachable at all.
    Loader {
        id: importsLoader

        anchors.fill: parent
        active: false
        source: uiDir + "/ImportsScreen.qml"
    }

    // Settings pushes the stored-credentials editor; the key row's gate is what
    // the last case below pins.
    Loader {
        id: settingsLoader

        anchors.fill: parent
        active: false
        source: uiDir + "/RadioReferenceAccountScreen.qml"
    }

    TestCase {
        id: tc

        name: "RadioReferenceScreen"
        when: windowShown

        property var screen: null
        property string previousRrUsername: ""

        function test_location_button_supported_and_busy() {
            testContext.setRadioReference("credentialsReady", true)
            testContext.setLocationSupported(true)
            var button = findChild(tc.screen, "radioReferenceNearby")
            verify(button !== null)
            tryVerify(function () { return button.visible && button.enabled })
            verify(button.width > 0, "nearby button needs a usable tap width")
            verify(button.height > 0, "nearby button needs a usable tap height")
            // The other test cases load Settings and Imports in sibling loaders.
            // Raise this screen and let its layout settle before delivering input.
            screenLoader.z = 1
            waitForRendering(button)
            var before = testContext.radioReferenceNearbyCalls()
            mouseClick(button, button.width / 2, button.height / 2)
            screenLoader.z = 0
            compare(testContext.radioReferenceNearbyCalls(), before + 1)
            testContext.setRadioReference("busy", true)
            tryVerify(function () { return !button.enabled })
            testContext.setLocationSupported(false)
        }

        function test_location_button_desktop_hidden() {
            var button = findChild(tc.screen, "radioReferenceNearby")
            verify(button !== null)
            verify(!button.visible)
        }

        function loadTwoSiteSystem() {
            testContext.setRadioReference("credentialsReady", true)
            testContext.setRadioReference("systemDetails", {
                                              "sid": 6673,
                                              "name": "Test System",
                                              "typeDescr": "Project 25",
                                              "flavorDescr": "Phase II"
                                          })
            testContext.setRadioReference("sites", [
                                              { "siteNumber": 1, "descr": "Alpha", "freqCount": 11,
                                                "controlFreqMhz": "851.0125", "simulcast": false,
                                                "freqMhz": "851.0125", "colorCode": "" },
                                              { "siteNumber": 2, "descr": "Bravo", "freqCount": 13,
                                                "controlFreqMhz": "852.5", "simulcast": false,
                                                "freqMhz": "852.5", "colorCode": "" }
                                          ])
            tryVerify(function () { return tc.screen.systemLoaded })
        }
        function leaveQuestion() {
            return findChild(tc.screen, "radioReferenceLeaveDiscard")
        }

        // Issue #718: the site selection and import options live on this screen
        // and start over when the system is opened again, so leaving with a
        // selection asks first. Untouched, every exit is immediate.
        function test_15_leaving_a_selection_asks_data() {
            return [
                { tag: "header back", exit: "header" },
                { tag: "All systems row", exit: "results" },
                { tag: "import option", exit: "header", option: true }
            ]
        }
        function test_15_leaving_a_selection_asks(data) {
            loadTwoSiteSystem()
            closedSpy.clear()
            var question = leaveQuestion()
            verify(question !== null)
            var leave = function () {
                if (data.exit === "header")
                    findChild(tc.screen, "radioReferenceBack").activate()
                else
                    findChild(tc.screen, "radioReferenceBackToResults").goBack()
            }
            leave()
            verify(!question.visible, "an untouched system leaves without asking")
            if (data.exit === "header")
                compare(closedSpy.count, 1)
            closedSpy.clear()
            loadTwoSiteSystem()
            if (data.option)
                tc.screen.encryptionPolicy = 2
            else
                tc.screen.toggleSite(1)
            leave()
            verify(question.visible, "a selection is asked about before it goes")
            compare(closedSpy.count, 0)
            findChild(question, "discardDialogKeep").activate()
            verify(!question.visible)
            if (data.option)
                compare(tc.screen.encryptionPolicy, 2)
            else
                compare(tc.screen.selectedSites, [1])
            leave()
            verify(question.visible)
            findChild(question, "discardDialogDiscard").activate()
            verify(!question.visible)
            if (data.exit === "header")
                compare(closedSpy.count, 1, "Discard closes the screen")
        }
        // A single trunked site picks itself: that is not the user's selection.
        function test_16_a_site_picked_by_the_screen_is_not_an_edit() {
            testContext.setRadioReference("credentialsReady", true)
            testContext.setRadioReference("systemDetails", { "sid": 6673, "name": "One", "typeDescr": "Project 25" })
            testContext.setRadioReference("sites", [
                                              { "siteNumber": 1, "descr": "Alpha", "freqCount": 11,
                                                "controlFreqMhz": "851.0125", "simulcast": false,
                                                "freqMhz": "851.0125", "colorCode": "" }
                                          ])
            tryVerify(function () { return tc.screen.systemLoaded })
            compare(tc.screen.selectedSites, [0])
            closedSpy.clear()
            findChild(tc.screen, "radioReferenceBack").activate()
            verify(!leaveQuestion().visible)
            compare(closedSpy.count, 1)
        }

        // The review step is the last of an import: a tap beside it keeps it.
        function test_17_the_review_sheet_keeps_on_a_side_tap() {
            loadTwoSiteSystem()
            var review = findChild(tc.screen, "radioReferenceReviewSheet")
            verify(review !== null)
            screenLoader.z = 1
            review.visible = true
            waitForRendering(review)
            verify(!review.hitsPanel(4, review.height / 2))
            mouseClick(review, 4, review.height / 2)
            wait(50)
            verify(review.visible, "a tap beside the review keeps it")
            review.visible = false
            screenLoader.z = 0
        }

        // Issue #803. A TETRA system and its import as buildImportPlan()
        // answers it: one trunked site, -fT, and here no talkgroups listed, so
        // nothing to write (17 of the 47 TETRA systems on RadioReference list
        // none).
        function loadTetraSystem(siteCount) {
            testContext.setRadioReference("credentialsReady", true)
            testContext.setRadioReference("systemDetails", {
                                              "sid": 10880,
                                              "name": "KorKor",
                                              "typeDescr": "TETRA",
                                              "flavorDescr": "Standard",
                                              "protocolToken": "tetra"
                                          })
            var sites = []
            for (var i = 0; i < siteCount; i++)
                sites.push({ "siteNumber": i + 1, "descr": "Cell " + (i + 1), "freqCount": 2,
                             "controlFreqMhz": "390.0125", "simulcast": false,
                             "freqMhz": "390.0125", "colorCode": "" })
            testContext.setRadioReference("sites", sites)
            tryVerify(function () { return tc.screen.systemLoaded })
        }
        function reviewBody() {
            var body = findChild(tc.screen, "radioReferenceReviewBody")
            verify(body !== null, "the review text has no name to find it by")
            return body
        }
        function openReview() {
            var review = findChild(tc.screen, "radioReferenceReviewSheet")
            screenLoader.z = 1
            review.visible = true
            waitForRendering(review)
            return review
        }
        function closeReview(review) {
            review.visible = false
            screenLoader.z = 0
        }

        // New feature (#803): a lone TETRA cell picks itself like any trunked
        // site, and the preview and the review both say what the import needs
        // to decode, before and at the last step. The note is read by its own
        // words, so TalkBack reaches it. Issue #806: every radio decodes TETRA
        // at 72 kHz, an Airspy's included, so the note no longer turns an
        // Airspy away.
        function test_18_a_tetra_import_says_what_it_needs() {
            loadTetraSystem(1)
            tryVerify(function () { return tc.screen.selectedSites.length === 1 },
                      2000, "the only cell of a TETRA system was not selected")
            tc.screen.plan = {ok: true, freqMhz: "390.0125", decodeFlag: "-fT", trunking: true,
                              scanList: false, warnings: []}
            var note = findChild(tc.screen, "radioReferenceTetraNote")
            verify(note !== null, "the TETRA note is missing")
            tryVerify(function () { return note.visible }, 2000, "the TETRA note is not shown for a TETRA system")
            compare(note.text, "TETRA needs a radio source at a 72 kHz bandwidth (leave the bandwidth empty for that). Some systems listed as TETRA actually run Tetrapol, which dsd-neo can't decode.")
            verify(note.text.indexOf("Airspy") < 0, "the TETRA note still turns an Airspy away: " + note.text)
            compare(note.Accessible.name, note.text, "the TETRA note is not read by its own words")

            var review = openReview()
            var body = reviewBody()
            verify(body.text.indexOf(note.text) >= 0, "the review does not repeat the TETRA note: " + body.text)
            compare(body.Accessible.name, body.text)
            closeReview(review)

            // Any other system says nothing about TETRA.
            loadTwoSiteSystem()
            tryVerify(function () { return !note.visible }, 2000, "a P25 system shows the TETRA note")
            tc.screen.toggleSite(0)
            tc.screen.plan = {ok: true, freqMhz: "851.0125", decodeFlag: "-f1", trunking: true,
                              scanList: false, warnings: []}
            review = openReview()
            verify(body.text.indexOf("TETRA") < 0, "a P25 review mentions TETRA: " + body.text)
            closeReview(review)
        }

        // Regression (#803 D-B4): an import that writes no file said
        // "Generated files:" over an empty list. Reachable on main with a lone
        // conventional repeater on a system without talkgroups, and now with
        // any TETRA system without them. The preview says what a trunked
        // import with no files does instead of "simply tunes".
        function test_19_an_import_without_files_says_so() {
            loadTetraSystem(1)
            tryVerify(function () { return tc.screen.selectedSites.length === 1 })
            tc.screen.plan = {ok: true, freqMhz: "390.0125", decodeFlag: "-fT", trunking: true,
                              scanList: false, warnings: []}
            var files = findChild(tc.screen, "radioReferencePlanFiles")
            verify(files !== null && files.visible)
            verify(files.text.indexOf("follows calls from this control channel") >= 0,
                   "a trunked import without files reads as a plain tune: " + files.text)
            var review = openReview()
            var body = reviewBody()
            verify(body.text.indexOf("Generated files:") < 0, "the review lists generated files that do not exist: " + body.text)
            verify(body.text.indexOf("No files are generated.") >= 0, "the review does not say no file is written: " + body.text)
            closeReview(review)

            tc.screen.plan = {ok: true, freqMhz: "390.0125", decodeFlag: "-fT", trunking: true, scanList: false,
                              warnings: [], groupCsvText: "Decimal,Mode,Alpha Tag\n170001,A,Ops\n"}
            review = openReview()
            verify(body.text.indexOf("Generated files:\nTalkgroup list") >= 0, "a written file is not listed: " + body.text)
            verify(body.text.indexOf("No files are generated.") < 0)
            closeReview(review)
            verify(files.text.indexOf("talkgroup list") >= 0)

            // A conventional repeater has no control channel: it still just tunes.
            testContext.setRadioReference("conventional", true)
            testContext.setRadioReference("trunked", false)
            tc.screen.plan = {ok: true, freqMhz: "444.525", decodeFlag: "-ft", trunking: false,
                              scanList: false, warnings: []}
            verify(files.text.indexOf("simply tunes this frequency") >= 0, files.text)
        }

        // Regression (#803 D-B5): the scanning sentence named only a USB
        // RTL-SDR and rtl_tcp, but -Y scans on an Airspy too, as the shared
        // import warning shown beside it says. The warning is the generator's
        // own string (rr_generate.c), as buildImportPlan() hands it over.
        function test_20_the_scanning_note_names_every_radio_that_scans() {
            var scanWarning = "Scanning across repeaters needs an RTL-SDR, rtl_tcp, SoapySDR or Airspy input, or a "
                    + "rigctl-controlled radio; on any other input the session stays on the first frequency."
            testContext.setRadioReference("conventional", true)
            testContext.setRadioReference("trunked", false)
            loadTwoSiteSystem()
            tc.screen.toggleSite(0)
            tc.screen.toggleSite(1)
            tc.screen.plan = {ok: true, freqMhz: "851.0125", decodeFlag: "-ft -Y", trunking: false,
                              scanList: true, warnings: [scanWarning]}
            var warning = null
            tryVerify(function () {
                warning = findChild(tc.screen, "radioReferencePlanWarning")
                return warning !== null && warning.visible
            }, 2000, "the preview does not show the scan warning")
            compare(warning.text, "• " + scanWarning)
            var review = openReview()
            var body = reviewBody()
            for (var radio of ["USB RTL-SDR", "rtl_tcp", "Airspy"])
                verify(body.text.indexOf(radio) >= 0, "the scanning note does not name " + radio + ": " + body.text)
            // The note and the warning beside it agree on the Airspy.
            verify(warning.text.indexOf("Airspy") >= 0 && body.text.indexOf("Airspy") >= 0)
            closeReview(review)
        }

        // Regression (#803 D-B6): the custom band plan advice sends the user
        // to a P25 band plan CSV, which only a P25 system reads.
        function test_21_the_band_plan_advice_is_for_p25_only() {
            var bandplan = findChild(tc.screen, "radioReferenceBandplanNote")
            verify(bandplan !== null, "the band plan note has no name to find it by")
            testContext.setRadioReference("credentialsReady", true)
            testContext.setRadioReference("systemDetails", {
                                              "sid": 6673, "name": "Test System", "typeDescr": "Project 25",
                                              "flavorDescr": "Phase II", "protocolToken": "p25",
                                              "hasCustomBandplan": true })
            tryVerify(function () { return bandplan.visible }, 2000, "a P25 system lost its band plan advice")
            // The same band plan flag on a TETRA system: the note goes.
            loadTetraSystem(1)
            var details = Object.assign({}, radioReference.systemDetails)
            details.hasCustomBandplan = true
            testContext.setRadioReference("systemDetails", details)
            tryVerify(function () { return !bandplan.visible }, 2000, "a TETRA system was told to import a P25 band plan")
        }

        // The per-site button and the conventional notes under the search
        // field are spaced and inset like the card's other controls: the
        // button ran full card width, flush against the field, and read as a
        // site row rather than a control.
        function test_22_the_controls_under_the_site_search_are_spaced() {
            loadTetraSystem(2)
            var search = findChild(tc.screen, "radioReferenceSiteSearch")
            var each = findChild(tc.screen, "radioReferenceEachSiteButton")
            verify(search !== null && each !== null)
            tryVerify(function () { return each.visible })
            verify(each.y >= search.y + search.height + 10,
                   "the per-site button touches the search field: " + each.y + " < " + (search.y + search.height + 10))
            compare(each.x, search.x, "the per-site button is not inset like the search field")
            compare(each.width, search.width, "the per-site button is not as wide as the search field")
            // A site row is the card's full width; its divider stays inside the
            // card's content padding, edge to edge with the search field.
            var divider = findChild(tc.screen, "radioReferenceSiteDivider")
            verify(divider !== null, "the site divider has no name to find it by")
            var left = divider.mapToItem(search.parent, 0, 0).x
            var right = divider.mapToItem(search.parent, divider.width, 0).x
            compare(left, search.x, "the site divider does not start at the content padding")
            verify(right <= search.x + search.width,
                   "the site divider runs past the content padding: " + right + " > " + (search.x + search.width))

            testContext.setRadioReference("conventional", true)
            testContext.setRadioReference("trunked", false)
            loadTwoSiteSystem()
            var count = findChild(tc.screen, "radioReferenceRepeaterCount")
            tryVerify(function () { return count.visible && !each.visible })
            verify(count.y >= search.y + search.height + 10, "the repeater count touches the search field")
        }

        function initTestCase() {
            tc.screen = screenLoader.item
            verify(tc.screen !== null, "RadioReferenceScreen.qml failed to load")
        }

        // Everything the cases below drive, back to what a fresh install shows.
        // The map is shared by the whole suite, so a case that left a system
        // loaded would hand the next one a screen it never set up.
        function init() {
            previousRrUsername = prefs.rrUsername;
            tc.screen.eachSite = false
            testContext.setLocationSupported(false)
            testContext.setRadioReference("hasAppKey", false)
            testContext.setRadioReference("buildHasAppKey", false)
            testContext.setRadioReference("credentialsReady", false)
            // Set independently, because the real model does: a system type this
            // build cannot import answers false to BOTH, so neither reading can
            // be derived from the other. The pair below is an ordinary trunked
            // system, which is what most cases here load.
            testContext.setRadioReference("conventional", false)
            testContext.setRadioReference("trunked", true)
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
            // The other shared map. The Settings case below stores a key
            // override, and the auth wording here reads prefs.rrAppKey — which
            // would make this case's result depend on TestCase running order.
            testContext.setPrefs("rrAppKey", "")
            tc.screen.reset()
            // Choosing a country opens the next browse sheet; each test starts with none open.
            for (var sheetName of ["radioReferenceCountrySheet", "radioReferenceStateSheet", "radioReferenceCountySheet"])
                findChild(tc.screen, sheetName).visible = false
        }

        function cleanup() {
            accountCommit.target = null;
            // A failed review case must not leave its sheet over the next case's taps.
            findChild(tc.screen, "radioReferenceReviewSheet").visible = false;
            tc.screen.visible = true;
            findChild(tc.screen, "radioReferencePasswordField").text = "";
            testContext.useLifecycleHost(false);
            testContext.useRadioReferenceModel(null);
            testContext.setPrefs("rrUsername", previousRrUsername);
            screenLoader.z = 0;
            settingsLoader.active = false;
            importsLoader.active = false;
        }

        function test_login_keeps_account_editor_reachable() {
            fakeAccount.resetAccount();
            testContext.setPrefs("rrUsername", "test-listener");
            testContext.useRadioReferenceModel(fakeAccount);
            screenLoader.z = 1;
            var password = findChild(tc.screen, "radioReferencePasswordField");
            password.text = "session-secret";
            password.editingFinished();
            compare(fakeAccount.sessionPassword, "session-secret");
            compare(fakeAccount.accountChecks, 1, "login must check the account once");
            var credentials = findChild(tc.screen, "radioReferenceCredentials");
            // Up through the check, so a password manager sees the fields until the verdict.
            verify(credentials.visible, "the form hid before the account check settled");
            fakeAccount.settle(2);
            tryCompare(credentials, "visible", false);
            var change = findChild(tc.screen, "radioReferenceChangeAccount");
            verify(change !== null, "successful login must leave a Change account action");
            verify(change.visible);
            verify(change.title.indexOf("test-listener") >= 0);
            change.activate();
            tryCompare(credentials, "visible", true);
            verify(findChild(tc.screen, "radioReferenceUsernameField").visible);
            password.text = "replacement-secret";
            password.editingFinished();
            compare(fakeAccount.accountChecks, 2, "a new password checks the account again");
            fakeAccount.settle(2);
            var checks = fakeAccount.accountChecks;
            findChild(tc.screen, "radioReferenceCheckAccountButton").clicked();
            compare(fakeAccount.accountChecks, checks + 1, "Check account re-runs a finished check");
            compare(fakeAccount.sessionPassword, "replacement-secret");
            fakeAccount.settle(2);
            var username = findChild(tc.screen, "radioReferenceUsernameField");
            accountCommit.target = username;
            accountCommit.clear();
            username.input.forceActiveFocus();
            verify(username.input.activeFocus);
            change.activate();
            tryCompare(credentials, "visible", false);
            compare(accountCommit.count, 1, "Done must commit the field being edited");
            verify(!username.input.activeFocus, "Done must release editor focus");
            compare(fakeAccount.accountChecks, checks + 1, "Done over checked, unchanged credentials asks nothing");
            fakeAccount.settle(4);
            tryCompare(credentials, "visible", true);
            verify(findChild(tc.screen, "radioReferenceNotice").text.indexOf("expired") >= 0);
            fakeAccount.settle(0);
            password.text = "";
        }

        function test_account_error_keeps_done_label_data() {
            return [
                {tag: "auth", error: "errorIsAuth"},
                {tag: "expired", error: "errorIsSubscription"}
            ];
        }

        function test_account_error_keeps_done_label(data) {
            testContext.setPrefs("rrUsername", "test-listener");
            testContext.setRadioReference("credentialsReady", true);
            var credentials = findChild(tc.screen, "radioReferenceCredentials");
            var change = findChild(tc.screen, "radioReferenceChangeAccount");
            verify(change !== null && change.visible);
            compare(change.title, "Account: test-listener · Change");
            testContext.setRadioReference(data.error, true);
            tryCompare(credentials, "visible", true);
            compare(change.title, "Account: test-listener · Done");
            var username = findChild(tc.screen, "radioReferenceUsernameField");
            accountCommit.target = username;
            accountCommit.clear();
            username.input.forceActiveFocus();
            verify(username.input.activeFocus);
            change.activate();
            compare(accountCommit.count, 1, "Done must commit even while an account error holds the form open");
            verify(!username.input.activeFocus);
            verify(credentials.visible, "the error must keep its correction fields available");
            compare(change.title, "Account: test-listener · Done");
            change.activate();
            verify(credentials.visible);
            compare(change.title, "Account: test-listener · Done");
            testContext.setRadioReference(data.error, false);
            tryCompare(credentials, "visible", false);
            compare(change.title, "Account: test-listener · Change");
            change.activate();
            tryCompare(credentials, "visible", true);
            compare(change.title, "Account: test-listener · Done");
            change.activate();
            tryCompare(credentials, "visible", false);
            compare(change.title, "Account: test-listener · Change");
        }

        function test_fresh_visit_resets_filters_and_multi_site_mode() {
            tc.screen.siteSearch = "old site";
            tc.screen.eachSite = true;
            var sheets = ["radioReferenceCountrySheet", "radioReferenceStateSheet", "radioReferenceCountySheet"];
            for (var name of sheets) {
                var search = findChild(findChild(tc.screen, name), "browseSearch");
                verify(search !== null, name + " search");
                search.text = "old place";
            }
            tc.screen.reset();
            compare(tc.screen.siteSearch, "");
            compare(tc.screen.eachSite, false);
            for (var name of sheets)
                compare(findChild(findChild(tc.screen, name), "browseSearch").text, "");
        }

        // The zip code or system ID typed on one visit is there on the next:
        // leaving the screen (header Back, system Back, a keyboard Back) never
        // asks to retype it. A loaded system still goes.
        function test_typed_queries_survive_a_new_visit_data() {
            return [{tag: "zip", mode: 0}, {tag: "system id", mode: 2}];
        }
        function test_typed_queries_survive_a_new_visit(data) {
            tc.screen.sourceMode = data.mode;
            tc.screen.zipText = "52401";
            tc.screen.sidText = "6673";
            tc.screen.reset();
            compare(tc.screen.zipText, "52401");
            compare(tc.screen.sidText, "6673");
            compare(tc.screen.sourceMode, data.mode, "the way in the query belongs to stays");
            // Browse picks are not typed, and they restart with the visit.
            tc.screen.sourceMode = 1;
            tc.screen.reset();
            compare(tc.screen.sourceMode, 0);
            compare(tc.screen.zipText, "52401");
            tc.screen.zipText = "";
            tc.screen.sidText = "";
        }

        function test_browse_empty_state_data() {
            return [
                {tag: "country", sheet: "radioReferenceCountrySheet", key: "countries", idKey: "coid"},
                {tag: "state", sheet: "radioReferenceStateSheet", key: "states", idKey: "stid"},
                {tag: "county", sheet: "radioReferenceCountySheet", key: "counties", idKey: "ctid"}
            ];
        }

        function test_browse_empty_state(data) {
            var sheet = findChild(tc.screen, data.sheet);
            sheet.visible = true;
            var notice = findChild(sheet, "browseEmptyState");
            verify(notice !== null);
            tryCompare(notice, "visible", true);
            compare(notice.text, "Nothing to show");
            testContext.setRadioReference("busy", true);
            compare(notice.text, "Loading…");
            testContext.setRadioReference("busy", false);
            compare(notice.text, "Nothing to show");
            var row = {name: "Alpha"};
            row[data.idKey] = 1;
            testContext.setRadioReference(data.key, [row]);
            tryCompare(notice, "visible", false);
            var search = findChild(sheet, "browseSearch");
            search.text = "missing";
            tryCompare(notice, "visible", true);
            compare(notice.text, "No matches");
            testContext.setRadioReference("busy", true);
            compare(notice.text, "No matches", "an unrelated request must not hide a populated list's filter result");
            testContext.setRadioReference("busy", false);
            search.text = "alpha";
            tryCompare(notice, "visible", false);
            sheet.visible = false;
        }

        function test_browse_parent_choice_clears_dependent_filters_data() {
            return [
                {tag: "country", choose: "chooseCountry", row: {coid: 2, name: "Canada"},
                 sheets: ["radioReferenceStateSheet", "radioReferenceCountySheet"]},
                {tag: "state", choose: "chooseState", row: {stid: 1, name: "Alabama"},
                 sheets: ["radioReferenceCountySheet"]}
            ];
        }

        function test_browse_parent_choice_clears_dependent_filters(data) {
            for (var name of data.sheets)
                findChild(findChild(tc.screen, name), "browseSearch").text = "old place";
            tc.screen[data.choose](data.row);
            for (var name of data.sheets)
                compare(findChild(findChild(tc.screen, name), "browseSearch").text, "",
                        "choosing a parent must clear " + name + "'s old filter");
            verify(findChild(tc.screen, data.sheets[0]).visible);
        }

        function test_credential_labels_and_hint() {
            compare(findChild(tc.screen, "radioReferenceUsernameField").label, "RadioReference username");
            compare(findChild(tc.screen, "radioReferenceAppKeyField").label, "RadioReference application key");
            compare(findChild(tc.screen, "radioReferencePasswordField").label, "Password");
            compare(findChild(tc.screen, "radioReferencePasswordHint").text,
                    "DSD-neo keeps your password only until the app closes and never saves it. A RadioReference premium subscription is required.");
            var heading = findChild(tc.screen, "radioReferenceAppKeyHeading");
            verify(heading.visible);
            compare(heading.text, "Application key");
            testContext.setRadioReference("buildHasAppKey", true);
            tryCompare(heading, "visible", false);
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
        function test_per_site_operation_allows_multiple_trunked_sites() {
            tc.screen.eachSite = true
            tc.screen.toggleSite(0)
            tc.screen.toggleSite(1)
            compare(tc.screen.selectedSites.length, 2)
            tc.screen.toggleSite(0)
            compare(tc.screen.selectedSites.length, 1)
            compare(tc.screen.selectedSites[0], 1)
        }

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
        // choice is the repeater, and two or more make one scanning system.
        function test_04_a_conventional_system_selects_several_repeaters() {
            testContext.setRadioReference("hasAppKey", true)
            testContext.setRadioReference("credentialsReady", true)
            testContext.setRadioReference("conventional", true)
            testContext.setRadioReference("trunked", false)
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
            // The count line reports the selection itself; the scan list grows
            // with every distinct repeater, so no ceiling is promised here.
            compare(count.text, "0 repeater(s) selected",
                    "the repeater count does not read the empty selection")

            tc.screen.toggleSite(0)
            tc.screen.toggleSite(1)
            compare(tc.screen.selectedSites.length, 2, "a conventional system refused a second repeater")
            compare(count.text, "2 repeater(s) selected", "the repeater count did not follow the selection")
            var hint = findChild(tc.screen, "radioReferenceRepeaterHint");
            verify(hint !== null && hint.visible);
            compare(hint.text, "One selected repeater tunes directly. Multiple selected repeaters are saved as one scanning system.");
            tc.screen.plan = {ok: true, scanList: true, freqMhz: "444.525", decodeFlag: "-ft"};
            var preview = findChild(tc.screen, "radioReferencePlanSummary");
            verify(preview !== null && preview.visible);
            verify(preview.text.indexOf("scanning system") >= 0);
            verify(preview.text.indexOf("scan list") < 0);
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
            compare(sheet.rows.length, 2, "the country sheet did not take the country list")
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
            compare(county.rows.length, 0, "an unfetched level did not read as empty")
        }

        // Issue #802 B12: RadioReference's own words (a place, a site, its
        // colour code) are drawn and measured as written, never as markup.
        function textShowing(item, value) {
            if (item.textFormat !== undefined && item.text === value && item.visible)
                return item
            var kids = item.children || []
            for (var i = 0; i < kids.length; ++i) {
                var found = textShowing(kids[i], value)
                if (found)
                    return found
            }
            return null
        }
        function verifyDrawnAsWritten(item, value, tag) {
            var text = null
            tryVerify(function () { text = textShowing(item, value); return text !== null }, 2000, tag + " is not shown")
            var mirror = Qt.createQmlObject("import QtQuick; Text { visible: false; textFormat: Text.PlainText }", root)
            mirror.font = text.font
            mirror.text = text.text
            var plainWidth = mirror.implicitWidth
            mirror.destroy()
            verify(Math.abs(text.implicitWidth - plainWidth) <= 0.5, tag + " draws as written: " + text.implicitWidth + " vs " + plainWidth)
            compare(text.textFormat, Text.PlainText, tag + " is plain text")
        }
        function test_07b_radioreference_names_draw_as_written() {
            testContext.setRadioReference("hasAppKey", true)
            testContext.setRadioReference("credentialsReady", true)
            testContext.setRadioReference("countries", [
                                              { "coid": 1, "name": "<b>United</b> States", "code": "US" }
                                          ])
            var sheet = findChild(tc.screen, "radioReferenceCountrySheet")
            sheet.visible = true
            verifyDrawnAsWritten(sheet, "<b>United</b> States", "a browse row")
            sheet.visible = false
            testContext.setRadioReference("conventional", true)
            testContext.setRadioReference("trunked", false)
            testContext.setRadioReference("systemDetails", {
                                              "sid": 9340,
                                              "name": "Users Group",
                                              "typeDescr": "DMR",
                                              "flavorDescr": "Conventional Networked"
                                          })
            testContext.setRadioReference("sites", [
                                              { "siteNumber": 310011, "descr": "<b>Zeta</b>", "freqCount": 1,
                                                "controlFreqMhz": "", "simulcast": false,
                                                "freqMhz": "444.525", "colorCode": "<i>1</i>" }
                                          ])
            verifyDrawnAsWritten(tc.screen, "<b>Zeta</b>", "a repeater's name")
            verifyDrawnAsWritten(tc.screen, "444.525 MHz · CC <i>1</i>", "a repeater's detail")
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
            var accountRow = findChild(tc.screen, "radioReferenceChangeAccount")
            verify(backRow !== null, "the back row is missing")
            verify(accountRow !== null && accountRow.visible, "the results stage must offer account editing")
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
            verify(!accountRow.visible, "the account row stayed up over a loaded system")

            // The model keeps the results list across closeSystem(); with the
            // system gone the screen must land back on it.
            testContext.setRadioReference("systemDetails", {})
            testContext.setRadioReference("sites", [])
            tryVerify(function () { return systemList.visible },
                      2000, "closing the system did not bring the results back")
            verify(sourcePanel.visible, "closing the system did not bring the find panel back")
            verify(accountRow.visible, "closing the system did not bring account editing back")
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
            testContext.setRadioReference("trunked", false)
            testContext.setRadioReference("sites", [
                                              { "siteNumber": 310011, "descr": "Zeta", "freqCount": 1,
                                                "controlFreqMhz": "", "simulcast": false,
                                                "freqMhz": "444.525", "colorCode": "1" }
                                          ])
            tryVerify(function () { return tc.screen.selectedSites.length === 0 },
                      2000, "a lone conventional repeater was selected uninvited")

            // The third state, and the reason this reads `trunked` rather than
            // `!conventional`: an LTR, MPT-1327 or Motorola Type II system has
            // no protocol row, so it is neither conventional NOR trunked.
            // buildImportPlan() refuses it outright, so selecting and lighting
            // up its one row promises an import that cannot happen.
            testContext.setRadioReference("conventional", false)
            testContext.setRadioReference("trunked", false)
            testContext.setRadioReference("sites", [
                                              { "siteNumber": 1, "descr": "Solo", "freqCount": 5,
                                                "controlFreqMhz": "851.0125", "simulcast": false,
                                                "freqMhz": "851.0125", "colorCode": "" }
                                          ])
            tryVerify(function () { return tc.screen.selectedSites.length === 0 },
                      2000, "an unimportable system had its only site selected for the user")
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

        // Nothing on this screen takes keyboard focus — every control is a
        // TapHandler — so a field only commits on Enter or when another field is
        // tapped. The button has to move focus itself, and it cannot be gated on
        // the committed state: that greys it out exactly when the last answer is
        // the one still sitting uncommitted in the field.
        function test_13_check_account_commits_the_field_still_being_typed_in() {
            var key = findChild(tc.screen, "radioReferenceAppKeyField")
            var button = findChild(tc.screen, "radioReferenceCheckAccountButton")
            tryVerify(function () { return key.visible && button.visible },
                      2000, "the keyless form did not offer a key field and a button")
            verify(button.enabled, "the button was dead with the form not yet committed")

            var commits = 0
            key.editingFinished.connect(function () { commits++ })
            key.input.forceActiveFocus()
            verify(key.input.activeFocus, "the key field did not take focus")

            // clicked() rather than a synthetic tap: OutlineButton's TapHandler
            // is covered where the component is, and what matters here is what
            // the screen's handler does once it fires.
            button.clicked()
            tryVerify(function () { return commits === 1 },
                      2000, "tapping Check account left the typed key uncommitted")
        }

        // A keyed build's key is not the user's to change, and fillAuth() makes
        // that true rather than merely unoffered: the baked key wins even when a
        // stored override is sitting in QSettings from a keyless build installed
        // over it. So the field stays hidden and the notice keeps naming only
        // username and password, override or no override.
        function test_14_a_keyed_build_ignores_a_stored_override() {
            testContext.setRadioReference("buildHasAppKey", true)
            var key = findChild(tc.screen, "radioReferenceAppKeyField")
            tryVerify(function () { return !key.visible },
                      2000, "a keyed build with no override still asked for a key")

            testContext.setPrefs("rrAppKey", "OVERRIDE_KEY_1a2")
            // Waited out rather than tryVerify'd: the assertion is that nothing
            // changes, which is true on the first frame however the binding is
            // written. Only a settled reading distinguishes the two.
            wait(200)
            verify(!key.visible,
                   "a stored override reopened a field this build does not let the user change")

            var notice = findChild(tc.screen, "radioReferenceNotice")
            testContext.setRadioReference("errorText", "Invalid Username or Password")
            testContext.setRadioReference("errorIsAuth", true)
            tryVerify(function () { return notice.visible && notice.text.indexOf("application key") < 0 },
                      2000, "the notice blamed a key this build ignores: " + notice.text)
        }

        // ---- The sign-in form (password managers, reveal, session password) ----

        function useFakeAccount() {
            fakeAccount.resetAccount();
            testContext.setPrefs("rrUsername", "test-listener");
            testContext.useRadioReferenceModel(fakeAccount);
            screenLoader.z = 1;
        }
        // Typed in, as a keyboard does: the field's `text: prefs.rrUsername`
        // binding stays, where assigning `text` would replace it.
        function typeInto(field, value) {
            field.input.forceActiveFocus();
            field.input.remove(0, field.input.text.length);
            field.input.insert(0, value);
        }
        function callsOf(op) {
            return signInAutofill.calls().filter(function (call) { return call.op === op; });
        }

        // Regression (B7): one Check tap sent two account checks. The button
        // finished the field's editing first, whose own commit checked, and
        // then the tap checked again. A real activate(), not clicked().
        function test_30_one_check_per_tap_data() {
            return [{tag: "password", field: "radioReferencePasswordField"},
                    {tag: "application key", field: "radioReferenceAppKeyField"}];
        }
        function test_30_one_check_per_tap(data) {
            useFakeAccount();
            if (data.tag === "application key") {
                fakeAccount.setPassword("session-secret");
                fakeAccount.buildHasAppKey = false;
                tc.screen.accountEditorOpen = true;
            }
            var field = findChild(tc.screen, data.field);
            var button = findChild(tc.screen, "radioReferenceCheckAccountButton");
            tryVerify(function () { return field.visible && button.visible && button.enabled; });
            typeInto(field, data.tag === "password" ? "session-secret" : "fixture-key");
            verify(field.input.activeFocus);
            button.activate();
            compare(fakeAccount.accountChecks, 1, "one tap, one account check");
            verify(!field.input.activeFocus, "the tap committed the field being typed in");
            compare(fakeAccount.sessionPassword, "session-secret");
            fakeAccount.buildHasAppKey = true;
        }

        // B7, once more: a field the user tapped out of checks when its edit
        // made the credentials whole, and a field left untouched does not
        // repeat a check already answered.
        function test_30b_an_untouched_field_does_not_recheck() {
            useFakeAccount();
            var password = findChild(tc.screen, "radioReferencePasswordField");
            var username = findChild(tc.screen, "radioReferenceUsernameField");
            typeInto(password, "session-secret");
            password.input.focus = false;
            compare(fakeAccount.accountChecks, 1);
            fakeAccount.settle(2);
            username.input.forceActiveFocus();
            username.input.focus = false;
            compare(fakeAccount.accountChecks, 1, "focus leaving an unchanged field checked again");
        }

        // Issue B3: the password can be shown, like every other secret field
        // in the app, without that tap finishing the field or checking.
        function test_31_the_password_reveals_without_a_check() {
            useFakeAccount();
            var password = findChild(tc.screen, "radioReferencePasswordField");
            var reveal = findChild(tc.screen, "radioReferencePasswordReveal");
            verify(reveal !== null, "the password field has no Show button");
            tryVerify(function () { return reveal.visible; });
            compare(password.input.echoMode, TextInput.Password);
            compare(reveal.Accessible.name, "Show password");
            compare(reveal.text, "Show");
            typeInto(password, "session-secret");
            reveal.activate();
            compare(password.input.echoMode, TextInput.Normal);
            compare(reveal.Accessible.name, "Hide password");
            compare(reveal.text, "Hide");
            verify(password.input.activeFocus, "Show took the field out of editing");
            compare(fakeAccount.accountChecks, 0, "Show checked the account");
            compare(fakeAccount.passwordCommits, 0, "Show committed the password");
            reveal.activate();
            compare(password.input.echoMode, TextInput.Password);
            reveal.activate();
            compare(password.input.echoMode, TextInput.Normal);
            // Masked again when the screen goes.
            tc.screen.visible = false;
            compare(password.input.echoMode, TextInput.Password, "a revealed password outlived the screen");
            tc.screen.visible = true;
            compare(password.input.echoMode, TextInput.Password);
            compare(reveal.Accessible.name, "Show password");
            // The key editors keep their own names.
            var keyReveal = Qt.createComponent(uiDir + "/RevealButton.qml").createObject(root);
            compare(keyReveal.Accessible.name, "Show key");
            keyReveal.revealed = true;
            compare(keyReveal.Accessible.name, "Hide key");
            keyReveal.destroy();
        }

        // The field holds a draft; the session password is the model's. An
        // empty draft keeps it, the draft goes with the screen, and an empty
        // field says a password is held.
        function test_32_the_session_password_outlives_its_draft() {
            useFakeAccount();
            var password = findChild(tc.screen, "radioReferencePasswordField");
            typeInto(password, "session-secret");
            password.input.focus = false;
            compare(fakeAccount.sessionPassword, "session-secret");
            fakeAccount.settle(2);
            var commits = fakeAccount.passwordCommits;
            password.text = "";
            password.input.forceActiveFocus();
            password.input.focus = false;
            compare(fakeAccount.sessionPassword, "session-secret", "an empty draft cleared the session password");
            compare(fakeAccount.passwordCommits, commits, "an empty draft was committed");
            compare(fakeAccount.accountChecks, 1, "an empty draft checked the account");
            password.text = "draft";
            tc.screen.visible = false;
            compare(password.text, "", "the draft outlived the screen");
            tc.screen.visible = true;
            compare(password.text, "");
            compare(password.placeholderText, "Entered for this session");
            compare(password.input.Accessible.description, "Password. Entered for this session");
            // Check with an empty draft checks the session password.
            findChild(tc.screen, "radioReferenceCheckAccountButton").activate();
            compare(fakeAccount.accountChecks, 2);
            compare(fakeAccount.sessionPassword, "session-secret");
        }

        // Regression (B4): the placeholder "password" under the label
        // "Password" was read twice ("Password. password").
        function test_33_the_password_field_reads_its_label_once() {
            var password = findChild(tc.screen, "radioReferencePasswordField");
            compare(password.placeholderText, "");
            var description = password.input.Accessible.description;
            compare(description, "Password");
            verify(!/password.*password/i.test(description), "read twice: " + description);
        }

        // The card stays up through the account check, so the password
        // manager sees the fields until the verdict, and hides once
        // RadioReference accepts them.
        function test_34_the_card_stays_up_through_the_check() {
            useFakeAccount();
            var credentials = findChild(tc.screen, "radioReferenceCredentials");
            var password = findChild(tc.screen, "radioReferencePasswordField");
            typeInto(password, "session-secret");
            password.input.focus = false;
            verify(fakeAccount.credentialsReady && fakeAccount.checkingAccount);
            wait(50);
            verify(credentials.visible, "the card hid while the account was being checked");
            fakeAccount.settle(2);
            tryCompare(credentials, "visible", false);
            // The form's result line reports checking and verified; the notice
            // line above reports a failure, once.
            findChild(tc.screen, "radioReferenceChangeAccount").activate();
            var result = findChild(tc.screen, "radioReferenceAccountResult");
            tryCompare(result, "visible", true);
            compare(result.text, "Account verified.");
            fakeAccount.settle(3);
            verify(!result.visible, "the rejection is said twice: " + result.text);
            verify(findChild(tc.screen, "radioReferenceNotice").text.indexOf("did not accept") >= 0);
            fakeAccount.settle(0);
        }

        // Check with a field empty says so where the button is.
        function test_35_check_with_an_empty_field_says_so() {
            useFakeAccount();
            var result = findChild(tc.screen, "radioReferenceAccountResult");
            findChild(tc.screen, "radioReferenceCheckAccountButton").activate();
            compare(fakeAccount.accountChecks, 0);
            tryCompare(result, "visible", true);
            compare(result.text, "Fill in every field above first.");
            var password = findChild(tc.screen, "radioReferencePasswordField");
            typeInto(password, "session-secret");
            password.input.focus = false;
            compare(result.text, "Checking your RadioReference account…");
            fakeAccount.settle(0);
        }

        // A visit is one password-manager session: begin on show, the two
        // fields tracked (never the application key), the username noted, end
        // on hide; a new visit is a new token.
        function test_40_each_visit_takes_its_own_token() {
            var username = findChild(tc.screen, "radioReferenceUsernameField");
            var password = findChild(tc.screen, "radioReferencePasswordField");
            tc.screen.visible = false;
            tc.screen.visible = true;
            var first = signInAutofill.liveToken();
            verify(first > 0, "the shown form holds no token");
            signInAutofill.clear();
            tc.screen.visible = false;
            compare(callsOf("end").length, 1);
            compare(callsOf("end")[0].token, first);
            compare(signInAutofill.liveToken(), 0);
            signInAutofill.clear();
            testContext.setPrefs("rrUsername", "test-listener");
            tc.screen.visible = true;
            var begins = callsOf("begin");
            compare(begins.length, 1);
            var token = begins[0].token;
            verify(token > first, "a new visit reused the token");
            var tracks = callsOf("track").filter(function (call) { return call.token === token; });
            var fields = tracks.map(function (call) { return call.field; });
            verify(fields.indexOf(1) >= 0 && fields.indexOf(2) >= 0, JSON.stringify(fields));
            for (var call of tracks) {
                verify(call.field === 1 || call.field === 2, "a field other than username and password was tracked");
                verify(call.input === (call.field === 1 ? username.input : password.input), "field " + call.field + " tracked the wrong input");
                verify(call.visible, "the shown card's fields were tracked hidden");
            }
            var notes = callsOf("note");
            compare(notes.length, 1);
            compare(notes[0].token, token);
            compare(notes[0].field, 1);
            compare(notes[0].text, "test-listener");
            // The card hiding (filled credentials) keeps the session and marks the fields hidden.
            signInAutofill.clear();
            var checks = testContext.radioReferenceAccountChecks();
            testContext.setRadioReference("credentialsReady", true);
            tryCompare(findChild(tc.screen, "radioReferenceCredentials"), "visible", false);
            tryVerify(function () { return callsOf("track").length >= 2; }, 2000, "the hidden card's fields were not reported");
            compare(callsOf("end").length, 0, "the card hiding ended the session");
            var hidden = callsOf("track");
            compare(hidden.length, 2);
            for (var call of hidden) {
                compare(call.token, token);
                verify(!call.visible, "a hidden card's field was tracked visible");
            }
            compare(testContext.radioReferenceAccountChecks(), checks, "the credentials turning ready checked by themselves");
        }

        // Within one commit or fill the card hides and shows again (the
        // credentials turn ready a moment before the check starts). That
        // flicker never reaches the password manager, which could end its
        // session once every field is gone, and never masks a revealed
        // password; the fields go only once RadioReference has answered.
        function test_43_the_fields_stay_shown_until_the_verdict_data() {
            return [{tag: "typed password"}, {tag: "fill"}];
        }
        function test_43_the_fields_stay_shown_until_the_verdict(data) {
            useFakeAccount();
            var credentials = findChild(tc.screen, "radioReferenceCredentials");
            var password = findChild(tc.screen, "radioReferencePasswordField");
            var reveal = findChild(tc.screen, "radioReferencePasswordReveal");
            var token = signInAutofill.liveToken();
            verify(token > 0);
            signInAutofill.clear();
            if (data.tag === "typed password") {
                typeInto(password, "session-secret");
                reveal.activate();
                compare(password.input.echoMode, TextInput.Normal);
                keyClick(Qt.Key_Return);
                verify(!password.input.activeFocus, "Enter did not finish the field");
            } else {
                // What the relay does: the password first, then the signal.
                fakeAccount.setPassword("filled-secret");
                signInAutofill.fill(token, false, true);
            }
            compare(fakeAccount.accountChecks, 1);
            wait(50);
            var hidden = callsOf("track").filter(function (call) { return !call.visible; });
            compare(hidden.length, 0, "the fields were reported hidden before the verdict: "
                    + callsOf("track").map(function (call) { return call.field + ":" + call.visible; }).join(" "));
            verify(credentials.visible);
            if (data.tag === "typed password")
                compare(password.input.echoMode, TextInput.Normal, "the commit masked a revealed password");
            fakeAccount.settle(2);
            tryCompare(credentials, "visible", false);
            tryVerify(function () {
                return callsOf("track").filter(function (call) { return !call.visible && call.token === token; }).length === 2;
            }, 2000, "the fields were not reported hidden after the verdict");
            compare(password.input.echoMode, TextInput.Password);
        }

        // After the verdict the hidden card drops its draft: Change shows an
        // empty field that says a password is held, as Settings does.
        function test_44_change_after_a_verdict_shows_the_session_placeholder() {
            useFakeAccount();
            var credentials = findChild(tc.screen, "radioReferenceCredentials");
            var password = findChild(tc.screen, "radioReferencePasswordField");
            typeInto(password, "session-secret");
            password.input.focus = false;
            fakeAccount.settle(2);
            tryCompare(credentials, "visible", false);
            tryCompare(password, "text", "");
            findChild(tc.screen, "radioReferenceChangeAccount").activate();
            tryCompare(credentials, "visible", true);
            compare(password.text, "");
            compare(password.placeholderText, "Entered for this session");
            compare(fakeAccount.sessionPassword, "session-secret");
        }

        // A fill from the password manager: the relay applied the values
        // (username through prefs, password in the model); the form commits
        // the field the fill left alone and checks the pair once.
        function test_41_a_fill_applies_and_checks_once_data() {
            return [{tag: "both", username: true, password: true},
                    {tag: "username only, password typed", username: true, password: false},
                    {tag: "password only, username typed", username: false, password: true},
                    // Regression: the stored username, picked while the field
                    // held another, uncommitted draft. prefs did not change, so
                    // the field kept the draft, which then overwrote the fill.
                    {tag: "the stored username over a typed draft", username: true, password: false, stored: true},
                    // ...and over a word still composing, which must not land after the fill.
                    {tag: "the stored username over a composing word", username: true, password: false, stored: true, composing: true},
                    // Regression: a password-only fill while the typed username
                    // still has a word composing checks the whole username.
                    {tag: "password only, username still composing", username: false, password: true, composing: true}];
        }
        function test_41_a_fill_applies_and_checks_once(data) {
            useFakeAccount();
            testContext.useLifecycleHost(true);
            var oldUsername = prefs.rrUsername;
            prefs.rrUsername = data.stored ? "filled-user" : "";
            var username = findChild(tc.screen, "radioReferenceUsernameField");
            var password = findChild(tc.screen, "radioReferencePasswordField");
            var typedUser = data.composing && !data.username ? "typed-user-end" : "typed-user";
            if (!data.username) {
                typeInto(username, "typed-user");
                if (data.composing)
                    verify(testContext.composeText("-end"), "the username field took no composing word");
            }
            if (data.stored) {
                // Typed but not committed: the username field keeps the focus.
                password.text = "typed-secret";
                typeInto(username, "x");
                if (data.composing)
                    verify(testContext.composeText("y"), "the username field took no composing word");
            } else if (!data.password) {
                password.text = "typed-secret";
                password.input.forceActiveFocus();
            }
            var token = signInAutofill.liveToken();
            verify(token > 0);
            // What the relay does before it says so.
            if (data.username)
                prefs.rrUsername = "filled-user";
            if (data.password)
                fakeAccount.setPassword("filled-secret");
            var commits = fakeAccount.passwordCommits;
            signInAutofill.fill(token, data.username, data.password);
            compare(fakeAccount.accountChecks, 1, "a fill checks the account once");
            compare(prefs.rrUsername, data.username ? "filled-user" : typedUser);
            compare(username.text, prefs.rrUsername);
            compare(username.input.preeditText, "", "a composing word outlived the fill");
            compare(fakeAccount.sessionPassword, data.password ? "filled-secret" : "typed-secret");
            if (data.password) {
                compare(password.text, "", "a filled password reached the field");
                compare(password.placeholderText, "Entered for this session");
                compare(fakeAccount.passwordCommits, commits, "the form committed a filled password again");
            }
            // Leaving the field afterwards asks nothing more.
            password.input.forceActiveFocus();
            password.input.focus = false;
            username.input.forceActiveFocus();
            username.input.focus = false;
            compare(fakeAccount.accountChecks, 1, "the fill's fields checked again");
            compare(prefs.rrUsername, data.username ? "filled-user" : typedUser, "leaving the field changed the username");
            compare(username.text, prefs.rrUsername);
            // The binding still follows prefs.
            prefs.rrUsername = "later-user";
            compare(username.text, "later-user", "the username field stopped following prefs");
            prefs.rrUsername = oldUsername;
        }

        // A fill for a token this form no longer holds is not for it.
        function test_42_a_stale_fill_is_ignored() {
            useFakeAccount();
            var password = findChild(tc.screen, "radioReferencePasswordField");
            var stale = signInAutofill.liveToken();
            tc.screen.visible = false;
            tc.screen.visible = true;
            verify(signInAutofill.liveToken() > stale);
            password.text = "typed-secret";
            fakeAccount.setPassword("filled-secret");
            var commits = fakeAccount.passwordCommits;
            signInAutofill.fill(stale, true, true);
            signInAutofill.fill(0, true, true);
            compare(fakeAccount.accountChecks, 0, "a stale fill checked the account");
            compare(password.text, "typed-secret", "a stale fill cleared the draft");
            compare(fakeAccount.passwordCommits, commits);
            // Settings showing takes a token of its own (the relay retires the
            // import form's), and its fill reaches that form alone.
            var importToken = signInAutofill.liveToken();
            settingsLoader.active = true;
            var settingsToken = signInAutofill.liveToken();
            verify(settingsToken > importToken, "the Settings form took no token of its own");
            signInAutofill.fill(settingsToken, false, true);
            compare(fakeAccount.accountChecks, 1, "the Settings form did not take its own fill, or both forms did");
            compare(password.text, "typed-secret", "the import form took the Settings form's fill");
            compare(findChild(settingsLoader.item, "radioReferencePasswordField").text, "");
            settingsLoader.active = false;
            password.text = "";
            // The import form takes the token back as a fresh visit would.
            tc.screen.visible = false;
            tc.screen.visible = true;
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
            verify(settingsCase.screen !== null, "RadioReferenceAccountScreen.qml failed to load")
        }

        function cleanupTestCase() {
            testContext.setRadioReference("available", false)
            testContext.setRadioReference("buildHasAppKey", false)
            testContext.setPrefs("rrAppKey", "")
        }

        function test_01_the_key_row_follows_what_this_build_needs() {
            var row = findChild(settingsCase.screen, "radioReferenceAppKeyField")
            verify(row !== null, "the application-key row is missing")

            // A keyless build needs the user's key, so the row is offered.
            testContext.setRadioReference("buildHasAppKey", false)
            testContext.setPrefs("rrAppKey", "")
            tryVerify(function () { return row.visible },
                      2000, "a keyless build hid the application-key row")

            // A keyed build asks for nothing — and keeps asking for nothing with
            // an override stored. fillAuth() ignores it there, so it is not a
            // credential in play, and a row for it would only invite editing a
            // key this build never sends.
            testContext.setRadioReference("buildHasAppKey", true)
            tryVerify(function () { return !row.visible },
                      2000, "a keyed build still offered the application-key row")

            testContext.setPrefs("rrAppKey", "OVERRIDE_KEY_1a2")
            // Waited out rather than tryVerify'd: what is asserted is that
            // nothing changes, which is trivially true on the fade's first
            // frame however the binding is written.
            wait(200)
            verify(!row.visible, "a keyed build offered a row for a key it does not use")

            testContext.setPrefs("rrAppKey", "")
        }
    }
}
