// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// Settings: appearance, units, listening, decoding, and the advanced tuner defaults
// folded shut. Every row writes straight through to the persisted preference.
Item {
    id: screen

    property bool advancedOpen: false
    property bool lockoutPending: false
    property bool requestedLockoutPersistence: true
    property string lockoutError: ""
    readonly property bool liveLockoutPersistence: metrics.persistTgLockouts
    readonly property bool lockoutSessionRunning: decoderHost.sessionState === 2
    readonly property bool lockoutEditable: !lockoutPending && (decoderHost.sessionState === 0 || decoderHost.sessionState === 4 || (lockoutSessionRunning && metrics.optionsKnown))

    onLiveLockoutPersistenceChanged: {
        if (lockoutPending && liveLockoutPersistence === requestedLockoutPersistence) {
            lockoutPending = false;
            lockoutAckTimer.stop();
        }
    }
    onLockoutSessionRunningChanged: {
        lockoutPending = false;
        lockoutAckTimer.stop();
    }

    Timer {
        id: lockoutAckTimer
        interval: 5000
        onTriggered: {
            screen.lockoutPending = false;
            screen.lockoutError = qsTr("The decoder has not confirmed the change. The switch shows its current setting.");
        }
    }

    function setLockoutPersistence(value) {
        if (!lockoutEditable)
            return;
        lockoutError = "";
        if (lockoutSessionRunning && !commands.setPersistTgLockouts(value)) {
            lockoutError = qsTr("Could not change the running decoder's setting. Try again.");
            return;
        }
        prefs.persistTgLockouts = value;
        requestedLockoutPersistence = value;
        lockoutPending = lockoutSessionRunning && liveLockoutPersistence !== value;
        if (lockoutPending)
            lockoutAckTimer.restart();
    }

    signal openDiagnostics
    signal openImports
    signal openRadioReference

    Rectangle {
        anchors.fill: parent
        color: Theme.bg
    }

    // Numeric advanced row: label left, small mono field + unit right.
    component ValueRow: Item {
        id: valueRow
        property string title: ""
        property string unit: ""
        property alias text: valueInput.text
        property bool showDivider: true
        signal edited(string text)
        width: parent ? parent.width : 0
        height: valueInput.implicitHeight + 16
        PlexTextField {
            id: valueInput
            x: Theme.cardPadding
            y: 8
            width: parent.width - 2 * Theme.cardPadding
            label: valueRow.title
            unit: valueRow.unit
            mono: true
            inputMethodHints: Qt.ImhFormattedNumbersOnly
            error: /^-?[0-9]+$/.test(text) && Number(text) >= -2147483648 && Number(text) <= 2147483647 ? "" : qsTr("Enter a whole number.")
            onEditingFinished: {
                if (!error.length)
                    valueRow.edited(text);
            }
        }
    }

    component DecimalRow: Item {
        id: decimalRow
        property string title: ""
        property string subtitle: ""
        property string unit: ""
        property alias text: decimalInput.text
        signal edited(string text)
        width: parent ? parent.width : 0
        height: decimalInput.implicitHeight + 16
        PlexTextField {
            id: decimalInput
            objectName: "hangtimePreferenceField"
            x: Theme.cardPadding
            y: 8
            width: parent.width - 2 * Theme.cardPadding
            label: decimalRow.title
            hint: decimalRow.subtitle
            unit: decimalRow.unit
            mono: true
            inputMethodHints: Qt.ImhFormattedNumbersOnly
            error: /^\d{1,2}(\.\d)?$/.test(text) && Number(text) <= 30 ? "" : qsTr("Enter seconds from 0 to 30, e.g. 2.0.")
            onEditingFinished: {
                if (!error.length)
                    decimalRow.edited(text);
            }
        }
    }

    PlexFlickable {
        anchors.fill: parent
        contentHeight: content.height + 2 * Theme.screenPadding
        clip: true

        Column {
            id: content

            x: (parent.width - width) / 2
            y: Theme.screenPadding
            width: Math.min(Theme.formWidth, parent.width - 2 * Theme.screenPadding)
            spacing: Theme.gap

            Text {
                text: qsTr("Settings")
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(24)
                font.weight: Font.Bold
                font.letterSpacing: -0.24
                color: Theme.textPrimary
            }

            // APPEARANCE
            UiPanel {
                width: parent.width
                height: appearanceColumn.height + 2 * Theme.cardPadding

                Column {
                    id: appearanceColumn

                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: Theme.cardPadding
                    spacing: 12

                    MicroLabel {
                        width: parent.width
                        wrapMode: Text.Wrap
                        text: qsTr("Appearance")
                    }

                    SegmentedControl {
                        width: parent.width
                        model: [qsTr("System"), qsTr("Light"), qsTr("Dark")]
                        currentIndex: prefs.appearance
                        onSelected: function (index) {
                            prefs.appearance = index;
                        }
                    }

                    Text {
                        width: parent.width
                        visible: prefs.appearance === 0
                        text: qsTr("Follows your phone's dark mode schedule.")
                        font.family: Theme.sans
                        font.pixelSize: Theme.fontSize(12)
                        color: Theme.textSubdued
                        wrapMode: Text.Wrap
                    }
                }
            }

            // UNITS
            UiPanel {
                width: parent.width
                height: unitsColumn.height + Theme.cardPadding + 4

                Column {
                    id: unitsColumn

                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.topMargin: Theme.cardPadding
                    spacing: 0

                    MicroLabel {
                        width: parent.width
                        wrapMode: Text.Wrap
                        text: qsTr("Units")
                        leftPadding: Theme.cardPadding
                        bottomPadding: 6
                    }

                    ToggleRow {
                        objectName: "metricUnitsToggle"
                        title: qsTr("Use metric units")
                        subtitle: prefs.metricUnits ? qsTr("Distances in kilometers (km)") : qsTr("Distances in miles (mi)")
                        checked: prefs.metricUnits
                        onToggled: function (state) {
                            prefs.metricUnits = state;
                        }
                    }
                }
            }

            // LISTENING
            UiPanel {
                width: parent.width
                height: listeningColumn.height + Theme.cardPadding + 4

                Column {
                    id: listeningColumn

                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.topMargin: Theme.cardPadding
                    spacing: 0

                    MicroLabel {
                        width: parent.width
                        wrapMode: Text.Wrap
                        text: qsTr("Listening")
                        leftPadding: Theme.cardPadding
                        bottomPadding: 6
                    }

                    Column {
                        width: parent.width
                        x: Theme.cardPadding
                        spacing: 6
                        Text {
                            width: parent.width - 2 * Theme.cardPadding
                            text: qsTr("Audio output")
                            wrapMode: Text.Wrap
                            color: Theme.textPrimary
                            font.pixelSize: Theme.fontSize(14)
                            font.bold: true
                        }
                        Text {
                            width: parent.width - 2 * Theme.cardPadding
                            text: decoderHost.audioRoute || qsTr("System default")
                            wrapMode: Text.Wrap
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontSize(14)
                        }
                    }

                    ToggleRow {
                        objectName: "persistTgLockoutsToggle"
                        title: qsTr("Save skipped talkgroups")
                        subtitle: qsTr("Applies to Skip immediately. Off keeps avoids until stop or list reload. Talkgroup-list edits still save.")
                        checked: screen.lockoutSessionRunning ? screen.liveLockoutPersistence : prefs.persistTgLockouts
                        enabled: screen.lockoutEditable
                        showDivider: true
                        onToggled: function (value) { screen.setLockoutPersistence(value); }
                    }

                    Text {
                        objectName: "tgLockoutError"
                        width: parent.width - 2 * Theme.cardPadding
                        x: Theme.cardPadding
                        visible: text.length > 0
                        text: screen.lockoutError
                        wrapMode: Text.Wrap
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontSize(12)
                    }

                    OutlineButton {
                        objectName: "clearTemporaryTgAvoidsButton"
                        width: parent.width - 2 * Theme.cardPadding
                        x: Theme.cardPadding
                        visible: screen.lockoutSessionRunning && metrics.temporaryTgAvoidCount > 0
                        enabled: screen.lockoutEditable
                        text: qsTr("Clear temporary TG avoids — current list (%1)").arg(metrics.temporaryTgAvoidCount)
                        onClicked: {
                            screen.lockoutError = commands.clearTemporaryTgAvoids(metrics.tgPolicyContext)
                                ? "" : qsTr("Could not clear temporary avoids. Try again.");
                        }
                    }

                    ToggleRow {
                        title: qsTr("Keep listening in background")
                        subtitle: qsTr("Notification controls are available when permission is allowed")
                        checked: prefs.backgroundListening
                        showDivider: decoderHost.keepScreenAwakeSupported || decoderHost.localDeviceBrokered
                        onToggled: function (state) {
                            prefs.backgroundListening = state;
                            var host = decoderHost;
                            if (state && typeof host.requestNotificationPermission === "function") {
                                prefs.notificationExplained = true;
                                host.requestNotificationPermission();
                            }
                        }
                    }

                    // WP-S2: opt-in applies only to hosts that broker local USB devices.
                    ToggleRow {
                        visible: decoderHost.localDeviceBrokered
                        title: qsTr("Start when a dongle is attached")
                        subtitle: qsTr("Resume the last USB system or scan list")
                        checked: prefs.autoStartOnAttach
                        showDivider: decoderHost.keepScreenAwakeSupported
                        onToggled: function (state) {
                            prefs.autoStartOnAttach = state;
                        }
                    }

                    ToggleRow {
                        // Hidden where the host cannot honor it: a switch that
                        // persists but changes nothing reads as broken.
                        visible: decoderHost.keepScreenAwakeSupported
                        title: qsTr("Keep screen awake")
                        checked: prefs.keepScreenAwake
                        onToggled: function (state) {
                            prefs.keepScreenAwake = state;
                        }
                    }
                }
            }

            // DECODING
            UiPanel {
                width: parent.width
                height: decodingColumn.height + Theme.cardPadding + 4

                Column {
                    id: decodingColumn

                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.topMargin: Theme.cardPadding
                    spacing: 0

                    MicroLabel {
                        width: parent.width
                        wrapMode: Text.Wrap
                        text: qsTr("Decoder defaults · next start")
                        leftPadding: Theme.cardPadding
                        bottomPadding: 6
                    }

                    ToggleRow {
                        title: qsTr("Skip encrypted calls")
                        subtitle: qsTr("Enabled talkgroups can play when keys are usable. Explicit exclusions stay blocked.")
                        checked: prefs.skipEncrypted
                        showDivider: true
                        onToggled: function (state) {
                            prefs.skipEncrypted = state;
                        }
                    }

                    ToggleRow {
                        title: qsTr("Auto tuner correction")
                        subtitle: qsTr("Fixes frequency drift on long runs")
                        checked: prefs.autoPpm
                        onToggled: function (state) {
                            prefs.autoPpm = state;
                        }
                    }

                    DecimalRow {
                        title: qsTr("Voice hang time")
                        subtitle: qsTr("Keeps a call's channel after voice stops. Also sets channel-scanning dwell (-Y); scan lists have separate dwell settings.")
                        unit: "s"
                        text: prefs.hangtimeSec.toFixed(1)
                        onEdited: function (value) {
                            prefs.hangtimeSec = Number(value);
                        }
                    }
                }
            }

            // TRUNKING DATA
            UiPanel {
                width: parent.width
                height: importsColumn.height + Theme.cardPadding + 4

                Column {
                    id: importsColumn

                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.topMargin: Theme.cardPadding
                    spacing: 0

                    MicroLabel {
                        width: parent.width
                        wrapMode: Text.Wrap
                        text: qsTr("Trunking data")
                        leftPadding: Theme.cardPadding
                        bottomPadding: 6
                    }

                    DisclosureRow {
                        title: qsTr("Imported files")
                        subtitle: qsTr("Channel maps, talkgroups, and keys")
                        onTapped: screen.openImports()
                    }
                }
            }

            UiPanel {
                width: parent.width
                height: diagnosticsColumn.implicitHeight + 24
                Column {
                    id: diagnosticsColumn
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.margins: 12
                    DisclosureRow {
                        title: qsTr("Diagnostics")
                        subtitle: qsTr("Current process and previous-run tail; not crash/ANR capture")
                        onTapped: screen.openDiagnostics()
                    }
                }
            }

            // RADIOREFERENCE ACCOUNT
            // The username and application key persist; the password never does
            // — it is asked for once per app session on the import screen.
            UiPanel {
                width: parent.width
                visible: radioReference.available
                height: rrColumn.height + Theme.cardPadding + 4

                Column {
                    id: rrColumn

                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.topMargin: Theme.cardPadding
                    spacing: 0

                    MicroLabel {
                        width: parent.width
                        wrapMode: Text.Wrap
                        text: qsTr("RadioReference account")
                        leftPadding: Theme.cardPadding
                        bottomPadding: 6
                    }

                    DisclosureRow {
                        title: qsTr("Import a system")
                        // Not "talkgroups and channel maps": that framing reads
                        // as trunked-only, and the import serves conventional
                        // systems just as well. Same line as the wizard's entry
                        // row on purpose — one action, one description — and
                        // anything longer elides on a phone-width row.
                        subtitle: qsTr("Fills in the frequency, decode mode and talkgroups")
                        showDivider: true
                        onTapped: screen.openRadioReference()
                    }

                    Item {

                        width: parent.width
                        height: settingsRrUsername.implicitHeight + 20

                        PlexTextField {
                            id: settingsRrUsername
                            x: Theme.cardPadding
                            y: 10
                            width: parent.width - 2 * Theme.cardPadding
                            text: prefs.rrUsername
                            label: qsTr("Username")
                            placeholderText: qsTr("radioreference.com username")
                            inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                            onEditingFinished: prefs.rrUsername = text
                        }
                    }

                    // A build that bakes the application key in does not ask for
                    // one: this row sat right under Username looking like a
                    // password box. Nor does it show a stored override left over
                    // from a keyless build — fillAuth() ignores it there, so it
                    // is not a credential in play and a field for it would only
                    // invite editing a key this build does not use.
                    Item {
                        objectName: "settingsRrAppKeyRow"
                        width: parent.width
                        height: settingsRrApplicationKey.implicitHeight + 20
                        visible: !radioReference.buildHasAppKey
                        PlexTextField {
                            id: settingsRrApplicationKey
                            x: Theme.cardPadding
                            y: 10
                            width: parent.width - 2 * Theme.cardPadding
                            text: prefs.rrAppKey
                            label: qsTr("Application key")
                            placeholderText: qsTr("application key")
                            inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                            onEditingFinished: prefs.rrAppKey = text
                        }
                    }

                    Text {
                        width: parent.width
                        leftPadding: Theme.cardPadding
                        rightPadding: Theme.cardPadding
                        bottomPadding: 6
                        text: qsTr("The password is asked for once per app session and is never saved. A RadioReference premium subscription is required.")
                        font.family: Theme.sans
                        font.pixelSize: Theme.fontSize(12)
                        color: Theme.textSubdued
                        wrapMode: Text.Wrap
                    }
                }
            }

            // ADVANCED (collapsible)
            UiPanel {
                width: parent.width
                height: advHeader.height + (screen.advancedOpen ? advColumn.height + 8 : 0) + Theme.cardPadding
                clip: true

                Behavior on height {
                    NumberAnimation {
                        duration: 150
                        easing.type: Easing.OutCubic
                    }
                }

                Item {
                    id: advHeader

                    width: parent.width
                    height: Math.max(48, advancedLabel.implicitHeight + 24)

                    MicroLabel {
                        id: advancedLabel
                        width: parent.width - 2 * Theme.cardPadding - 28
                        wrapMode: Text.Wrap
                        anchors.left: parent.left
                        anchors.leftMargin: Theme.cardPadding
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Advanced defaults · next start")
                    }

                    Caret {
                        anchors.right: parent.right
                        anchors.rightMargin: Theme.cardPadding
                        anchors.verticalCenter: parent.verticalCenter
                        rotation: screen.advancedOpen ? 180 : 0
                        color: Theme.textSubdued

                        Behavior on rotation {
                            NumberAnimation {
                                duration: 150
                                easing.type: Easing.OutCubic
                            }
                        }
                    }

                    TapHandler {
                        onTapped: screen.advancedOpen = !screen.advancedOpen
                    }
                }

                Column {
                    id: advColumn

                    anchors.top: advHeader.bottom
                    anchors.left: parent.left
                    anchors.right: parent.right
                    visible: screen.advancedOpen
                    spacing: 0

                    ValueRow {
                        title: qsTr("Tuner gain")
                        unit: "dB"
                        text: String(prefs.gainDb)
                        onEdited: function (value) {
                            var parsed = parseInt(value);
                            if (!isNaN(parsed))
                                prefs.gainDb = parsed;
                        }
                    }

                    ValueRow {
                        title: qsTr("PPM correction")
                        text: String(prefs.ppm)
                        onEdited: function (value) {
                            var parsed = parseInt(value);
                            if (!isNaN(parsed))
                                prefs.ppm = parsed;
                        }
                    }

                    ValueRow {
                        title: qsTr("Bandwidth")
                        unit: "kHz"
                        text: String(prefs.bandwidthKhz)
                        onEdited: function (value) {
                            var parsed = parseInt(value);
                            if (!isNaN(parsed) && parsed > 0)
                                prefs.bandwidthKhz = parsed;
                        }
                    }

                    ToggleRow {
                        title: qsTr("Bias tee")
                        subtitle: qsTr("Powers an external LNA")
                        checked: prefs.biasTee
                        showDivider: true
                        onToggled: function (state) {
                            prefs.biasTee = state;
                        }
                    }

                    Item {

                        width: parent.width
                        height: settingsExtraArgs.implicitHeight + 20

                        PlexTextField {
                            id: settingsExtraArgs
                            x: Theme.cardPadding
                            y: 10
                            width: parent.width - 2 * Theme.cardPadding
                            text: prefs.extraArgs
                            label: qsTr("Extra decoder arguments")
                            placeholderText: qsTr("e.g. -C chan.csv -G group.csv")
                            inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                            onEditingFinished: prefs.extraArgs = text
                        }
                    }
                }
            }

            OutlineButton {
                width: parent.width
                text: qsTr("Open source licenses")
                onClicked: {
                    var host = decoderHost;
                    licenseText.text = typeof host.licenseNotices === "function" ? host.licenseNotices() : "";
                    licenses.visible = true;
                }
            }
            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: "DSD-neo " + appVersionText.replace(/^v/, "") + " · GPL-3.0"
                color: Theme.textSubdued
                font.pixelSize: Theme.fontSize(12)
                wrapMode: Text.Wrap
            }
        }
    }
    ModalSheet {
        id: licenses
        accessibleName: qsTr("Open source licenses")
        TextEdit {
            id: licenseText
            width: parent.width
            readOnly: true
            selectByMouse: true
            textFormat: TextEdit.PlainText
            wrapMode: TextEdit.Wrap
            color: Theme.textPrimary
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(14)
        }
        OutlineButton {
            width: parent.width
            text: qsTr("Close")
            onClicked: licenses.visible = false
        }
    }
}
