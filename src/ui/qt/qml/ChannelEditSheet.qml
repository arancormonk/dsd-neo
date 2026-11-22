// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import "Util.js" as Util

// Discussion #900 F3: one DMR Tier III channel's frequency, from the Channels
// sheet. A channel already listed keeps its number; one without a frequency,
// or a new one, takes a number 1..4094. The edit is sent against the channel
// as it was when the editor opened (its frequency and where that came from,
// the network and the session), so the decoder refuses it when any of those
// moved on. While the answer is due nothing more can be sent; a refusal is
// said here and the editor stays open; an applied edit closes it (applied()).
ModalSheet {
    id: sheet
    objectName: "channelEditSheet"
    panelObjectName: "channelEditSheetPanel"
    accessibleName: heading.text
    // A form sheet (issue #718): an outside tap keeps it; Close, Back and Escape ask first.
    closesOnOutsideTap: false
    property var channels: dmrChannels
    property var bridge: commands

    // The edit the decoder applied: CommandBridge's answer map.
    signal applied(var result)
    // A refusal that came after the editor was closed, for the Channels sheet to say.
    signal refusedClosed(string text)

    // The channel as it was when the editor opened (DmrChannelsModel::channel()),
    // and the network and session the edit expects.
    property var opened: ({lpcn: 0, valid: false, hz: 0, originCode: 0, hidden: false, yours: false})
    property bool listed: false
    property double generation: 0
    property string session: ""
    property string initialNumber: ""
    property string initialFrequency: ""
    property double request: 0
    readonly property bool pending: request > 0
    property string message: ""
    // Navigation.announce()'s record of what was read out.
    property string announced: ""
    property int announcements: 0
    readonly property bool readOnly: channels.trunkScanActive === true

    // A frequency typed in MHz to the Hz, without the rounding of a float
    // product: -1 for text that is not one.
    function hzOf(text) {
        var parts = /^(\d{1,5})(?:\.(\d{0,6}))?$/.exec(String(text));
        if (!parts)
            return -1;
        return Number(parts[1]) * 1000000 + Number(((parts[2] || "") + "000000").substring(0, 6));
    }
    function hzInRange(hz) {
        return hz >= channels.minHz && hz <= channels.maxHz;
    }
    readonly property int number: /^\d{1,4}$/.test(numberField.text) ? Number(numberField.text) : 0
    readonly property bool numberValid: number >= channels.minChannel && number <= channels.maxChannel
    readonly property double typedHz: hzOf(frequencyField.text)
    readonly property bool frequencyValid: hzInRange(typedHz)

    // @p lpcn 0 opens an empty editor (Add channel); @p isListed a channel
    // from the Channels list, which keeps its number.
    function openFor(lpcn, isListed) {
        opened = lpcn > 0 ? channels.channel(lpcn) : ({lpcn: 0, valid: false, hz: 0, originCode: 0, hidden: false, yours: false});
        listed = isListed && lpcn > 0;
        generation = channels.generation;
        session = channels.session;
        initialNumber = lpcn > 0 ? String(lpcn) : "";
        initialFrequency = opened.hz > 0 ? Util.mhzValueText(opened.hz) : "";
        numberField.text = initialNumber;
        frequencyField.text = initialFrequency;
        request = 0;
        message = "";
        open();
        if (!listed && lpcn === 0)
            numberField.input.forceActiveFocus();
    }
    function requestClose() {
        if (!pending && (numberField.text !== initialNumber || frequencyField.text !== initialFrequency)) {
            discard.ask(function () {
                sheet.visible = false;
            });
        } else
            visible = false;
    }
    dismissHandler: requestClose
    DiscardDialog {
        id: discard
        parent: sheet
    }

    function refusalText(result) {
        switch (result) {
        case "stale":
            return qsTr("This channel changed before the edit arrived. Check it and try again.");
        case "trunk-scan":
            return qsTr("Channel editing is available when one system is running.");
        case "full":
            return qsTr("You already have %1 channels of your own. Remove one first.").arg(channels.maxEntries);
        case "invalid":
            return qsTr("dsd-neo cannot use this channel number or frequency.");
        case "search":
            return qsTr("Channel editing is not available while Signal search runs.");
        case "dropped":
            return qsTr("The session stopped before the edit was made.");
        }
        return qsTr("The decoder did not take the edit. Try again.");
    }
    function say(text) {
        message = text;
        Navigation.announce(sheet, messageLine, text);
    }
    // Send @p op for the channel in the number field: against the channel as
    // the editor opened it, or, for a number typed since, as the sheet lists
    // it now.
    function send(op, hz) {
        if (pending || readOnly || !numberValid)
            return;
        Navigation.commitComposing(true);
        var lpcn = number;
        var expected = lpcn === opened.lpcn ? opened : channels.channel(lpcn);
        message = "";
        var ok = bridge.setDmrChannel(lpcn, op, hz, expected.hz || 0, expected.originCode || 0, generation,
                                      channels.sessionSystemUid, session);
        if (!ok) {
            say(qsTr("The edit could not be sent. Try again."));
            return;
        }
        request = bridge.lastDmrChannelRequest;
        Qt.inputMethod.hide();
    }
    function answered(result) {
        if (!pending || Number(result.request) !== request)
            return;
        request = 0;
        if (result.result === "ok") {
            visible = false;
            applied(result);
            return;
        }
        var text = refusalText(String(result.result));
        if (visible)
            say(text);
        else
            refusedClosed(text);
    }
    Connections {
        target: sheet.bridge
        ignoreUnknownSignals: true
        function onDmrChannelResultReceived(result) {
            sheet.answered(result);
        }
    }

    ReadableText {
        id: heading
        heading: true
        objectName: "channelEditHeading"
        width: parent.width
        text: sheet.opened.lpcn > 0 ? qsTr("Channel %1").arg(sheet.opened.lpcn) : qsTr("Add channel")
        wrapMode: Text.Wrap
        color: Theme.textPrimary
        font.pixelSize: Theme.fontSize(20)
    }
    ReadableText {
        objectName: "channelEditNotKept"
        width: parent.width
        visible: !sheet.channels.sessionHasSavedSystem
        text: qsTr("Not kept: this session is not a saved system.")
        wrapMode: Text.Wrap
        color: Theme.textSecondary
        font.pixelSize: Theme.fontSize(13)
    }
    PlexTextField {
        id: numberField
        objectName: "channelEditNumber"
        width: parent.width
        visible: !sheet.listed
        label: qsTr("Channel number")
        mono: true
        inputMethodHints: Qt.ImhDigitsOnly
        input.validator: RegularExpressionValidator {
            regularExpression: /^\d{0,4}$/
        }
        error: text.length > 0 && !sheet.numberValid ? qsTr("Enter a channel number from 1 to 4094.") : ""
        nextField: null
        onAccepted: frequencyField.forceActiveFocus()
    }
    ReadableText {
        objectName: "channelEditHelp"
        width: parent.width
        visible: !sheet.listed
        text: qsTr("Use the channel number dsd-neo shows. If your list counts each timeslot (DSD+, some RadioReference lists), take (number − 1) ÷ 2 and drop the remainder.")
        wrapMode: Text.Wrap
        color: Theme.textSecondary
        font.pixelSize: Theme.fontSize(13)
    }
    // Read in the field's name, "Frequency in MHz" (issue #782).
    MicroLabel {
        objectName: "channelEditFrequencyCaption"
        topPadding: 4
        text: qsTr("Frequency")
        Accessible.ignored: true
    }
    FrequencyField {
        id: frequencyField
        width: parent.width
        fieldObjectName: "channelEditFrequency"
        pixelSize: 26
        accepts: function (text) {
            return sheet.hzInRange(sheet.hzOf(text));
        }
        invalidText: qsTr("Enter a frequency from 0.1 to 2147.483647 MHz")
        onAccepted: {
            if (saveButton.enabled && saveButton.inputValid)
                saveButton.activate();
        }
    }
    // Only checked evidence that this carrier is the site's control channel
    // (a checked C_ALOHA or C_SYS_Parms while no call is followed) offers it.
    OutlineButton {
        objectName: "channelEditUseCc"
        width: parent.width
        text: qsTr("Use control channel frequency")
        enabled: sheet.channels.ccConfirmedHere === true && sheet.channels.ccHz > 0 && !sheet.pending && !sheet.readOnly
        onClicked: frequencyField.text = Util.mhzValueText(sheet.channels.ccHz)
    }
    GradientButton {
        id: saveButton
        objectName: "channelEditSave"
        width: parent.width
        text: sheet.listed ? qsTr("Apply") : qsTr("Add channel")
        enabled: !sheet.pending && !sheet.readOnly && (inputValid || Navigation.composing)
        inputValid: sheet.numberValid && sheet.frequencyValid
        onClicked: sheet.send(sheet.channels.opSet, sheet.typedHz)
    }
    // One per channel, by where its frequency came from: a channel from the
    // file or the air can be hidden, a hidden one restored, and one of yours
    // removed (back to the frequency it had before, if any).
    OutlineButton {
        objectName: "channelEditHide"
        width: parent.width
        visible: sheet.listed && !sheet.opened.yours && !sheet.opened.hidden
        text: qsTr("Hide")
        enabled: !sheet.pending && !sheet.readOnly
        onClicked: sheet.send(sheet.channels.opHide, 0)
    }
    OutlineButton {
        objectName: "channelEditRestore"
        width: parent.width
        visible: sheet.listed && sheet.opened.hidden === true
        text: qsTr("Restore")
        enabled: !sheet.pending && !sheet.readOnly
        onClicked: sheet.send(sheet.channels.opRestore, 0)
    }
    OutlineButton {
        objectName: "channelEditRemove"
        width: parent.width
        visible: sheet.listed && sheet.opened.yours === true && !sheet.opened.hidden
        text: qsTr("Remove")
        enabled: !sheet.pending && !sheet.readOnly
        onClicked: sheet.send(sheet.channels.opRestore, 0)
    }
    ReadableText {
        id: messageLine
        objectName: "channelEditMessage"
        width: parent.width
        visible: text.length > 0
        text: sheet.message
        Accessible.role: Accessible.AlertMessage
        wrapMode: Text.Wrap
        color: Theme.alert
        font.pixelSize: Theme.fontSize(13)
    }
    // The way out a form sheet keeps visible (issue #718); it asks before an
    // edit goes. Open while an edit is due: closing does not take it back.
    OutlineButton {
        objectName: "channelEditClose"
        width: parent.width
        text: qsTr("Close")
        onClicked: sheet.requestClose()
    }
}
