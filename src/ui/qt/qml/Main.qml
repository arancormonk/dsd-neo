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
        var label = source.sourceType === "airspy" ? qsTr("Airspy R2 / Mini") : source.sourceType === "rtltcp" ? "RTL-TCP " + source.host + ":" + source.port : source.sourceType === "tcp" ? "TCP audio " + source.host + ":" + source.port : source.sourceType === "udp" ? qsTr("UDP audio port %1").arg(source.port) : source.sourceType === "file" ? qsTr("Replay: %1").arg(source.filePath.substring(source.filePath.lastIndexOf('/') + 1)) : qsTr("USB RTL-SDR");
        return source.extraArgs ? qsTr("Configured source: %1 (Advanced arguments are also in use)").arg(label) : label;
    }
    function failureMessage() {
        var host = decoderHost;
        var kind = host.inputFailureKind || 0;
        if (startError.length)
            return startError;
        // The answer to the access request a start waits on, not the last
        // session's failure: that session is over, and this is what holds the
        // start up now ("USB permission denied").
        if (usbAccessText.length)
            return usbAccessText;
        if (receiverNoticeText.length)
            return receiverNoticeText;
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
        if (kind === 7)
            return qsTr("The receiver could not be opened or stopped receiving. Check its USB connection and settings.");
        if (kind === 6)
            return qsTr("The source configuration could not be applied. Review its settings and imported files.");
        // Kinds 8 and up belong to the start attempt itself, never to an
        // earlier session (android/run_status.h AttemptFailureKind).
        if (kind === 8)
            return qsTr("The previous session is still stopping. Tap Retry to start this one once it has stopped.");
        if (kind === 9)
            return qsTr("The decoder did not respond in time, so the session was not started. Tap Retry, or open Details if this keeps happening.");
        var receiverFailure = receiverStartFailure(false);
        if (receiverFailure.length)
            return receiverFailure;
        return awaitingUsbAccess ? usbAccessText : qsTr("The source could not be started. Review its settings or open Details.");
    }
    /**
     * A start that could not take its USB receiver before the session began
     * (kinds 10-12, android/run_status.h): the receiver went away, lost its
     * permission, or could not be reopened. Worded for the card, whose Retry
     * asks for the receiver again, or for Signal search's own screen (@p search),
     * where the search is started again from its controls. Empty for any other
     * failure.
     */
    function receiverStartFailure(search) {
        var kind = decoderHost ? decoderHost.inputFailureKind || 0 : 0;
        if (kind < 10 || kind > 12)
            return "";
        var type = attemptedSource && attemptedSource.system ? attemptedSource.system.sourceType : "";
        var receiver = type === "airspy" ? qsTr("Airspy") : type === "usb" ? qsTr("RTL-SDR") : qsTr("The receiver");
        if (search) {
            if (kind === 10)
                return qsTr("%1 needs USB permission again, so Signal search was not started. Start the search again, then allow access when asked.").arg(receiver);
            if (kind === 11)
                return qsTr("%1 could not be reopened, so Signal search was not started. Start the search again, or unplug it and plug it back in if this keeps happening.").arg(receiver);
            return qsTr("%1 is no longer connected, so Signal search was not started. Plug it back in, then start the search again.").arg(receiver);
        }
        if (kind === 10)
            return qsTr("%1 needs USB permission again, so the session was not started. Tap Retry, then allow access when asked.").arg(receiver);
        if (kind === 11)
            return qsTr("%1 could not be reopened, so the session was not started. Tap Retry, or unplug it and plug it back in if this keeps happening.").arg(receiver);
        return qsTr("%1 is no longer connected, so the session was not started. Plug it back in, then tap Retry.").arg(receiver);
    }
    /**
     * A start attempt's own failure (kinds 8-12, android/run_status.h) worded
     * for Signal search's screen, which has no Retry or Details: the search is
     * started again from its controls. Empty for any other failure.
     */
    function searchStartFailure() {
        var kind = decoderHost ? decoderHost.inputFailureKind || 0 : 0;
        if (kind === 8)
            return qsTr("The previous session is still stopping, so Signal search was not started. Start the search again once it has stopped.");
        if (kind === 9)
            return qsTr("The decoder did not respond in time, so Signal search was not started. Start the search again, or open Diagnostics in Settings if this keeps happening.");
        return receiverStartFailure(true);
    }
    // #647 D2a: the host's word on the last session's receiver, scoped to that
    // session. Listen gives it a notice once the session is over; while it
    // winds down, the Monitor's pill says LOST.
    readonly property var receiverHealth: decoderHost && decoderHost.receiverHealth ? decoderHost.receiverHealth : ({state: "ok", source: ""})
    readonly property bool receiverEnded: (receiverHealth.state === "lost" || receiverHealth.state === "failed") && !(decoderHost && decoderHost.sessionActive)
    /**
     * Why the last session stopped, when its receiver ended it, and what gets
     * it back. Reconnect resumes the attempt Main started; after an Activity
     * recreation there is none, so the notice says to start it again after
     * Reconnect. @p search words it for Signal search's own screen, where the
     * search is started again from its controls. A stalled RTL-SDR the app
     * could not recover in place owes a USB reset, which the next open does
     * (#782): its notice says so. Nothing blames the cable: the stream stopped,
     * and the app cannot know why.
     */
    function receiverNotice(search) {
        var lost = receiverHealth.state === "lost";
        var resets = receiverHealth.source === "usb";
        var receiver = receiverHealth.source === "airspy" ? qsTr("Airspy") : resets ? qsTr("RTL-SDR") : qsTr("The receiver");
        if (search) {
            if (lost)
                return qsTr("%1 was unplugged, so %2 stopped. Plug it back in, then start the search again.").arg(receiver).arg(qsTr("Signal search"));
            return (resets ? qsTr("%1 stopped sending samples, so %2 stopped. Start the search again to reset it.") : qsTr("%1 stopped sending samples, so %2 stopped. Start the search again.")).arg(receiver).arg(qsTr("Signal search"));
        }
        var attempt = attemptedSource;
        if (!attempt) {
            if (lost)
                return qsTr("%1 was unplugged, so %2 stopped. Plug it back in, tap Reconnect, then start it again.").arg(receiver).arg(qsTr("the session"));
            return (resets ? qsTr("%1 stopped sending samples, so %2 stopped. Tap Reconnect to reset it, then start it again.") : qsTr("%1 stopped sending samples, so %2 stopped. Tap Reconnect, then start it again.")).arg(receiver).arg(qsTr("the session"));
        }
        var session = attempt.kind !== "explore" ? attempt.system.name || qsTr("the session") : attempt.system.signalSearch ? qsTr("Signal search") : qsTr("Explore");
        if (lost)
            return qsTr("%1 was unplugged, so %2 stopped. Plug it back in, then tap Reconnect.").arg(receiver).arg(session);
        return (resets ? qsTr("%1 stopped sending samples, so %2 stopped. Tap Reconnect to reset it and start again.") : qsTr("%1 stopped sending samples, so %2 stopped. Tap Reconnect to start again.")).arg(receiver).arg(session);
    }
    readonly property string receiverNoticeText: receiverEnded ? receiverNotice(false) : ""
    // Whether the failure on screen is that notice: Reconnect on Listen, the search's own wording there.
    readonly property bool receiverNoticeShown: receiverNoticeText.length > 0 && failureText === receiverNoticeText
    function retrySource() {
        if (decoderHost.sessionActive || decoderHost.transitioning)
            return;
        if (!attemptedSource) {
            // D2a: a recreated Activity has no verified link from the
            // service's run to a saved system or scan list, so Reconnect only
            // asks for that receiver again; the notice says to start it after.
            if (receiverEnded)
                decoderHost.requestLocalDeviceAccessForSource(receiverHealth.source);
            return;
        }
        // The attempt itself, not just its kind: a failed Signal search start
        // is retried as a search, and an Explore-here start as that frequency.
        pendingRestart = {
            kind: attemptedSource.kind,
            uid: attemptedSource.uid,
            system: attemptedSource.system,
            remember: attemptedSource.remember !== false
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
        if (attemptedSource.system && attemptedSource.system.signalSearch) {
            // A failed search is edited where it was set up.
            mainRoot.openSignalSearchOn(attemptedSource.system);
        } else if (attemptedSource.kind === "explore" && attemptedSource.system && attemptedSource.remember === false) {
            // A failed Explore here is edited as that attempt: its receiver
            // and frequency, still not where Explore resumes.
            openExploreSetupOn(attemptedSource.system);
        } else if (attemptedSource.kind === "explore") {
            openExploreSetup();
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
    // below), when the banner is dismissed, when another system starts, or when
    // Signal search abandons the start it asked for.
    property bool awaitingUsbAccess: false
    // WP-D4: a restart waits for confirmed Idle and resolves stable identity then.
    property var pendingRestart: null
    function cancelPendingRestart() {
        pendingRestart = null;
        leavingSearch = false;
    }
    // Set while Signal search waits for its session to stop before a handoff
    // (leaveSearchFor()). The search screen stays the root until the decoder is
    // idle and says it is stopping. Back cancels the handoff, back to that
    // screen and its controls, so a stop that never finishes cannot lock it.
    property bool leavingSearch: false
    function restartSite(uid) {
        // Refused before cancelling: a restart that cannot run leaves a
        // handoff already waiting alone.
        if (decoderHost.sessionState !== 2 || savedSystems.rowForUid(uid) < 0)
            return;
        cancelPendingRestart();
        pendingStart = null;
        pendingStartRow = -1;
        pendingStartRemember = true;
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
        if (request.leaveSearch) {
            // The search has stopped: its screen gives way to what replaces it.
            leavingSearch = false;
            signalSearchScreen.localFailure = "";
            mainRoot.signalSearchOpen = false;
        }
        if (request.kind === "explore") {
            if (request.system && request.system.signalSearch) {
                // A search starts from its screen: its ranges are armed there.
                mainRoot.openSignalSearchOn(request.system);
                signalSearchScreen.startSearch();
            } else if (request.system && request.remember === false) {
                // Explore here: that detection's frequency, without becoming
                // where Explore resumes, on the receiver attached now.
                mainRoot.startWithMap(mainRoot.onAttachedReceiver(request.system), -1, false, {
                    remember: false
                });
            } else {
                startExploring();
            }
            return;
        }
        var scan = request.kind === "scan";
        if (!scan) {
            var refusal = savedStartRefusal(request.uid);
            if (refusal.length) {
                // The refused system is the attempt Home's failure card names:
                // Retry asks again, and Edit opens that system.
                attemptedSource = {
                    kind: "saved",
                    uid: request.uid,
                    system: savedSystems.getByUid(request.uid),
                    remember: true
                };
                startError = refusal;
                return;
            }
        }
        var row = scan ? scanLists.rowForUid(request.uid) : savedSystems.rowForUid(request.uid);
        if (row < 0) {
            startError = qsTr("This source was removed. Choose another source.");
            return;
        }
        if (scan)
            startScanList(row);
        else
            startSystem(row);
    }
    /** Why saved system @p uid cannot start now, or empty when it can. */
    function savedStartRefusal(uid) {
        var row = savedSystems.rowForUid(uid);
        if (row < 0)
            return qsTr("This source was removed. Choose another source.");
        var system = savedSystems.get(row);
        if (system.avoidSite)
            return qsTr("“%1” is avoided. Remove the avoid in its site list to start it.").arg(system.name);
        return "";
    }
    /**
     * Leave Signal search for another session, as one operation.
     *
     * @a request is {kind: "explore", system, remember: false} or
     * {kind: "saved", uid}. The search's own start is abandoned first (its arm
     * and any USB-access wait, so a late grant cannot turn the replacement into
     * a search), the screen closes, and the request runs once the decoder is
     * idle; until then the search screen stays, saying it is stopping
     * (leavingSearch). A saved system is checked before anything stops, the
     * refusal shown on the search screen, and again when it starts.
     */
    function leaveSearchFor(request) {
        if (request.kind === "saved") {
            var refusal = savedStartRefusal(request.uid);
            if (refusal.length) {
                signalSearchScreen.localFailure = refusal;
                return;
            }
        }
        abandonSignalSearchStart();
        cancelPendingRestart();
        // Pinned open, as Stop pins it: a screen Activity recreation reopened
        // is only bound to status.owned, which the search clears before the
        // decoder is idle, and Back must leave the stopped search on it.
        mainRoot.signalSearchOpen = true;
        var state = decoderHost.sessionState;
        pendingRestart = Object.assign({
            leaveSearch: true
        }, request);
        if (state === 0) {
            finishPendingRestart();
            return;
        }
        leavingSearch = true;
        // Stopping: finished on Idle, as any pending restart is.
        if (state === 1 || state === 2 || state === 4)
            decoderHost.stop();
    }
    /** Explore here: the detection's own receiver settings, mode and gain. */
    function exploreHereSystem(hit, screenSource) {
        var source = mainRoot.sessionSystem && mainRoot.sessionSystem.signalSearch ? mainRoot.sessionSystem : screenSource;
        var system = Object.assign({}, signalSearchScreen.searchModel.systemForHit(hit, source));
        system.name = qsTr("Exploring");
        system.trunking = false;
        delete system.signalSearch;
        delete system.searchUnresolved;
        delete system.uid;
        return mainRoot.onAttachedReceiver(system);
    }
    /**
     * A copy of @p system on receiver kind @p kind, keeping the settings that
     * kind takes. An Airspy's own settings (its serial among them) only go
     * with an Airspy; a switch to one drops an RTL's PPM, bias tee and tuner
     * gain, since an Airspy takes its gains and tee from its own map.
     */
    function onReceiver(system, kind) {
        var moved = Object.assign({}, system);
        if (kind !== moved.sourceType) {
            moved.sourceType = kind;
            if (kind === "airspy") {
                moved.gainDb = -1;
                delete moved.ppm;
                delete moved.biasTee;
            }
        }
        if (moved.sourceType !== "airspy")
            delete moved.airspy;
        return moved;
    }
    /** A copy of @p system on the receiver attached now, as every Explore start resolves it (rtl_tcp stays). */
    function onAttachedReceiver(system) {
        return mainRoot.onReceiver(system, mainRoot.effectiveSource(system.sourceType));
    }
    /** Open Signal search on the receiver @p system ran on, resolved against the one attached now. */
    function openSignalSearchOn(system) {
        signalSearchScreen.seed(mainRoot.effectiveSource(system.sourceType || "usb") || "usb", system.host || prefs.searchHost || "192.168.1.10", system.port || prefs.searchPort || 1234);
        mainRoot.signalSearchOpen = true;
    }
    // The system map waiting for USB access and its row (-1 for exploration).
    property var pendingStart: null
    property int pendingStartRow: -1
    // Whether the waiting start, once granted, may become where Explore
    // resumes: an Explore-here start keeps false through the permission dialog.
    property bool pendingStartRemember: true
    // Leaving Signal search abandons the start it asked for: closing the screen
    // (its Back, the system Back, the window), a later Start it refuses, or Stop.
    // The arm is cancelled, so a USB grant still finishing that start would run a
    // plain decoder session behind Home instead of the search. A start pending
    // from Home is not the search's and keeps waiting.
    function abandonSignalSearchStart() {
        if (signalSearchScreen.searchModel)
            signalSearchScreen.searchModel.cancelArm();
        if (!mainRoot.pendingStart || !mainRoot.pendingStart.signalSearch)
            return;
        mainRoot.awaitingUsbAccess = false;
        mainRoot.pendingStart = null;
        mainRoot.pendingStartRow = -1;
        mainRoot.pendingStartRemember = true;
    }
    // Issue #580: a restore replaces the systems a waiting start or restart
    // names, so entering one drops both rather than letting them run after it.
    function dropPendingStarts() {
        cancelPendingRestart();
        if (mainRoot.pendingStart && mainRoot.pendingStart.signalSearch)
            abandonSignalSearchStart();
        mainRoot.awaitingUsbAccess = false;
        mainRoot.pendingStart = null;
        mainRoot.pendingStartRow = -1;
        mainRoot.pendingStartRemember = true;
    }
    // WP-S1 shares the saved-session USB gate and initialization bookkeeping.
    property bool pendingScanList: false
    property bool sessionScanList: false
    property bool scanListOpen: false
    property string scanWarnings: ""
    readonly property string usbAccessText: awaitingUsbAccess && decoderHost && !decoderHost.localDeviceReady ? decoderHost.localDeviceStatus : ""
    readonly property string failureText: startError.length > 0 ? startError : usbAccessText.length > 0 ? usbAccessText : receiverNoticeText.length > 0 ? receiverNoticeText : hostFailure

    // WP-S2: consume each attachment once; blocked events are never deferred.
    Binding {
        target: uiController
        property: "autoStartBlocked"
        // A backup flow counts for its whole length: its system pickers show no sheet.
        // So does a permission prompt of the host's own, until it is answered or expires (#647).
        value: mainRoot.sessionDestination.length > 0 || mainRoot.wizardOpen || mainRoot.scanListOpen || mainRoot.exploreSetupOpen || mainRoot.signalSearchOpen || mainRoot.diagnosticsOpen || mainRoot.licensesOpen || mainRoot.callRecordingsOpen || mainRoot.radioReferenceAccountOpen || mainRoot.importsOpen || mainRoot.radioReferenceOpen || mainRoot.spectrumOpen || mainRoot.talkgroupsOpen || mainRoot.awaitingUsbAccess || (!!decoderHost && decoderHost.localDeviceRequestPending === true) || homeScreen.managementSheetOpen || siteChooser.visible || Navigation.modals.length > 0 || backup.flowActive
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
        if (leavingSearch) {
            cancelPendingRestart();
            return;
        }
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
    Component.onCompleted: {
        updateSystemBars();
        adoptReattachedSession();
    }
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
        value: mainRoot.signalSearchOpen && mainRoot.sessionActive() ? [signalSearchScreen] : mainRoot.monitorMode ? [monitor] : !prefs.onboardingDone ? [onboarding] : [nav, mainRoot.currentTab === 0 ? homeScreen : mainRoot.currentTab === 1 ? historyRoot : settingsRoot]
    }
    NavigationLayer {
        surface: wizard
        active: mainRoot.wizardOpen
        onLeave: wizard.requestBack()
    }
    NavigationLayer {
        surface: signalSearchScreen
        active: mainRoot.signalSearchOpen && !mainRoot.sessionActive()
        onLeave: signalSearchScreen.requestClose()
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
        id: sessionToolsLayer
        surface: sessionTools
        active: mainRoot.sessionDestination.length > 0
        onLeave: mainRoot.sessionDestination = ""
    }
    NavigationLayer {
        id: importsLayer
        surface: importsScreen
        active: mainRoot.importsOpen && (!mainRoot.monitorMode || mainRoot.sessionDestination === "settings")
        onLeave: mainRoot.importsOpen = false
    }
    NavigationLayer {
        id: diagnosticsLayer
        surface: diagnosticsScreen
        active: mainRoot.diagnosticsOpen
        onLeave: mainRoot.diagnosticsOpen = false
    }
    NavigationLayer {
        id: licensesLayer
        surface: licensesScreen
        active: mainRoot.licensesOpen
        onLeave: mainRoot.licensesOpen = false
    }
    NavigationLayer {
        id: callRecordingsLayer
        surface: callRecordingsScreen
        active: mainRoot.callRecordingsOpen
        onLeave: mainRoot.callRecordingsOpen = false
    }
    NavigationLayer {
        id: radioReferenceAccountLayer
        surface: radioReferenceAccountScreen
        active: mainRoot.radioReferenceAccountOpen
        onLeave: mainRoot.radioReferenceAccountOpen = false
    }
    NavigationLayer {
        surface: radioReferenceScreen
        active: mainRoot.radioReferenceOpen
        // Through the screen, which asks before a site selection goes (issue #718).
        onLeave: radioReferenceScreen.requestClose()
    }
    property bool wizardOpen: false
    property bool exploreSetupOpen: false
    property bool signalSearchOpen: typeof signalSearch !== "undefined" && decoderHost.sessionActive && !!signalSearch.status.owned
    property bool licensesOpen: false
    property bool callRecordingsOpen: false // issue #708
    property bool radioReferenceAccountOpen: false
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
    // The standalone saved system the session runs, by UID: empty for a scan
    // list, Explore, a signal search or a wizard test. What the session may
    // write back to (radio controls, the decryption profile for future starts).
    readonly property string sessionSavedSystemUid: !mainRoot.exploring && !mainRoot.sessionScanList && mainRoot.sessionSystem
        && !mainRoot.sessionSystem.signalSearch ? (mainRoot.sessionSystem.uid || "") : ""
    property bool awaitingSessionInitialized: false
    // What the session was started as ("scan", "explore" or "saved") and the UID
    // that names it, decided when the host accepts the start. The
    // post-initialization edge records these rather than the session as it
    // stands then: an Explore taken while the session was still starting (issue
    // #671) changes what it is now, not what was started, and a scan list
    // recorded with no UID could never be resumed.
    property string sessionStartKind: ""
    property string sessionStartUid: ""

    property bool sessionReachedRunning: false

    function recordSessionInitialized() {
        if (!mainRoot.awaitingSessionInitialized || !mainRoot.sessionSystem)
            return;
        mainRoot.awaitingSessionInitialized = false;
        // Accepted argv is only a request. These writes belong to the engine's
        // post-initialization edge, after file validation and tuner open.
        if (Qt.platform.os === "android" && prefs.backgroundListening && !prefs.notificationExplained && decoderHost.notificationPermissionNeeded)
            notificationExplanation.visible = true;
        prefs.lastStartedKind = mainRoot.sessionStartKind;
        prefs.lastStartedUid = mainRoot.sessionStartUid;
        if (mainRoot.sessionStartKind === "scan")
            scanLists.touch(scanLists.rowForUid(prefs.lastStartedUid));
        else if (mainRoot.sessionStartKind === "saved")
            savedSystems.touch(savedSystems.rowForUid(prefs.lastStartedUid));
    }
    Connections {
        target: decoderHost
        function onSessionInitialized() {
            mainRoot.recordSessionInitialized();
        }
        function onSessionStateChanged() {
            if (decoderHost.sessionState === 4) {
                // A search that failed to stop hands off nothing: its screen
                // closes onto Home's failure card.
                if (mainRoot.leavingSearch)
                    mainRoot.signalSearchOpen = false;
                mainRoot.cancelPendingRestart();
            }
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
                mainRoot.adoptReattachedSession();
                mainRoot.syncOperatorHold();
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
    // Whether this explore session writes where it got to back to Explore's
    // prefs; false for Explore here.
    property bool exploreRemembered: true
    // Sampled while an explore session runs, not read at the end of one: the
    // metrics are cleared the moment the session stops, so asking then would
    // persist a zero and lose the frequency the user had found.
    property string lastExploreFreqMhz: ""

    Connections {
        target: metrics
        function onTunerChanged() {
            if (mainRoot.exploring && metrics.centerFreqHz > 0)
                mainRoot.lastExploreFreqMhz = Util.mhzValueText(metrics.centerFreqHz);
        }
        function onControlChanged() {
            mainRoot.syncOperatorHold();
        }
    }

    // Issue #671: the operator holding the tuner -- a scan paused by Go to (or by
    // a frequency entered anywhere else), an owner released by Explore from
    // here, or any manual tune that moved the receiver off its session -- as the
    // decoder reports it, never as a command was queued. The session map the
    // monitor names gives way to "Exploring" while the hold lasts and comes back
    // when a resume ends it, so a pause or a resume made from the API or the
    // terminal names the session the same way. A scan list's session becomes an
    // Explore one for the hold: the receiver is the operator's to tune by hand,
    // tap and drag included, until Resume scan (and for good if the paused scan
    // is then stopped). History needs none of this: each call carries who held
    // the tuner when it began.
    property bool operatorHolds: false
    // The session's own map and saved row while the operator holds the tuner:
    // what Resume scan returns to, and what names the targets once it has.
    property var pausedScanSystem: null
    property int pausedSessionRow: -1
    // An Explore asked for and not yet answered: the release is in the queue, or
    // the options that say whether anything owned the tuner have not arrived.
    property bool exploreRequested: false

    // A recreated Activity finds a session it did not start (the service
    // outlived the old one): it takes the identity of the start the host last
    // accepted (the acceptedStart pref pair, written with the start, so a session
    // recreated before its initialization edge is still itself), as a start in
    // this UI would have set it. A scan list is still one -- its targets are
    // named from the list, a pause hands the receiver to the operator with
    // Resume scan; an Explore session is still free to tune; a saved system has
    // its map and row back (its title, sites, Edit, the key sheet's system). A
    // start in this UI sets the identity itself, and keeps it after the session
    // ends: a late initialization edge still records it.
    function adoptReattachedSession() {
        if (decoderHost.sessionState !== 2 || mainRoot.sessionStartKind.length > 0 || mainRoot.pendingStart)
            return;
        mainRoot.sessionStartKind = prefs.acceptedStartKind || "";
        mainRoot.sessionStartUid = prefs.acceptedStartUid || "";
        if (mainRoot.sessionStartKind === "scan") {
            mainRoot.sessionScanList = true;
            var list = scanLists.getByUid(mainRoot.sessionStartUid);
            if (list && list.uid)
                mainRoot.sessionSystem = list;
        } else if (mainRoot.sessionStartKind === "explore") {
            mainRoot.exploring = true;
            // Where it got to before the recreation is not known here.
            mainRoot.exploreRemembered = false;
        } else if (mainRoot.sessionStartKind === "saved" && mainRoot.sessionStartUid.length > 0) {
            var sys = savedSystems.getByUid(mainRoot.sessionStartUid);
            if (sys && sys.uid) {
                mainRoot.sessionSystem = sys;
                mainRoot.sessionRow = savedSystems.rowForUid(sys.uid);
            }
        }
        mainRoot.syncOperatorHold();
    }

    // The receiver an Explore map stands on when the operator takes the tuner:
    // the session map's, or with none (a session the UI could not name) the
    // running input's own, so the pill and Save as system match the receiver
    // rather than assume a USB one. Off a radio there is none to name.
    function heldReceiver(map) {
        if (map && map.sourceType)
            return map;
        return metrics.radioSource || {};
    }

    function forgetOperatorHold() {
        mainRoot.operatorHolds = false;
        mainRoot.pausedScanSystem = null;
        mainRoot.pausedSessionRow = -1;
        mainRoot.exploreRequested = false;
    }

    function syncOperatorHold() {
        // The lifecycle's Running, which a reattached Activity finds as well as a start.
        if (decoderHost.sessionState !== 2 || !metrics.optionsKnown)
            return;
        if (metrics.operatorTuned && !mainRoot.operatorHolds) {
            mainRoot.operatorHolds = true;
            mainRoot.pausedScanSystem = mainRoot.sessionSystem;
            mainRoot.pausedSessionRow = mainRoot.sessionRow;
            mainRoot.sessionRow = -1;
            var held = mainRoot.heldReceiver(mainRoot.sessionSystem);
            mainRoot.sessionSystem = mainRoot.exploreSystem(held.sourceType || "", held.host || "", held.port || 0, metrics.centerFreqHz > 0 ? Util.mhzValueText(metrics.centerFreqHz) : "");
            if (mainRoot.sessionScanList) {
                // Not Explore's to remember: the list is coming back.
                mainRoot.exploring = true;
                mainRoot.exploreRemembered = false;
            }
        } else if (!metrics.operatorTuned && mainRoot.operatorHolds) {
            // Resumed: the scan has the tuner again, and the session is what it
            // was started as.
            mainRoot.sessionSystem = mainRoot.pausedScanSystem;
            mainRoot.sessionRow = mainRoot.pausedSessionRow;
            mainRoot.exploring = mainRoot.sessionStartKind === "explore";
            mainRoot.exploreRemembered = true;
            mainRoot.operatorHolds = false;
            mainRoot.pausedScanSystem = null;
            mainRoot.pausedSessionRow = -1;
        }
        if (!mainRoot.exploreRequested)
            return;
        if (mainRoot.operatorHolds) {
            mainRoot.exploreRequested = false;
        } else if (!metrics.tunerControlled && metrics.scanKind === 0) {
            // Nothing owned the tuner, so the decoder has no hold to report.
            mainRoot.exploreRequested = false;
            mainRoot.exploreWithNoOwner();
        }
    }

    // Explore from a session nothing was steering (a conventional saved system):
    // the receiver has not moved yet, so the decoder has no hold to report, and
    // the session detaches here. History is left alone: the calls heard until the
    // first tune are still the system's, and that tune stamps the ones after it
    // as the operator's.
    function exploreWithNoOwner() {
        var was = mainRoot.heldReceiver(mainRoot.sessionSystem);
        mainRoot.sessionSystem = mainRoot.exploreSystem(was.sourceType || "", was.host || "", was.port || 0, mainRoot.lastExploreFreqMhz);
        // Which also means the saved row is no longer this session's, so the
        // session's edit action must not open -- and push CSVs into -- a system
        // the session detached from.
        mainRoot.sessionRow = -1;
    }

    // Suppresses a failure banner the user has read. Reset on the next start so a
    // repeat of the same failure is reported again rather than swallowed.
    property string dismissedFailure: ""
    readonly property bool showFailure: !monitorMode && failureText.length > 0 && failureText !== dismissedFailure
    function revealFailure() {
        // Settings subpages can outlive a clean stop. If the failure arrives
        // afterward, leave them through navigation so Home is actually exposed
        // and their input/navigation cleanup still runs.
        for (var layer of [importsLayer, diagnosticsLayer, licensesLayer, callRecordingsLayer, radioReferenceAccountLayer]) {
            if (layer.active)
                layer.leave();
        }
        currentTab = 0;
        // B5: on a brokered host Home keeps its scroll position across a
        // session, which can leave the notice and the receiver's panel out of
        // view. The desktop keeps where it was.
        if (decoderHost && decoderHost.localDeviceBrokered)
            homeScreen.revealTop();
    }
    onShowFailureChanged: {
        // A host failure can arrive after the session-active notification.
        if (showFailure)
            revealFailure();
    }

    // Dismissing the banner abandons a start still waiting on USB access; the
    // flag must not linger, or a dongle detached minutes later while idle would
    // resurrect a permission banner the user never asked about. It abandons a
    // restart Retry queued for the decoder's next idle turn as well (B13).
    onDismissedFailureChanged: {
        if (dismissedFailure.length > 0) {
            cancelPendingRestart();
            awaitingUsbAccess = false;
            mainRoot.pendingStart = null;
            mainRoot.pendingStartRow = -1;
            mainRoot.pendingStartRemember = true;
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
            var options = {
                remember: mainRoot.pendingStartRemember
            };
            mainRoot.pendingStart = null;
            mainRoot.pendingStartRow = -1;
            mainRoot.pendingStartRemember = true;
            // Issue #580: a backup flow holds the screen, and a restore replaces
            // the setup this start would run on. The grant ends the wait and
            // starts nothing.
            if (backup.flowActive) {
                if (sys && sys.signalSearch)
                    mainRoot.abandonSignalSearchStart();
                return;
            }
            // A start Home left waiting is no search: Signal search opened
            // meanwhile closes, so its screen does not take that session for one.
            if (sys && !sys.signalSearch && mainRoot.signalSearchOpen) {
                mainRoot.abandonSignalSearchStart();
                mainRoot.signalSearchOpen = false;
            }
            if (sys)
                mainRoot.startWithMap(sys, row, scan, options);
        }
    }

    onMonitorModeChanged: {
        // Pushed session screens must close with the session rather than showing
        // stale content when the next one starts.
        if (!monitorMode) {
            // Where the exploring got to, so the next one resumes there rather
            // than back at the start frequency.
            // An Explore-here session went where a detection was, which is not
            // where the user's own exploring got to.
            if (mainRoot.exploring && mainRoot.exploreRemembered && mainRoot.lastExploreFreqMhz.length > 0)
                prefs.exploreFreqMhz = mainRoot.lastExploreFreqMhz;
            mainRoot.exploring = false;
            mainRoot.exploreRemembered = true;
            mainRoot.forgetOperatorHold();
            mainRoot.spectrumOpen = false;
            mainRoot.talkgroupsOpen = false;
            sessionRadio.visible = false;
            sessionMenu.visible = false;
            if (mainRoot.sessionDestination.length > 0) {
                if (mainRoot.failureText.length > 0 && mainRoot.failureText !== mainRoot.dismissedFailure)
                    mainRoot.revealFailure();
                else
                    mainRoot.currentTab = mainRoot.sessionDestination === "history" ? 1 : 2;
                sessionToolsLayer.leave();
            }
            // Row indices shift when a system is removed, and Home is reachable
            // again from here; a row remembered past its session would name a
            // different system by the time anything read it.
            mainRoot.sessionRow = -1;
        }
        if (monitorMode) {
            // The frequency field usually still holds focus; the keyboard would
            // cover the session that just appeared.
            Qt.inputMethod.hide();
            // The recent-calls pane and the heard talkgroups show the running
            // session's calls. A start has set this already. A UI reattaching to
            // a session it did not start (the service survived an Activity
            // restart) takes the session the history kept, so it sees the calls
            // its predecessor logged in that session, not the whole persisted log.
            monitorView.historySession = callHistory.session;
            talkgroups.historySession = callHistory.session;
        }
    }

    // Closing the window finishes the Android Activity, and Qt then terminates the
    // process — taking the service that owns the engine with it. Background instead
    // when the host can; and when background listening is off, stop first so the
    // radio does not keep playing from a window the user just dismissed.
    onClosing: function (close) {
        // A handoff under way finishes even if the window goes to the background.
        if (!leavingSearch)
            cancelPendingRestart();
        if (Navigation.back(mainRoot)) {
            close.accepted = false;
            return;
        }
        if (!prefs.backgroundListening && mainRoot.sessionActive())
            decoderHost.stop();
        close.accepted = !decoderHost.moveToBackground();
    }

    function startSystem(row) {
        // Refused before cancelling, as restartSite() is: a USB attachment
        // while a handoff stops must not drop it.
        if (row < 0 || (decoderHost.sessionState !== 0 && decoderHost.sessionState !== 4))
            return;
        cancelPendingRestart();
        var sys = savedSystems.get(row);
        if (sys)
            mainRoot.startWithMap(sys, row, false);
    }

    function scanTargetLabel() {
        // The session's own map while the operator holds the tuner, not the
        // Explore one standing in for it.
        var list = mainRoot.operatorHolds ? mainRoot.pausedScanSystem : mainRoot.sessionSystem;
        if (!list || !list.entries)
            list = prefs.lastStartedKind === "scan" ? scanLists.getByUid(prefs.lastStartedUid) : null;
        if (list && list.targetSource === "csv")
            return metrics.scanTargetId || qsTr("Target %1").arg(metrics.scanTargetOrdinal);
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
     *
     * @a options.remember (default true) is whether an explore session may
     * become where Explore resumes; it is kept through a USB-access wait.
     */
    function startWithMap(sys, row, scan, options) {
        cancelPendingRestart();
        mainRoot.awaitingSessionInitialized = false;
        mainRoot.sessionReachedRunning = false;
        if (!sys || !sys.sourceType)
            return;
        var remember = !options || options.remember !== false;
        var previousSource = attemptedSource;
        attemptedSource = {
            kind: scan ? "scan" : row < 0 ? "explore" : "saved",
            uid: sys.uid || "",
            system: Object.assign({}, sys),
            remember: remember
        };
        completionMessage = "";
        var wantedSerial = sys.sourceType === "airspy" ? String((sys.airspy || {}).serial || "").toUpperCase() : "";
        if ((sys.sourceType === "usb" || sys.sourceType === "airspy") && decoderHost.localDeviceBrokered && (!decoderHost.localDeviceReady || decoderHost.localDeviceSource !== sys.sourceType || (wantedSerial.length > 0 && String(decoderHost.localDeviceSerial).toUpperCase() !== wantedSerial))) {
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
            mainRoot.pendingStartRemember = remember;
            decoderHost.requestLocalDeviceAccessForSource(sys.sourceType, String((sys.airspy || {}).serial || ""));
            return;
        }
        mainRoot.dismissedFailure = "";
        mainRoot.startError = "";
        mainRoot.awaitingUsbAccess = false;
        mainRoot.pendingStart = null;
        mainRoot.pendingStartRow = -1;
        mainRoot.pendingStartRemember = true;
        // What the start is, said here and not read off the map: an edited Explore start can carry a saved
        // system's uid, and must not record into that system's call recordings.
        var built = scan ? scanListStarter.start(sys, decoderHost) : sessionArgs.start(sys, decoderHost, row >= 0 ? "saved" : sys.signalSearch ? "search" : "explore");
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
            } else if (built.error === "am-needs-radio") {
                mainRoot.startError = qsTr("“%1” decodes AM, which needs a radio source: network and file audio arrives already demodulated. Edit the source to correct it.").arg(sys.name);
            } else if (built.error === "call-recording-option" || built.error === "call-recording-static-wav") {
                mainRoot.startError = built.errorText;
            } else if (built.error === "recording-missing") {
                mainRoot.startError = qsTr("The recording for “%1” is missing. Edit the system and choose its recording again.").arg(sys.name);
            } else if (built.error === "tetra-needs-iq") {
                mainRoot.startError = qsTr("“%1” decodes TETRA, which needs a radio source or a symbol capture (.bin): network audio and audio files arrive already demodulated. Edit the source to correct it.").arg(sys.name);
            } else if (built.error === "am-bandwidth") {
                mainRoot.startError = qsTr("“%1” decodes AM, whose 6 kHz channel needs a DSP bandwidth of 8 kHz or more. Set the source's bandwidth, or the default bandwidth in Settings, to %2 kHz.").arg(sys.name).arg(built.bandwidthChoices);
            } else {
                mainRoot.startError = qsTr("The session options are invalid. Review them before starting.");
            }
            return;
        }
        // Side effects only after the host accepts: a refused start must not
        // stamp lastHeard, re-attribute history rows, or hide the previous
        // session's calls from the monitor pane.
        if (!(built.started)) {
            // A host that refuses with nothing to publish over a run on screen
            // (starting or running) leaves that run as it was: its source stays
            // the one a failure names and Retry starts, and no card is put over it.
            if (decoderHost.failureText.length === 0) {
                if (decoderHost.sessionState === 1 || decoderHost.sessionState === 2)
                    attemptedSource = previousSource;
                else
                    mainRoot.startError = qsTr("“%1” could not be started.").arg(sys.name);
            }
            return;
        }
        mainRoot.exploring = (row < 0) && !sys.signalSearch;
        mainRoot.sessionSystem = sys;
        mainRoot.sessionScanList = !!scan;
        var startExploring = !scan && (row < 0) && !sys.signalSearch;
        mainRoot.sessionStartKind = scan ? "scan" : startExploring ? "explore" : "saved";
        mainRoot.sessionStartUid = startExploring ? "" : sys.uid || "";
        prefs.acceptedStartKind = mainRoot.sessionStartKind;
        prefs.acceptedStartUid = mainRoot.sessionStartUid;
        mainRoot.forgetOperatorHold();
        mainRoot.scanWarnings = scan ? built.warnings.join("\n") : "";
        mainRoot.awaitingSessionInitialized = true;
        mainRoot.sessionReachedRunning = decoderHost.sessionState === 2;
        if (mainRoot.sessionReachedRunning && !decoderHost.signalsSessionInitialized)
            mainRoot.recordSessionInitialized();
        mainRoot.sessionRow = scan ? -1 : row;
        // The session's intent, decided here and nowhere else: a system someone
        // saved is a thing to listen to, and the spectrum watches it. Only a
        // session with no saved system behind it is free to wander.
        mainRoot.exploring = !scan && (row < 0) && !sys.signalSearch;
        mainRoot.exploreRemembered = remember;
        // Belongs to the session that just ended. Carried into this one it would be
        // written back to prefs on stop as if it were where this exploring got to —
        // a frequency from two sessions ago, on a session that may never have moved.
        mainRoot.lastExploreFreqMhz = "";
        // The previous session may have committed calls since the last 250 ms
        // tick; ingest them under its own label before the label changes hands,
        // or its tail calls read as the new system's.
        uiController.flushHistory();
        // A new decode session on this system, begun after its predecessor's tail is
        // logged above, in one call: the history's background capture never runs on
        // a label, system or session only partly changed.
        // The monitor's recent-calls pane and the heard talkgroups show this session,
        // not the whole log. They select the rows the history logs in it, not rows
        // stamped after a clock reading: a replay's calls carry the capture's time,
        // and the decode clock may not have moved to it yet.
        callHistory.beginSessionOn(sys.name, (!scan && sys.uid) ? sys.uid : "");
        monitorView.historySession = callHistory.session;
        talkgroups.historySession = callHistory.session;
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

    /**
     * The receiver to start on, given the one remembered: Util.effectiveSource()
     * over what the host sees plugged in. Every Explore and Signal search entry
     * goes through here, so a remembered RTL dongle with only an Airspy attached
     * (or the reverse) starts on the Airspy everywhere, and rtl_tcp stays put.
     */
    function effectiveSource(remembered) {
        return Util.effectiveSource(remembered, String(decoderHost.attachedDeviceSource || ""));
    }

    // The Explore-here map an open Explore setup edits (Edit on its failure),
    // or null when the setup edits the remembered answer.
    property var exploreSetupBase: null
    /** Open Explore setup on the remembered answer, with the attached receiver resolved before its baseline. */
    function openExploreSetup() {
        mainRoot.exploreSetupBase = null;
        exploreSetup.reset(mainRoot.effectiveSource(prefs.exploreSourceType), prefs.exploreHost, prefs.explorePort, prefs.exploreFreqMhz);
        mainRoot.exploreSetupOpen = true;
    }

    /**
     * Open Explore setup on an Explore-here @p system: its receiver, resolved
     * against the one attached now, and its frequency. Its start keeps the
     * rest of that map (mode, gain) and, like Explore here itself, leaves
     * the remembered Explore answer alone.
     */
    function openExploreSetupOn(system) {
        var base = mainRoot.onAttachedReceiver(system);
        exploreSetup.reset(base.sourceType, base.host, base.port, base.freqMhz);
        mainRoot.exploreSetupBase = base;
        mainRoot.exploreSetupOpen = true;
    }

    /** Start an explore session from the remembered source and frequency. */
    function startExploring() {
        cancelPendingRestart();
        var source = prefs.exploreSourceType;
        if (source !== "usb" && source !== "airspy" && source !== "rtltcp") {
            // Nothing remembered to start from; ask instead of guessing.
            mainRoot.openExploreSetup();
            return;
        }
        mainRoot.startWithMap(mainRoot.exploreSystem(mainRoot.effectiveSource(source), prefs.exploreHost, prefs.explorePort, prefs.exploreFreqMhz), -1, false);
    }

    /**
     * Fill Signal search's receiver when Home opens it: the radio the last search
     * started on, else Explore's, else a USB dongle, then resolved against what
     * is attached. The endpoint comes with the answer it belongs to.
     */
    function seedSignalSearch() {
        var fromSearch = prefs.searchSourceType.length > 0;
        var remembered = fromSearch ? prefs.searchSourceType : prefs.exploreSourceType;
        signalSearchScreen.seed(mainRoot.effectiveSource(remembered) || "usb", (fromSearch ? prefs.searchHost : prefs.exploreHost) || "192.168.1.10", (fromSearch ? prefs.searchPort : prefs.explorePort) || 1234);
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
        opacity: (mainRoot.monitorMode || mainRoot.wizardOpen || mainRoot.exploreSetupOpen || mainRoot.signalSearchOpen || mainRoot.diagnosticsOpen || mainRoot.licensesOpen || mainRoot.callRecordingsOpen || mainRoot.radioReferenceAccountOpen || mainRoot.importsOpen || mainRoot.radioReferenceOpen || mainRoot.scanListOpen || mainRoot.sessionDestination.length > 0 || !prefs.onboardingDone) ? 0.0 : 1.0
        visible: opacity > 0.0
        enabled: opacity > 0.9 && !siteChooser.visible

        HomeScreen {
            id: homeScreen
            objectName: "homeScreen"
            exploreSource: mainRoot.effectiveSource(prefs.exploreSourceType)
            failure: ({
                    message: mainRoot.showFailure ? mainRoot.failureMessage() : "",
                    source: mainRoot.sourceLabel(),
                    canRetry: !decoderHost.sessionActive && !decoderHost.transitioning,
                    retryText: mainRoot.receiverNoticeShown ? qsTr("Reconnect") : qsTr("Retry")
                })
            // B1: an Airspy is as much a brokered receiver as an RTL-SDR.
            localDeviceRelevant: !mainRoot.showFailure || !mainRoot.attemptedSource || mainRoot.attemptedSource.system.sourceType === "usb" || mainRoot.attemptedSource.system.sourceType === "airspy"
            requestedSource: mainRoot.awaitingUsbAccess && mainRoot.pendingStart ? mainRoot.pendingStart.sourceType : decoderHost && decoderHost.localDeviceRequestedSource ? decoderHost.localDeviceRequestedSource : ""
            completionMessage: mainRoot.completionMessage
            onRetryFailure: mainRoot.retrySource()
            onEditFailure: mainRoot.editFailedSource()
            onDismissFailure: {
                mainRoot.dismissedFailure = mainRoot.failureText;
                // Gives up the attempt the card reports, so it can never start later.
                decoderHost.dismissFailure();
            }
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
            onImportSystem: mainRoot.openRadioReference(false)
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
            onSignalSearch: {
                mainRoot.seedSignalSearch();
                mainRoot.signalSearchOpen = true;
            }
            onExploreSetup: mainRoot.openExploreSetup()
        }

        HistoryScreen {
            id: historyRoot
            objectName: "idleHistoryScreen"
            x: mainRoot.expanded ? nav.width : 0
            y: 0
            width: parent.width - x
            height: parent.height - (mainRoot.expanded ? 0 : nav.height)
            visible: mainRoot.currentTab === 1
        }

        SettingsScreen {
            id: settingsRoot
            objectName: "idleSettingsScreen"
            x: mainRoot.expanded ? nav.width : 0
            y: 0
            width: parent.width - x
            height: parent.height - (mainRoot.expanded ? 0 : nav.height)
            visible: mainRoot.currentTab === 2

            onOpenDiagnostics: mainRoot.diagnosticsOpen = true
            onOpenImports: mainRoot.importsOpen = true
            onOpenRadioReferenceAccount: mainRoot.radioReferenceAccountOpen = true
            onOpenLicenses: mainRoot.licensesOpen = true
            onOpenCallRecordings: mainRoot.callRecordingsOpen = true
            backupNotice: backupFlow.notice
            backupNoticeProblem: backupFlow.noticeProblem
            onExportBackupRequested: backupFlow.beginExport()
            onRestoreBackupRequested: backupFlow.beginRestore()
            onCancelRestoreRequested: backupFlow.cancelPendingRestore()
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

        onClosed: {
            mainRoot.exploreSetupBase = null;
            mainRoot.exploreSetupOpen = false;
        }
        onStart: function (sourceType, host, port, freqMhz) {
            var base = mainRoot.exploreSetupBase;
            mainRoot.exploreSetupBase = null;
            if (base) {
                // Editing a failed Explore here: a one-off frequency, as the
                // attempt was, so the remembered answer is not rewritten.
                var sys = Object.assign(mainRoot.onReceiver(base, sourceType), {
                    host: host,
                    port: port,
                    freqMhz: freqMhz
                });
                mainRoot.exploreSetupOpen = false;
                mainRoot.startWithMap(sys, -1, false, {
                    remember: false
                });
                return;
            }
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
        objectName: "scanListScreen"
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
        sessionSystemUid: mainRoot.sessionSavedSystemUid
        onSystemProfileSaved: function (uid) {
            if (mainRoot.sessionSavedSystemUid === uid)
                mainRoot.sessionSystem = savedSystems.getByUid(uid);
        }
        scanTargetName: mainRoot.scanTargetLabel()
        sitesAvailable: mainRoot.running && !mainRoot.diagnosticsOpen && !mainRoot.licensesOpen && !mainRoot.callRecordingsOpen && !mainRoot.radioReferenceAccountOpen && !mainRoot.importsOpen && mainRoot.sessionSystem && savedSystems.siteCount(savedSystems.rowForUid(mainRoot.sessionSystem.uid || "")) > 0
        onOpenSites: {
            mainRoot.cancelPendingRestart();
            siteChooser.openFor(savedSystems.rowForUid(mainRoot.sessionSystem.uid));
        }
        opacity: mainRoot.monitorMode ? 1.0 : 0.0
        visible: opacity > 0.0
        // Stand down beneath overlays so the monitor cannot acquire passive tap
        // grabs. Otherwise closing an overlay can deliver the same release to
        // "Stop listening" underneath. Session-menu rows keep passive grabs so
        // the sheet can scroll when a drag starts on a destination or Cancel.
        enabled: opacity > 0.9 && !mainRoot.signalSearchOpen && !mainRoot.wizardOpen && !mainRoot.spectrumOpen && !mainRoot.radioReferenceOpen && !mainRoot.talkgroupsOpen && !talkgroupsScreen.visible && !siteChooser.visible && !sessionMenu.visible && !sessionRadio.visible && !mainRoot.diagnosticsOpen && !mainRoot.licensesOpen && !mainRoot.callRecordingsOpen && !mainRoot.radioReferenceAccountOpen && !(mainRoot.importsOpen && mainRoot.sessionDestination === "settings") && mainRoot.sessionDestination.length === 0

        onOpenSpectrum: {
            spectrumLoader.active = true;
            mainRoot.spectrumOpen = true;
        }
        onOpenTalkgroups: mainRoot.talkgroupsOpen = true
        onOpenSessionMenu: sessionMenu.open()
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
            scanList: mainRoot.sessionScanList
            sharedRadioSheet: sessionRadio

            onClosed: mainRoot.spectrumOpen = false
            onExploreFromHere: {
                // The engine is the authority on who owns the tuner; this only
                // asks, and a request the queue turns away changes nothing. The
                // session's identity follows the decoder's answer, not the ask
                // (syncOperatorHold()): the release ends the call on air, and
                // that call began under the system it was heard on.
                if (!commands.releaseTuner())
                    return;
                // A scan pauses rather than ends (issue #671): its list stays
                // this session's for Resume scan, and where the operator goes
                // meanwhile is not where an Explore session got to.
                var scan = metrics.scanKind !== 0;
                mainRoot.exploring = true;
                mainRoot.exploreRemembered = !scan;
                if (!scan && metrics.optionsKnown)
                    mainRoot.sessionScanList = false;
                // Seeded here rather than waiting for the next tuner reading: the
                // session may never move again, and an empty value would leave the
                // stop handler with nothing to remember this band by.
                //
                // Read once and guarded once. A zero reading — no tuner, or the
                // options snapshot briefly unavailable — is not a frequency, and
                // Util.mhzValueText(0) is the string "0.0000", which the session map,
                // the monitor header and the save-as-system wizard would all then
                // carry as if it were one. Empty is what monitorMeta() already
                // knows to leave out.
                if (metrics.centerFreqHz > 0)
                    mainRoot.lastExploreFreqMhz = Util.mhzValueText(metrics.centerFreqHz);
                mainRoot.exploreRequested = true;
                mainRoot.syncOperatorHold();
            }
            onSaveAsSystem: function (freqHz) {
                wizard.openForFound(mainRoot.sessionSystem, Util.mhzValueText(freqHz));
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

    SignalSearchScreen {
        id: signalSearchScreen
        anchors.fill: safeArea
        visible: mainRoot.signalSearchOpen
        enabled: visible && !mainRoot.wizardOpen
        searchModel: typeof signalSearch !== "undefined" ? signalSearch : null
        sessionActive: decoderHost.sessionActive
        leaving: mainRoot.leavingSearch
        sessionSource: metrics.radioSource || ({})
        // A receiver's ending, and a start attempt's own failure, are worded for
        // this screen: the search starts again from here.
        failure: mainRoot.receiverNoticeShown ? mainRoot.receiverNotice(true) : mainRoot.failureText === mainRoot.hostFailure && mainRoot.searchStartFailure().length > 0 ? mainRoot.searchStartFailure() : mainRoot.failureText
        onBackRequested: mainRoot.requestBack()
        onClosed: {
            mainRoot.abandonSignalSearchStart();
            mainRoot.signalSearchOpen = false;
        }
        onStartRefused: mainRoot.abandonSignalSearchStart()
        onStop: {
            mainRoot.abandonSignalSearchStart();
            // Stopped from this screen, the search stays on it, ready to restart,
            // whether Home opened it or a running search reopened it after
            // Activity recreation, where only signalSearchOpen's status.owned
            // binding held it open and would close it with the session.
            mainRoot.signalSearchOpen = true;
            decoderHost.stop();
        }
        onStart: function (sourceType, host, port, frequency) {
            // Remembered before the start, as Explore's answer is: a start that
            // fails is still the radio the user chose. Its own keys, so a search
            // never moves where Explore resumes.
            prefs.searchSourceType = sourceType;
            prefs.searchHost = host;
            prefs.searchPort = port;
            var sys = mainRoot.exploreSystem(sourceType, host, port, frequency);
            sys.signalSearch = true;
            sys.name = qsTr("Signal search");
            sys.bandwidthKhz = 48;
            mainRoot.startWithMap(sys, -1, false);
            mainRoot.spectrumOpen = false;
        }
        onExploreDetection: function (hit) {
            mainRoot.leaveSearchFor({
                kind: "explore",
                system: mainRoot.exploreHereSystem(hit, mainRoot.exploreSystem(sourceType, host, port, hit.freqMHz)),
                remember: false
            });
        }
        onStartSavedSystem: function (uid) {
            mainRoot.leaveSearchFor({
                kind: "saved",
                uid: uid
            });
        }
        onSaveDetection: function (hit) {
            var source = mainRoot.sessionSystem || mainRoot.exploreSystem(sourceType, host, port, hit.freqMHz);
            var found = searchModel.systemForHit(hit, source);
            wizard.openForFound(found, hit.freqMHz, found);
            mainRoot.wizardOpen = true;
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
        objectName: "sessionTools"
        Keys.onEscapePressed: mainRoot.requestBack()
        Keys.onBackPressed: mainRoot.requestBack()
        function restoreLayerFocus() {
            if (visible && enabled) Qt.callLater(function () {
                if (sessionTools.visible && sessionTools.enabled && Navigation.allows(sessionTools) && !Navigation.contains(sessionTools, mainRoot.activeFocusItem))
                    sessionTools.forceActiveFocus();
            });
        }
        onVisibleChanged: restoreLayerFocus()
        onEnabledChanged: restoreLayerFocus()
        anchors.fill: safeArea
        visible: mainRoot.sessionDestination.length > 0
        enabled: visible && !mainRoot.diagnosticsOpen && !mainRoot.licensesOpen && !mainRoot.callRecordingsOpen && !mainRoot.radioReferenceAccountOpen && !mainRoot.importsOpen && !mainRoot.radioReferenceOpen
        Rectangle {
            anchors.fill: parent
            color: Theme.bg
        }
        Item {
            id: sessionToolsHeader
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.margins: Theme.screenPadding
            height: 46

            IconButton {
                id: sessionToolsBack
                objectName: "sessionToolsBack"
                icon: "back"
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                onClicked: sessionToolsLayer.leave()
            }
            Text {
                objectName: "sessionToolsTitle"
                anchors.left: sessionToolsBack.right
                anchors.leftMargin: 14
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: mainRoot.sessionDestination === "history" ? qsTr("History") : qsTr("Settings")
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(22)
                font.weight: Font.Bold
                font.letterSpacing: -0.22
                color: Theme.textPrimary
                elide: Text.ElideRight
            }
        }
        HistoryScreen {
            objectName: "sessionHistoryScreen"
            showTitle: false
            anchors.top: sessionToolsHeader.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            visible: mainRoot.sessionDestination === "history"
        }
        SettingsScreen {
            objectName: "sessionSettingsScreen"
            showTitle: false
            anchors.top: sessionToolsHeader.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            visible: mainRoot.sessionDestination === "settings"
            onOpenDiagnostics: mainRoot.diagnosticsOpen = true
            onOpenImports: mainRoot.importsOpen = true
            onOpenRadioReferenceAccount: mainRoot.radioReferenceAccountOpen = true
            onOpenLicenses: mainRoot.licensesOpen = true
            onOpenCallRecordings: mainRoot.callRecordingsOpen = true
            backupNotice: backupFlow.notice
            backupNoticeProblem: backupFlow.noticeProblem
            onExportBackupRequested: backupFlow.beginExport()
            onRestoreBackupRequested: backupFlow.beginRestore()
            onCancelRestoreRequested: backupFlow.cancelPendingRestore()
        }
    }
    ActionMenu {
        id: sessionMenu
        objectName: "sessionMenu"
        anchors.fill: safeArea
        title: qsTr("Session options")
        actions: {
            var items = [];
            if (monitor.sitesAvailable)
                items.push({text: qsTr("Sites"), key: "sites", objectName: "sessionMenuSites"});
            if (metrics.radioInput) {
                items.push({text: qsTr("Radio"), key: "radio", objectName: "sessionMenuRadio"});
                items.push({text: qsTr("Spectrum"), key: "spectrum", objectName: "sessionMenuSpectrum"});
            }
            items.push({text: qsTr("Talkgroups"), key: "talkgroups", objectName: "sessionMenuTalkgroups"});
            items.push({text: qsTr("History"), key: "history", objectName: "sessionMenuHistory"});
            items.push({text: qsTr("Settings"), key: "settings", objectName: "sessionMenuSettings"});
            items.push({text: qsTr("Diagnostics"), key: "diagnostics", objectName: "sessionMenuDiagnostics"});
            if (mainRoot.sessionRow >= 0)
                items.push({text: qsTr("Edit saved system"), key: "edit", objectName: "sessionMenuEdit"});
            return items;
        }
        onTriggered: function (index) {
            var key = actions[index].key;
            if (key === "sites")
                monitor.openSites();
            else if (key === "radio")
                sessionRadio.open();
            else if (key === "spectrum") {
                spectrumLoader.active = true;
                mainRoot.spectrumOpen = true;
            } else if (key === "talkgroups")
                mainRoot.talkgroupsOpen = true;
            else if (key === "history" || key === "settings")
                mainRoot.sessionDestination = key;
            else if (key === "diagnostics")
                mainRoot.diagnosticsOpen = true;
            else if (key === "edit" && mainRoot.sessionRow >= 0) {
                wizard.openForEdit(mainRoot.sessionRow);
                mainRoot.wizardOpen = true;
            }
        }
    }

    // Shared by Monitor and Spectrum; opening Radio never allocates the waterfall.
    RadioSettingsPersistence {
        id: radioPersistence
        systemUid: mainRoot.sessionSavedSystemUid
        active: decoderHost.running
        onSaved: mainRoot.sessionSystem = savedSystems.getByUid(systemUid)
    }

    RadioSheet {
        id: sessionRadio
        anchors.fill: safeArea
        saveError: radioPersistence.error
        savesToSystem: radioPersistence.systemUid.length > 0
        onSettingRequested: function (key, value) { radioPersistence.request(key, value); }
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

    // The log is ticked every 250 ms; only an open screen takes its rows (issue #718).
    Binding {
        target: diagnosticsLog
        property: "active"
        value: diagnosticsScreen.visible
    }

    LicensesScreen {
        id: licensesScreen
        objectName: "licensesScreen"
        anchors.fill: safeArea
        visible: mainRoot.licensesOpen
        enabled: visible
        onClosed: mainRoot.licensesOpen = false
    }

    // Issue #708: pushed from either Settings, over the tab shell and the session tools alike.
    CallRecordingsScreen {
        id: callRecordingsScreen
        objectName: "callRecordingsScreen"
        anchors.fill: safeArea
        visible: mainRoot.callRecordingsOpen
        enabled: visible
        onClosed: mainRoot.callRecordingsOpen = false
    }

    RadioReferenceAccountScreen {
        id: radioReferenceAccountScreen
        objectName: "radioReferenceAccountScreen"
        anchors.fill: safeArea
        visible: mainRoot.radioReferenceAccountOpen
        enabled: visible
        onClosed: mainRoot.radioReferenceAccountOpen = false
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

    // ---- RadioReference import (pushed from the library or the wizard) ----
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
            // Session tools appear later in this file than the wizard, so they
            // can cover it. Retire their navigation layer before the handoff.
            mainRoot.sessionDestination = "";
            Qt.inputMethod.hide();
            if (!mainRoot.radioReferenceFromWizard) {
                // Opened from Home, Settings or the library: the source, gain and name
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
        currentSiteUid: mainRoot.sessionSystem ? (mainRoot.sessionSystem.uid || "") : ""
        // Any input cancels a site restart still waiting; a Signal search
        // handoff instead takes no input until it finishes (leavingSearch).
        onUserAction: {
            if (!mainRoot.leavingSearch)
                mainRoot.cancelPendingRestart();
        }
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
        objectName: "failureDetails"
        anchors.fill: safeArea
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

    // ---- Backup export and restore (issue #580) ----
    // One flow for both Settings screens. Above the scan-list editor (z 20) and
    // the scan warnings (z 21), so its sheets cover every layer, the session's
    // Settings included; with no sheet up it is an empty item taps pass through.
    BackupFlow {
        id: backupFlow
        z: 22
        anchors.fill: safeArea
        onRestoreBegun: mainRoot.dropPendingStarts()
    }
}
