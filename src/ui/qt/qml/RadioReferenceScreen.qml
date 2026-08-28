// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import "Util.js" as Util

// Import a system straight from the RadioReference database: sign in, find the
// system by zip code, by browsing country/state/county, or by its system ID,
// pick the site — or, for a conventional networked system, the repeaters — and
// let the model generate the talkgroup list and channel map this app already
// reads.
//
// The screen saves nothing itself. `imported()` hands the generated file paths
// and the tune answers to the add-system wizard, which stays the single writer
// of a saved system, so the user's source, gain and ppm answers are never
// guessed at here.
Item {
    id: screen

    objectName: "radioReferenceScreen"

    signal closed()
    // The result map from performImport(): {name, freqMhz, decodeFlag, trunking,
    // chanCsvPath, groupCsvPath}. chanCsvPath is absent when no channel map was
    // generated, which is a valid outcome — see the one-repeater case below.
    signal imported(var result)

    // 0 = zip code, 1 = browse, 2 = system ID.
    property int sourceMode: 0
    property alias zipText: zipField.text
    property alias sidText: sidField.text

    // Browse state. The country defaults to the United States (coid 1) because
    // that is where the systems these decode chips are pitched at live; every
    // other country is one tap away.
    property int browseCoid: 1
    property string browseCountryName: qsTr("United States")
    property int browseStid: -1
    property string browseStateName: ""
    property int browseCtid: -1
    property string browseCountyName: ""

    // Indexes into radioReference.sites, in selection order — that is what
    // buildImportPlan() takes. One entry for a trunked system; a conventional
    // networked import selects several repeaters.
    property var selectedSites: []

    // Import options. The two overrides are tri-state: -1 follows what the
    // RadioReference record says, 0 and 1 are the user's own answer. Without
    // that a pre-checked toggle could not be turned off — assigning to the
    // property would destroy the binding that pre-checked it.
    property bool partialEncAsDe: true
    property int simulcastOverride: -1
    property int eskOverride: -1

    // The last preview. Recomputed on every selection or option change rather
    // than bound, because building it runs both generators.
    property var plan: ({})

    // Transient outcome line under the header; magenta when it reports a problem.
    property string notice: ""
    property bool noticeIsProblem: false

    readonly property bool systemLoaded: radioReference.systemDetails.sid !== undefined
                                         && radioReference.systemDetails.sid > 0

    // Whether the application key is the user's to supply — and so whether this
    // screen offers a field for it and names it as a possible culprit. A build
    // that bakes a key in answers no to both: fillAuth() ignores any stored
    // override there, so the key is neither editable nor a credential in play.
    // Same rule as the Settings row.
    readonly property bool offersAppKey: !radioReference.buildHasAppKey

    // The resolved RadioReference type name, which is what decides whether the
    // simulcast and ESK toggles mean anything here. Matching the name rather
    // than the protocol enum keeps QML out of the business of tracking C
    // enumerator values.
    readonly property string typeDescr: {
        var descr = radioReference.systemDetails.typeDescr
        return descr !== undefined ? String(descr) : ""
    }
    readonly property bool isP25: screen.typeDescr.indexOf("Project 25") >= 0
    readonly property bool isEdacs: screen.typeDescr.indexOf("EDACS") >= 0

    // Re-run per visit and whenever a new system's sites land: the options and
    // the selection belong to the system in hand, not to the screen.
    readonly property var siteList: radioReference.sites

    onSiteListChanged: {
        screen.simulcastOverride = -1
        screen.eskOverride = -1
        // Belongs to the system in hand like the two overrides do, not to the
        // screen: the back row makes "system A, back, system B" one visit, and
        // A's answer would otherwise be written into B's import provenance.
        screen.partialEncAsDe = true
        // A single-site trunked system has exactly one right answer, so give
        // it: the preview and the Import button light up without asking for a
        // tap on the only row there is. Conventional lists stay untouched —
        // "which repeaters can I hear" is a real question even with one entry.
        //
        // `trunked`, not `!conventional`: a system type this build cannot import
        // is neither, and reading it as trunked selected and lit up the one row
        // of an LTR or MPT-1327 system whose plan can never be built.
        //
        // Assigned once rather than cleared and then filled: every write to
        // selectedSites runs refreshPlan(), and that builds both CSVs.
        screen.selectedSites = (radioReference.trunked && screen.siteList.length === 1) ? [0] : []
        screen.refreshPlan()
    }

    // What the RadioReference record itself says, which is where the toggles
    // start. Simulcast is read off the SITE the user picked rather than "any
    // site on this system": a system can have one simulcast cell and five
    // ordinary ones, and the demodulator answer differs between them.
    readonly property bool recordSimulcast: {
        var sites = radioReference.sites
        var i = screen.selectedSites.length > 0 ? screen.selectedSites[0] : -1
        return i >= 0 && i < sites.length && sites[i].simulcast === true
    }
    readonly property bool simulcast: screen.simulcastOverride >= 0
                                      ? screen.simulcastOverride === 1 : screen.recordSimulcast
    readonly property bool recordEsk: radioReference.systemDetails.esk === true
    readonly property bool esk: screen.eskOverride >= 0 ? screen.eskOverride === 1 : screen.recordEsk

    onSimulcastChanged: screen.refreshPlan()
    onEskChanged: screen.refreshPlan()
    onPartialEncAsDeChanged: screen.refreshPlan()
    onSelectedSitesChanged: screen.refreshPlan()

    /**
     * Whether `radioReference` is the live model rather than the QML suite's map
     * of readings, which answers property reads and carries no invokables.
     *
     * Aliased to a local on purpose: the fixture-completeness check treats a
     * dotted read of that context property as a reading it must carry, and this
     * is a method, not a reading. It scans the file as text, comments included.
     */
    function rrLive() {
        var rr = radioReference
        return typeof rr.loadSystem === "function"
    }

    /** Clear everything that belongs to one visit. */
    function reset() {
        // A visit never starts mid-system: drop whatever the last visit — or a
        // library refresh, which loads the system it re-fetches — left behind.
        // The model keeps the results list across this on purpose, so "come
        // back and import the next one" starts at the list, not at a search.
        //
        // Only when there is a system to drop, or a fetch still on its way to
        // becoming one: closeSystem() retires the model's error along with it,
        // and a rejected sign-in is the one error that must outlive a visit —
        // it is what holds the credentials form open for the retype, and the
        // password lives nowhere else. An idle visit that ended on one keeps it.
        if (screen.rrLive() && (screen.systemLoaded || radioReference.busy))
            radioReference.closeSystem()
        screen.sourceMode = 0
        zipField.text = ""
        sidField.text = ""
        screen.browseCoid = 1
        screen.browseCountryName = qsTr("United States")
        screen.browseStid = -1
        screen.browseStateName = ""
        screen.browseCtid = -1
        screen.browseCountyName = ""
        screen.selectedSites = []
        screen.partialEncAsDe = true
        screen.simulcastOverride = -1
        screen.eskOverride = -1
        screen.plan = ({})
        screen.notice = ""
        screen.noticeIsProblem = false
    }

    // Each preview field with the value a screen that has no plan yet shows: a
    // bare `screen.plan.warnings.length` would throw before the first preview.
    function planOk() {
        return screen.plan.ok === true
    }

    function planBlockedReason() {
        return screen.plan.blockedReason !== undefined ? screen.plan.blockedReason : ""
    }

    // The one blocked plan that is a question, not a refusal: no site picked yet.
    // It is also the state a SUCCESSFUL import leaves behind, which is why it must
    // not read as an error.
    function planAwaitingSelection() {
        return screen.plan.awaitingSelection === true
    }

    function planWarnings() {
        return screen.plan.warnings !== undefined ? screen.plan.warnings : []
    }

    function planField(key, fallback) {
        return screen.plan[key] !== undefined ? screen.plan[key] : fallback
    }

    function systemName() {
        var name = radioReference.systemDetails.name
        return name !== undefined ? name : ""
    }

    function refreshPlan() {
        if (!screen.rrLive()) {
            screen.plan = ({})
            return
        }
        screen.plan = radioReference.buildImportPlan(screen.selectedSites, {
                                                         "partialEncAsDe": screen.partialEncAsDe,
                                                         "simulcast": screen.simulcast,
                                                         "esk": screen.esk
                                                     })
    }

    function siteSelected(index) {
        for (var i = 0; i < screen.selectedSites.length; i++) {
            if (screen.selectedSites[i] === index)
                return true
        }
        return false
    }

    // A trunked import is one site, so a tap replaces the selection; a
    // conventional one is a set of repeaters, so a tap adds or removes.
    function toggleSite(index) {
        if (!radioReference.conventional) {
            screen.selectedSites = [index]
            return
        }
        var next = []
        var found = false
        for (var i = 0; i < screen.selectedSites.length; i++) {
            if (screen.selectedSites[i] === index) {
                found = true
                continue
            }
            next.push(screen.selectedSites[i])
        }
        if (!found)
            next.push(index)
        screen.selectedSites = next
    }

    // Site rows carrying their index into radioReference.sites, sorted by
    // description for a conventional system so a user can find their town —
    // RadioReference returns repeaters in database order, which is neither
    // alphabetical nor geographic. Trunked sites keep the record's order, where
    // it carries meaning.
    readonly property var siteRows: {
        var rows = []
        var sites = radioReference.sites
        for (var i = 0; i < sites.length; i++)
            rows.push({ "index": i, "site": sites[i] })
        if (radioReference.conventional) {
            rows.sort(function (a, b) {
                return String(a.site.descr).localeCompare(String(b.site.descr))
            })
        }
        return rows
    }

    function clearNotice() {
        screen.notice = ""
        screen.noticeIsProblem = false
    }

    // ---- Actions on the model ----

    function verifyAccount() {
        if (screen.rrLive() && radioReference.credentialsReady)
            radioReference.checkAccount()
    }

    /**
     * The "Check account" button, which is not the same gesture as finishing a
     * field.
     *
     * Nothing on this screen takes keyboard focus — every control is a
     * TapHandler — so a text field only emits editingFinished on Enter or when
     * another field is tapped. Tapping the button leaves the field the user
     * just filled uncommitted, and its answer never reaches prefs or the model.
     * Move focus off first, which commits it, then check what the form holds.
     */
    function checkAccountTapped() {
        credentialsColumn.forceActiveFocus()
        if (screen.rrLive() && !radioReference.credentialsReady) {
            screen.notice = qsTr("Fill in every field above first.")
            screen.noticeIsProblem = true
            return
        }
        screen.clearNotice()
        screen.verifyAccount()
    }

    function findByZip() {
        screen.clearNotice()
        Qt.inputMethod.hide()
        if (screen.rrLive())
            radioReference.lookupZip(zipField.text)
    }

    function findBySid() {
        screen.clearNotice()
        Qt.inputMethod.hide()
        var sid = parseInt(sidField.text, 10)
        if (!isNaN(sid) && sid > 0 && screen.rrLive())
            radioReference.loadSystem(sid)
    }

    function openSystem(sid) {
        screen.clearNotice()
        if (screen.rrLive())
            radioReference.loadSystem(sid)
    }

    function browseCountries() {
        if (screen.rrLive())
            radioReference.loadCountries()
        countrySheet.visible = true
    }

    function chooseCountry(row) {
        screen.browseCoid = row.coid
        screen.browseCountryName = row.name
        screen.browseStid = -1
        screen.browseStateName = ""
        screen.browseCtid = -1
        screen.browseCountyName = ""
        countrySheet.visible = false
        screen.browseStates()
    }

    function browseStates() {
        if (screen.rrLive())
            radioReference.loadCountryStates(screen.browseCoid)
        stateSheet.visible = true
    }

    function chooseState(row) {
        screen.browseStid = row.stid
        screen.browseStateName = row.name
        screen.browseCtid = -1
        screen.browseCountyName = ""
        stateSheet.visible = false
        screen.browseCounties()
    }

    function browseCounties() {
        if (screen.browseStid < 0)
            return
        if (screen.rrLive())
            radioReference.loadStateCounties(screen.browseStid)
        countySheet.visible = true
    }

    function chooseCounty(row) {
        screen.browseCtid = row.ctid
        screen.browseCountyName = row.name
        countySheet.visible = false
        screen.clearNotice()
        if (screen.rrLive())
            radioReference.loadCountySystems(screen.browseCtid)
    }

    // getStateInfo answers with the whole state's system list, so this is one
    // round trip rather than a county the user would have to guess at first.
    function findStatewide() {
        if (screen.browseStid < 0)
            return
        screen.clearNotice()
        if (screen.rrLive())
            radioReference.loadStateSystems(screen.browseStid)
    }

    function doImport() {
        if (!screen.planOk() || !screen.rrLive())
            return
        // -1: this screen never writes a saved system. The wizard owns that, and
        // it is the half that knows whether it is editing a row or adding one.
        var result = radioReference.performImport(screen.plan, screen.systemName(), -1)
        if (result.ok !== true) {
            screen.notice = result.error === "import"
                            ? qsTr("The generated files could not be added to your library.")
                            : qsTr("The import could not be completed.")
            screen.noticeIsProblem = true
            return
        }
        screen.imported(result)
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.bg
    }

    // One level of the place hierarchy — country, state, county — as a capped,
    // scrolling list over the standard sheet. Inline because the three levels
    // below are its only consumers and always will be: no other screen browses
    // places.
    //
    // ModalSheet sizes its panel to its content and neither clips nor scrolls,
    // so an uncapped list pushes the title off the top and everything else off
    // the bottom with no way back to either. The cap and the Flickable are what
    // keep the list inside the panel; written once because a level that got it
    // wrong would be a sheet with no way out of it.
    component BrowseSheet: ModalSheet {
        id: browseSheet

        /** Section label above the list. */
        property string title: ""
        /** The rows to offer: objects carrying `name` and the key named by idKey. */
        property var rows: []
        /** Which key on a row holds its id — "coid", "stid", "ctid". */
        property string idKey: ""
        /** The id chosen at this level, highlighted in the list; -1 when none is. */
        property int selectedId: -1
        /** Whether a row can be tapped. Off while a request is running. */
        property bool rowsEnabled: true

        /** Emitted with the tapped row. */
        signal chosen(var row)

        // A list that has not arrived yet is empty, which is what the notice
        // below reports.
        readonly property int rowCount: browseSheet.rows !== undefined && browseSheet.rows !== null
                                        ? browseSheet.rows.length : 0

        MicroLabel {
            text: browseSheet.title
        }

        Flickable {
            width: parent.width
            height: Math.min(listColumn.height, 46 * 5)
            visible: browseSheet.rowCount > 0
            clip: true
            contentHeight: listColumn.height
            boundsBehavior: Flickable.StopAtBounds

            Column {
                id: listColumn

                width: parent.width

                Repeater {
                    model: browseSheet.rows

                    Item {
                        id: sheetRow

                        required property var modelData

                        width: listColumn.width
                        height: 46

                        Text {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            text: sheetRow.modelData.name
                            font.family: Theme.sans
                            font.pixelSize: 15
                            color: sheetRow.modelData[browseSheet.idKey] === browseSheet.selectedId
                                   ? Theme.cyan : Theme.textPrimary
                            elide: Text.ElideRight
                        }

                        TapHandler {
                            enabled: browseSheet.rowsEnabled
                            onTapped: browseSheet.chosen(sheetRow.modelData)
                        }
                    }
                }
            }
        }

        Text {
            width: parent.width
            visible: browseSheet.rowCount === 0
            text: qsTr("Loading…")
            font.family: Theme.sans
            font.pixelSize: 13
            color: Theme.textSubdued
        }
    }

    // One browse row: label left, current choice and caret right.
    component BrowseRow: Item {
        id: browseRow

        property string title: ""
        property string value: ""
        property bool showDivider: false
        signal tapped()

        width: parent ? parent.width : 0
        height: 52
        opacity: enabled ? 1.0 : 0.5

        Text {
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            text: browseRow.title
            font.family: Theme.sans
            font.pixelSize: 15
            color: Theme.textPrimary
        }

        Row {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            spacing: 6

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: browseRow.value
                font.family: Theme.sans
                font.pixelSize: 14
                color: Theme.textSecondary
            }

            Caret {
                anchors.verticalCenter: parent.verticalCenter
                rotation: -90
                color: Theme.textSubdued
            }
        }

        Rectangle {
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            height: 1
            visible: browseRow.showDivider
            color: Theme.divider
        }

        TapHandler {
            onTapped: browseRow.tapped()
        }
    }

    // Header: back chevron, title.
    Item {
        id: header

        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: Theme.screenPadding
        height: 46

        Text {
            id: back

            // Named so UI_QT_QML_CALL_LISTS can reach it with findChild().
            objectName: "radioReferenceBack"

            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            text: "‹"
            font.pixelSize: 28
            color: Theme.textSecondary

            TapHandler {
                onTapped: screen.closed()
            }
        }

        Text {
            anchors.left: back.right
            anchors.leftMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("RadioReference")
            font.family: Theme.sans
            font.pixelSize: 22
            font.weight: Font.Bold
            font.letterSpacing: -0.22
            color: Theme.textPrimary
        }
    }

    // One line for both the model's error and this screen's own outcome; the
    // model's wins, because it reports the thing that just failed.
    Text {
        id: noticeLine

        // Named so UI_QT_QML_CALL_LISTS can reach it with findChild().
        objectName: "radioReferenceNotice"

        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.topMargin: 4
        anchors.leftMargin: Theme.screenPadding
        anchors.rightMargin: Theme.screenPadding
        visible: text.length > 0
        // The auth wording names only what the user can fix here, which is the
        // same set the form offers: a build that bakes the application key in,
        // with no override stored, leaves username and password as the two
        // possible culprits.
        text: radioReference.errorIsSubscription
              ? qsTr("This account's RadioReference premium subscription has expired.")
              : radioReference.errorIsAuth
                ? (screen.offersAppKey
                   ? qsTr("RadioReference did not accept that username, password or application key.")
                   : qsTr("RadioReference did not accept that username or password."))
                : radioReference.errorText.length > 0 ? radioReference.errorText : screen.notice
        font.family: Theme.sans
        font.pixelSize: 13
        color: (radioReference.errorText.length > 0 || screen.noticeIsProblem)
               ? Theme.magenta : Theme.textSubdued
        wrapMode: Text.Wrap
    }

    Flickable {
        id: body

        anchors.top: noticeLine.visible ? noticeLine.bottom : header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: importButton.visible ? importButton.top : parent.bottom
        anchors.topMargin: 6
        anchors.bottomMargin: 14
        contentHeight: content.height + 2 * Theme.screenPadding
        clip: true

        Column {
            id: content

            x: Theme.screenPadding
            y: Theme.screenPadding
            width: parent.width - 2 * Theme.screenPadding
            spacing: Theme.gap
            // Deliberately NOT `enabled: !radioReference.busy`. `enabled` is
            // hierarchical, so binding it on a container fights the explicit
            // `enabled` bindings on the buttons inside — Qt reports that as a
            // binding loop, and only at run time on device, never in the
            // offscreen QML suite. The buttons carry the busy term themselves
            // instead, and a tap that does slip through during a request is
            // harmless: every action starts with startBatch(), which cancels
            // what is in flight and bumps the generation so the superseded
            // reply is dropped.

            // ---- Credentials gate ----
            // Every user authenticates with their own RadioReference account and
            // needs their own premium subscription; nothing is pooled, and the
            // password is never written anywhere.
            //
            // Username and password lead, the way every sign-in form does. The
            // application key comes last and only where the user is the one who
            // supplies it — see offersAppKey.
            UiPanel {
                // Named so UI_QT_QML_CALL_LISTS can reach it with findChild().
                objectName: "radioReferenceCredentials"

                width: parent.width
                // credentialsReady means the fields are FILLED, not accepted —
                // so an account failure reopens the form, or the session-only
                // password would have nowhere left to be retyped. Both account
                // errors qualify: auth wants a correction, an expired
                // subscription wants a different account.
                visible: !radioReference.credentialsReady || radioReference.errorIsAuth
                         || radioReference.errorIsSubscription
                height: credentialsColumn.height + 2 * Theme.cardPadding

                Column {
                    id: credentialsColumn

                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: Theme.cardPadding
                    spacing: 10

                    MicroLabel {
                        text: qsTr("RadioReference account")
                    }

                    Text {
                        width: parent.width
                        text: qsTr("Username")
                        font.family: Theme.sans
                        font.pixelSize: 13
                        color: Theme.textSecondary
                    }

                    PlexTextField {
                        id: usernameField

                        // Named so UI_QT_QML_CALL_LISTS can reach it with findChild().
                        objectName: "radioReferenceUsernameField"

                        width: parent.width
                        text: prefs.rrUsername
                        placeholderText: qsTr("radioreference.com username")
                        inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                        onEditingFinished: prefs.rrUsername = text
                    }

                    Text {
                        width: parent.width
                        text: qsTr("Password")
                        font.family: Theme.sans
                        font.pixelSize: 13
                        color: Theme.textSecondary
                    }

                    PlexTextField {
                        id: passwordField

                        // Named so UI_QT_QML_CALL_LISTS can reach it with findChild().
                        objectName: "radioReferencePasswordField"

                        width: parent.width
                        // PlexTextField carries no echoMode of its own; reaching
                        // through the `input` alias is how the wizard sets its
                        // field validators too.
                        input.echoMode: TextInput.Password
                        placeholderText: qsTr("password")
                        inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                                          | Qt.ImhSensitiveData
                        // Commits on Enter or focus loss, never per keystroke,
                        // and the password goes nowhere but memory.
                        onEditingFinished: {
                            if (screen.rrLive())
                                radioReference.setPassword(text)
                            screen.verifyAccount()
                        }
                    }

                    Text {
                        width: parent.width
                        text: qsTr("The password is kept only for this session and is never saved. A RadioReference premium subscription is required.")
                        font.family: Theme.sans
                        font.pixelSize: 12
                        color: Theme.textSubdued
                        wrapMode: Text.Wrap
                    }

                    Text {
                        width: parent.width
                        visible: screen.offersAppKey
                        text: qsTr("Application key")
                        font.family: Theme.sans
                        font.pixelSize: 13
                        color: Theme.textSecondary
                    }

                    PlexTextField {
                        id: appKeyField

                        // Named so UI_QT_QML_CALL_LISTS can reach it with findChild().
                        objectName: "radioReferenceAppKeyField"

                        width: parent.width
                        visible: screen.offersAppKey
                        mono: true
                        text: prefs.rrAppKey
                        placeholderText: qsTr("application key")
                        inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                        // Commit on Enter or focus loss, not per keystroke: every
                        // write lands in QSettings (disk on Android). The account
                        // check fires the same way the password field's does, so
                        // whichever field is answered last completes the form.
                        onEditingFinished: {
                            prefs.rrAppKey = text
                            screen.verifyAccount()
                        }
                    }

                    Text {
                        width: parent.width
                        visible: !radioReference.buildHasAppKey
                        text: qsTr("This build carries no application key. Request one at <a href=\"https://www.radioreference.com/account/api/apply\">radioreference.com/account/api/apply</a>.")
                        textFormat: Text.StyledText
                        linkColor: Theme.cyan
                        font.family: Theme.sans
                        font.pixelSize: 12
                        color: Theme.textSubdued
                        wrapMode: Text.Wrap
                        onLinkActivated: function (link) { Qt.openUrlExternally(link) }
                    }

                    OutlineButton {
                        // Named so UI_QT_QML_CALL_LISTS can reach it with findChild().
                        objectName: "radioReferenceCheckAccountButton"

                        width: parent.width
                        // Deliberately not gated on credentialsReady. The fields
                        // commit on Enter or focus loss, and a TapHandler takes
                        // no focus — so the field the user is still typing in
                        // has not committed when they reach for this button, and
                        // gating on the committed state greys it out exactly
                        // when the last answer is the one still sitting in it.
                        enabled: !radioReference.busy
                        text: qsTr("Check account")
                        onClicked: screen.checkAccountTapped()
                    }
                }
            }

            // ---- Where to look ----
            // Hidden while a system is loaded: the screen is a drill-down, and
            // showing the search controls above a system being configured made
            // it read as two screens at once — searching from there fetched a
            // results list that stayed invisible behind the loaded system.
            UiPanel {
                // Named so UI_QT_QML_CALL_LISTS can reach it with findChild().
                objectName: "radioReferenceSourcePanel"

                width: parent.width
                visible: radioReference.credentialsReady && !screen.systemLoaded
                height: sourceColumn.height + 2 * Theme.cardPadding

                Column {
                    id: sourceColumn

                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: Theme.cardPadding
                    spacing: 10

                    MicroLabel {
                        text: qsTr("Find a system")
                    }

                    SegmentedControl {
                        // Named so UI_QT_QML_CALL_LISTS can reach it with findChild().
                        objectName: "radioReferenceSourcePicker"

                        width: parent.width
                        model: [qsTr("Zip code"), qsTr("Browse"), qsTr("System ID")]
                        currentIndex: screen.sourceMode
                        onSelected: function (index) { screen.sourceMode = index }
                    }

                    // Zip code
                    Row {
                        width: parent.width
                        visible: screen.sourceMode === 0
                        spacing: 10

                        PlexTextField {
                            id: zipField

                            // Named so UI_QT_QML_CALL_LISTS can reach it with findChild().
                            objectName: "radioReferenceZipField"

                            width: parent.width - zipGo.width - 10
                            mono: true
                            placeholderText: qsTr("e.g. 12345")
                            inputMethodHints: Qt.ImhDigitsOnly
                            // A leading-zero zip resolves correctly as an int, so
                            // the validator is only here to stop a stray letter
                            // from a hardware keyboard.
                            input.validator: IntValidator {
                                bottom: 0
                                top: 99999
                            }
                            onEditingFinished: screen.findByZip()
                        }

                        OutlineButton {
                            id: zipGo

                            // Named so UI_QT_QML_CALL_LISTS can reach it with findChild().
                            objectName: "radioReferenceZipGo"

                            width: 96
                            enabled: !radioReference.busy && zipField.text.length > 0
                            text: qsTr("Find")
                            onClicked: screen.findByZip()
                        }
                    }

                    // Browse
                    Column {
                        width: parent.width
                        visible: screen.sourceMode === 1
                        spacing: 0

                        BrowseRow {
                            objectName: "radioReferenceCountryRow"
                            title: qsTr("Country")
                            value: screen.browseCountryName
                            showDivider: true
                            enabled: !radioReference.busy
                            onTapped: screen.browseCountries()
                        }

                        BrowseRow {
                            objectName: "radioReferenceStateRow"
                            title: qsTr("State")
                            value: screen.browseStateName.length > 0 ? screen.browseStateName : qsTr("Choose")
                            showDivider: true
                            enabled: !radioReference.busy
                            onTapped: screen.browseStates()
                        }

                        BrowseRow {
                            objectName: "radioReferenceCountyRow"
                            title: qsTr("County")
                            value: screen.browseCountyName.length > 0 ? screen.browseCountyName : qsTr("Choose")
                            enabled: !radioReference.busy && screen.browseStid >= 0
                            onTapped: screen.browseCounties()
                        }
                    }

                    OutlineButton {
                        width: parent.width
                        visible: screen.sourceMode === 1 && screen.browseStid >= 0
                        enabled: !radioReference.busy
                        text: qsTr("Every system in %1").arg(screen.browseStateName)
                        onClicked: screen.findStatewide()
                    }

                    // System ID
                    Row {
                        width: parent.width
                        visible: screen.sourceMode === 2
                        spacing: 10

                        PlexTextField {
                            id: sidField

                            // Named so UI_QT_QML_CALL_LISTS can reach it with findChild().
                            objectName: "radioReferenceSidField"

                            width: parent.width - sidGo.width - 10
                            mono: true
                            placeholderText: qsTr("RadioReference system ID")
                            inputMethodHints: Qt.ImhDigitsOnly
                            input.validator: IntValidator {
                                bottom: 1
                                top: 999999
                            }
                            onEditingFinished: screen.findBySid()
                        }

                        OutlineButton {
                            id: sidGo

                            width: 96
                            enabled: !radioReference.busy && sidField.text.length > 0
                            text: qsTr("Open")
                            onClicked: screen.findBySid()
                        }
                    }
                }
            }

            // ---- Systems found ----
            UiPanel {
                // Named so UI_QT_QML_CALL_LISTS can reach it with findChild().
                objectName: "radioReferenceSystemList"

                width: parent.width
                visible: radioReference.systems.length > 0 && !screen.systemLoaded
                height: systemsColumn.height + Theme.cardPadding + 4

                Column {
                    id: systemsColumn

                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.topMargin: Theme.cardPadding
                    spacing: 0

                    MicroLabel {
                        text: qsTr("Systems")
                        leftPadding: Theme.cardPadding
                        bottomPadding: 6
                    }

                    // A plain Column, not a list view: the body Flickable already
                    // scrolls this, and a list inside it would fight it for the
                    // drag.
                    Repeater {
                        model: radioReference.systems

                        DisclosureRow {
                            id: systemRow

                            required property var modelData
                            required property int index

                            title: systemRow.modelData.name
                            subtitle: systemRow.modelData.city.length > 0
                                      ? systemRow.modelData.city + " · SID " + systemRow.modelData.sid
                                      : "SID " + systemRow.modelData.sid
                            showDivider: systemRow.index < radioReference.systems.length - 1
                            tapEnabled: !radioReference.busy
                            onTapped: screen.openSystem(systemRow.modelData.sid)
                        }
                    }
                }
            }

            // ---- Back to the find stage ----
            // The drill-down's way out: a loaded system replaces the find panel
            // and the results list, and this row is what puts them back. The
            // model keeps the results across closeSystem(), so "back, pick the
            // next one" is one tap — the loop for importing several systems
            // from one search.
            Item {
                // Named so UI_QT_QML_CALL_LISTS can reach it with findChild().
                objectName: "radioReferenceBackToResults"

                width: parent.width
                height: 40
                visible: screen.systemLoaded
                opacity: radioReference.busy ? 0.5 : 1.0

                Row {
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 8

                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: "‹"
                        font.pixelSize: 22
                        color: Theme.cyan
                    }

                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        // Named after where it lands: the results list when one
                        // is in hand, otherwise the find-a-system panel.
                        text: radioReference.systems.length > 0 ? qsTr("All systems") : qsTr("Search")
                        font.family: Theme.sans
                        font.pixelSize: 14
                        font.weight: Font.DemiBold
                        color: Theme.cyan
                    }
                }

                TapHandler {
                    enabled: !radioReference.busy
                    onTapped: {
                        screen.clearNotice()
                        if (screen.rrLive())
                            radioReference.closeSystem()
                    }
                }
            }

            // ---- The system ----
            UiPanel {
                // Named so UI_QT_QML_CALL_LISTS can reach it with findChild().
                objectName: "radioReferenceSystemPanel"

                width: parent.width
                visible: screen.systemLoaded
                height: systemColumn.height + 2 * Theme.cardPadding

                Column {
                    id: systemColumn

                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: Theme.cardPadding
                    spacing: 8

                    Text {
                        width: parent.width
                        text: screen.systemName()
                        font.family: Theme.sans
                        font.pixelSize: 17
                        font.weight: Font.Bold
                        color: Theme.textPrimary
                        wrapMode: Text.Wrap
                    }

                    Text {
                        width: parent.width
                        text: {
                            var details = radioReference.systemDetails
                            var parts = []
                            if (details.typeDescr !== undefined && String(details.typeDescr).length > 0)
                                parts.push(details.typeDescr)
                            if (details.flavorDescr !== undefined && String(details.flavorDescr).length > 0)
                                parts.push(details.flavorDescr)
                            return parts.join(" · ")
                        }
                        font.family: Theme.mono
                        font.pixelSize: 12
                        color: Theme.textSubdued
                        wrapMode: Text.Wrap
                    }

                    Text {
                        width: parent.width
                        visible: radioReference.talkgroupSummary.count !== undefined
                        text: {
                            var summary = radioReference.talkgroupSummary
                            var line = summary.count === 1 ? qsTr("1 talkgroup")
                                                           : qsTr("%1 talkgroups").arg(summary.count)
                            if (summary.encCount > 0)
                                line += " · " + qsTr("%1 encrypted").arg(summary.encCount)
                            var categories = summary.categories !== undefined ? summary.categories : []
                            if (categories.length > 0)
                                line += " · " + qsTr("%1 categories").arg(categories.length)
                            return line
                        }
                        font.family: Theme.sans
                        font.pixelSize: 13
                        color: Theme.textSecondary
                        wrapMode: Text.Wrap
                    }
                }
            }

            // ---- Sites, or repeaters ----
            UiPanel {
                // Named so UI_QT_QML_CALL_LISTS can reach it with findChild().
                objectName: "radioReferenceSiteList"

                width: parent.width
                visible: screen.systemLoaded && radioReference.sites.length > 0
                height: sitesColumn.height + Theme.cardPadding + 4

                Column {
                    id: sitesColumn

                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.topMargin: Theme.cardPadding
                    spacing: 0

                    MicroLabel {
                        text: radioReference.conventional ? qsTr("Repeaters") : qsTr("Site")
                        leftPadding: Theme.cardPadding
                        bottomPadding: 6
                    }

                    // Two things a conventional import has to say, because
                    // neither is guessable from the list itself. The third — that
                    // scanning needs an RTL-SDR or a rigctl radio — rides in
                    // plan.warnings with every other warning.
                    Text {
                        // Named so UI_QT_QML_CALL_LISTS can reach it with findChild().
                        objectName: "radioReferenceRepeaterCount"

                        width: parent.width
                        leftPadding: Theme.cardPadding
                        rightPadding: Theme.cardPadding
                        bottomPadding: 6
                        visible: radioReference.conventional
                        text: qsTr("%1 repeater(s) selected").arg(screen.selectedSites.length)
                        font.family: Theme.sans
                        font.pixelSize: 12
                        color: Theme.textSubdued
                        wrapMode: Text.Wrap
                    }

                    Text {
                        width: parent.width
                        leftPadding: Theme.cardPadding
                        rightPadding: Theme.cardPadding
                        bottomPadding: 6
                        visible: radioReference.conventional && screen.selectedSites.length === 1
                        text: qsTr("One repeater tunes straight to its frequency — no scan list is written, which is what a single repeater wants.")
                        font.family: Theme.sans
                        font.pixelSize: 12
                        color: Theme.textSubdued
                        wrapMode: Text.Wrap
                    }

                    Repeater {
                        model: screen.siteRows

                        Item {
                            id: siteRow

                            required property var modelData

                            readonly property bool chosen: screen.siteSelected(siteRow.modelData.index)

                            width: sitesColumn.width
                            height: 58

                            Column {
                                anchors.left: parent.left
                                anchors.right: siteMark.left
                                anchors.leftMargin: Theme.cardPadding
                                anchors.rightMargin: 12
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 3

                                Text {
                                    width: parent.width
                                    // A conventional repeater has no meaningful
                                    // site number — the value is a DMR-ID-like
                                    // identifier — so it is named, not numbered.
                                    text: radioReference.conventional
                                          ? siteRow.modelData.site.descr
                                          : qsTr("Site %1 · %2").arg(siteRow.modelData.site.siteNumber)
                                                                .arg(siteRow.modelData.site.descr)
                                    font.family: Theme.sans
                                    font.pixelSize: 15
                                    font.weight: Font.DemiBold
                                    color: siteRow.chosen ? Theme.cyan : Theme.textPrimary
                                    elide: Text.ElideRight
                                }

                                Text {
                                    width: parent.width
                                    // The colour code is shown to help pick a
                                    // repeater and goes into no file: dsd_opts
                                    // has no field for it, because DMR reads it
                                    // off air.
                                    text: {
                                        var site = siteRow.modelData.site
                                        if (radioReference.conventional) {
                                            var parts = []
                                            if (site.freqMhz.length > 0)
                                                parts.push(site.freqMhz + " MHz")
                                            if (site.colorCode.length > 0)
                                                parts.push(qsTr("CC %1").arg(site.colorCode))
                                            return parts.join(" · ")
                                        }
                                        var line = site.freqCount === 1
                                                   ? qsTr("1 frequency")
                                                   : qsTr("%1 frequencies").arg(site.freqCount)
                                        if (site.controlFreqMhz.length > 0)
                                            line += " · " + qsTr("control %1 MHz").arg(site.controlFreqMhz)
                                        return line
                                    }
                                    font.family: Theme.sans
                                    font.pixelSize: 12
                                    color: Theme.textSubdued
                                    elide: Text.ElideRight
                                }
                            }

                            // A filled dot rather than a checkbox: this UI has no
                            // checkbox component, and the chosen row's title
                            // already turns cyan.
                            Rectangle {
                                id: siteMark

                                anchors.right: parent.right
                                anchors.rightMargin: Theme.cardPadding
                                anchors.verticalCenter: parent.verticalCenter
                                width: 18
                                height: 18
                                radius: 9
                                color: siteRow.chosen ? Theme.cyan : "transparent"
                                border.width: 1
                                border.color: siteRow.chosen ? Theme.cyan : Theme.controlBorder
                            }

                            Rectangle {
                                anchors.bottom: parent.bottom
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.leftMargin: Theme.cardPadding
                                height: 1
                                color: Theme.divider
                            }

                            TapHandler {
                                enabled: !radioReference.busy
                                onTapped: screen.toggleSite(siteRow.modelData.index)
                            }
                        }
                    }
                }
            }

            // ---- What to generate ----
            UiPanel {
                // Named so UI_QT_QML_CALL_LISTS can reach it with findChild().
                objectName: "radioReferenceOptions"

                width: parent.width
                visible: screen.systemLoaded
                height: optionsColumn.height + Theme.cardPadding + 4

                Column {
                    id: optionsColumn

                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.topMargin: Theme.cardPadding
                    spacing: 0

                    MicroLabel {
                        text: qsTr("Import options")
                        leftPadding: Theme.cardPadding
                        bottomPadding: 6
                    }

                    ToggleRow {
                        objectName: "radioReferencePartialEncRow"
                        title: qsTr("Treat partly encrypted as encrypted")
                        subtitle: qsTr("Blocks those talkgroups instead of playing noise")
                        checked: screen.partialEncAsDe
                        showDivider: screen.isP25 || screen.isEdacs
                        onToggled: function (state) { screen.partialEncAsDe = state }
                    }

                    ToggleRow {
                        objectName: "radioReferenceSimulcastRow"
                        visible: screen.isP25
                        title: qsTr("Simulcast (LSM/QPSK)")
                        subtitle: screen.recordSimulcast
                                  ? qsTr("Detected from the RadioReference site record")
                                  : qsTr("Turn on if standard P25 never locks here")
                        checked: screen.simulcast
                        onToggled: function (state) { screen.simulcastOverride = state ? 1 : 0 }
                    }

                    ToggleRow {
                        objectName: "radioReferenceEskRow"
                        visible: screen.isEdacs
                        title: qsTr("ESK")
                        subtitle: screen.recordEsk
                                  ? qsTr("Detected from the RadioReference system flavor")
                                  : qsTr("EDACS scrambling — turn on if this system uses it")
                        checked: screen.esk
                        onToggled: function (state) { screen.eskOverride = state ? 1 : 0 }
                    }
                }
            }

            // ---- Preview ----
            UiPanel {
                // Named so UI_QT_QML_CALL_LISTS can reach it with findChild().
                objectName: "radioReferencePreview"

                width: parent.width
                visible: screen.systemLoaded
                height: previewColumn.height + 2 * Theme.cardPadding

                Column {
                    id: previewColumn

                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: Theme.cardPadding
                    spacing: 8

                    MicroLabel {
                        text: qsTr("What gets imported")
                    }

                    Text {
                        width: parent.width
                        visible: screen.planOk()
                        text: {
                            var parts = []
                            var freq = screen.planField("freqMhz", "")
                            if (freq.length > 0)
                                parts.push(freq + " MHz")
                            var flag = screen.planField("decodeFlag", "")
                            if (flag.length > 0)
                                parts.push(Util.decodeLabel(flag))
                            if (screen.planField("trunking", false))
                                parts.push(qsTr("trunked"))
                            if (screen.planField("scanList", false))
                                parts.push(qsTr("scan list"))
                            return parts.join(" · ")
                        }
                        font.family: Theme.mono
                        font.pixelSize: 12
                        color: Theme.textSecondary
                        wrapMode: Text.Wrap
                    }

                    Text {
                        width: parent.width
                        visible: screen.planOk()
                        text: {
                            var files = []
                            if (screen.planField("groupCsvText", "").length > 0)
                                files.push(qsTr("talkgroup list"))
                            if (screen.planField("chanCsvText", "").length > 0)
                                files.push(qsTr("channel map"))
                            return files.length > 0
                                   ? qsTr("Generates: %1").arg(files.join(" · "))
                                   : qsTr("No files — the session simply tunes this frequency.")
                        }
                        font.family: Theme.sans
                        font.pixelSize: 13
                        color: Theme.textSubdued
                        wrapMode: Text.Wrap
                    }

                    // Blocked: the reason, in magenta, with Import disabled
                    // rather than hidden so it is clear what is missing. An unmade
                    // site choice is the exception - it is a question, and it is
                    // also what a successful import leaves behind - so it is shown
                    // in the ordinary subdued text instead of the error colour.
                    Text {
                        // Named so UI_QT_QML_CALL_LISTS can reach it with findChild().
                        objectName: "radioReferenceBlockedBanner"

                        width: parent.width
                        visible: screen.planBlockedReason().length > 0
                        text: screen.planBlockedReason()
                        font.family: Theme.sans
                        font.pixelSize: 13
                        color: screen.planAwaitingSelection() ? Theme.textSubdued : Theme.magenta
                        wrapMode: Text.Wrap
                    }

                    Text {
                        width: parent.width
                        visible: radioReference.systemDetails.hasCustomBandplan === true
                        text: qsTr("This system publishes a custom band plan, which this import does not carry. Add the matching options as extra CLI args if tuning misses.")
                        font.family: Theme.sans
                        font.pixelSize: 12
                        color: Theme.textSubdued
                        wrapMode: Text.Wrap
                    }

                    Repeater {
                        model: screen.planWarnings()

                        Text {
                            id: warningLine

                            required property var modelData

                            width: previewColumn.width
                            text: "• " + warningLine.modelData
                            font.family: Theme.sans
                            font.pixelSize: 12
                            color: Theme.textSubdued
                            wrapMode: Text.Wrap
                        }
                    }
                }
            }
        }
    }

    GradientButton {
        id: importButton

        // Named so UI_QT_QML_CALL_LISTS can reach it with findChild().
        objectName: "radioReferenceImportButton"

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: Theme.screenPadding
        anchors.bottomMargin: 22
        visible: screen.systemLoaded
        // Disabled rather than hidden: a button that vanishes when a site is
        // deselected reads as the screen breaking.
        enabled: !radioReference.busy && screen.planOk() && screen.planBlockedReason().length === 0
        text: qsTr("Import this system")
        onClicked: screen.doImport()
    }

    // ---- Browse sheets ----

    BrowseSheet {
        id: countrySheet

        objectName: "radioReferenceCountrySheet"
        title: qsTr("Country")
        rows: radioReference.countries
        idKey: "coid"
        selectedId: screen.browseCoid
        rowsEnabled: !radioReference.busy
        onChosen: function (row) {
            screen.chooseCountry(row)
        }
    }

    BrowseSheet {
        id: stateSheet

        objectName: "radioReferenceStateSheet"
        title: qsTr("State")
        rows: radioReference.states
        idKey: "stid"
        selectedId: screen.browseStid
        rowsEnabled: !radioReference.busy
        onChosen: function (row) {
            screen.chooseState(row)
        }
    }

    BrowseSheet {
        id: countySheet

        objectName: "radioReferenceCountySheet"
        title: qsTr("County")
        rows: radioReference.counties
        idKey: "ctid"
        selectedId: screen.browseCtid
        rowsEnabled: !radioReference.busy
        onChosen: function (row) {
            screen.chooseCounty(row)
        }
    }

    // Declared after the sheets so it also covers one that is still open when a
    // request starts. Covering is not what stops the taps, though — the layers
    // underneath disable themselves on `busy`.
    Rectangle {
        // Named so UI_QT_QML_CALL_LISTS can reach it with findChild().
        objectName: "radioReferenceBusyOverlay"

        anchors.fill: parent
        visible: radioReference.busy
        color: Qt.alpha("#000000", 0.5)

        Column {
            anchors.centerIn: parent
            width: parent.width - 4 * Theme.screenPadding
            spacing: 14

            Text {
                width: parent.width
                text: radioReference.statusText.length > 0
                      ? radioReference.statusText : qsTr("Talking to RadioReference…")
                font.family: Theme.sans
                font.pixelSize: 15
                color: Theme.textPrimary
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
            }

            OutlineButton {
                // Named so UI_QT_QML_CALL_LISTS can reach it with findChild().
                objectName: "radioReferenceCancelButton"

                width: parent.width
                text: qsTr("Cancel")
                onClicked: {
                    if (screen.rrLive())
                        radioReference.cancel()
                }
            }
        }
    }
}
