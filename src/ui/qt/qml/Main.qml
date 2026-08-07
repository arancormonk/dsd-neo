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
    // asked. Cleared once the device is ready or another system starts.
    property bool awaitingUsbAccess: false
    readonly property string usbAccessText:
        awaitingUsbAccess && decoderHost && !decoderHost.localDeviceReady ? decoderHost.localDeviceStatus : ""
    readonly property string failureText: startError.length > 0 ? startError
                                          : usbAccessText.length > 0 ? usbAccessText : hostFailure

    property int currentTab: 0
    property bool wizardOpen: false
    // The saved-system map the running session was started from.
    property var sessionSystem: null

    // Suppresses a failure banner the user has read. Reset on the next start so a
    // repeat of the same failure is reported again rather than swallowed.
    property string dismissedFailure: ""
    readonly property bool showFailure:
        !monitorMode && failureText.length > 0 && failureText !== dismissedFailure

    onMonitorModeChanged: {
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
            // and it keeps updating as the permission dialog resolves.
            mainRoot.dismissedFailure = ""
            mainRoot.startError = ""
            mainRoot.awaitingUsbAccess = true
            decoderHost.requestLocalDeviceAccess()
            return
        }
        mainRoot.dismissedFailure = ""
        mainRoot.startError = ""
        mainRoot.awaitingUsbAccess = false
        var args = Util.buildArgs(sys, prefs)
        if (!args) {
            // buildArgs refuses for exactly two reasons; blame the field that is
            // actually wrong or the user re-checks a frequency that was fine.
            mainRoot.startError = !Util.freqValid(sys.freqMhz)
                ? qsTr("“%1” has no valid frequency — long-press its card to edit it.").arg(sys.name)
                : qsTr("“%1” has an invalid PPM correction — long-press its card to edit it.").arg(sys.name)
            return
        }
        // Side effects only after the host accepts: a refused start must not
        // stamp lastHeard, re-attribute history rows, or hide the previous
        // session's calls from the monitor pane.
        if (!decoderHost.start(args)) {
            if (decoderHost.failureText.length === 0)
                mainRoot.startError = qsTr("“%1” could not be started.").arg(sys.name)
            return
        }
        mainRoot.sessionSystem = sys
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
