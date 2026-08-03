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
            // Only a hint: the host asks the content provider for the document's real
            // display name and falls back to this when there is no provider to ask.
            var reference = selectedFile.toString()
            var hint = reference.substring(reference.lastIndexOf('/') + 1)
            var path = decoderHost.importContentUri(reference, hint)
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
                        // Without a floor of zero the status text's natural width becomes
                        // this column's minimum, and the column is shared with every
                        // field below it -- see the note on the value column.
                        Layout.minimumWidth: 0
                        visible: pane.inputKey === "usb" && decoderHost && decoderHost.localDeviceBrokered

                        Label {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
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
                        Layout.minimumWidth: 0
                        visible: pane.inputKey === "usb" || pane.inputKey === "rtltcp"
                        // dB of tuner gain, tuner error in PPM, and channel bandwidth in
                        // kHz. 30 dB and 48 kHz suit a P25/DMR control channel on a stock
                        // dongle; 0 PPM is "uncorrected", which is right until the offset
                        // has actually been measured for the stick in hand.
                        TextField { id: rtlGain; Layout.fillWidth: true; Layout.minimumWidth: 0; text: "30"; inputMethodHints: Qt.ImhDigitsOnly }
                        TextField { id: rtlPpm; Layout.fillWidth: true; Layout.minimumWidth: 0; text: "0" }
                        TextField { id: rtlBw; Layout.fillWidth: true; Layout.minimumWidth: 0; text: "48"; inputMethodHints: Qt.ImhDigitsOnly }
                    }

                    // Only for the local dongle: over rtl_tcp the bias tee belongs
                    // to whoever runs the server. Spans both columns and carries its own
                    // label, like the decode checkboxes: given a label column of its own
                    // the explanatory text set this column's minimum width, and the column
                    // is shared, so every field above it was pushed off the right edge.
                    CheckBox {
                        id: biasTee
                        Layout.columnSpan: 2
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        visible: pane.inputKey === "usb"
                        text: qsTr("Bias tee (powers an LNA)")
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
                        // The first entry passes no -f flag, which is not "nothing": the
                        // decoder's own default already has P25 phase 1 and 2, DMR and YSF
                        // enabled together, and that is what the label has to say. "-fa"
                        // adds every remaining decoder, which on a trunked control channel
                        // synthesises audio out of data -- hence the warning, not a name
                        // that reads like the safer choice.
                        model: [
                            // No "(default)" suffix on the first entry of either combo:
                            // it is what the box already displays on a fresh form, and
                            // the box is one grid column wide on a phone, where anything
                            // longer than about a dozen capitals elides silently.
                            { label: qsTr("P25/DMR/YSF"), flag: "" },
                            { label: qsTr("All decoders"), flag: "-fa" },
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
                        // Passing no -m flag leaves the modulation unlocked: the decoder
                        // starts on C4FM and re-selects if it detects otherwise, which is
                        // what "Default" used to mean and never said. The label names the
                        // starting point too, because auto-selection is not free -- this
                        // P25 LSM system reads at 1.4 dB and never locks until QPSK is
                        // chosen, and choosing any entry below pins it (mod_cli_lock) so
                        // the decoder cannot drift back off it.
                        //
                        // "LSM" is in the QPSK label deliberately: it is the word on the
                        // system, not the word in the modulation table.
                        //
                        // "-ma" is absent. It sets every optimization at once and the
                        // engine logs "Don't use the -ma switch" when it does; Extra args
                        // still reaches it for anyone who means it.
                        model: [
                            { label: qsTr("Auto (C4FM first)"), flag: "" },
                            { label: qsTr("C4FM"), flag: "-mc" },
                            { label: qsTr("QPSK / LSM"), flag: "-mq" },
                            { label: qsTr("GFSK"), flag: "-mg" }
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
