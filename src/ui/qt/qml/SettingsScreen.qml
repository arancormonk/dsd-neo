// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import "Util.js" as Util

// Settings: appearance, units, listening, decoding, and the advanced tuner defaults
// folded shut. Every row writes straight through to the persisted preference.
Item {
    id: screen

    property bool showTitle: true
    property bool advancedOpen: false
    property bool lockoutPending: false
    property bool requestedLockoutPersistence: true
    property string lockoutError: ""
    readonly property string versionText: "DSD-neo " + appVersionText.replace(/^v/, "")
    readonly property bool liveLockoutPersistence: metrics.persistTgLockouts
    // A running session, and whether a live row may be edited now: stopped (the
    // preference alone) or running with the session's options known.
    readonly property bool sessionRunning: decoderHost.sessionState === 2
    readonly property bool liveEditable: decoderHost.sessionState === 0 || decoderHost.sessionState === 4 || (sessionRunning && metrics.optionsKnown)
    readonly property bool lockoutEditable: !lockoutPending && liveEditable
    // Relaxed CRC: the app preference every session the app starts gets (-F), and
    // applied live to a running one, whose own snapshot the switch shows until it
    // confirms, as the lockout row above does.
    property bool relaxedCrcPending: false
    property bool requestedRelaxedCrc: false
    property string relaxedCrcError: ""
    readonly property bool liveRelaxedCrc: metrics.relaxedCrcConfigured === true
    readonly property bool relaxedCrcEditable: !relaxedCrcPending && liveEditable

    // Issue #574. In AppPrefs::ScreenMode code order, so a choice's index is its
    // code: 0 System default, 1 Always on, 2 Dim between calls, 3 Off between calls.
    readonly property var screenModeNames: [qsTr("System default"), qsTr("Always on"), qsTr("Dim between calls"), qsTr("Off between calls")]
    // Only the two between-calls modes wait for a delay.
    readonly property bool screenDelayShown: decoderHost.screenPolicySupported && prefs.screenMode >= 2
    readonly property string screenModeSubtitle: {
        var delay = screenDelayText(prefs.screenDelaySec);
        var mode = prefs.screenMode;
        var detail = mode === 1 ? qsTr("Stays on while DSD-neo is open")
            : mode === 2 ? qsTr("While listening, dims after %1 without audio. Audio or a tap brightens it.").arg(delay)
            : mode === 3 ? qsTr("While listening, dims after %1 without audio, then turns off. Audio turns it back on if DSD-neo was on screen when it went off.").arg(delay)
            : qsTr("Follows your phone's screen timeout");
        return qsTr("%1 · %2").arg(screenModeNames[mode] || screenModeNames[0]).arg(detail);
    }

    function screenDelayText(seconds) {
        if (seconds < 60)
            return qsTr("%1 seconds").arg(seconds);
        return seconds === 60 ? qsTr("1 minute") : qsTr("%1 minutes").arg(seconds / 60);
    }

    onLiveLockoutPersistenceChanged: {
        if (lockoutPending && liveLockoutPersistence === requestedLockoutPersistence) {
            lockoutPending = false;
            lockoutAckTimer.stop();
        }
    }
    onSessionRunningChanged: {
        lockoutPending = false;
        lockoutAckTimer.stop();
        relaxedCrcPending = false;
        relaxedCrcAckTimer.stop();
        relaxedCrcError = "";
    }
    onLiveRelaxedCrcChanged: {
        if (relaxedCrcPending && liveRelaxedCrc === requestedRelaxedCrc) {
            relaxedCrcPending = false;
            relaxedCrcAckTimer.stop();
        }
    }

    Timer {
        id: relaxedCrcAckTimer
        interval: 5000
        onTriggered: {
            screen.relaxedCrcPending = false;
            screen.relaxedCrcError = qsTr("The decoder has not confirmed the change. The switch shows its current setting.");
        }
    }

    Timer {
        id: lockoutAckTimer
        interval: 5000
        onTriggered: {
            screen.lockoutPending = false;
            screen.lockoutError = qsTr("The decoder has not confirmed the change. The switch shows its current setting.");
        }
    }

    Timer {
        id: versionCopied
        interval: 2000
    }

    function setLockoutPersistence(value) {
        if (!lockoutEditable)
            return;
        lockoutError = "";
        if (sessionRunning && !commands.setPersistTgLockouts(value)) {
            lockoutError = qsTr("Could not change the running decoder's setting. Try again.");
            return;
        }
        prefs.persistTgLockouts = value;
        requestedLockoutPersistence = value;
        lockoutPending = sessionRunning && liveLockoutPersistence !== value;
        if (lockoutPending)
            lockoutAckTimer.restart();
    }

    function setRelaxedCrc(value) {
        if (!relaxedCrcEditable)
            return;
        relaxedCrcError = "";
        if (sessionRunning && !commands.setRelaxedCrc(value)) {
            relaxedCrcError = qsTr("Could not change the running decoder's setting. Try again.");
            return;
        }
        prefs.relaxedCrc = value;
        requestedRelaxedCrc = value;
        relaxedCrcPending = sessionRunning && liveRelaxedCrc !== value;
        if (relaxedCrcPending)
            relaxedCrcAckTimer.restart();
    }

    // Issue #708, D8: call recording applies to a running session as well as the
    // next start. The switches show the preference; the line under them says how
    // the running session took it (recordingStatus is scoped to the session).
    readonly property bool recordingLive: decoderHost.sessionActive === true
    readonly property var recordingNotices: {
        var notes = [];
        if (recordingLive) {
            var state = recordingStatus.commandState;
            var refusal = recordingStatus.refusal;
            if (state === "pending")
                notes.push({text: qsTr("Applying…"), problem: false});
            else if (state === "refused")
                notes.push({text: qsTr("Not applied while Signal search runs. Applies at the next start."), problem: false});
            else if (state === "failed")
                notes.push({text: qsTr("The decoder couldn't apply the change. Applies at the next start."), problem: true});
            if (refusal === "storage-unavailable")
                notes.push({text: qsTr("This session has no storage for call recordings. Applies at the next start."), problem: false});
            else if (refusal === "static-wav")
                notes.push({text: qsTr("This session records to one file (-w in Extra decoder arguments). Applies at the next start."), problem: false});
            else if (refusal === "starting")
                notes.push({text: qsTr("The session is still starting. Applies at the next start."), problem: false});
            var failures = recordingStatus.failures;
            if (failures > 0) {
                var kind = recordingStatus.lastError;
                var why = kind === "write" && recordings.lowSpace ? qsTr("This phone's storage is full.")
                    : kind === "write" ? qsTr("Writing the file failed.")
                    : kind === "open" ? qsTr("The file couldn't be created.")
                    : qsTr("The file couldn't be finished. It's kept under Unfinished recordings.");
                notes.push({text: (failures === 1 ? qsTr("A call couldn't be recorded.") : qsTr("%1 calls couldn't be recorded.").arg(Util.countText(failures))) + " " + why, problem: true});
            }
            if (recordingStatus.journal === "failed")
                notes.push({text: qsTr("New call recordings can't be listed in the app this session. Stop and start listening to try again."), problem: true});
        }
        if (!recordings.available)
            notes.push({text: qsTr("Storage for call recordings isn't available right now. Saved recordings are kept."), problem: true});
        else if (recordings.lowSpace && prefs.recordCalls)
            notes.push({text: recordings.freeBytes >= 0 ? qsTr("Only %1 free on this phone. Calls stop recording when it's full.").arg(Util.sizeText(recordings.freeBytes)) : qsTr("This phone is low on space. Calls stop recording when it's full."), problem: true});
        if (recordings.removedByLimit > 0)
            notes.push({text: recordings.removedByLimit === 1 ? qsTr("The storage limit removed the oldest recording.") : qsTr("The storage limit removed the %1 oldest recordings.").arg(Util.countText(recordings.removedByLimit)), problem: false});
        return notes;
    }
    readonly property string recordingUsage: !recordings.indexed ? qsTr("Counting recordings…") : recordings.recordingCount + recordings.unfinishedCount === 0 ? qsTr("No recordings yet") : Util.recordingUsageText(recordings.usedBytes, prefs.recordingLimitMb, recordings.recordingCount + recordings.unfinishedCount)

    function setRecordCalls(on) {
        prefs.recordCalls = on;
        if (recordingLive)
            commands.setRecordCalls(on, prefs.recordUnknownOnly);
    }
    // Unknown-only travels with Record calls: it applies only while calls record.
    function setRecordUnknownOnly(on) {
        prefs.recordUnknownOnly = on;
        if (recordingLive && prefs.recordCalls)
            commands.setRecordUnknownOnly(on);
    }

    signal openDiagnostics
    signal openImports
    signal openCallRecordings
    signal openRadioReferenceAccount
    signal openLicenses

    // Issue #580: Main owns the one backup flow both Settings screens use; these
    // reach it, and the flow's last result comes back for the card.
    property string backupNotice: ""
    property bool backupNoticeProblem: false
    signal exportBackupRequested
    signal restoreBackupRequested
    signal cancelRestoreRequested

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
        objectName: "settingsScroll"
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
                visible: screen.showTitle
                height: screen.showTitle ? implicitHeight : 0
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
                        objectName: "settingsAppearanceHeader"
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
                        objectName: "settingsUnitsHeader"
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
                        objectName: "settingsListeningHeader"
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
                        title: qsTr("Save avoided talkgroups")
                        subtitle: qsTr("Applies to Avoid TG immediately, saving it to the talkgroup list. Off keeps avoids until stop or list reload, and the button reads Avoid TG. Skip is unaffected. Talkgroup-list edits still save.")
                        checked: screen.sessionRunning ? screen.liveLockoutPersistence : prefs.persistTgLockouts
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
                        visible: screen.sessionRunning && metrics.temporaryTgAvoidCount + metrics.callSkipCount > 0
                        enabled: screen.lockoutEditable
                        text: qsTr("Clear temporary avoids and call skips — current list (%1)").arg(Util.idText(metrics.temporaryTgAvoidCount + metrics.callSkipCount))
                        onClicked: {
                            screen.lockoutError = commands.clearTemporaryTgAvoids(metrics.tgPolicyContext)
                                ? "" : qsTr("Could not clear temporary avoids. Try again.");
                        }
                    }

                    ToggleRow {
                        objectName: "backgroundListeningToggle"
                        title: qsTr("Keep listening in background")
                        subtitle: qsTr("Notification controls are available when permission is allowed")
                        checked: prefs.backgroundListening
                        showDivider: decoderHost.screenPolicySupported || decoderHost.localDeviceBrokered
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
                        objectName: "autoStartOnAttachToggle"
                        visible: decoderHost.localDeviceBrokered
                        title: qsTr("Start when a dongle is attached")
                        subtitle: qsTr("Resume the last USB-receiver system or scan list")
                        checked: prefs.autoStartOnAttach
                        showDivider: decoderHost.screenPolicySupported
                        onToggled: function (state) {
                            prefs.autoStartOnAttach = state;
                        }
                    }

                    // Issue #574. Both rows are hidden where the host cannot honor
                    // them: a setting that persists but changes nothing reads as broken.
                    DisclosureRow {
                        objectName: "screenModeRow"
                        visible: decoderHost.screenPolicySupported
                        title: qsTr("Screen")
                        subtitle: screen.screenModeSubtitle
                        showDivider: screen.screenDelayShown
                        onTapped: screenChoices.open(title, screen.screenModeNames, function (index) {
                            prefs.screenMode = index;
                        })
                    }

                    DisclosureRow {
                        objectName: "screenDelayRow"
                        visible: screen.screenDelayShown
                        title: prefs.screenMode === 3 ? qsTr("Turn off after") : qsTr("Dim after")
                        subtitle: screen.screenDelayText(prefs.screenDelaySec)
                        onTapped: {
                            var seconds = prefs.screenDelayChoices;
                            screenChoices.open(title, seconds.map(function (value) {
                                return screen.screenDelayText(value);
                            }), function (index) {
                                prefs.screenDelaySec = seconds[index];
                            });
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
                        objectName: "settingsDecodingHeader"
                        width: parent.width
                        wrapMode: Text.Wrap
                        text: qsTr("Decoding · next start")
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
                        objectName: "relaxedCrcToggle"
                        title: qsTr("Relaxed CRC")
                        subtitle: qsTr("Applies right away, unlike the rest of this section, and at every start. Accepts control and data whose CRC check failed, and marks them. Needed for DMR RAS (Restricted Access to System) systems. A CRC option in Extra decoder arguments wins.")
                        checked: screen.sessionRunning ? screen.liveRelaxedCrc : prefs.relaxedCrc
                        enabled: screen.relaxedCrcEditable
                        showDivider: !relaxedCrcNote.visible && !relaxedCrcErrorText.visible
                        onToggled: function (value) { screen.setRelaxedCrc(value); }
                    }

                    // A scan row with its own -F or --strict-crc decides for the channel
                    // on air: the switch edits the default beneath it.
                    Text {
                        id: relaxedCrcNote
                        objectName: "relaxedCrcChannelNote"
                        width: parent.width - 2 * Theme.cardPadding
                        x: Theme.cardPadding
                        visible: screen.sessionRunning && metrics.relaxedCrcRowOverride === true
                        text: metrics.relaxedCrcEffective === true ? qsTr("This channel: On (scan row)") : qsTr("This channel: Off (scan row)")
                        wrapMode: Text.Wrap
                        color: Theme.textSecondary
                        font.family: Theme.sans
                        font.pixelSize: Theme.fontSize(12)
                        bottomPadding: relaxedCrcErrorText.visible ? 4 : 12
                    }

                    Text {
                        id: relaxedCrcErrorText
                        objectName: "relaxedCrcError"
                        width: parent.width - 2 * Theme.cardPadding
                        x: Theme.cardPadding
                        visible: text.length > 0
                        text: screen.relaxedCrcError
                        wrapMode: Text.Wrap
                        color: Theme.textSecondary
                        font.family: Theme.sans
                        font.pixelSize: Theme.fontSize(12)
                        bottomPadding: 12
                    }

                    Rectangle {
                        x: Theme.cardPadding
                        width: parent.width - Theme.cardPadding
                        height: 1
                        visible: relaxedCrcNote.visible || relaxedCrcErrorText.visible
                        color: Theme.divider
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

            // CALL RECORDING (issue #708)
            UiPanel {
                width: parent.width
                height: recordingColumn.height + Theme.cardPadding + 4

                Column {
                    id: recordingColumn

                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.topMargin: Theme.cardPadding
                    spacing: 0

                    MicroLabel {
                        objectName: "settingsCallRecordingHeader"
                        width: parent.width
                        wrapMode: Text.Wrap
                        text: qsTr("Call recording")
                        leftPadding: Theme.cardPadding
                        bottomPadding: 6
                    }

                    ToggleRow {
                        objectName: "recordCallsToggle"
                        title: qsTr("Record calls")
                        subtitle: qsTr("Saves each call you hear so you can replay it from History")
                        checked: prefs.recordCalls
                        showDivider: true
                        onToggled: function (value) { screen.setRecordCalls(value); }
                    }

                    ToggleRow {
                        objectName: "recordUnknownOnlyToggle"
                        title: qsTr("Only unknown talkgroups")
                        subtitle: qsTr("Skip talkgroups in your talkgroup list")
                        checked: prefs.recordUnknownOnly
                        enabled: prefs.recordCalls
                        showDivider: recordingNoticeColumn.visible === false
                        onToggled: function (value) { screen.setRecordUnknownOnly(value); }
                    }

                    Column {
                        id: recordingNoticeColumn
                        objectName: "recordingNotices"
                        x: Theme.cardPadding
                        width: parent.width - 2 * Theme.cardPadding
                        visible: screen.recordingNotices.length > 0
                        topPadding: 2
                        bottomPadding: 12
                        spacing: 6

                        Repeater {
                            model: screen.recordingNotices
                            Text {
                                required property var modelData
                                width: parent.width
                                text: modelData.text
                                textFormat: Text.PlainText
                                wrapMode: Text.Wrap
                                font.family: Theme.sans
                                font.pixelSize: Theme.fontSize(12)
                                color: modelData.problem ? Theme.alert : Theme.textSecondary
                                Accessible.role: modelData.problem ? Accessible.AlertMessage : Accessible.StaticText
                                Accessible.name: text
                            }
                        }
                    }

                    Rectangle {
                        x: Theme.cardPadding
                        width: parent.width - Theme.cardPadding
                        height: 1
                        visible: recordingNoticeColumn.visible
                        color: Theme.divider
                    }

                    DisclosureRow {
                        objectName: "recordingLimitRow"
                        title: qsTr("Storage limit")
                        subtitle: qsTr("%1 · Oldest recordings are removed first").arg(Util.limitText(prefs.recordingLimitMb))
                        showDivider: true
                        onTapped: {
                            var limits = prefs.recordingLimitChoices;
                            screenChoices.open(title, limits.map(function (mb) {
                                return Util.limitText(mb);
                            }), function (index) {
                                prefs.recordingLimitMb = limits[index];
                            });
                        }
                    }

                    DisclosureRow {
                        objectName: "manageCallRecordingsRow"
                        title: qsTr("Manage call recordings")
                        subtitle: screen.recordingUsage
                        onTapped: screen.openCallRecordings()
                    }
                }
            }

            // RADIO DEFAULTS (collapsible)
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
                        objectName: "settingsRadioDefaultsHeader"
                        width: parent.width - 2 * Theme.cardPadding - 28
                        wrapMode: Text.Wrap
                        anchors.left: parent.left
                        anchors.leftMargin: Theme.cardPadding
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Radio defaults · next start")
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

            // LIBRARIES
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
                        objectName: "settingsLibrariesHeader"
                        width: parent.width
                        wrapMode: Text.Wrap
                        text: qsTr("Libraries")
                        leftPadding: Theme.cardPadding
                        bottomPadding: 6
                    }

                    DisclosureRow {
                        objectName: "settingsImportsRow"
                        title: qsTr("Imported files")
                        subtitle: qsTr("Channel maps, talkgroups, keys, band plans, radio IDs")
                        onTapped: screen.openImports()
                    }
                }
            }

            // ACCOUNT
            UiPanel {
                width: parent.width
                visible: radioReference.available
                height: accountColumn.height + Theme.cardPadding + 4

                Column {
                    id: accountColumn
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.topMargin: Theme.cardPadding
                    spacing: 0

                    MicroLabel {
                        objectName: "settingsAccountHeader"
                        width: parent.width
                        wrapMode: Text.Wrap
                        text: qsTr("Account")
                        leftPadding: Theme.cardPadding
                        bottomPadding: 6
                    }

                    DisclosureRow {
                        objectName: "settingsAccountRow"
                        title: qsTr("RadioReference")
                        subtitle: qsTr("Username and application key")
                        onTapped: screen.openRadioReferenceAccount()
                    }
                }
            }

            // BACKUP
            UiPanel {
                width: parent.width
                height: backupColumn.height + Theme.cardPadding + 4

                Column {
                    id: backupColumn
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.topMargin: Theme.cardPadding
                    spacing: 0

                    MicroLabel {
                        objectName: "settingsBackupHeader"
                        width: parent.width
                        wrapMode: Text.Wrap
                        text: qsTr("Backup")
                        leftPadding: Theme.cardPadding
                        bottomPadding: 6
                    }

                    DisclosureRow {
                        objectName: "settingsBackupExportRow"
                        title: qsTr("Export backup")
                        subtitle: qsTr("Encrypted file with your settings, systems, scan lists, imported files and keys")
                        showDivider: true
                        onTapped: screen.exportBackupRequested()
                    }

                    // A restore replaces what a running session uses, so it waits
                    // for Stop; one already waiting finishes or is cancelled first.
                    // A backup job on the worker also shuts the row, but is no
                    // reason to stop listening.
                    DisclosureRow {
                        objectName: "settingsBackupRestoreRow"
                        readonly property bool available: backup.restoreAllowed && !backup.restorePending
                        enabled: available
                        showCaret: available
                        title: qsTr("Restore from backup")
                        subtitle: !backup.restoreAllowed && decoderHost.sessionActive ? qsTr("Stop listening to restore") : backup.restorePending ? qsTr("A restore is waiting to finish") : qsTr("Replaces your setup on this phone")
                        onTapped: screen.restoreBackupRequested()
                    }

                    Text {
                        objectName: "settingsBackupNotice"
                        x: Theme.cardPadding
                        width: parent.width - 2 * Theme.cardPadding
                        visible: text.length > 0
                        bottomPadding: 10
                        text: screen.backupNotice
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        font.family: Theme.sans
                        font.pixelSize: Theme.fontSize(13)
                        color: screen.backupNoticeProblem ? Theme.alert : Theme.textSecondary
                        Accessible.role: screen.backupNoticeProblem ? Accessible.AlertMessage : Accessible.StaticText
                        Accessible.name: text
                    }

                    Column {
                        x: Theme.cardPadding
                        width: parent.width - 2 * Theme.cardPadding
                        visible: backup.restorePending
                        bottomPadding: 10
                        spacing: 10

                        // Once its apply has started, only the next start can finish it.
                        Text {
                            objectName: "settingsBackupPendingNotice"
                            width: parent.width
                            text: backup.restoreCancellable ? qsTr("Restore is ready. Close DSD-neo and open it again to finish.") : Util.restorePartWayText()
                            wrapMode: Text.Wrap
                            font.family: Theme.sans
                            font.pixelSize: Theme.fontSize(13)
                            color: Theme.alert
                            Accessible.role: Accessible.AlertMessage
                            Accessible.name: text
                        }

                        OutlineButton {
                            objectName: "settingsBackupCancelRestore"
                            visible: backup.restoreCancellable
                            width: parent.width
                            text: qsTr("Cancel restore")
                            onClicked: screen.cancelRestoreRequested()
                        }
                    }
                }
            }

            // ABOUT & SUPPORT
            UiPanel {
                width: parent.width
                height: supportColumn.height + Theme.cardPadding + 4

                Column {
                    id: supportColumn
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.topMargin: Theme.cardPadding
                    spacing: 0

                    MicroLabel {
                        objectName: "settingsSupportHeader"
                        width: parent.width
                        wrapMode: Text.Wrap
                        text: qsTr("About & support")
                        leftPadding: Theme.cardPadding
                        bottomPadding: 6
                    }

                    DisclosureRow {
                        objectName: "settingsDiagnosticsRow"
                        title: qsTr("Diagnostics")
                        subtitle: qsTr("Current process and previous-run tail; not crash/ANR capture")
                        showDivider: true
                        onTapped: screen.openDiagnostics()
                    }

                    DisclosureRow {
                        objectName: "settingsLicensesRow"
                        title: qsTr("Open source licenses")
                        showDivider: true
                        onTapped: screen.openLicenses()
                    }

                    DisclosureRow {
                        objectName: "settingsVersionRow"
                        title: qsTr("Version")
                        subtitle: versionCopied.running ? qsTr("Version copied") : screen.versionText + " · GPL-3.0"
                        showCaret: false
                        onTapped: {
                            if (decoderHost.copyText(screen.versionText))
                                versionCopied.restart();
                        }
                    }
                }
            }
        }
    }

    // The Screen and delay rows' choices.
    ChoiceSheet {
        id: screenChoices
        objectName: "screenChoices"
        choiceObjectNamePrefix: "screenChoice"
    }
}
