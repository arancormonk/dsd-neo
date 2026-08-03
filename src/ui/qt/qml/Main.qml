// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: root

    visible: true
    width: 480
    height: 900
    title: qsTr("DSD-neo")

    // One screen with two purposes. Idle is for configuring a session; the moment one
    // is asked for, the screen becomes about watching it, and it changes back when the
    // session ends. Status and events exist only in the second — a phone cannot show
    // both jobs at once, and panels reporting on a decoder that is not running are
    // worse than absent, because they keep their last live values.
    readonly property bool monitorMode: decoderHost ? decoderHost.sessionActive : false
    readonly property bool running: decoderHost ? decoderHost.running : false
    readonly property bool transitioning: decoderHost ? decoderHost.transitioning : false
    readonly property string failureText: decoderHost ? decoderHost.failureText : ""

    // A directly attached dongle needs no permission gesture on desktop; on Android
    // the host has to obtain the descriptor first.
    readonly property bool localDeviceBlocked:
        setup.inputKey === "usb" && decoderHost && decoderHost.localDeviceBrokered && !decoderHost.localDeviceReady

    // The exact argv the running session was started with, for the summary chip.
    property var sessionArgs: []
    // Suppresses a failure banner the user has read. Reset on the next start so a
    // repeat of the same failure is reported again rather than swallowed.
    property string dismissedFailure: ""
    readonly property bool showFailure:
        !monitorMode && failureText.length > 0 && failureText !== dismissedFailure

    onMonitorModeChanged: {
        if (!monitorMode)
            return
        // Bring the session into view rather than merely revealing it: the frequency
        // field usually still holds focus, so the keyboard would cover the panes that
        // just appeared, and the list has to start at the newest row.
        Qt.inputMethod.hide()
        monitor.scrollToNewest()
    }

    // Closing the window finishes the Android Activity, and Qt then terminates the
    // process — taking the service that owns the engine with it. Background instead.
    // Refused only when the host actually took the window somewhere: a host that
    // cannot background itself would otherwise leave a window nothing can close.
    onClosing: function (close) {
        close.accepted = !decoderHost.moveToBackground()
    }

    function startSession() {
        root.dismissedFailure = ""
        var args = setup.buildArgs()
        root.sessionArgs = args
        decoderHost.start(args)
    }

    header: ToolBar {
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 12
            anchors.rightMargin: 12

            Label {
                text: qsTr("DSD-neo")
                font.pixelSize: 20
                Layout.fillWidth: true
            }

            Label {
                text: decoderHost ? decoderHost.statusText : ""
                opacity: 0.8
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 10

        // A start that dies inside the platform layer otherwise leaves nothing behind
        // but a return to the setup screen, which reads as "nothing happened".
        Frame {
            Layout.fillWidth: true
            visible: root.showFailure

            RowLayout {
                anchors.fill: parent
                spacing: 8

                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: root.failureText
                }

                ToolButton {
                    text: "✕"
                    onClicked: root.dismissedFailure = root.failureText
                }
            }
        }

        // Both panes stay instantiated: a Loader would discard whatever the user has
        // typed into the setup form every time a session starts.
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            SetupPane {
                id: setup
                anchors.fill: parent
                opacity: root.monitorMode ? 0.0 : 1.0
                visible: opacity > 0.0
                enabled: opacity > 0.9

                Behavior on opacity {
                    NumberAnimation { duration: 150; easing.type: Easing.OutCubic }
                }
            }

            MonitorPane {
                id: monitor
                anchors.fill: parent
                opacity: root.monitorMode ? 1.0 : 0.0
                visible: opacity > 0.0
                enabled: opacity > 0.9
                summaryText: setup.summaryText
                onSummaryClicked: argsSheet.open()

                Behavior on opacity {
                    NumberAnimation { duration: 150; easing.type: Easing.OutCubic }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            BusyIndicator {
                visible: root.transitioning
                running: root.transitioning
                implicitWidth: 28
                implicitHeight: 28
            }

            Button {
                // Starting without a descriptor would just fail in the engine, so the
                // button asks for the dongle first when that is what is missing. While
                // the service is mid-transition it says so and refuses input: the
                // second tap it used to accept was rejected further down with no sign
                // of it on screen.
                text: root.transitioning
                      ? decoderHost.statusText
                      : (root.running
                         ? qsTr("Stop")
                         : (root.localDeviceBlocked ? qsTr("Connect dongle") : qsTr("Start")))
                highlighted: true
                enabled: !root.transitioning
                Layout.fillWidth: true
                onClicked: {
                    if (root.running)
                        decoderHost.stop()
                    else if (root.localDeviceBlocked)
                        decoderHost.requestLocalDeviceAccess()
                    else
                        root.startSession()
                }
            }

            // Both act on the live engine, so they belong to the session rather than
            // to the screen that configures one.
            Button {
                text: qsTr("Mute")
                visible: root.monitorMode
                enabled: root.running
                onClicked: commands.toggleMute()
            }

            Button {
                text: qsTr("Lockout")
                visible: root.monitorMode
                enabled: root.running
                onClicked: commands.lockoutSlot(0)
            }
        }

        // The finished session's log stays one tap away instead of on screen: it is
        // history the moment the engine stops, not status.
        ItemDelegate {
            Layout.fillWidth: true
            visible: !root.monitorMode && eventLog.count > 0
            text: qsTr("Last session — %1 events").arg(eventLog.count)
            onClicked: logSheet.open()
        }
    }

    Popup {
        id: logSheet

        anchors.centerIn: Overlay.overlay
        width: Math.round(root.width * 0.92)
        height: Math.round(root.height * 0.8)
        modal: true
        padding: 12

        ColumnLayout {
            anchors.fill: parent
            spacing: 8

            RowLayout {
                Layout.fillWidth: true

                Label {
                    Layout.fillWidth: true
                    font.pixelSize: 16
                    text: qsTr("Last session")
                }

                Button {
                    text: qsTr("Close")
                    onClicked: logSheet.close()
                }
            }

            EventLogView {
                Layout.fillWidth: true
                Layout.fillHeight: true
            }
        }
    }

    // What the collapsed chip expands to. The argv is the whole truth about how the
    // session was configured, and unlike the form it cannot drift from it.
    Popup {
        id: argsSheet

        anchors.centerIn: Overlay.overlay
        width: Math.round(root.width * 0.92)
        modal: true
        padding: 12

        // No height and no anchors: the popup takes its height from this layout, so
        // filling the parent instead would make the two depend on each other. The width
        // is pinned to the popup's, though: left to itself the layout takes its width
        // from the argv line's natural length, which is one unbroken run of monospace
        // wider than any phone, so Layout.fillWidth had nothing to resolve against and
        // the options this sheet exists to show ran off the right edge unwrapped.
        ColumnLayout {
            width: argsSheet.availableWidth
            spacing: 8

            Label {
                Layout.fillWidth: true
                font.pixelSize: 16
                text: qsTr("Session options")
            }

            Label {
                Layout.fillWidth: true
                font.family: monoFontFamily
                font.pixelSize: 12
                // WrapAnywhere, not Wrap: an input spec is a single long token with no
                // spaces to break at, so word wrapping alone would still overflow.
                wrapMode: Text.WrapAnywhere
                text: root.sessionArgs.join(" ")
            }

            Label {
                Layout.fillWidth: true
                opacity: 0.7
                wrapMode: Text.Wrap
                text: qsTr("Changing these takes effect the next time you start.")
            }

            Button {
                Layout.alignment: Qt.AlignRight
                text: qsTr("Close")
                onClicked: argsSheet.close()
            }
        }
    }
}
