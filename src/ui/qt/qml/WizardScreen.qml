// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtQuick.Dialogs
import "Util.js" as Util

// The add-system wizard: source → tune → name. Three screens instead of one form,
// each asking one question, with everything advanced folded away.
Item {
    id: wizard

    signal closed()
    signal saved(int row)

    property int step: 0
    // -1 appends a new system; >= 0 edits in place.
    property int editRow: -1

    // Step 1 state
    property string sourceType: "usb"
    property alias hostText: hostField.text
    property alias portText: portField.text
    property alias fileText: fileField.text

    // Step 2 state
    property alias freqText: freqField.text
    property string decodeFlag: ""
    property bool trunking: true
    property bool advancedOpen: false
    property alias gainText: gainField.text
    property alias ppmText: ppmField.text
    property alias bwText: bwField.text
    // Tri-state: -1 follows the app-wide Settings pref, 0 forces off, 1 forces
    // on. An explicit Off must survive a global On — it means this dongle or
    // antenna must not be fed the tee's 4.5 V.
    property int biasTee: -1
    property alias extraText: extraField.text

    // Step 3 state
    property alias nameText: nameField.text

    readonly property bool radioSource: sourceType === "usb" || sourceType === "rtltcp"

    // Per-source conventions, not one shared guess: 1234 is rtl_tcp's port, but
    // GQRX/SDR++ audio streams (docs/network-audio.md) use 7355 and a TCP audio
    // source is usually a player on this device. Accepting the wrong prefill
    // leaves the session silent with no error.
    function defaultHostFor(type) {
        return type === "tcp" ? "127.0.0.1" : "192.168.1.10"
    }

    function defaultPortFor(type) {
        return (type === "udp" || type === "tcp") ? "7355" : "1234"
    }

    // Swap the prefill when the source type changes, but never text the user
    // already edited away from the previous type's default.
    function applySourceDefaults(prevType) {
        if (hostField.text === defaultHostFor(prevType))
            hostField.text = defaultHostFor(sourceType)
        if (portField.text === defaultPortFor(prevType))
            portField.text = defaultPortFor(sourceType)
    }

    function openForAdd(preferNetwork) {
        editRow = -1
        step = 0
        sourceType = preferNetwork ? "rtltcp" : "usb"
        hostField.text = defaultHostFor(sourceType)
        portField.text = defaultPortFor(sourceType)
        fileField.text = ""
        freqField.text = "851.375"
        decodeFlag = ""
        trunking = true
        advancedOpen = false
        gainField.text = ""
        ppmField.text = ""
        bwField.text = ""
        biasTee = -1
        extraField.text = ""
        nameField.text = ""
    }

    function openForEdit(row) {
        var sys = savedSystems.get(row)
        editRow = row
        step = 0
        sourceType = sys.sourceType
        hostField.text = sys.host
        portField.text = sys.port > 0 ? String(sys.port) : ""
        fileField.text = sys.filePath
        freqField.text = sys.freqMhz
        decodeFlag = sys.decodeFlag
        trunking = sys.trunking
        advancedOpen = false
        gainField.text = sys.gainDb >= 0 ? String(sys.gainDb) : ""
        ppmField.text = sys.ppm
        bwField.text = sys.bandwidthKhz > 0 ? String(sys.bandwidthKhz) : ""
        biasTee = sys.biasTee
        extraField.text = sys.extraArgs
        nameField.text = sys.name
    }

    // parseInt alone lets a hardware-keyboard "abc" become NaN, which QVariant
    // then reads as 0 — and a gain of 0 is a meaningful override, not "unset".
    // NaN must collapse to the field's explicit "no override" value instead.
    function intOr(text, fallback) {
        var v = parseInt(text, 10)
        return isNaN(v) ? fallback : v
    }

    function portValid() {
        var p = parseInt(portText, 10)
        return !isNaN(p) && p >= 1 && p <= 65535
    }

    function stepValid() {
        if (step === 0) {
            if (sourceType === "rtltcp" || sourceType === "tcp")
                return hostText.length > 0 && portValid()
            if (sourceType === "udp")
                return portValid()
            if (sourceType === "file")
                return fileText.length > 0
            return true
        }
        if (step === 1)
            return !radioSource || sessionArgs.freqValid(freqText)
        return nameText.trim().length > 0
    }

    function commit() {
        var sys = {
            name: nameText.trim(),
            sourceType: sourceType,
            host: hostText,
            port: portText.length > 0 ? intOr(portText, 0) : 0,
            freqMhz: freqText,
            decodeFlag: decodeFlag,
            trunking: trunking,
            gainDb: gainText.length > 0 ? intOr(gainText, -1) : -1,
            ppm: ppmText,
            bandwidthKhz: bwText.length > 0 ? intOr(bwText, -1) : -1,
            biasTee: biasTee,
            extraArgs: extraText.trim(),
            filePath: fileText
        }
        if (editRow >= 0) {
            savedSystems.update(editRow, sys)
            wizard.saved(editRow)
        } else {
            savedSystems.add(sys)
            wizard.saved(savedSystems.count - 1)
        }
    }

    FileDialog {
        id: fileDialog

        onAccepted: {
            var reference = selectedFile.toString()
            var hint = reference.substring(reference.lastIndexOf('/') + 1)
            var path = decoderHost.importContentUri(reference, hint)
            if (path.length > 0)
                fileField.text = path
        }
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.bg
    }

    // Header: back chevron, title, segmented progress.
    Item {
        id: header

        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: Theme.screenPadding
        height: 46

        Text {
            id: back
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            text: "‹"
            font.pixelSize: 28
            color: Theme.textSecondary

            TapHandler {
                onTapped: {
                    if (wizard.step > 0)
                        wizard.step--
                    else
                        wizard.closed()
                }
            }
        }

        Text {
            anchors.left: back.right
            anchors.leftMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            text: wizard.editRow >= 0 ? qsTr("Edit system") : qsTr("Add system")
            font.family: Theme.sans
            font.pixelSize: 22
            font.weight: Font.Bold
            font.letterSpacing: -0.22
            color: Theme.textPrimary
        }

        Row {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            spacing: 6

            Repeater {
                model: 3

                Rectangle {
                    required property int index

                    width: 20
                    height: 4
                    radius: 2
                    color: index <= wizard.step ? Theme.cyan : Theme.controlBorder
                }
            }
        }
    }

    MicroLabel {
        id: stepLabel
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.topMargin: 6
        anchors.leftMargin: Theme.screenPadding
        text: wizard.step === 0 ? qsTr("Step 1 of 3 · Source")
              : wizard.step === 1 ? qsTr("Step 2 of 3 · Tune")
              : qsTr("Step 3 of 3 · Name")
    }

    Flickable {
        anchors.top: stepLabel.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: continueButton.top
        anchors.topMargin: 14
        anchors.bottomMargin: 14
        contentHeight: stepColumn.height
        clip: true

        Column {
            id: stepColumn

            x: Theme.screenPadding
            width: parent.width - 2 * Theme.screenPadding
            spacing: Theme.gap

            // ---- Step 1: source ----
            Column {
                width: parent.width
                visible: wizard.step === 0
                spacing: Theme.gap

                Text {
                    text: qsTr("Where does the signal come from?")
                    font.family: Theme.sans
                    font.pixelSize: 15
                    font.weight: Font.DemiBold
                    color: Theme.textPrimary
                }

                Flow {
                    width: parent.width
                    spacing: 10

                    Repeater {
                        model: [
                            { label: qsTr("USB dongle"), key: "usb" },
                            { label: qsTr("RTL-TCP"), key: "rtltcp" },
                            { label: qsTr("UDP audio"), key: "udp" },
                            { label: qsTr("TCP audio"), key: "tcp" },
                            { label: qsTr("File"), key: "file" }
                        ]

                        DecodeChip {
                            required property var modelData

                            text: modelData.label
                            selected: wizard.sourceType === modelData.key
                            onClicked: {
                                var prev = wizard.sourceType
                                wizard.sourceType = modelData.key
                                if (prev !== modelData.key)
                                    wizard.applySourceDefaults(prev)
                            }
                        }
                    }
                }

                Text {
                    width: parent.width
                    visible: wizard.sourceType === "usb"
                    text: qsTr("An RTL-SDR dongle on a USB-OTG cable. Most public-safety listening starts here.")
                    font.family: Theme.sans
                    font.pixelSize: 13
                    color: Theme.textSubdued
                    wrapMode: Text.Wrap
                }

                Column {
                    width: parent.width
                    visible: wizard.sourceType === "rtltcp" || wizard.sourceType === "tcp"
                    spacing: 10

                    Text {
                        text: qsTr("Host")
                        font.family: Theme.sans
                        font.pixelSize: 13
                        color: Theme.textSecondary
                    }

                    PlexTextField {
                        id: hostField
                        width: parent.width
                        mono: true
                        text: "192.168.1.10"
                        inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoAutoUppercase
                    }
                }

                Column {
                    width: parent.width
                    visible: wizard.sourceType !== "usb" && wizard.sourceType !== "file"
                    spacing: 10

                    Text {
                        text: wizard.sourceType === "udp" ? qsTr("Listen port") : qsTr("Port")
                        font.family: Theme.sans
                        font.pixelSize: 13
                        color: Theme.textSecondary
                    }

                    PlexTextField {
                        id: portField
                        width: parent.width
                        mono: true
                        text: "1234"
                        // The hint only picks the soft keyboard; without the
                        // validator a hardware-keyboard "7,355" truncates to
                        // port 7 with no error.
                        inputMethodHints: Qt.ImhDigitsOnly
                        input.validator: IntValidator {
                            bottom: 1
                            top: 65535
                        }
                    }
                }

                Column {
                    width: parent.width
                    visible: wizard.sourceType === "file"
                    spacing: 10

                    Text {
                        text: qsTr("Audio or capture file")
                        font.family: Theme.sans
                        font.pixelSize: 13
                        color: Theme.textSecondary
                    }

                    Row {
                        width: parent.width
                        spacing: 10

                        PlexTextField {
                            id: fileField
                            width: parent.width - browse.width - 10
                            mono: true
                            placeholderText: qsTr("pick a .wav or .bin")
                        }

                        OutlineButton {
                            id: browse
                            width: 96
                            text: qsTr("Browse")
                            onClicked: fileDialog.open()
                        }
                    }
                }
            }

            // ---- Step 2: tune ----
            Column {
                width: parent.width
                visible: wizard.step === 1
                spacing: Theme.gap

                UiPanel {
                    width: parent.width
                    visible: wizard.radioSource
                    height: freqColumn.height + 2 * Theme.cardPadding

                    Column {
                        id: freqColumn

                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: Theme.cardPadding
                        spacing: 8

                        Text {
                            text: qsTr("Frequency")
                            font.family: Theme.sans
                            font.pixelSize: 14
                            color: Theme.textSecondary
                        }

                        Row {
                            spacing: 8

                            TextInput {
                                id: freqField

                                width: Math.max(implicitWidth, 60)
                                text: "851.375"
                                font.family: Theme.mono
                                font.pixelSize: 32
                                font.weight: Font.Medium
                                color: Theme.textPrimary
                                inputMethodHints: Qt.ImhFormattedNumbersOnly
                                // The hint only picks the soft keyboard; a hardware
                                // keyboard types anything. The validator is what keeps
                                // "851.375M" out of the saved system.
                                validator: RegularExpressionValidator {
                                    regularExpression: /^\d{1,5}(\.\d{0,6})?$/
                                }
                                selectionColor: Qt.alpha(Theme.cyan, 0.35)
                                selectedTextColor: Theme.textPrimary
                            }

                            Text {
                                text: "MHz"
                                anchors.baseline: freqField.baseline
                                font.family: Theme.mono
                                font.pixelSize: 15
                                color: Theme.textSubdued
                            }
                        }

                        Rectangle {
                            width: parent.width
                            height: 2
                            color: Theme.cyan
                        }

                        Text {
                            width: parent.width
                            text: qsTr("Tune to the system's control channel — find it on RadioReference.")
                            font.family: Theme.sans
                            font.pixelSize: 13
                            color: Theme.textSubdued
                            wrapMode: Text.Wrap
                        }
                    }
                }

                Text {
                    text: qsTr("What should we decode?")
                    font.family: Theme.sans
                    font.pixelSize: 15
                    font.weight: Font.DemiBold
                    color: Theme.textPrimary
                }

                Flow {
                    width: parent.width
                    spacing: 10

                    Repeater {
                        model: Util.DECODE_MODES

                        DecodeChip {
                            required property var modelData

                            text: modelData.label
                            selected: wizard.decodeFlag === modelData.flag
                            onClicked: wizard.decodeFlag = modelData.flag
                        }
                    }
                }

                Text {
                    width: parent.width
                    text: Util.decodeHint(wizard.decodeFlag)
                    font.family: Theme.sans
                    font.pixelSize: 13
                    color: Theme.textSubdued
                    wrapMode: Text.Wrap
                }

                UiPanel {
                    width: parent.width
                    height: 66

                    Column {
                        anchors.left: parent.left
                        anchors.right: trunkSwitch.left
                        anchors.leftMargin: Theme.cardPadding
                        anchors.rightMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 3

                        Text {
                            width: parent.width
                            text: qsTr("Follow calls across channels")
                            font.family: Theme.sans
                            font.pixelSize: 15
                            font.weight: Font.DemiBold
                            color: Theme.textPrimary
                            elide: Text.ElideRight
                        }

                        Text {
                            width: parent.width
                            text: qsTr("Trunking — recommended for public safety")
                            font.family: Theme.sans
                            font.pixelSize: 13
                            color: Theme.textSubdued
                            elide: Text.ElideRight
                        }
                    }

                    PlexSwitch {
                        id: trunkSwitch
                        anchors.right: parent.right
                        anchors.rightMargin: Theme.cardPadding
                        anchors.verticalCenter: parent.verticalCenter
                        checked: wizard.trunking
                        onToggled: function (state) { wizard.trunking = state }
                    }
                }

                // Every source type gets the panel (the extra-flags field applies
                // to all of them); the tuner rows inside gate on radioSource.
                UiPanel {
                    width: parent.width
                    height: advHeader.height + (wizard.advancedOpen ? advBody.height + 6 : 0)
                    clip: true

                    Behavior on height {
                        NumberAnimation { duration: 150; easing.type: Easing.OutCubic }
                    }

                    Item {
                        id: advHeader

                        width: parent.width
                        height: 66

                        Column {
                            anchors.left: parent.left
                            anchors.right: chevron.left
                            anchors.leftMargin: Theme.cardPadding
                            anchors.rightMargin: 12
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 3

                            Text {
                                width: parent.width
                                text: qsTr("Advanced")
                                font.family: Theme.sans
                                font.pixelSize: 15
                                font.weight: Font.DemiBold
                                color: Theme.textSecondary
                                elide: Text.ElideRight
                            }

                            Text {
                                width: parent.width
                                text: wizard.radioSource
                                      ? qsTr("Gain, PPM, bandwidth, bias tee — defaults work")
                                      : qsTr("Extra decoder flags — defaults work")
                                font.family: Theme.sans
                                font.pixelSize: 13
                                color: Theme.textSubdued
                                elide: Text.ElideRight
                            }
                        }

                        Caret {
                            id: chevron
                            anchors.right: parent.right
                            anchors.rightMargin: Theme.cardPadding
                            anchors.verticalCenter: parent.verticalCenter
                            rotation: wizard.advancedOpen ? 0 : -90
                            color: Theme.textSubdued

                            Behavior on rotation {
                                NumberAnimation { duration: 150; easing.type: Easing.OutCubic }
                            }
                        }

                        TapHandler {
                            onTapped: wizard.advancedOpen = !wizard.advancedOpen
                        }
                    }

                    Column {
                        id: advBody

                        anchors.top: advHeader.bottom
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.leftMargin: Theme.cardPadding
                        anchors.rightMargin: Theme.cardPadding
                        spacing: 10
                        visible: wizard.advancedOpen

                        Row {
                            width: parent.width
                            visible: wizard.radioSource
                            spacing: 10

                            Column {
                                width: (parent.width - 20) / 3
                                spacing: 6

                                Text {
                                    text: qsTr("Gain dB")
                                    font.family: Theme.sans
                                    font.pixelSize: 12
                                    color: Theme.textSecondary
                                }

                                PlexTextField {
                                    id: gainField
                                    width: parent.width
                                    mono: true
                                    placeholderText: String(prefs.gainDb)
                                    // Like the PPM field: the hint does not
                                    // constrain hardware keyboards, and a
                                    // non-numeric entry would otherwise read
                                    // back as an explicit 0 dB override.
                                    inputMethodHints: Qt.ImhDigitsOnly
                                    input.validator: IntValidator {
                                        bottom: 0
                                        top: 99
                                    }
                                }
                            }

                            Column {
                                width: (parent.width - 20) / 3
                                spacing: 6

                                Text {
                                    text: qsTr("PPM")
                                    font.family: Theme.sans
                                    font.pixelSize: 12
                                    color: Theme.textSecondary
                                }

                                PlexTextField {
                                    id: ppmField
                                    width: parent.width
                                    mono: true
                                    placeholderText: String(prefs.ppm)
                                    // Signed integer only: this string is spliced
                                    // verbatim into the rtl input spec, and a hint
                                    // alone does not constrain hardware keyboards.
                                    inputMethodHints: Qt.ImhFormattedNumbersOnly
                                    input.validator: IntValidator {
                                        bottom: -999
                                        top: 999
                                    }
                                }
                            }

                            Column {
                                width: (parent.width - 20) / 3
                                spacing: 6

                                Text {
                                    text: qsTr("BW kHz")
                                    font.family: Theme.sans
                                    font.pixelSize: 12
                                    color: Theme.textSecondary
                                }

                                PlexTextField {
                                    id: bwField
                                    width: parent.width
                                    mono: true
                                    placeholderText: String(prefs.bandwidthKhz)
                                    inputMethodHints: Qt.ImhDigitsOnly
                                    input.validator: IntValidator {
                                        bottom: 1
                                        top: 9999
                                    }
                                }
                            }
                        }

                        Column {
                            width: parent.width
                            spacing: 6
                            // rtl-tcp too: the engine applies the bias token on
                            // remote dongles, and an LNA on the far end needs it
                            // just as much as a local one.
                            visible: wizard.radioSource

                            Text {
                                text: qsTr("Bias tee")
                                font.family: Theme.sans
                                font.pixelSize: 14
                                color: Theme.textPrimary
                            }

                            Text {
                                text: qsTr("Powers an external LNA. Off wins over the app-wide setting.")
                                font.family: Theme.sans
                                font.pixelSize: 12
                                color: Theme.textSubdued
                            }

                            SegmentedControl {
                                width: parent.width
                                model: [qsTr("App default"), qsTr("On"), qsTr("Off")]
                                currentIndex: wizard.biasTee === 1 ? 1 : wizard.biasTee === 0 ? 2 : 0
                                onSelected: function (index) {
                                    wizard.biasTee = index === 1 ? 1 : index === 2 ? 0 : -1
                                }
                            }
                        }

                        Column {
                            width: parent.width
                            spacing: 6

                            Text {
                                text: qsTr("Extra CLI flags")
                                font.family: Theme.sans
                                font.pixelSize: 12
                                color: Theme.textSecondary
                            }

                            PlexTextField {
                                id: extraField
                                width: parent.width
                                mono: true
                                // Appended after the app-wide extras from Settings;
                                // per-system channel/group imports live here.
                                placeholderText: qsTr("e.g. -C chan.csv -G group.csv")
                                inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                            }
                        }

                        Item {
                            width: parent.width
                            height: 8
                        }
                    }
                }
            }

            // ---- Step 3: name ----
            Column {
                width: parent.width
                visible: wizard.step === 2
                spacing: Theme.gap

                Text {
                    text: qsTr("What should we call it?")
                    font.family: Theme.sans
                    font.pixelSize: 15
                    font.weight: Font.DemiBold
                    color: Theme.textPrimary
                }

                PlexTextField {
                    id: nameField
                    width: parent.width
                    placeholderText: qsTr("e.g. Hamilton Co P25")
                }

                Text {
                    width: parent.width
                    text: qsTr("The name is yours — county, agency, whatever you'll recognize on the home screen.")
                    font.family: Theme.sans
                    font.pixelSize: 13
                    color: Theme.textSubdued
                    wrapMode: Text.Wrap
                }
            }
        }
    }

    GradientButton {
        id: continueButton

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: Theme.screenPadding
        anchors.bottomMargin: 22
        text: wizard.step < 2 ? qsTr("Continue") : qsTr("Save system")
        enabled: wizard.stepValid()
        onClicked: {
            if (wizard.step < 2) {
                wizard.step++
            } else {
                wizard.commit()
            }
        }
    }

}
