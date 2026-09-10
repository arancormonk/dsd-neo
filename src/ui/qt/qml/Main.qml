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
    property var attemptedSource: null
    property string completionMessage: ""
    function sourceLabel() {
        var source = attemptedSource ? attemptedSource.system : null;
        if (!source)
            return "";
        var label = source.sourceType === "rtltcp" ? "RTL-TCP " + source.host + ":" + source.port : source.sourceType === "tcp" ? "TCP audio " + source.host + ":" + source.port : source.sourceType === "udp" ? qsTr("UDP audio port %1").arg(source.port) : source.sourceType === "file" ? qsTr("Replay: %1").arg(source.filePath.substring(source.filePath.lastIndexOf('/') + 1)) : qsTr("USB RTL-SDR");
        return source.extraArgs ? qsTr("Configured source: %1 (Advanced arguments are also in use)").arg(label) : label;
    }
    function failureMessage() {
        var host = decoderHost;
        var kind = host.inputFailureKind || 0;
        if (startError.length)
            return startError;
        if (kind === 1)
            return qsTr("Connection refused. Check the server address, port, and whether the server is running.");
        if (kind === 2)
            return qsTr("Connection timed out. Check the server and network connection.");
        if (kind === 3)
            return qsTr("The host could not be found. Check its name or enter its IP address.");
        if (kind === 4)
            return qsTr("The network connection could not be opened.");
        if (kind === 5)
            return qsTr("The replay file could not be opened. Reselect an available, supported file.");
        if (kind === 6)
            return qsTr("The source configuration could not be applied. Review its settings and imported files.");
        return awaitingUsbAccess ? usbAccessText : qsTr("The source could not be started. Review its settings or open Details.");
    }
    function retrySource() {
        if (!attemptedSource || decoderHost.sessionActive || decoderHost.transitioning)
            return;
        pendingRestart = {
            kind: attemptedSource.kind,
            uid: attemptedSource.uid
        };
        if (decoderHost.sessionState === 4)
            decoderHost.stop();
        if (decoderHost.sessionState === 0)
            Qt.callLater(finishPendingRestart);
    }
    function editFailedSource() {
        cancelPendingRestart();
        if (!attemptedSource)
            return;
        if (attemptedSource.kind === "explore") {
            exploreSetup.reset(prefs.exploreSourceType, prefs.exploreHost, prefs.explorePort, prefs.exploreFreqMhz);
            exploreSetupOpen = true;
        } else {
            var row = attemptedSource.kind === "scan" ? scanLists.rowForUid(attemptedSource.uid) : savedSystems.rowForUid(attemptedSource.uid);
            if (row < 0) {
                startError = qsTr("This source was removed. Choose another saved source.");
                return;
            }
            if (attemptedSource.kind === "scan") {
                scanListEditor.openFor(row);
                scanListOpen = true;
            } else {
                wizard.openForEdit(row);
                wizardOpen = true;
            }
        }
    }
    // Set while a USB start is blocked on device access, so the platform's live
    // detail ("USB permission denied", "No RTL-SDR attached") reaches the screen
    // as it changes — the permission dialog answers long after the tap that
    // asked. Cleared when access is granted (which resumes the pending start
    // below), when the banner is dismissed, or when another system starts.
    property bool awaitingUsbAccess: false
    // WP-D4: a restart waits for confirmed Idle and resolves stable identity then.
    property var pendingRestart: null
    function cancelPendingRestart() {
        pendingRestart = null;
    }
    function restartSite(uid) {
        cancelPendingRestart();
        if (decoderHost.sessionState !== 2 || savedSystems.rowForUid(uid) < 0)
            return;
        pendingStart = null;
        pendingStartRow = -1;
        awaitingUsbAccess = false;
        pendingRestart = {
            uid: uid
        };
        decoderHost.stop();
    }
    function finishPendingRestart() {
        if (!pendingRestart || decoderHost.sessionState !== 0)
            return;
        var request = pendingRestart;
        pendingRestart = null;
        if (request.kind === "explore") {
            startExploring();
            return;
        }
        var scan = request.kind === "scan";
        var row = scan ? scanLists.rowForUid(request.uid) : savedSystems.rowForUid(request.uid);
        if (row < 0) {
            startError = qsTr("This source was removed. Choose another source.");
            return;
        }
        if (scan)
            startScanList(row);
        else if (!savedSystems.get(row).avoidSite)
            startSystem(row);
    }
    // The system map waiting for USB access and its row (-1 for exploration).
    property var pendingStart: null
    property int pendingStartRow: -1
    // WP-S1 shares the saved-session USB gate and initialization bookkeeping.
    property bool pendingScanList: false
    property bool sessionScanList: false
    property bool scanListOpen: false
    property string scanWarnings: ""
    readonly property string usbAccessText: awaitingUsbAccess && decoderHost && !decoderHost.localDeviceReady ? decoderHost.localDeviceStatus : ""
    readonly property string failureText: startError.length > 0 ? startError : usbAccessText.length > 0 ? usbAccessText : hostFailure

    // WP-S2: consume each attachment once; blocked events are never deferred.
    Binding {
        target: uiController
        property: "autoStartBlocked"
        value: mainRoot.wizardOpen || mainRoot.scanListOpen || mainRoot.exploreSetupOpen || mainRoot.diagnosticsOpen || mainRoot.importsOpen || mainRoot.radioReferenceOpen || mainRoot.spectrumOpen || mainRoot.talkgroupsOpen || mainRoot.awaitingUsbAccess || homeScreen.managementSheetOpen || siteChooser.visible
    }
    Connections {
        target: uiController
        function onAutoStartRequested(kind, uid) {
            var row = kind === "saved" ? savedSystems.rowForUid(uid) : kind === "scan" ? scanLists.rowForUid(uid) : -1;
            if (row < 0)
                return;
            if (kind === "saved")
                mainRoot.startSystem(row);
            else
                mainRoot.startScanList(row);
        }
    }

    readonly property bool expanded: safeArea.width >= 840
    property int currentTab: 0
    property string sessionDestination: ""
    function requestBack() {
        cancelPendingRestart();
        if (Navigation.back(mainRoot))
            return;
        if (currentTab !== 0 && !monitorMode) {
            currentTab = 0;
            return;
        }
        if (!prefs.backgroundListening && sessionActive())
            decoderHost.stop();
        if (!decoderHost.moveToBackground())
            mainRoot.close();
    }
    function sessionActive() {
        return decoderHost && decoderHost.sessionActive;
    }
    function updateSystemBars() {
        var host = decoderHost;
        if (host && typeof host.setDarkAppearance === "function")
            host.setDarkAppearance(Theme.dark);
    }
    Component.onCompleted: updateSystemBars()
    onActiveChanged: {
        if (active)
            updateSystemBars();
    }
    Connections {
        target: Theme
        function onDarkChanged() {
            mainRoot.updateSystemBars();
        }
    }
    Connections {
        target: decoderHost
        ignoreUnknownSignals: true
        function onBackRequested() {
            mainRoot.requestBack();
        }
    }
    Binding {
        target: Navigation
        property: "rootSurfaces"
        value: mainRoot.monitorMode ? [monitor] : !prefs.onboardingDone ? [onboarding] : [nav, mainRoot.currentTab === 0 ? homeScreen : mainRoot.currentTab === 1 ? historyRoot : settingsRoot]
    }
    NavigationLayer {
        surface: wizard
        active: mainRoot.wizardOpen
        onLeave: wizard.requestBack()
    }
    NavigationLayer {
        surface: exploreSetup
        active: mainRoot.exploreSetupOpen
        onLeave: exploreSetup.requestClose()
    }
    NavigationLayer {
        surface: scanListEditor
        active: mainRoot.scanListOpen
        onLeave: scanListEditor.requestClose()
    }
    NavigationLayer {
        surface: spectrumLoader
        active: mainRoot.spectrumOpen
        onLeave: mainRoot.spectrumOpen = false
    }
    NavigationLayer {
        surface: talkgroupsScreen
        active: mainRoot.talkgroupsOpen
        onLeave: mainRoot.talkgroupsOpen = false
    }
    NavigationLayer {
        surface: sessionTools
        active: mainRoot.sessionDestination.length > 0
        onLeave: mainRoot.sessionDestination = ""
    }
    NavigationLayer {
        surface: importsScreen
        active: mainRoot.importsOpen && (!mainRoot.monitorMode || mainRoot.sessionDestination === "settings")
        onLeave: mainRoot.importsOpen = false
    }
    NavigationLayer {
        surface: diagnosticsScreen
        active: mainRoot.diagnosticsOpen
        onLeave: mainRoot.diagnosticsOpen = false
    }
    NavigationLayer {
        surface: radioReferenceScreen
        active: mainRoot.radioReferenceOpen
        onLeave: mainRoot.radioReferenceOpen = false
    }
    property bool wizardOpen: false
    property bool exploreSetupOpen: false
    property bool diagnosticsOpen: false // WP-F5
    property bool importsOpen: false
    onRadioReferenceOpenChanged: {
        if (!radioReferenceOpen)
            radioReference.cancel();
    }
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
    property bool talkgroupsOpen: false
    // The saved-system map the running session was started from.
    property var sessionSystem: null
    property bool awaitingSessionInitialized: false

    property bool sessionReachedRunning: false

    function recordSessionInitialized() {
        if (!mainRoot.awaitingSessionInitialized || !mainRoot.sessionSystem)
            return;
        mainRoot.awaitingSessionInitialized = false;
        // Accepted argv is only a request. These writes belong to the engine's
        // post-initialization edge, after file validation and tuner open.
        if (Qt.platform.os === "android" && prefs.backgroundListening && !prefs.notificationExplained)
            notificationExplanation.visible = true;
        prefs.lastStartedKind = mainRoot.sessionScanList ? "scan" : mainRoot.exploring ? "explore" : "saved";
        prefs.lastStartedUid = mainRoot.exploring ? "" : mainRoot.sessionSystem.uid || "";
        if (mainRoot.sessionScanList)
            scanLists.touch(scanLists.rowForUid(prefs.lastStartedUid));
        else if (!mainRoot.exploring)
            savedSystems.touch(savedSystems.rowForUid(prefs.lastStartedUid));
    }
    Connections {
        target: decoderHost
        function onSessionInitialized() {
            mainRoot.recordSessionInitialized();
        }
        function onSessionStateChanged() {
            if (decoderHost.sessionState === 4)
                mainRoot.cancelPendingRestart();
            if (decoderHost.sessionState === 0 && mainRoot.attemptedSource && mainRoot.attemptedSource.system.sourceType === "file") {
                var host = decoderHost;
                mainRoot.completionMessage = host.terminalReason === 1 ? qsTr("Replay completed: %1").arg(mainRoot.attemptedSource.system.filePath.substring(mainRoot.attemptedSource.system.filePath.lastIndexOf('/') + 1)) : host.terminalReason === 2 ? qsTr("Replay stopped") : "";
            }
            if (decoderHost.sessionState === 0 && mainRoot.pendingRestart)
                Qt.callLater(mainRoot.finishPendingRestart);

            if (decoderHost.sessionState === 2) {
                mainRoot.sessionReachedRunning = true;

                if (!decoderHost.signalsSessionInitialized)
                    mainRoot.recordSessionInitialized();
            } else if (decoderHost.sessionState === 0 && mainRoot.sessionReachedRunning) {
                mainRoot.awaitingSessionInitialized = false;
            }
        }
    }
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
                mainRoot.lastExploreFreqMhz = Util.mhzText(metrics.centerFreqHz);
        }
    }

    // Suppresses a failure banner the user has read. Reset on the next start so a
    // repeat of the same failure is reported again rather than swallowed.
    property string dismissedFailure: ""
    readonly property bool showFailure: !monitorMode && failureText.length > 0 && failureText !== dismissedFailure

    // Dismissing the banner abandons a start still waiting on USB access; the
    // flag must not linger, or a dongle detached minutes later while idle would
    // resurrect a permission banner the user never asked about.
    onDismissedFailureChanged: {
        if (dismissedFailure.length > 0) {
            awaitingUsbAccess = false;
            mainRoot.pendingStart = null;
            mainRoot.pendingStartRow = -1;
        }
    }

    // The USB permission dialog answers long after the tap that asked: when the
    // grant lands, finish that start instead of leaving a screen that looks like
    // nothing happened until a second tap.
    Connections {
        target: decoderHost
        function onLocalDeviceChanged() {
            if (!mainRoot.awaitingUsbAccess || !decoderHost.localDeviceReady)
                return;
            mainRoot.awaitingUsbAccess = false;
            var sys = mainRoot.pendingStart;
            var row = mainRoot.pendingStartRow;
            var scan = mainRoot.pendingScanList;
            mainRoot.pendingStart = null;
            mainRoot.pendingStartRow = -1;
            if (sys)
                mainRoot.startWithMap(sys, row, scan);
        }
    }

    onMonitorModeChanged: {
        // Pushed session screens must close with the session rather than showing
        // stale content when the next one starts.
        if (!monitorMode) {
            // Where the exploring got to, so the next one resumes there rather
            // than back at the start frequency.
            if (mainRoot.exploring && mainRoot.lastExploreFreqMhz.length > 0)
                prefs.exploreFreqMhz = mainRoot.lastExploreFreqMhz;
            mainRoot.exploring = false;
            mainRoot.spectrumOpen = false;
            mainRoot.talkgroupsOpen = false;
            // Row indices shift when a system is removed, and Home is reachable
            // again from here; a row remembered past its session would name a
            // different system by the time anything read it.
            mainRoot.sessionRow = -1;
        }
        if (monitorMode) {
            // The frequency field usually still holds focus; the keyboard would
            // cover the session that just appeared.
            Qt.inputMethod.hide();
            // Reattaching to a session this UI process did not start (service
            // survived an Activity restart): bound the recent-calls pane to the
            // last hour rather than the whole persisted log.
            if (monitorView.minWhen === 0)
                monitorView.minWhen = Math.floor(Date.now() / 1000) - 3600;
            if (talkgroups.sinceWhen === 0)
                talkgroups.sinceWhen = monitorView.minWhen;
        }
    }

    // Closing the window finishes the Android Activity, and Qt then terminates the
    // process — taking the service that owns the engine with it. Background instead
    // when the host can; and when background listening is off, stop first so the
    // radio does not keep playing from a window the user just dismissed.
    onClosing: function (close) {
        cancelPendingRestart();
        if (Navigation.back(mainRoot)) {
            close.accepted = false;
            return;
        }
        if (!prefs.backgroundListening && mainRoot.monitorMode)
            decoderHost.stop();
        close.accepted = !decoderHost.moveToBackground();
    }

    function startSystem(row) {
        cancelPendingRestart();
        if (row < 0 || (decoderHost.sessionState !== 0 && decoderHost.sessionState !== 4))
            return;
        var sys = savedSystems.get(row);
        if (sys)
            mainRoot.startWithMap(sys, row, false);
    }

    function scanTargetLabel() {
        var list = mainRoot.sessionSystem;
        if (!list || !list.entries)
            list = prefs.lastStartedKind === "scan" ? scanLists.getByUid(prefs.lastStartedUid) : null;
        var entries = list && list.entries ? list.entries : [];
        for (var i = 0; i < entries.length; ++i) {
            var entry = entries[i];
            if (entry.uid !== metrics.scanTargetId)
                continue;
            if (entry.kind === "system")
                return savedSystems.getByUid(entry.systemUid).name || qsTr("Saved system");
            return entry.name || (entry.freqMhz ? entry.freqMhz + " MHz" : qsTr("Frequency"));
        }
        return qsTr("Target %1").arg(metrics.scanTargetOrdinal);
    }

    function startScanList(row) {
        cancelPendingRestart();
        var list = scanLists.get(row);
        if (list && list.uid)
            mainRoot.startWithMap(list, row, true);
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
    function startWithMap(sys, row, scan) {
        cancelPendingRestart();
        mainRoot.awaitingSessionInitialized = false;
        mainRoot.sessionReachedRunning = false;
        if (!sys || !sys.sourceType)
            return;
        attemptedSource = {
            kind: scan ? "scan" : row < 0 ? "explore" : "saved",
            uid: sys.uid || "",
            system: Object.assign({}, sys)
        };
        completionMessage = "";
        if (sys.sourceType === "usb" && decoderHost.localDeviceBrokered && !decoderHost.localDeviceReady) {
            // Not a silent return: the platform's status line is the only thing
            // that can say why ("USB permission denied", "No RTL-SDR attached"),
            // and it keeps updating as the permission dialog resolves. When the
            // grant lands, the Connections above resumes this start.
            mainRoot.dismissedFailure = "";
            mainRoot.startError = "";
            mainRoot.awaitingUsbAccess = true;
            mainRoot.pendingStart = sys;
            mainRoot.pendingStartRow = row;
            mainRoot.pendingScanList = !!scan;
            decoderHost.requestLocalDeviceAccess();
            return;
        }
        mainRoot.dismissedFailure = "";
        mainRoot.startError = "";
        mainRoot.awaitingUsbAccess = false;
        mainRoot.pendingStart = null;
        mainRoot.pendingStartRow = -1;
        var built = scan ? scanListStarter.start(sys, decoderHost) : sessionArgs.start(sys, decoderHost);
        if (!built.ok) {
            // Match the rejected field without exposing a prohibited argument or a key.
            if (scan) {
                mainRoot.startError = built.error;
            } else if (built.error === "frequency") {
                mainRoot.startError = qsTr("“%1” has no valid frequency. Edit the source to correct it.").arg(sys.name);
            } else if (built.error === "ppm") {
                mainRoot.startError = qsTr("“%1” has an invalid PPM correction. Edit the source to correct it.").arg(sys.name);
            } else if (built.error === "hangtime") {
                mainRoot.startError = qsTr("Enter hang time in seconds from 0 to 30.");
            } else if (built.error === "encryption") {
                mainRoot.startError = qsTr("“%1” has an invalid decryption configuration. Edit the source to correct it.").arg(sys.name);
            } else if (built.error === "unsafe-option") {
                mainRoot.startError = qsTr("The session contains a prohibited extra option or grouped short options. Remove prohibited options and write each short option separately.");
            } else {
                mainRoot.startError = qsTr("The session options are invalid. Review them before starting.");
            }
            return;
        }
        // Side effects only after the host accepts: a refused start must not
        // stamp lastHeard, re-attribute history rows, or hide the previous
        // session's calls from the monitor pane.
        if (!(built.started)) {
            if (decoderHost.failureText.length === 0)
                mainRoot.startError = qsTr("“%1” could not be started.").arg(sys.name);
            return;
        }
        mainRoot.exploring = (row < 0);
        mainRoot.sessionSystem = sys;
        mainRoot.sessionScanList = !!scan;
        mainRoot.scanWarnings = scan ? built.warnings.join("\n") : "";
        mainRoot.awaitingSessionInitialized = true;
        mainRoot.sessionReachedRunning = decoderHost.sessionState === 2;
        if (mainRoot.sessionReachedRunning && !decoderHost.signalsSessionInitialized)
            mainRoot.recordSessionInitialized();
        mainRoot.sessionRow = scan ? -1 : row;
        // The session's intent, decided here and nowhere else: a system someone
        // saved is a thing to listen to, and the spectrum watches it. Only a
        // session with no saved system behind it is free to wander.
        mainRoot.exploring = !scan && (row < 0);
        // Belongs to the session that just ended. Carried into this one it would be
        // written back to prefs on stop as if it were where this exploring got to —
        // a frequency from two sessions ago, on a session that may never have moved.
        mainRoot.lastExploreFreqMhz = "";
        // The previous session may have committed calls since the last 250 ms
        // tick; ingest them under its own label before the label changes hands,
        // or its tail calls read as the new system's.
        uiController.flushHistory();
        callHistory.sessionLabel = sys.name;
        callHistory.sessionUid = (!scan && sys.uid) ? sys.uid : "";
        // The monitor's recent-calls pane shows this session, not the whole log.
        monitorView.minWhen = Math.floor(Date.now() / 1000);
        talkgroups.sinceWhen = monitorView.minWhen;
    }

    /**
     * Push the RadioReference import screen.
     *
     * @a fromWizard records where it was opened from, which is what decides
     * whether the finished import fills in an open wizard or opens a fresh one.
     */
    function openRadioReference(fromWizard) {
        cancelPendingRestart();
        mainRoot.radioReferenceFromWizard = fromWizard;
        radioReferenceScreen.reset();
        mainRoot.radioReferenceOpen = true;
    }

    /** Start an explore session from the remembered source and frequency. */
    function startExploring() {
        cancelPendingRestart();
        var source = prefs.exploreSourceType;
        if (source !== "usb" && source !== "rtltcp") {
            // Nothing remembered to start from; ask instead of guessing.
            mainRoot.exploreSetupOpen = true;
            return;
        }
        mainRoot.startWithMap(mainRoot.exploreSystem(source, prefs.exploreHost, prefs.explorePort, prefs.exploreFreqMhz), -1, false);
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
        };
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
        focus: true
        Keys.onEscapePressed: mainRoot.requestBack()
        Keys.onBackPressed: mainRoot.requestBack()
        anchors.fill: parent
        anchors.topMargin: mainRoot.SafeArea.margins.top
        anchors.leftMargin: mainRoot.SafeArea.margins.left
        anchors.rightMargin: mainRoot.SafeArea.margins.right
        anchors.bottomMargin: mainRoot.SafeArea.margins.bottom
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: mainRoot.SafeArea.margins.bottom
        color: nav.color
        visible: shell.visible
    }
    // ---- Tab shell ----
    Item {
        id: shell

        anchors.fill: safeArea
        opacity: (mainRoot.monitorMode || mainRoot.wizardOpen || mainRoot.exploreSetupOpen || mainRoot.diagnosticsOpen || mainRoot.importsOpen || mainRoot.radioReferenceOpen || mainRoot.scanListOpen || mainRoot.sessionDestination.length > 0 || !prefs.onboardingDone) ? 0.0 : 1.0
        visible: opacity > 0.0
        enabled: opacity > 0.9 && !siteChooser.visible

        HomeScreen {
            id: homeScreen
            objectName: "homeScreen"
            failure: ({
                    message: mainRoot.showFailure ? mainRoot.failureMessage() : "",
                    source: mainRoot.sourceLabel(),
                    canRetry: !decoderHost.sessionActive && !decoderHost.transitioning
                })
            usbRelevant: !mainRoot.showFailure || !mainRoot.attemptedSource || mainRoot.attemptedSource.system.sourceType === "usb"
            completionMessage: mainRoot.completionMessage
            onRetryFailure: mainRoot.retrySource()
            onEditFailure: mainRoot.editFailedSource()
            onDismissFailure: mainRoot.dismissedFailure = mainRoot.failureText
            onFailureDetails: failureDetails.visible = true
            onReplayAgain: mainRoot.retrySource()
            x: mainRoot.expanded ? nav.width : 0
            y: 0
            width: parent.width - x
            height: parent.height - (mainRoot.expanded ? 0 : nav.height)
            visible: mainRoot.currentTab === 0

            onAddSystem: {
                wizard.openForAdd(false);
                mainRoot.wizardOpen = true;
            }
            onNetworkSource: {
                wizard.openForAdd(true);
                mainRoot.wizardOpen = true;
            }
            onChooseSites: function (row) {
                mainRoot.cancelPendingRestart();
                siteChooser.openFor(row);
            }
            onPlaySystem: function (row) {
                mainRoot.startSystem(row);
            }
            onPlayScanList: function (row) {
                mainRoot.startScanList(row);
            }
            onEditScanList: function (row) {
                scanListEditor.openFor(row);
                mainRoot.scanListOpen = true;
            }
            onAddScanList: {
                scanListEditor.openFor(-1);
                mainRoot.scanListOpen = true;
            }
            onEditSystem: function (row) {
                wizard.openForEdit(row);
                mainRoot.wizardOpen = true;
            }
            onExplore: mainRoot.startExploring()
            onExploreSetup: {
                exploreSetup.reset(prefs.exploreSourceType, prefs.exploreHost, prefs.explorePort, prefs.exploreFreqMhz);
                mainRoot.exploreSetupOpen = true;
            }
        }

        HistoryScreen {
            id: historyRoot
            x: mainRoot.expanded ? nav.width : 0
            y: 0
            width: parent.width - x
            height: parent.height - (mainRoot.expanded ? 0 : nav.height)
            visible: mainRoot.currentTab === 1
        }

        SettingsScreen {
            id: settingsRoot
            x: mainRoot.expanded ? nav.width : 0
            y: 0
            width: parent.width - x
            height: parent.height - (mainRoot.expanded ? 0 : nav.height)
            visible: mainRoot.currentTab === 2

            onOpenDiagnostics: mainRoot.diagnosticsOpen = true
            onOpenImports: mainRoot.importsOpen = true
            onOpenRadioReference: mainRoot.openRadioReference(false)
        }

        BottomNav {
            id: nav
            objectName: "primaryNavigation"
            x: 0
            y: mainRoot.expanded ? 0 : parent.height - height
            width: mainRoot.expanded ? Math.max(104, Theme.fontSize(11) * 6 + 24) : parent.width
            height: mainRoot.expanded ? parent.height : Math.max(62, Theme.fontSize(11) + 46)
            vertical: mainRoot.expanded
            currentIndex: mainRoot.currentTab
            onSelected: function (index) {
                mainRoot.cancelPendingRestart();
                mainRoot.currentTab = index;
            }
        }
    }

    // ---- Explore setup (pushed; idle only, since it starts a session) ----
    ExploreSetupScreen {
        id: exploreSetup

        anchors.fill: safeArea
        opacity: mainRoot.exploreSetupOpen && !mainRoot.monitorMode ? 1.0 : 0.0
        visible: opacity > 0.0
        enabled: opacity > 0.9

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
            prefs.exploreSourceType = sourceType;
            prefs.exploreHost = host;
            prefs.explorePort = port;
            prefs.exploreFreqMhz = freqMhz;
            mainRoot.exploreSetupOpen = false;
            mainRoot.startWithMap(mainRoot.exploreSystem(sourceType, host, port, freqMhz), -1, false);
        }
    }

    UiPanel {
        z: 21
        visible: mainRoot.monitorMode && mainRoot.scanWarnings.length > 0
        anchors.left: safeArea.left
        anchors.right: safeArea.right
        anchors.bottom: safeArea.bottom
        height: scanWarningText.implicitHeight + 32
        Text {
            id: scanWarningText
            anchors.fill: parent
            anchors.margins: 16
            text: mainRoot.scanWarnings + qsTr("\nTap to dismiss")
            wrapMode: Text.Wrap
            color: Theme.textPrimary
            font.family: Theme.sans
        }
        PointerBarrier {
            onClicked: mainRoot.scanWarnings = ""
        }
    }

    // WP-S1 editor, kept instantiated so closing a keyboard does not discard edits.
    ScanListScreen {
        id: scanListEditor
        anchors.fill: safeArea
        visible: mainRoot.scanListOpen
        z: 20
        onClosed: mainRoot.scanListOpen = false
    }

    // ---- Live monitor (owns the screen while a session is active) ----
    MonitorScreen {
        id: monitor

        objectName: "monitorScreen"
        anchors.fill: safeArea
        system: mainRoot.sessionSystem
        scanTargetName: mainRoot.scanTargetLabel()
        sitesAvailable: mainRoot.running && !mainRoot.diagnosticsOpen && !mainRoot.importsOpen && mainRoot.sessionSystem && savedSystems.siteCount(savedSystems.rowForUid(mainRoot.sessionSystem.uid || "")) > 0
        onOpenSites: {
            mainRoot.cancelPendingRestart();
            siteChooser.openFor(savedSystems.rowForUid(mainRoot.sessionSystem.uid));
        }
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
        enabled: opacity > 0.9 && !mainRoot.wizardOpen && !mainRoot.spectrumOpen && !mainRoot.radioReferenceOpen && !mainRoot.talkgroupsOpen && !talkgroupsScreen.visible && !siteChooser.visible && !mainRoot.diagnosticsOpen && !(mainRoot.importsOpen && mainRoot.sessionDestination === "settings") && mainRoot.sessionDestination.length === 0

        onOpenSpectrum: {
            spectrumLoader.active = true;
            mainRoot.spectrumOpen = true;
        }
        onOpenTalkgroups: mainRoot.talkgroupsOpen = true
        onOpenSessionMenu: sessionMenu.visible = true
        onEditSystem: {
            // Only a session started from a saved row has a system to edit; a
            // reattached or quick-start session has no row to write back to.
            if (mainRoot.sessionRow >= 0) {
                wizard.openForEdit(mainRoot.sessionRow);
                mainRoot.wizardOpen = true;
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
        enabled: opacity > 0.9 && !mainRoot.wizardOpen && !talkgroupsScreen.visible
    }

    Component {
        id: spectrumScreen

        SpectrumScreen {
            objectName: "spectrumScreen"
            exploring: mainRoot.exploring

            onClosed: mainRoot.spectrumOpen = false
            onExploreFromHere: {
                // The engine is the authority on who owns the tuner; this only
                // asks. The pill follows the options snapshot, so it flips a poll
                // later whether or not the request was honoured.
                commands.releaseTuner();
                mainRoot.exploring = true;
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
                var exploreFreqMhz = metrics.centerFreqHz > 0 ? Util.mhzText(metrics.centerFreqHz) : "";
                if (exploreFreqMhz.length > 0)
                    mainRoot.lastExploreFreqMhz = exploreFreqMhz;
                // The session is no longer the saved system it started as: its
                // frequency is now whatever the user makes it, and the calls it
                // logs from here on did not come from that system.
                mainRoot.sessionSystem = mainRoot.exploreSystem(mainRoot.sessionSystem ? mainRoot.sessionSystem.sourceType : "usb", mainRoot.sessionSystem ? mainRoot.sessionSystem.host : "", mainRoot.sessionSystem ? mainRoot.sessionSystem.port : 0, exploreFreqMhz);
                // Which also means the saved row is no longer this session's, so
                // the monitor's edit gesture must not open — and push CSVs into —
                // a system the session detached from.
                mainRoot.sessionRow = -1;
                uiController.flushHistory();
                callHistory.sessionLabel = qsTr("Exploring");
                callHistory.sessionUid = "";
            }
            onSaveAsSystem: function (freqHz) {
                wizard.openForFound(mainRoot.sessionSystem, Util.mhzText(freqHz));
                mainRoot.wizardOpen = true;
            }
        }
    }

    // WP-D1: associate only the retained completion, using the captured system UUID.
    TalkgroupSaveFlow {
        id: talkgroupSave
        onSaved: function (uid, path) {
            if (mainRoot.sessionSystem && mainRoot.sessionSystem.uid === uid)
                mainRoot.sessionSystem = Object.assign({}, mainRoot.sessionSystem, {
                    groupCsvPath: path
                });
        }
    }

    // ---- Talkgroups (pushed over the monitor) ----
    TalkgroupsScreen {
        id: talkgroupsScreen

        objectName: "talkgroupsScreen"
        anchors.fill: safeArea
        systemName: monitor.systemName
        canSaveList: !talkgroups.persistent && (talkgroupSave.pending === null || talkgroupSave.retryAvailable)
        saveListText: talkgroupSave.retryAvailable ? qsTr("Try again") : qsTr("Save talkgroup list")
        saveMessage: talkgroupSave.message
        onSaveListRequested: talkgroupSave.save(mainRoot.sessionSystem ? mainRoot.sessionSystem.uid || "" : "", talkgroups.policyContext, talkgroups.policyGeneration)
        opacity: mainRoot.monitorMode && mainRoot.talkgroupsOpen && !mainRoot.wizardOpen ? 1.0 : 0.0
        visible: opacity > 0.0
        enabled: opacity > 0.9 && mainRoot.monitorMode && mainRoot.talkgroupsOpen && !mainRoot.wizardOpen && !mainRoot.radioReferenceOpen

        onClosed: {
            mainRoot.talkgroupsOpen = false;
            Qt.inputMethod.hide();
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

        onClosed: {
            mainRoot.wizardOpen = false;
            // Over a session the monitor comes back underneath; without this it
            // returns behind the keyboard the name field was still holding.
            Qt.inputMethod.hide();
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
                var was = mainRoot.sessionSystem || {};
                var sys = savedSystems.get(row);
                // Clearing a picker to "None" is a change like any other, and
                // only the clear commands can express it — the import commands
                // all reject an empty path. Without this the file the user just
                // deselected keeps mapping channels, naming talkgroups and
                // decrypting for the rest of the session.
                if (sys.chanCsvPath !== was.chanCsvPath) {
                    if (sys.chanCsvPath.length > 0)
                        commands.importChannelMap(sys.chanCsvPath);
                    else if ((was.chanCsvPath || "").length > 0)
                        commands.clearChannelMap();
                }
                if (sys.groupCsvPath !== was.groupCsvPath) {
                    if (sys.groupCsvPath.length > 0)
                        commands.importGroupList(sys.groupCsvPath);
                    else if ((was.groupCsvPath || "").length > 0)
                        commands.clearGroupList();
                }
                if (sys.keyCsvPath !== was.keyCsvPath || sys.keyCsvHex !== was.keyCsvHex) {
                    if (sys.keyCsvPath.length > 0)
                        commands.importKeys(sys.keyCsvPath, sys.keyCsvHex);
                    else if ((was.keyCsvPath || "").length > 0)
                        commands.clearKeys();
                }
                // No clear counterpart: a band plan the session has already
                // merged with what it heard off the air cannot be taken back,
                // so "None" only takes effect at the next start.
                if (sys.p25BandplanCsvPath !== (was.p25BandplanCsvPath || "") && sys.p25BandplanCsvPath.length > 0)
                    commands.importP25Bandplan(sys.p25BandplanCsvPath);
                if (sys.srcCsvPath !== (was.srcCsvPath || "")) {
                    if (sys.srcCsvPath.length > 0)
                        commands.importSrcList(sys.srcCsvPath);
                    else if ((was.srcCsvPath || "").length > 0)
                        commands.clearSrcList();
                }
                // The monitor header reads sessionSystem; without this it keeps
                // naming and metering the system as it was before the edit.
                mainRoot.sessionSystem = sys;
            }
            mainRoot.wizardOpen = false;
            mainRoot.currentTab = 0;
            Qt.inputMethod.hide();
        }
    }

    Item {
        id: sessionTools
        anchors.fill: safeArea
        visible: mainRoot.sessionDestination.length > 0
        enabled: visible && !mainRoot.diagnosticsOpen && !mainRoot.importsOpen && !mainRoot.radioReferenceOpen
        Rectangle {
            anchors.fill: parent
            color: Theme.bg
        }
        OutlineButton {
            id: returnToSession
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.margins: Theme.screenPadding
            text: mainRoot.monitorMode ? qsTr("Back to Monitor") : qsTr("Back to Listen")
            onClicked: mainRoot.sessionDestination = ""
        }
        HistoryScreen {
            anchors.top: returnToSession.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            visible: mainRoot.sessionDestination === "history"
        }
        SettingsScreen {
            anchors.top: returnToSession.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            visible: mainRoot.sessionDestination === "settings"
            onOpenDiagnostics: mainRoot.diagnosticsOpen = true
            onOpenImports: mainRoot.importsOpen = true
            onOpenRadioReference: mainRoot.openRadioReference(false)
        }
    }
    ModalSheet {
        id: sessionMenu
        accessibleName: qsTr("Session options")
        OutlineButton {
            width: parent.width
            text: qsTr("Sites")
            visible: monitor.sitesAvailable
            onClicked: {
                sessionMenu.visible = false;
                monitor.openSites();
            }
        }
        OutlineButton {
            width: parent.width
            text: qsTr("Spectrum")
            visible: metrics.radioInput
            onClicked: {
                sessionMenu.visible = false;
                spectrumLoader.active = true;
                mainRoot.spectrumOpen = true;
            }
        }
        OutlineButton {
            width: parent.width
            text: qsTr("Talkgroups")
            onClicked: {
                sessionMenu.visible = false;
                mainRoot.talkgroupsOpen = true;
            }
        }
        OutlineButton {
            width: parent.width
            text: qsTr("History")
            onClicked: {
                sessionMenu.visible = false;
                mainRoot.sessionDestination = "history";
            }
        }
        OutlineButton {
            width: parent.width
            text: qsTr("Settings")
            onClicked: {
                sessionMenu.visible = false;
                mainRoot.sessionDestination = "settings";
            }
        }
        OutlineButton {
            width: parent.width
            text: qsTr("Diagnostics")
            onClicked: {
                sessionMenu.visible = false;
                mainRoot.diagnosticsOpen = true;
            }
        }
        OutlineButton {
            width: parent.width
            text: qsTr("Edit saved system")
            visible: mainRoot.sessionRow >= 0
            onClicked: {
                sessionMenu.visible = false;
                monitor.editSystem();
            }
        }
        OutlineButton {
            width: parent.width
            text: qsTr("Close")
            onClicked: sessionMenu.visible = false
        }
    }

    // WP-F5 diagnostics overlay.
    DiagnosticsScreen {
        id: diagnosticsScreen
        objectName: "diagnosticsScreen"
        anchors.fill: safeArea
        visible: mainRoot.diagnosticsOpen
        enabled: visible
        onClosed: mainRoot.diagnosticsOpen = false
    }

    // ---- Imported-files library (pushed from Settings) ----
    // The monitor owns the screen once a session goes active, so this layer
    // stands down for it the way the explore setup and onboarding do — two lit,
    // enabled full-screen layers means one tap lands on both, and the bottom
    // "Import file" button sits exactly over "Stop listening".
    ImportsScreen {
        id: importsScreen
        objectName: "importsScreen"

        anchors.fill: safeArea
        // So a RadioReference refresh can tell whether the file it just replaced
        // is one the running session is actually using.
        sessionSystem: mainRoot.sessionSystem
        opacity: mainRoot.importsOpen && (!mainRoot.monitorMode || mainRoot.sessionDestination === "settings") ? 1.0 : 0.0
        visible: opacity > 0.0
        enabled: opacity > 0.9

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

        onClosed: {
            mainRoot.radioReferenceOpen = false;
            Qt.inputMethod.hide();
        }
        // The wizard is the single writer of a saved system, so the generated
        // files and the tune answers go to it rather than to savedSystems.add().
        onImported: function (result) {
            mainRoot.radioReferenceOpen = false;
            Qt.inputMethod.hide();
            if (!mainRoot.radioReferenceFromWizard) {
                // Opened from Settings or the library: the source, gain and name
                // are still unanswered, so the wizard asks for them.
                mainRoot.importsOpen = false;
                wizard.openForAdd(false);
                mainRoot.wizardOpen = true;
            }
            mainRoot.radioReferenceFromWizard = false;
            wizard.applyRadioReference(result);
        }
    }

    SiteChooserSheet {
        id: siteChooser
        anchors.fill: safeArea
        onUserAction: mainRoot.cancelPendingRestart()
        onEditSite: function (row) {
            wizard.openForEdit(row);
            mainRoot.wizardOpen = true;
        }
        onStartSite: function (row) {
            mainRoot.startSystem(row);
        }
        onRestartSite: function (uid) {
            mainRoot.restartSite(uid);
        }
    }

    ModalSheet {
        id: failureDetails
        accessibleName: qsTr("Source failure details")
        Text {
            width: parent.width
            text: mainRoot.sourceLabel() + "\n\n" + mainRoot.failureText + (decoderHost.inputFailureCode ? qsTr("\nNative error: %1").arg(decoderHost.inputFailureCode) : "")
            textFormat: Text.PlainText
            wrapMode: Text.WrapAnywhere
            color: Theme.textPrimary
            font.pixelSize: Theme.fontSize(14)
        }
        OutlineButton {
            width: parent.width
            text: qsTr("Close")
            onClicked: failureDetails.visible = false
        }
    }

    ModalSheet {
        id: notificationExplanation
        objectName: "notificationExplanation"
        parent: mainRoot.contentItem
        anchors.fill: safeArea
        accessibleName: qsTr("Background listening")
        dismissHandler: function () {
            prefs.notificationExplained = true;
            visible = false;
        }
        Text {
            width: parent.width
            text: qsTr("Listening can continue when you leave the app. Allow notifications to keep playback status and Stop available outside the app. You can keep using the app if you decline.")
            wrapMode: Text.Wrap
            color: Theme.textPrimary
            font.pixelSize: Theme.fontSize(16)
        }
        GradientButton {
            width: parent.width
            text: qsTr("Allow notifications")
            onClicked: {
                prefs.notificationExplained = true;
                notificationExplanation.visible = false;
                decoderHost.requestNotificationPermission();
            }
        }
        OutlineButton {
            width: parent.width
            text: qsTr("Not now")
            onClicked: {
                prefs.notificationExplained = true;
                notificationExplanation.visible = false;
            }
        }
    }

    // ---- First-run onboarding ----
    OnboardingScreen {
        id: onboarding
        anchors.fill: safeArea
        opacity: !prefs.onboardingDone && !mainRoot.monitorMode ? 1.0 : 0.0
        visible: opacity > 0.0
        enabled: opacity > 0.9

        onGetStarted: prefs.onboardingDone = true
        onNetworkSource: {
            prefs.onboardingDone = true;
            wizard.openForAdd(true);
            mainRoot.wizardOpen = true;
        }
    }
}
