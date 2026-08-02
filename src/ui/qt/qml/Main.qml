// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

ApplicationWindow {
    id: root

    visible: true
    width: 480
    height: 900
    title: qsTr("DSD-neo")

    readonly property bool running: decoderHost ? decoderHost.running : false

    // Closing the window finishes the Android Activity, and Qt then terminates the
    // process — taking the service that owns the engine with it. Background instead.
    onClosing: function (close) {
        close.accepted = false
        decoderHost.moveToBackground()
    }

    // Builds the CLI-shaped argv the host hands to the engine. Reusing the CLI
    // parser is what buys the whole option surface for free.
    function buildArgs() {
        var args = ["--frontend", "none"]

        if (inputMode.currentIndex === 0) {
            var spec = "rtltcp:" + rtlHost.text + ":" + rtlPort.text
            if (rtlFreq.text.length > 0)
                spec += ":" + rtlFreq.text + ":" + rtlGain.text + ":" + rtlPpm.text + ":" + rtlBw.text + ":0:2"
            args.push("-i", spec)
        } else if (inputMode.currentIndex === 1) {
            args.push("-i", "udp:0.0.0.0:" + udpPort.text)
        } else if (inputMode.currentIndex === 2) {
            args.push("-i", "tcp:" + tcpHost.text + ":" + tcpPort.text)
        } else {
            args.push("-i", filePath.text)
        }

        args.push("-o", "pulse")

        if (decodeMode.currentValue.length > 0)
            args.push(decodeMode.currentValue)
        if (modulation.currentValue.length > 0)
            args.push(modulation.currentValue)
        if (trunking.checked)
            args.push("-T")
        if (encLockout.checked)
            args.push("--enc-lockout")

        var extra = extraArgs.text.trim()
        if (extra.length > 0) {
            var parts = extra.split(/\s+/)
            for (var i = 0; i < parts.length; i++)
                args.push(parts[i])
        }
        return args
    }

    // The engine opens real filesystem paths, so whatever the platform picker hands
    // back is materialized by the host first.
    FileDialog {
        id: fileDialog

        onAccepted: {
            var reference = selectedFile.toString()
            var name = reference.substring(reference.lastIndexOf('/') + 1)
            var path = decoderHost.importContentUri(reference, name)
            if (path.length > 0)
                filePath.text = path
        }
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

        // Two scroll surfaces, side by side rather than nested: the settings above
        // and the event log below. A phone cannot show all of this at once, and a
        // list inside a scrolling page fights the user for the drag gesture.
        ScrollView {
            id: settingsScroll

            Layout.fillWidth: true
            Layout.fillHeight: true
            // Configure when idle, watch when running.
            Layout.preferredHeight: root.running ? 2 : 5
            clip: true
            contentWidth: availableWidth

            ColumnLayout {
                width: settingsScroll.availableWidth
                spacing: 10

                GroupBox {
                    title: qsTr("Input")
                    Layout.fillWidth: true

                    GridLayout {
                        anchors.fill: parent
                        columns: 2
                        columnSpacing: 8
                        rowSpacing: 6

                        Label { text: qsTr("Source") }
                        ComboBox {
                            id: inputMode
                            Layout.fillWidth: true
                            model: [qsTr("RTL-TCP"), qsTr("UDP PCM"), qsTr("TCP PCM"), qsTr("Local file")]
                        }

                        Label { text: qsTr("Host"); visible: inputMode.currentIndex === 0 }
                        TextField {
                            id: rtlHost
                            Layout.fillWidth: true
                            visible: inputMode.currentIndex === 0
                            text: "192.168.1.10"
                            inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoAutoUppercase
                        }

                        Label { text: qsTr("Port"); visible: inputMode.currentIndex === 0 }
                        TextField {
                            id: rtlPort
                            Layout.fillWidth: true
                            visible: inputMode.currentIndex === 0
                            text: "1234"
                            inputMethodHints: Qt.ImhDigitsOnly
                        }

                        Label { text: qsTr("Frequency"); visible: inputMode.currentIndex === 0 }
                        TextField {
                            id: rtlFreq
                            Layout.fillWidth: true
                            visible: inputMode.currentIndex === 0
                            text: "769.76875M"
                            placeholderText: qsTr("e.g. 851.375M")
                        }

                        Label { text: qsTr("Gain / PPM / BW"); visible: inputMode.currentIndex === 0 }
                        RowLayout {
                            Layout.fillWidth: true
                            visible: inputMode.currentIndex === 0
                            TextField { id: rtlGain; Layout.fillWidth: true; text: "13"; inputMethodHints: Qt.ImhDigitsOnly }
                            TextField { id: rtlPpm; Layout.fillWidth: true; text: "-2" }
                            TextField { id: rtlBw; Layout.fillWidth: true; text: "48"; inputMethodHints: Qt.ImhDigitsOnly }
                        }

                        Label { text: qsTr("Bind port"); visible: inputMode.currentIndex === 1 }
                        TextField {
                            id: udpPort
                            Layout.fillWidth: true
                            visible: inputMode.currentIndex === 1
                            text: "7355"
                            inputMethodHints: Qt.ImhDigitsOnly
                        }

                        Label { text: qsTr("Host"); visible: inputMode.currentIndex === 2 }
                        TextField {
                            id: tcpHost
                            Layout.fillWidth: true
                            visible: inputMode.currentIndex === 2
                            text: "127.0.0.1"
                            inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoAutoUppercase
                        }

                        Label { text: qsTr("Port"); visible: inputMode.currentIndex === 2 }
                        TextField {
                            id: tcpPort
                            Layout.fillWidth: true
                            visible: inputMode.currentIndex === 2
                            text: "7355"
                            inputMethodHints: Qt.ImhDigitsOnly
                        }

                        Label { text: qsTr("File"); visible: inputMode.currentIndex === 3 }
                        RowLayout {
                            Layout.fillWidth: true
                            visible: inputMode.currentIndex === 3

                            TextField {
                                id: filePath
                                Layout.fillWidth: true
                                placeholderText: qsTr("pick a .wav or .bin")
                                inputMethodHints: Qt.ImhNoAutoUppercase
                            }

                            Button {
                                text: qsTr("Browse")
                                onClicked: fileDialog.open()
                            }
                        }
                    }
                }

                GroupBox {
                    title: qsTr("Decode")
                    Layout.fillWidth: true

                    GridLayout {
                        anchors.fill: parent
                        columns: 2
                        columnSpacing: 8
                        rowSpacing: 6

                        Label { text: qsTr("Mode") }
                        ComboBox {
                            id: decodeMode
                            Layout.fillWidth: true
                            textRole: "label"
                            valueRole: "flag"
                            // "Default" passes no -f flag, i.e. the decoder's own default
                            // frame sync. "-fa" (Auto) enables every decoder at once, which
                            // on a trunked control channel synthesises audio out of data.
                            model: [
                                { label: qsTr("Default"), flag: "" },
                                { label: qsTr("Auto (all decoders)"), flag: "-fa" },
                                { label: qsTr("P25 Phase 1"), flag: "-f1" },
                                { label: qsTr("P25 Phase 2"), flag: "-f2" },
                                { label: qsTr("DMR"), flag: "-fs" },
                                { label: qsTr("NXDN48"), flag: "-fi" },
                                { label: qsTr("NXDN96"), flag: "-fn" },
                                { label: qsTr("D-STAR"), flag: "-fd" },
                                { label: qsTr("YSF"), flag: "-fy" },
                                { label: qsTr("M17"), flag: "-fz" }
                            ]
                        }

                        Label { text: qsTr("Modulation") }
                        ComboBox {
                            id: modulation
                            Layout.fillWidth: true
                            textRole: "label"
                            valueRole: "flag"
                            model: [
                                { label: qsTr("Default"), flag: "" },
                                { label: qsTr("C4FM"), flag: "-mc" },
                                { label: qsTr("QPSK"), flag: "-mq" },
                                { label: qsTr("GFSK"), flag: "-mg" },
                                { label: qsTr("Auto"), flag: "-ma" }
                            ]
                        }

                        CheckBox { id: trunking; text: qsTr("Trunking (-T)"); checked: true }
                        CheckBox { id: encLockout; text: qsTr("Enc lockout"); checked: true }

                        Label { text: qsTr("Extra args") }
                        TextField {
                            id: extraArgs
                            Layout.fillWidth: true
                            placeholderText: qsTr("e.g. -C chan.csv -G group.csv")
                            inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Button {
                text: root.running ? qsTr("Stop") : qsTr("Start")
                highlighted: true
                Layout.fillWidth: true
                onClicked: {
                    if (root.running)
                        decoderHost.stop()
                    else
                        decoderHost.start(root.buildArgs())
                }
            }

            Button {
                text: qsTr("Mute")
                enabled: root.running
                onClicked: commands.toggleMute()
            }

            Button {
                text: qsTr("Lockout")
                enabled: root.running
                onClicked: commands.lockoutSlot(0)
            }
        }

        GroupBox {
            title: qsTr("Status")
            Layout.fillWidth: true

            GridLayout {
                anchors.fill: parent
                columns: 2
                columnSpacing: 8
                rowSpacing: 2

                Label { text: qsTr("Stream") }
                Label {
                    Layout.fillWidth: true
                    font.family: monoFontFamily
                    text: (metrics.streamActive ? qsTr("active") : qsTr("idle"))
                          + "  " + metrics.symbolRateHz + " sym/s  " + metrics.outputRateHz + " Hz"
                }

                Label { text: qsTr("SNR") }
                Label {
                    Layout.fillWidth: true
                    font.family: monoFontFamily
                    text: metrics.snrDb.toFixed(1) + " dB   "
                          + (metrics.carrierLock ? qsTr("lock") : qsTr("no lock"))
                          + "   " + metrics.cfoHz.toFixed(0) + " Hz"
                }

                Label { text: qsTr("Gain") }
                Label { Layout.fillWidth: true; font.family: monoFontFamily; text: metrics.tunerGainText }

                Label { text: qsTr("Slot 1") }
                Label { Layout.fillWidth: true; elide: Text.ElideRight; font.family: monoFontFamily; text: metrics.slot1Text }

                Label { text: qsTr("Slot 2") }
                Label { Layout.fillWidth: true; elide: Text.ElideRight; font.family: monoFontFamily; text: metrics.slot2Text }
            }
        }

        Label {
            Layout.fillWidth: true
            visible: metrics.messageText.length > 0
            wrapMode: Text.Wrap
            text: metrics.messageText
        }

        GroupBox {
            title: qsTr("Events")
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredHeight: root.running ? 5 : 2

            ListView {
                id: eventView
                anchors.fill: parent
                clip: true
                model: eventLog
                spacing: 1

                delegate: Label {
                    width: eventView.width
                    font.family: monoFontFamily
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                    text: model.text
                }

                ScrollBar.vertical: ScrollBar {}
            }
        }
    }
}
