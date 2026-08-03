// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

// Everything that configures a session, and nothing that reports on one. This owns
// the field values, so it stays instantiated while the monitoring view is on screen —
// unloading it would throw away whatever the user typed.
Item {
    id: pane

    // Keyed rather than indexed: the visibility bindings below would otherwise all
    // shift every time an input source is added.
    readonly property string inputKey: inputMode.currentValue ? inputMode.currentValue : "usb"

    // One line describing the configured session, for the collapsed chip the
    // monitoring view shows in place of this pane.
    readonly property string summaryText: {
        var parts = [inputMode.currentText]
        if (pane.inputKey === "usb" || pane.inputKey === "rtltcp")
            parts.push(rtlFreq.text)
        else if (pane.inputKey === "udp")
            parts.push(":" + udpPort.text)
        else if (pane.inputKey === "tcp")
            parts.push(tcpHost.text + ":" + tcpPort.text)
        else if (filePath.text.length > 0)
            parts.push(filePath.text.substring(filePath.text.lastIndexOf('/') + 1))

        if (decodeMode.currentValue.length > 0)
            parts.push(decodeMode.currentText)
        if (modulation.currentValue.length > 0)
            parts.push(modulation.currentText)
        if (trunking.checked)
            parts.push(qsTr("trunking"))
        return parts.join(" · ")
    }

    // Frequency/gain/PPM/bandwidth tail shared by the local dongle and rtl_tcp — the
    // same tuner options either way, only the transport differs.
    function tuningTail() {
        if (rtlFreq.text.length === 0)
            return ""
        return ":" + rtlFreq.text + ":" + rtlGain.text + ":" + rtlPpm.text + ":" + rtlBw.text + ":0:2"
    }

    // Builds the CLI-shaped argv the host hands to the engine. Reusing the CLI
    // parser is what buys the whole option surface for free.
    function buildArgs() {
        var args = ["--frontend", "none"]
        var spec = ""

        if (pane.inputKey === "usb") {
            spec = "rtl:0" + pane.tuningTail()
            if (biasTee.checked)
                spec += ":bias"
            args.push("-i", spec)
        } else if (pane.inputKey === "rtltcp") {
            spec = "rtltcp:" + rtlHost.text + ":" + rtlPort.text + pane.tuningTail()
            args.push("-i", spec)
        } else if (pane.inputKey === "udp") {
            args.push("-i", "udp:0.0.0.0:" + udpPort.text)
        } else if (pane.inputKey === "tcp") {
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

    ScrollView {
        id: settingsScroll

        anchors.fill: parent
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
                        textRole: "label"
                        valueRole: "key"
                        model: [
                            { label: qsTr("RTL-SDR (USB)"), key: "usb" },
                            { label: qsTr("RTL-TCP"), key: "rtltcp" },
                            { label: qsTr("UDP PCM"), key: "udp" },
                            { label: qsTr("TCP PCM"), key: "tcp" },
                            { label: qsTr("Local file"), key: "file" }
                        ]
                    }

                    Label { text: qsTr("Device"); visible: pane.inputKey === "usb" && decoderHost && decoderHost.localDeviceBrokered }
                    RowLayout {
                        Layout.fillWidth: true
                        visible: pane.inputKey === "usb" && decoderHost && decoderHost.localDeviceBrokered

                        Label {
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            text: (decoderHost && decoderHost.localDeviceStatus.length > 0)
                                  ? decoderHost.localDeviceStatus
                                  : qsTr("not connected")
                        }

                        Button {
                            text: qsTr("Connect")
                            onClicked: decoderHost.requestLocalDeviceAccess()
                        }
                    }

                    Label { text: qsTr("Host"); visible: pane.inputKey === "rtltcp" }
                    TextField {
                        id: rtlHost
                        Layout.fillWidth: true
                        visible: pane.inputKey === "rtltcp"
                        text: "192.168.1.10"
                        inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoAutoUppercase
                    }

                    Label { text: qsTr("Port"); visible: pane.inputKey === "rtltcp" }
                    TextField {
                        id: rtlPort
                        Layout.fillWidth: true
                        visible: pane.inputKey === "rtltcp"
                        text: "1234"
                        inputMethodHints: Qt.ImhDigitsOnly
                    }

                    Label { text: qsTr("Frequency"); visible: pane.inputKey === "usb" || pane.inputKey === "rtltcp" }
                    TextField {
                        id: rtlFreq
                        Layout.fillWidth: true
                        visible: pane.inputKey === "usb" || pane.inputKey === "rtltcp"
                        text: "851.375M"
                        placeholderText: qsTr("e.g. 851.375M")
                    }

                    Label { text: qsTr("Gain / PPM / BW"); visible: pane.inputKey === "usb" || pane.inputKey === "rtltcp" }
                    RowLayout {
                        Layout.fillWidth: true
                        visible: pane.inputKey === "usb" || pane.inputKey === "rtltcp"
                        TextField { id: rtlGain; Layout.fillWidth: true; text: "13"; inputMethodHints: Qt.ImhDigitsOnly }
                        TextField { id: rtlPpm; Layout.fillWidth: true; text: "-2" }
                        TextField { id: rtlBw; Layout.fillWidth: true; text: "48"; inputMethodHints: Qt.ImhDigitsOnly }
                    }

                    // Only for the local dongle: over rtl_tcp the bias tee belongs
                    // to whoever runs the server.
                    Label { text: qsTr("Bias tee"); visible: pane.inputKey === "usb" }
                    CheckBox {
                        id: biasTee
                        visible: pane.inputKey === "usb"
                        text: qsTr("power an LNA over the antenna feed")
                    }

                    Label { text: qsTr("Bind port"); visible: pane.inputKey === "udp" }
                    TextField {
                        id: udpPort
                        Layout.fillWidth: true
                        visible: pane.inputKey === "udp"
                        text: "7355"
                        inputMethodHints: Qt.ImhDigitsOnly
                    }

                    Label { text: qsTr("Host"); visible: pane.inputKey === "tcp" }
                    TextField {
                        id: tcpHost
                        Layout.fillWidth: true
                        visible: pane.inputKey === "tcp"
                        text: "127.0.0.1"
                        inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoAutoUppercase
                    }

                    Label { text: qsTr("Port"); visible: pane.inputKey === "tcp" }
                    TextField {
                        id: tcpPort
                        Layout.fillWidth: true
                        visible: pane.inputKey === "tcp"
                        text: "7355"
                        inputMethodHints: Qt.ImhDigitsOnly
                    }

                    Label { text: qsTr("File"); visible: pane.inputKey === "file" }
                    RowLayout {
                        Layout.fillWidth: true
                        visible: pane.inputKey === "file"

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
}
