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
    // The saved-system row whose start is waiting on that grant.
    property int pendingUsbRow: -1
    readonly property string usbAccessText:
        awaitingUsbAccess && decoderHost && !decoderHost.localDeviceReady ? decoderHost.localDeviceStatus : ""
    readonly property string failureText: startError.length > 0 ? startError
                                          : usbAccessText.length > 0 ? usbAccessText : hostFailure

    property int currentTab: 0
    property bool wizardOpen: false
    // The spectrum view is pushed over the monitor, so it can only be open
    // while a session is; ending one has to take it down with it.
    property bool spectrumOpen: false
    // The saved-system map the running session was started from.
    property var sessionSystem: null

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
            pendingUsbRow = -1
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
            var row = mainRoot.pendingUsbRow
            mainRoot.pendingUsbRow = -1
            if (row >= 0)
                mainRoot.startSystem(row)
        }
    }

    onMonitorModeChanged: {
        // The spectrum layer lives above the monitor; when the session goes so
        // does it, or the next one would open onto a stale panorama.
        if (!monitorMode)
            mainRoot.spectrumOpen = false
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
            mainRoot.pendingUsbRow = row
            decoderHost.requestLocalDeviceAccess()
            return
        }
        mainRoot.dismissedFailure = ""
        mainRoot.startError = ""
        mainRoot.awaitingUsbAccess = false
        mainRoot.pendingUsbRow = -1
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
        // The previous session may have committed calls since the last 250 ms
        // tick; ingest them under its own label before the label changes hands,
        // or its tail calls read as the new system's.
        uiController.flushHistory()
        callHistory.sessionLabel = sys.name
        // The monitor's recent-calls pane shows this session, not the whole log.
        monitorView.minWhen = Math.floor(Date.now() / 1000)
        savedSystems.touch(row)
    }

    // ---- Tab shell ----
    Item {
        id: shell

        anchors.fill: parent
        opacity: (mainRoot.monitorMode || mainRoot.wizardOpen || !prefs.onboardingDone) ? 0.0 : 1.0
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

    // ---- Add-system wizard (pushed) ----
    WizardScreen {
        id: wizard

        anchors.fill: parent
        opacity: mainRoot.wizardOpen && !mainRoot.monitorMode ? 1.0 : 0.0
        visible: opacity > 0.0
        enabled: opacity > 0.9

        Behavior on opacity {
            NumberAnimation { duration: 150; easing.type: Easing.OutCubic }
        }

        onClosed: mainRoot.wizardOpen = false
        onSaved: function (row) {
            mainRoot.wizardOpen = false
            mainRoot.currentTab = 0
        }
    }

    // ---- Live monitor (owns the screen while a session is active) ----
    MonitorScreen {
        id: monitor

        anchors.fill: parent
        system: mainRoot.sessionSystem
        opacity: mainRoot.monitorMode ? 1.0 : 0.0
        visible: opacity > 0.0
        enabled: opacity > 0.9

        Behavior on opacity {
            NumberAnimation { duration: 150; easing.type: Easing.OutCubic }
        }

        onOpenSpectrum: {
            spectrumLoader.active = true
            mainRoot.spectrumOpen = true
        }
    }

    // ---- Spectrum (pushed over the monitor) ----
    // Built on first open, then kept. Its waterfall holds a full-resolution
    // history image, which is real memory to hand every user who never opens
    // the view — and rebuilding it on each visit would throw that history away.
    Loader {
        id: spectrumLoader

        anchors.fill: parent
        active: false
        sourceComponent: spectrumScreen
        opacity: mainRoot.monitorMode && mainRoot.spectrumOpen ? 1.0 : 0.0
        visible: opacity > 0.0
        enabled: opacity > 0.9

        Behavior on opacity {
            NumberAnimation { duration: 150; easing.type: Easing.OutCubic }
        }
    }

    Component {
        id: spectrumScreen

        SpectrumScreen {
            onClosed: mainRoot.spectrumOpen = false
        }
    }

    // ---- First-run onboarding ----
    OnboardingScreen {
        anchors.fill: parent
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
