// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtQuick.Window
import "Util.js" as Util

Window {
    id: mainRoot

    visible: true
    width: 420
    height: 900
    title: qsTr("DSD-neo")
    color: Theme.bg

    // The screen has two modes with one principle carried over from the old UI:
    // idle is for choosing what to hear, and the moment a session is asked for the
    // screen becomes about the session. The tab shell stays instantiated underneath
    // so nothing typed or scrolled is lost when a session ends.
    readonly property bool monitorMode: decoderHost ? decoderHost.sessionActive : false
    readonly property bool running: decoderHost ? decoderHost.running : false
    readonly property bool transitioning: decoderHost ? decoderHost.transitioning : false
    readonly property string hostFailure: decoderHost ? decoderHost.failureText : ""
    // A start the UI refused before the host was ever asked (bad saved config).
    property string startError: ""
    // Set while a USB start is blocked on device access, so the platform's live
    // detail ("USB permission denied", "No RTL-SDR attached") reaches the screen
    // as it changes — the permission dialog answers long after the tap that
    // asked. Cleared when access is granted (which resumes the pending start
    // below), when the banner is dismissed, or when another system starts.
    property bool awaitingUsbAccess: false
    // The system map whose start is waiting on that grant, and the row it came
    // from (-1 when it came from nowhere, i.e. an explore session). The map, not
    // just the row: an explore start has no row to look up again.
    property var pendingStart: null
    property int pendingStartRow: -1
    readonly property string usbAccessText:
        awaitingUsbAccess && decoderHost && !decoderHost.localDeviceReady ? decoderHost.localDeviceStatus : ""
    readonly property string failureText: startError.length > 0 ? startError
                                          : usbAccessText.length > 0 ? usbAccessText : hostFailure

    property int currentTab: 0
    property bool wizardOpen: false
    property bool exploreSetupOpen: false
    property bool importsOpen: false
    property bool radioReferenceOpen: false
    // Whether the RadioReference screen was pushed from the wizard. Coming back
    // to an open wizard fills in the answers it is already asking for; coming
    // back to Settings or the imports library opens one instead, so the import
    // still ends as a saved system.
    property bool radioReferenceFromWizard: false
    // The saved-systems row the running session was started from, or -1. Lets a
    // wizard save on that same row push its CSV files into the live session.
    property int sessionRow: -1
    // The spectrum view is pushed over the monitor, so it can only be open
    // while a session is; ending one has to take it down with it.
    property bool spectrumOpen: false
    // The saved-system map the running session was started from.
    property var sessionSystem: null
    // Whether this session is free to be retuned by hand. False for anything
    // started from a saved system — its card names a frequency, and wandering off
    // it makes the card a lie and mis-files the calls heard afterwards. Set at
    // start, and again if the user asks to explore from where they are.
    property bool exploring: false
    // Sampled while an explore session runs, not read at the end of one: the
    // metrics are cleared the moment the session stops, so asking then would
    // persist a zero and lose the frequency the user had found.
    property string lastExploreFreqMhz: ""

    Connections {
        target: metrics
        function onTunerChanged() {
            if (mainRoot.exploring && metrics.centerFreqHz > 0)
                mainRoot.lastExploreFreqMhz = Util.mhzText(metrics.centerFreqHz)
        }
    }

    // Suppresses a failure banner the user has read. Reset on the next start so a
    // repeat of the same failure is reported again rather than swallowed.
    property string dismissedFailure: ""
    readonly property bool showFailure:
        !monitorMode && failureText.length > 0 && failureText !== dismissedFailure

    // Dismissing the banner abandons a start still waiting on USB access; the
    // flag must not linger, or a dongle detached minutes later while idle would
    // resurrect a permission banner the user never asked about.
    onDismissedFailureChanged: {
        if (dismissedFailure.length > 0) {
            awaitingUsbAccess = false
            mainRoot.pendingStart = null
            mainRoot.pendingStartRow = -1
        }
    }

    // The USB permission dialog answers long after the tap that asked: when the
    // grant lands, finish that start instead of leaving a screen that looks like
    // nothing happened until a second tap.
    Connections {
        target: decoderHost
        function onLocalDeviceChanged() {
            if (!mainRoot.awaitingUsbAccess || !decoderHost.localDeviceReady)
                return
            mainRoot.awaitingUsbAccess = false
            var sys = mainRoot.pendingStart
            var row = mainRoot.pendingStartRow
            mainRoot.pendingStart = null
            mainRoot.pendingStartRow = -1
            if (sys)
                mainRoot.startWithMap(sys, row)
        }
    }

    onMonitorModeChanged: {
        // The spectrum layer lives above the monitor; when the session goes so
        // does it, or the next one would open onto a stale panorama.
        if (!monitorMode) {
            // Where the exploring got to, so the next one resumes there rather
            // than back at the start frequency.
            if (mainRoot.exploring && mainRoot.lastExploreFreqMhz.length > 0)
                prefs.exploreFreqMhz = mainRoot.lastExploreFreqMhz
            mainRoot.exploring = false
            mainRoot.spectrumOpen = false
            // Row indices shift when a system is removed, and Home is reachable
            // again from here; a row remembered past its session would name a
            // different system by the time anything read it.
            mainRoot.sessionRow = -1
        }
        if (monitorMode) {
            // The frequency field usually still holds focus; the keyboard would
            // cover the session that just appeared.
            Qt.inputMethod.hide()
            // Reattaching to a session this UI process did not start (service
            // survived an Activity restart): bound the recent-calls pane to the
            // last hour rather than the whole persisted log.
            if (monitorView.minWhen === 0)
                monitorView.minWhen = Math.floor(Date.now() / 1000) - 3600
        }
    }

    // Closing the window finishes the Android Activity, and Qt then terminates the
    // process — taking the service that owns the engine with it. Background instead
    // when the host can; and when background listening is off, stop first so the
    // radio does not keep playing from a window the user just dismissed.
    onClosing: function (close) {
        if (!prefs.backgroundListening && mainRoot.running)
            decoderHost.stop()
        close.accepted = !decoderHost.moveToBackground()
    }

    function startSystem(row) {
        var sys = savedSystems.get(row)
        if (sys)
            mainRoot.startWithMap(sys, row)
    }

    /**
     * Start a session from a system map.
     *
     * @a row is the saved-systems row it came from, or -1 when it came from
     * nowhere — an explore session is an ordinary session over a map that was
     * never saved, so everything here except the row bookkeeping is shared. The
     * alternative, a second start path, would have to re-implement the USB
     * permission dance below, and would get it wrong on exactly the install
     * where it matters: a fresh one.
     */
    function startWithMap(sys, row) {
        if (!sys || !sys.sourceType)
            return
        if (sys.sourceType === "usb" && decoderHost.localDeviceBrokered && !decoderHost.localDeviceReady) {
            // Not a silent return: the platform's status line is the only thing
            // that can say why ("USB permission denied", "No RTL-SDR attached"),
            // and it keeps updating as the permission dialog resolves. When the
            // grant lands, the Connections above resumes this start.
            mainRoot.dismissedFailure = ""
            mainRoot.startError = ""
            mainRoot.awaitingUsbAccess = true
            mainRoot.pendingStart = sys
            mainRoot.pendingStartRow = row
            decoderHost.requestLocalDeviceAccess()
            return
        }
        mainRoot.dismissedFailure = ""
        mainRoot.startError = ""
        mainRoot.awaitingUsbAccess = false
        mainRoot.pendingStart = null
        mainRoot.pendingStartRow = -1
        var built = sessionArgs.build(sys)
        if (!built.ok) {
            // The builder refuses for exactly two reasons; blame the field that
            // is actually wrong or the user re-checks a frequency that was fine.
            mainRoot.startError = built.error === "frequency"
                ? qsTr("“%1” has no valid frequency — long-press its card to edit it.").arg(sys.name)
                : qsTr("“%1” has an invalid PPM correction — long-press its card to edit it.").arg(sys.name)
            return
        }
        // Side effects only after the host accepts: a refused start must not
        // stamp lastHeard, re-attribute history rows, or hide the previous
        // session's calls from the monitor pane.
        if (!decoderHost.start(built.args)) {
            if (decoderHost.failureText.length === 0)
                mainRoot.startError = qsTr("“%1” could not be started.").arg(sys.name)
            return
        }
        mainRoot.sessionSystem = sys
        mainRoot.sessionRow = row
        // The session's intent, decided here and nowhere else: a system someone
        // saved is a thing to listen to, and the spectrum watches it. Only a
        // session with no saved system behind it is free to wander.
        mainRoot.exploring = (row < 0)
        // Belongs to the session that just ended. Carried into this one it would be
        // written back to prefs on stop as if it were where this exploring got to —
        // a frequency from two sessions ago, on a session that may never have moved.
        mainRoot.lastExploreFreqMhz = ""
        // The previous session may have committed calls since the last 250 ms
        // tick; ingest them under its own label before the label changes hands,
        // or its tail calls read as the new system's.
        uiController.flushHistory()
        callHistory.sessionLabel = sys.name
        // The monitor's recent-calls pane shows this session, not the whole log.
        monitorView.minWhen = Math.floor(Date.now() / 1000)
        savedSystems.touch(row)
    }

    /**
     * Push the RadioReference import screen.
     *
     * @a fromWizard records where it was opened from, which is what decides
     * whether the finished import fills in an open wizard or opens a fresh one.
     */
    function openRadioReference(fromWizard) {
        mainRoot.radioReferenceFromWizard = fromWizard
        radioReferenceScreen.reset()
        mainRoot.radioReferenceOpen = true
    }

    /** Start an explore session from the remembered source and frequency. */
    function startExploring() {
        var source = prefs.exploreSourceType
        if (source !== "usb" && source !== "rtltcp") {
            // Nothing remembered to start from; ask instead of guessing.
            mainRoot.exploreSetupOpen = true
            return
        }
        mainRoot.startWithMap(mainRoot.exploreSystem(source, prefs.exploreHost, prefs.explorePort,
                                                     prefs.exploreFreqMhz), -1)
    }

    /**
     * The system map an explore session runs on.
     *
     * Deliberately shaped like a saved system so the ordinary start path, the
     * monitor header and the args builder all work on it unchanged — but with no
     * decode flag, because exploring should hear whatever it lands on, and with
     * trunking off, because a trunker would take the tuner straight back.
     */
    function exploreSystem(source, host, port, freqMhz) {
        return {
            name: qsTr("Exploring"),
            sourceType: source,
            host: host,
            port: port,
            freqMhz: freqMhz,
            decodeFlag: "",
            trunking: false
        }
    }

    // ---- Safe area ----
    // From Android 15 (targetSdk 35+) the window is edge-to-edge: the status
    // bar and gesture-nav bar are transparent overlays on the window, whose
    // color paints the full bleed behind them. Every UI layer anchors to this
    // item rather than the window, which keeps content out from under the
    // bars. On desktop the margins are zero and this is the whole window.
    // Bound to the window's margins, not this item's own: positioning an item
    // by its own safe area is a binding loop.
    Item {
        id: safeArea

        objectName: "safeArea"
        anchors.fill: parent
        anchors.topMargin: mainRoot.SafeArea.margins.top
        anchors.leftMargin: mainRoot.SafeArea.margins.left
        anchors.rightMargin: mainRoot.SafeArea.margins.right
        anchors.bottomMargin: mainRoot.SafeArea.margins.bottom
    }

    // ---- Tab shell ----
    Item {
        id: shell

        anchors.fill: safeArea
        opacity: (mainRoot.monitorMode || mainRoot.wizardOpen || mainRoot.exploreSetupOpen
                  || mainRoot.importsOpen || mainRoot.radioReferenceOpen
                  || !prefs.onboardingDone) ? 0.0 : 1.0
        visible: opacity > 0.0
        enabled: opacity > 0.9

        Behavior on opacity {
            NumberAnimation { duration: 150; easing.type: Easing.OutCubic }
        }

        HomeScreen {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: nav.top
            visible: mainRoot.currentTab === 0

            onAddSystem: {
                wizard.openForAdd(false)
                mainRoot.wizardOpen = true
            }
            onNetworkSource: {
                wizard.openForAdd(true)
                mainRoot.wizardOpen = true
            }
            onPlaySystem: function (row) { mainRoot.startSystem(row) }
            onEditSystem: function (row) {
                wizard.openForEdit(row)
                mainRoot.wizardOpen = true
            }
            onExplore: mainRoot.startExploring()
            onExploreSetup: {
                exploreSetup.reset(prefs.exploreSourceType, prefs.exploreHost, prefs.explorePort,
                                   prefs.exploreFreqMhz)
                mainRoot.exploreSetupOpen = true
            }
        }

        HistoryScreen {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: nav.top
            visible: mainRoot.currentTab === 1
        }

        SettingsScreen {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: nav.top
            visible: mainRoot.currentTab === 2

            onOpenImports: mainRoot.importsOpen = true
            onOpenRadioReference: mainRoot.openRadioReference(false)
        }

        BottomNav {
            id: nav

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            currentIndex: mainRoot.currentTab
            onSelected: function (index) { mainRoot.currentTab = index }
        }
    }

    // ---- Explore setup (pushed; idle only, since it starts a session) ----
    ExploreSetupScreen {
        id: exploreSetup

        anchors.fill: safeArea
        opacity: mainRoot.exploreSetupOpen && !mainRoot.monitorMode ? 1.0 : 0.0
        visible: opacity > 0.0
        enabled: opacity > 0.9

        Behavior on opacity {
            NumberAnimation { duration: 150; easing.type: Easing.OutCubic }
        }

        onClosed: mainRoot.exploreSetupOpen = false
        onStart: function (sourceType, host, port, freqMhz) {
            // Remembered before the start, not after: a start that fails is still
            // the answer the user gave, and asking again would be the app
            // forgetting what it was just told.
            // All four unconditionally: they are one answer to "how to start
            // exploring", and startExploring() reads all four back together.
            // Skipping the empty ones leaves the previous answer's endpoint
            // behind, so switching rtltcp -> usb keeps the old host and the
            // setup sheet shows it again as the current setting. An out-of-range
            // port is corrected by AppPrefs' own sanitiser, not by not writing it.
            prefs.exploreSourceType = sourceType
            prefs.exploreHost = host
            prefs.explorePort = port
            prefs.exploreFreqMhz = freqMhz
            mainRoot.exploreSetupOpen = false
            mainRoot.startWithMap(mainRoot.exploreSystem(sourceType, host, port, freqMhz), -1)
        }
    }

    // ---- Live monitor (owns the screen while a session is active) ----
    MonitorScreen {
        id: monitor

        objectName: "monitorScreen"
        anchors.fill: safeArea
        system: mainRoot.sessionSystem
        opacity: mainRoot.monitorMode ? 1.0 : 0.0
        visible: opacity > 0.0
        // The wizard ("Save as a system"), the spectrum, and the RadioReference
        // screen the wizard pushes from its tune step all open over a running
        // session, and TapHandlers never take exclusive grabs, so without this a
        // tap on the layer above also lands on "Stop listening", which sits at
        // exactly the same rect underneath all three and ends the session.
        // That is what "Explore from here" — the one way out of view-only — did
        // instead of offering to hand the tuner over. The RadioReference term is
        // what lets that screen stay lit over the monitor rather than standing
        // down into a three-layer deadlock.
        enabled: opacity > 0.9 && !mainRoot.wizardOpen && !mainRoot.spectrumOpen
                 && !mainRoot.radioReferenceOpen

        Behavior on opacity {
            NumberAnimation { duration: 150; easing.type: Easing.OutCubic }
        }

        onOpenSpectrum: {
            spectrumLoader.active = true
            mainRoot.spectrumOpen = true
        }
        onEditSystem: {
            // Only a session started from a saved row has a system to edit; a
            // reattached or quick-start session has no row to write back to.
            if (mainRoot.sessionRow >= 0) {
                wizard.openForEdit(mainRoot.sessionRow)
                mainRoot.wizardOpen = true
            }
        }
    }

    // ---- Spectrum (pushed over the monitor) ----
    // Built on first open, then kept. Its waterfall holds a full-resolution
    // history image, which is real memory to hand every user who never opens
    // the view — and rebuilding it on each visit would throw that history away.
    Loader {
        id: spectrumLoader

        anchors.fill: safeArea
        active: false
        sourceComponent: spectrumScreen
        opacity: mainRoot.monitorMode && mainRoot.spectrumOpen && !mainRoot.wizardOpen ? 1.0 : 0.0
        visible: opacity > 0.0
        // The wizard can now open over a running session ("Save as a system"), and
        // TapHandlers never take exclusive grabs — without this, a tap meant for a
        // wizard field also reaches the spectrum underneath and retunes the radio.
        enabled: opacity > 0.9 && !mainRoot.wizardOpen

        Behavior on opacity {
            NumberAnimation { duration: 150; easing.type: Easing.OutCubic }
        }
    }

    Component {
        id: spectrumScreen

        SpectrumScreen {
            exploring: mainRoot.exploring

            onClosed: mainRoot.spectrumOpen = false
            onExploreFromHere: {
                // The engine is the authority on who owns the tuner; this only
                // asks. The pill follows the options snapshot, so it flips a poll
                // later whether or not the request was honoured.
                commands.releaseTuner()
                mainRoot.exploring = true
                // Seeded here rather than waiting for the next tuner reading: the
                // session may never move again, and an empty value would leave the
                // stop handler with nothing to remember this band by.
                //
                // Read once and guarded once. A zero reading — no tuner, or the
                // options snapshot briefly unavailable — is not a frequency, and
                // Util.mhzText(0) is the string "0.0000", which the session map,
                // the monitor header and the save-as-system wizard would all then
                // carry as if it were one. Empty is what monitorMeta() already
                // knows to leave out.
                var exploreFreqMhz = metrics.centerFreqHz > 0 ? Util.mhzText(metrics.centerFreqHz) : ""
                if (exploreFreqMhz.length > 0)
                    mainRoot.lastExploreFreqMhz = exploreFreqMhz
                // The session is no longer the saved system it started as: its
                // frequency is now whatever the user makes it, and the calls it
                // logs from here on did not come from that system.
                mainRoot.sessionSystem = mainRoot.exploreSystem(
                    mainRoot.sessionSystem ? mainRoot.sessionSystem.sourceType : "usb",
                    mainRoot.sessionSystem ? mainRoot.sessionSystem.host : "",
                    mainRoot.sessionSystem ? mainRoot.sessionSystem.port : 0,
                    exploreFreqMhz)
                // Which also means the saved row is no longer this session's, so
                // the monitor's edit gesture must not open — and push CSVs into —
                // a system the session detached from.
                mainRoot.sessionRow = -1
                uiController.flushHistory()
                callHistory.sessionLabel = qsTr("Exploring")
            }
            onSaveAsSystem: function (freqHz) {
                wizard.openForFound(mainRoot.sessionSystem, Util.mhzText(freqHz))
                mainRoot.wizardOpen = true
            }
        }
    }

    // ---- Add-system wizard (pushed) ----
    // Declared after the monitor and the spectrum because declaration order is
    // z-order among siblings: the monitor paints an opaque background, so a
    // wizard declared before it would be visible, enabled and completely covered.
    WizardScreen {
        id: wizard

        objectName: "wizardScreen"

        anchors.fill: safeArea
        opacity: mainRoot.wizardOpen ? 1.0 : 0.0
        visible: opacity > 0.0
        // Stands down for the RadioReference screen it can push, for the same
        // reason the library and that screen stand down for the monitor: the
        // RadioReference screen is a plain Item with no full-bleed input
        // handler, so a tap on its header strip or button margins would
        // otherwise land on the wizard field underneath as well.
        enabled: opacity > 0.9 && !mainRoot.radioReferenceOpen

        Behavior on opacity {
            NumberAnimation { duration: 150; easing.type: Easing.OutCubic }
        }

        onClosed: {
            mainRoot.wizardOpen = false
            // Over a session the monitor comes back underneath; without this it
            // returns behind the keyboard the name field was still holding.
            Qt.inputMethod.hide()
        }
        onOpenRadioReference: mainRoot.openRadioReference(true)
        onSaved: function (row) {
            // Saving the system the running session was started from applies its
            // CSV files live — the session's argv was built at start, so without
            // this a changed talkgroup list would silently wait for a restart.
            // Only files that actually changed are pushed: re-importing a channel
            // map replaces the live one wholesale (discarding anything the
            // protocol learned since), and re-importing keys bumps the
            // encrypted-target key epoch, so a name-only edit must not do either.
            if (decoderHost.running && row >= 0 && row === mainRoot.sessionRow) {
                var was = mainRoot.sessionSystem || {}
                var sys = savedSystems.get(row)
                // Clearing a picker to "None" is a change like any other, and
                // only the clear commands can express it — the import commands
                // all reject an empty path. Without this the file the user just
                // deselected keeps mapping channels, naming talkgroups and
                // decrypting for the rest of the session.
                if (sys.chanCsvPath !== was.chanCsvPath) {
                    if (sys.chanCsvPath.length > 0)
                        commands.importChannelMap(sys.chanCsvPath)
                    else if ((was.chanCsvPath || "").length > 0)
                        commands.clearChannelMap()
                }
                if (sys.groupCsvPath !== was.groupCsvPath) {
                    if (sys.groupCsvPath.length > 0)
                        commands.importGroupList(sys.groupCsvPath)
                    else if ((was.groupCsvPath || "").length > 0)
                        commands.clearGroupList()
                }
                if (sys.keyCsvPath !== was.keyCsvPath || sys.keyCsvHex !== was.keyCsvHex) {
                    if (sys.keyCsvPath.length > 0)
                        commands.importKeys(sys.keyCsvPath, sys.keyCsvHex)
                    else if ((was.keyCsvPath || "").length > 0)
                        commands.clearKeys()
                }
                // The monitor header reads sessionSystem; without this it keeps
                // naming and metering the system as it was before the edit.
                mainRoot.sessionSystem = sys
            }
            mainRoot.wizardOpen = false
            mainRoot.currentTab = 0
            Qt.inputMethod.hide()
        }
    }

    // ---- Imported-files library (pushed from Settings) ----
    // The monitor owns the screen once a session goes active, so this layer
    // stands down for it the way the explore setup and onboarding do — two lit,
    // enabled full-screen layers means one tap lands on both, and the bottom
    // "Import file" button sits exactly over "Stop listening".
    ImportsScreen {
        objectName: "importsScreen"

        anchors.fill: safeArea
        // So a RadioReference refresh can tell whether the file it just replaced
        // is one the running session is actually using.
        sessionSystem: mainRoot.sessionSystem
        opacity: mainRoot.importsOpen && !mainRoot.monitorMode ? 1.0 : 0.0
        visible: opacity > 0.0
        enabled: opacity > 0.9

        Behavior on opacity {
            NumberAnimation { duration: 150; easing.type: Easing.OutCubic }
        }

        onClosed: mainRoot.importsOpen = false
        onOpenRadioReference: mainRoot.openRadioReference(false)
    }

    // ---- RadioReference import (pushed from Settings, the library, or the
    // wizard) ----
    // Declared after the imports library because declaration order is z-order
    // among siblings and this opens over it.
    //
    // Unlike the library, this does NOT stand down for the monitor. The wizard
    // opens over a running session ("Save as a system") and pushes this screen
    // from its tune step, so a !monitorMode term here left all three layers
    // inert at once — this one unlit and disabled, the wizard disabled by its
    // own !radioReferenceOpen term, the monitor disabled by !wizardOpen — with
    // no back key anywhere in the tree to escape it. It owns a full-bleed
    // opaque Theme.bg backdrop, so it covers the session exactly as the wizard
    // does; the monitor gives up its taps below instead.
    RadioReferenceScreen {
        id: radioReferenceScreen

        objectName: "radioReferenceScreen"

        anchors.fill: safeArea
        opacity: mainRoot.radioReferenceOpen ? 1.0 : 0.0
        visible: opacity > 0.0
        enabled: opacity > 0.9

        Behavior on opacity {
            NumberAnimation { duration: 150; easing.type: Easing.OutCubic }
        }

        onClosed: {
            mainRoot.radioReferenceOpen = false
            Qt.inputMethod.hide()
        }
        // The wizard is the single writer of a saved system, so the generated
        // files and the tune answers go to it rather than to savedSystems.add().
        onImported: function (result) {
            mainRoot.radioReferenceOpen = false
            Qt.inputMethod.hide()
            if (!mainRoot.radioReferenceFromWizard) {
                // Opened from Settings or the library: the source, gain and name
                // are still unanswered, so the wizard asks for them.
                mainRoot.importsOpen = false
                wizard.openForAdd(false)
                mainRoot.wizardOpen = true
            }
            mainRoot.radioReferenceFromWizard = false
            wizard.applyRadioReference(result)
        }
    }

    // ---- First-run onboarding ----
    OnboardingScreen {
        anchors.fill: safeArea
        opacity: !prefs.onboardingDone && !mainRoot.monitorMode ? 1.0 : 0.0
        visible: opacity > 0.0
        enabled: opacity > 0.9

        Behavior on opacity {
            NumberAnimation { duration: 150; easing.type: Easing.OutCubic }
        }

        onGetStarted: prefs.onboardingDone = true
        onNetworkSource: {
            prefs.onboardingDone = true
            wizard.openForAdd(true)
            mainRoot.wizardOpen = true
        }
    }
}
