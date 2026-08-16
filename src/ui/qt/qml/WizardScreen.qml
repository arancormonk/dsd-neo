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
    // Asks Main.qml to push the RadioReference screen over this one; the result
    // comes back through applyRadioReference().
    signal openRadioReference()

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
    // Imported trunking-data files; stored library paths, empty = none.
    property string chanCsvPath: ""
    property string groupCsvPath: ""
    property string keyCsvPath: ""
    property bool keyCsvHex: false
    // Which field the shared FileDialog is serving: "source" is the step-1
    // audio/capture pick; "chan"/"group"/"keys" are the trunking-data picks.
    property string pickerTarget: "source"
    // Dec/hex choice for a new key-file import from the picker sheet.
    property bool pickerKeyHex: false
    // Outcome of the last trunking-data import, shown under the picker rows.
    // Without it a file that could not be read, or that parsed to nothing,
    // leaves the row reading "None" with no explanation.
    property string csvNotice: ""
    property bool csvNoticeIsProblem: false

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
        chanCsvPath = ""
        groupCsvPath = ""
        keyCsvPath = ""
        keyCsvHex = false
        csvNotice = ""
        csvNoticeIsProblem = false
    }

    /**
     * Open on something found while exploring, with the source and frequency
     * already answered.
     *
     * Opens at step 2 rather than step 3: the source is settled, but what to
     * decode and whether to follow calls are real questions about the system just
     * found, and answering them for the user would produce a card that does not
     * work. @a sys supplies the source; it is the explore session's own map.
     */
    function openForFound(sys, freqMhz) {
        editRow = -1
        step = 1
        sourceType = (sys && sys.sourceType === "rtltcp") ? "rtltcp" : "usb"
        hostField.text = sys && sys.host ? sys.host : defaultHostFor(sourceType)
        portField.text = sys && sys.port > 0 ? String(sys.port) : defaultPortFor(sourceType)
        fileField.text = ""
        freqField.text = freqMhz
        decodeFlag = ""
        trunking = true
        advancedOpen = false
        gainField.text = ""
        ppmField.text = ""
        bwField.text = ""
        biasTee = -1
        extraField.text = ""
        nameField.text = ""
        chanCsvPath = ""
        groupCsvPath = ""
        keyCsvPath = ""
        keyCsvHex = false
        csvNotice = ""
        csvNoticeIsProblem = false
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
        chanCsvPath = sys.chanCsvPath
        groupCsvPath = sys.groupCsvPath
        keyCsvPath = sys.keyCsvPath
        keyCsvHex = sys.keyCsvHex
        csvNotice = ""
        csvNoticeIsProblem = false
    }

    // Display line for a picker row: the file's name, or "None".
    function csvLabel(path) {
        if (path.length === 0)
            return qsTr("None")
        var row = importedFiles.rowForPath(path)
        return row >= 0 ? importedFiles.get(row).name : path.substring(path.lastIndexOf('/') + 1)
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
            filePath: fileText,
            chanCsvPath: chanCsvPath,
            groupCsvPath: groupCsvPath,
            keyCsvPath: keyCsvPath,
            keyCsvHex: keyCsvHex
        }
        if (editRow >= 0) {
            savedSystems.update(editRow, sys)
            wizard.saved(editRow)
        } else {
            savedSystems.add(sys)
            wizard.saved(savedSystems.count - 1)
        }
    }

    // One trunking-data picker row: the disclosure shape with the picker sheet
    // behind it and the library label as its helper line. The helper reads as an
    // answer once a file is chosen, which is what the brighter colour says.
    component CsvPickerRow: DisclosureRow {
        id: pickerRow

        property string target: ""
        property string path: ""

        subtitle: wizard.csvLabel(pickerRow.path)
        subtitleColor: pickerRow.path.length > 0 ? Theme.textSecondary : Theme.textSubdued
        onTapped: {
            wizard.pickerTarget = pickerRow.target
            wizard.pickerKeyHex = wizard.pickerTarget === "keys" ? wizard.keyCsvHex : false
            csvSheet.visible = true
        }
    }

    // Assign an imported/picked library path to the field the picker serves.
    function assignCsvPath(target, path, hex) {
        csvNotice = ""
        csvNoticeIsProblem = false
        if (target === "chan") {
            chanCsvPath = path
        } else if (target === "group") {
            groupCsvPath = path
        } else if (target === "keys") {
            keyCsvPath = path
            keyCsvHex = hex
        }
    }

    /**
     * Fill the tune answers from a finished RadioReference import.
     *
     * The generated files go through the same seam a hand-picked one does, so
     * the wizard stays the single writer of the saved system and the user's
     * source, gain and ppm answers survive. @a result is performImport()'s map;
     * chanCsvPath is absent when no channel map was generated, which is a valid
     * outcome for a single conventional repeater. An absent file therefore
     * leaves whatever the user had already picked alone — clearing it would
     * silently strip a hand-picked -C from a system they were only editing.
     */
    function applyRadioReference(result) {
        if (result.chanCsvPath !== undefined && result.chanCsvPath.length > 0)
            wizard.assignCsvPath("chan", result.chanCsvPath, false)
        if (result.groupCsvPath !== undefined && result.groupCsvPath.length > 0)
            wizard.assignCsvPath("group", result.groupCsvPath, false)
        if (result.freqMhz && result.freqMhz.length > 0)
            freqField.text = result.freqMhz
        wizard.decodeFlag = result.decodeFlag
        wizard.trunking = result.trunking
        // Only when the wizard has no name yet: an edit already has one the user
        // chose, and RadioReference's is a database title, not their label.
        if (nameField.text.trim().length === 0 && result.name)
            nameField.text = result.name
        wizard.csvNotice = qsTr("Imported from RadioReference")
        wizard.csvNoticeIsProblem = false
        // Land on the tune step: the frequency, decode flag and files are all
        // answered now, and step 2 is where they are shown.
        if (wizard.step < 1)
            wizard.step = 1
    }

    // No CSV name filter for the trunking-data picks: on Android it becomes a
    // SAF MIME filter, and the Files app indexes .csv as
    // text/comma-separated-values — not the text/csv Qt asks for — which greys
    // out exactly the files the user came to pick.
    FileDialog {
        id: fileDialog

        onAccepted: {
            var reference = selectedFile.toString()
            var hint = reference.substring(reference.lastIndexOf('/') + 1)
            if (wizard.pickerTarget === "source") {
                var path = decoderHost.importContentUri(reference, hint)
                if (path.length > 0)
                    fileField.text = path
                return
            }
            var type = wizard.pickerTarget === "keys"
                       ? (wizard.pickerKeyHex ? "keysHex" : "keysDec") : wizard.pickerTarget
            var result = importedFiles.importFile(reference, hint, type)
            if (!result.ok) {
                wizard.csvNotice = qsTr("Could not read that file")
                wizard.csvNoticeIsProblem = true
                return
            }
            wizard.assignCsvPath(wizard.pickerTarget, result.path, wizard.pickerKeyHex)
            if (result.error === "empty") {
                wizard.csvNotice = qsTr("%1 has no usable rows — check the file format.").arg(result.name)
                wizard.csvNoticeIsProblem = true
            } else {
                wizard.csvNotice = ""
                wizard.csvNoticeIsProblem = false
            }
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
                            onClicked: {
                                wizard.pickerTarget = "source"
                                fileDialog.open()
                            }
                        }
                    }
                }
            }

            // ---- Step 2: tune ----
            Column {
                width: parent.width
                visible: wizard.step === 1
                spacing: Theme.gap

                // The database can answer this whole step — frequency, decode
                // mode, trunking, files — and it covers conventional systems as
                // much as trunked ones. So the entry leads the step instead of
                // trailing the file pickers, where it read as a trunked-only
                // CSV utility discovered only after answering everything by
                // hand. A Column collapses an invisible child, so nothing
                // moves where the feature is absent.
                UiPanel {
                    width: parent.width
                    visible: radioReference.available
                    height: 66

                    DisclosureRow {
                        // Named so UI_QT_QML_CALL_LISTS can reach it with findChild().
                        objectName: "wizardRadioReferenceRow"

                        anchors.verticalCenter: parent.verticalCenter
                        title: qsTr("Import from RadioReference…")
                        subtitle: qsTr("Fills in the frequency, decode mode and talkgroups")
                        onTapped: wizard.openRadioReference()
                    }
                }

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

                        FrequencyField {
                            id: freqField

                            width: parent.width
                            text: "851.375"
                        }

                        Text {
                            width: parent.width
                            // Only a build without the importer sends the user
                            // to the website; with it, the entry above IS the
                            // way to look the frequency up.
                            text: radioReference.available
                                  ? qsTr("Tune to the system's control channel.")
                                  : qsTr("Tune to the system's control channel — find it on RadioReference.")
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

                // Trunking data: the imported CSVs this system starts with. One
                // reusable row per file kind, each opening the shared picker sheet.
                UiPanel {
                    width: parent.width
                    height: csvColumn.height + Theme.cardPadding + 4

                    Column {
                        id: csvColumn

                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.topMargin: Theme.cardPadding
                        spacing: 0

                        MicroLabel {
                            text: qsTr("Trunking data")
                            leftPadding: Theme.cardPadding
                            bottomPadding: 6
                        }

                        CsvPickerRow {
                            title: qsTr("Channel map")
                            target: "chan"
                            path: wizard.chanCsvPath
                            showDivider: true
                        }

                        CsvPickerRow {
                            title: qsTr("Talkgroups")
                            target: "group"
                            path: wizard.groupCsvPath
                            showDivider: true
                        }

                        CsvPickerRow {
                            title: qsTr("Encryption keys")
                            target: "keys"
                            path: wizard.keyCsvPath
                        }

                        Text {
                            width: parent.width
                            leftPadding: Theme.cardPadding
                            rightPadding: Theme.cardPadding
                            bottomPadding: 6
                            visible: wizard.csvNotice.length > 0
                            text: wizard.csvNotice
                            font.family: Theme.sans
                            font.pixelSize: 12
                            color: wizard.csvNoticeIsProblem ? Theme.magenta : Theme.textSubdued
                            wrapMode: Text.Wrap
                        }

                        Text {
                            width: parent.width
                            leftPadding: Theme.cardPadding
                            rightPadding: Theme.cardPadding
                            bottomPadding: 6
                            text: qsTr("Imported files are shared between systems. Manage them in Settings.")
                            font.family: Theme.sans
                            font.pixelSize: 12
                            color: Theme.textSubdued
                            wrapMode: Text.Wrap
                        }
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

    // Picker for one trunking-data field: the library's files of that kind,
    // "None", and a fresh import.
    ModalSheet {
        id: csvSheet

        readonly property string currentPath: wizard.pickerTarget === "chan" ? wizard.chanCsvPath
                                              : wizard.pickerTarget === "group" ? wizard.groupCsvPath
                                              : wizard.keyCsvPath
        readonly property var entries: {
            var n = importedFiles.count // dependency: recompute when the library changes
            if (!visible || wizard.pickerTarget === "source")
                return []
            return wizard.pickerTarget === "keys"
                   ? importedFiles.entriesForType("keysDec").concat(importedFiles.entriesForType("keysHex"))
                   : importedFiles.entriesForType(wizard.pickerTarget)
        }

        function entrySummary(entry) {
            var noun = wizard.pickerTarget === "chan"
                       ? (entry.accepted === 1 ? qsTr("channel") : qsTr("channels"))
                       : wizard.pickerTarget === "group"
                         ? (entry.accepted === 1 ? qsTr("talkgroup") : qsTr("talkgroups"))
                         : (entry.accepted === 1 ? qsTr("key") : qsTr("keys"))
            var line = entry.accepted + " " + noun
            if (wizard.pickerTarget === "keys")
                line += " · " + (entry.type === "keysHex" ? qsTr("hex") : qsTr("decimal"))
            return line
        }

        MicroLabel {
            text: wizard.pickerTarget === "chan" ? qsTr("Channel map")
                  : wizard.pickerTarget === "group" ? qsTr("Talkgroups")
                  : qsTr("Encryption keys")
        }

        // The library is unbounded, and the sheet is centred with no scrolling of
        // its own: past a handful of files an unclipped Repeater would push the
        // "None"/import controls off the bottom and the title off the top, with
        // no way to reach either. Cap the list and let it scroll instead.
        Flickable {
            width: parent.width
            height: Math.min(entryColumn.height, 46 * 5)
            visible: csvSheet.entries && csvSheet.entries.length > 0
            clip: true
            contentHeight: entryColumn.height
            boundsBehavior: Flickable.StopAtBounds

            Column {
                id: entryColumn

                width: parent.width

                Repeater {
                    model: csvSheet.entries

                    Item {
                        required property var modelData

                        width: entryColumn.width
                        height: 46

                        Column {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 2

                            Text {
                                width: parent.width
                                text: modelData.name
                                font.family: Theme.sans
                                font.pixelSize: 15
                                font.weight: Font.DemiBold
                                color: modelData.path === csvSheet.currentPath ? Theme.cyan : Theme.textPrimary
                                elide: Text.ElideRight
                            }

                            Text {
                                width: parent.width
                                text: csvSheet.entrySummary(modelData)
                                font.family: Theme.sans
                                font.pixelSize: 12
                                color: Theme.textSubdued
                                elide: Text.ElideRight
                            }
                        }

                        TapHandler {
                            onTapped: {
                                wizard.assignCsvPath(wizard.pickerTarget, modelData.path,
                                                     modelData.type === "keysHex")
                                csvSheet.visible = false
                            }
                        }
                    }
                }
            }
        }

        OutlineButton {
            width: parent.width
            visible: csvSheet.currentPath.length > 0
            text: qsTr("None")
            onClicked: {
                wizard.assignCsvPath(wizard.pickerTarget, "", false)
                csvSheet.visible = false
            }
        }

        SegmentedControl {
            width: parent.width
            visible: wizard.pickerTarget === "keys"
            model: [qsTr("Decimal keys"), qsTr("Hex keys")]
            currentIndex: wizard.pickerKeyHex ? 1 : 0
            // Also re-reads the already-assigned file: the control opens showing
            // that file's dec/hex state, so flipping it has to change it —
            // otherwise it looks like a setting and silently does nothing.
            onSelected: function (index) {
                wizard.pickerKeyHex = index === 1
                if (wizard.pickerTarget === "keys" && wizard.keyCsvPath.length > 0)
                    wizard.keyCsvHex = wizard.pickerKeyHex
            }
        }

        GradientButton {
            width: parent.width
            text: qsTr("Import new file")
            onClicked: {
                csvSheet.visible = false
                fileDialog.open()
            }
        }
    }
}
