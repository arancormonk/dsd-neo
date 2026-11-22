// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import "Util.js" as Util

// Discussion #900 F3: one DMR Tier III channel's frequency, from the Channels
// sheet. A channel from a row keeps its number; Add channel takes a number
// 1..4094. The edit is sent against the channel as the editor read it (its
// frequency and where that came from, the network and the session), so the
// decoder refuses it when any of those moved on; after such a refusal the
// editor reads the channel again and keeps what was typed. The Channels sheet
// sends the edit (submit) and handles its answer: while it is due nothing
// more can be sent (pending); a refusal is said here (refused()) and the
// editor stays open; an applied edit closes it.
ModalSheet {
    id: sheet
    objectName: "channelEditSheet"
    panelObjectName: "channelEditSheetPanel"
    accessibleName: heading.text
    // A form sheet (issue #718): an outside tap keeps it; Close, Back and Escape ask first.
    closesOnOutsideTap: false
    property var channels: dmrChannels
    // Sends an edit: (lpcn, op, hz, expected channel, generation, session) to
    // the request id, 0 when nothing was sent.
    property var submit: null
    property bool pending: false

    // The channel as it was when the editor opened (DmrChannelsModel::channel()),
    // and the network and session the edit expects.
    property var opened: ({lpcn: 0, valid: false, hz: 0, originCode: 0, hidden: false, yours: false})
    // Opened from Add channel: the number is typed.
    property bool adding: false
    property bool listed: false
    property double generation: 0
    property string session: ""
    property string initialNumber: ""
    property string initialFrequency: ""
    // The request this editor sent, until it is reopened.
    property double sent: 0
    // A refusal said the channel moved on: the editor follows the record
    // until the next send.
    property bool stale: false
    property bool removeArmed: false
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
    readonly property int typedNumber: /^\d{1,4}$/.test(numberField.text) ? Number(numberField.text) : 0
    readonly property int number: adding ? typedNumber : opened.lpcn
    readonly property bool numberValid: number >= channels.minChannel && number <= channels.maxChannel
    readonly property double typedHz: hzOf(frequencyField.text)
    readonly property bool frequencyValid: hzInRange(typedHz)

    // @p lpcn 0 opens an empty editor (Add channel); @p isListed a channel
    // from the Channels list, which keeps its number.
    function openFor(lpcn, isListed) {
        opened = lpcn > 0 ? channels.channel(lpcn) : ({lpcn: 0, valid: false, hz: 0, originCode: 0, hidden: false, yours: false});
        adding = lpcn <= 0;
        listed = isListed && lpcn > 0;
        generation = channels.generation;
        session = channels.session;
        initialNumber = lpcn > 0 ? String(lpcn) : "";
        initialFrequency = opened.hz > 0 ? Util.mhzValueText(opened.hz) : "";
        numberField.text = initialNumber;
        frequencyField.text = initialFrequency;
        sent = 0;
        stale = false;
        removeArmed = false;
        message = "";
        open();
        if (adding)
            numberField.input.forceActiveFocus();
    }
    // The channel, the network and the session as the record has them now;
    // what was typed stays.
    function reread() {
        var was = opened;
        var lpcn = adding ? number : opened.lpcn;
        if (lpcn > 0)
            opened = channels.channel(lpcn);
        // The actions follow where the channel's frequency now comes from.
        if (!adding)
            listed = opened.originCode > 0;
        generation = channels.generation;
        session = channels.session;
        // An armed Remove asks again only when the channel it would remove
        // changed: another channel's update leaves it armed.
        if (opened.lpcn !== was.lpcn || opened.originCode !== was.originCode || opened.hz !== was.hz
                || opened.hidden !== was.hidden)
            removeArmed = false;
    }
    Connections {
        target: sheet.channels
        enabled: sheet.stale && sheet.visible
        function onChannelsChanged() {
            sheet.reread();
        }
        function onStateChanged() {
            sheet.reread();
        }
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
            return qsTr("This channel changed while you were editing it. Check it, then try again.");
        case "trunk-scan":
            return qsTr("Channel editing is available when one system is running.");
        case "full":
            return qsTr("You already have %1 channels of your own. Remove one first.").arg(channels.maxEntries);
        case "invalid":
            return qsTr("dsd-neo cannot use this channel number or frequency.");
        case "search":
            return qsTr("Channels can't be changed while Signal search is running.");
        case "dropped":
            return qsTr("Listening stopped before the change was made.");
        }
        return qsTr("dsd-neo did not make the change. Try again.");
    }
    function say(text) {
        message = text;
        Navigation.announce(sheet, messageLine, text);
    }
    // Send @p op for the channel in the number field: against the channel as
    // the editor opened it, or, for a number typed since, as the sheet lists
    // it now.
    function send(op, hz) {
        if (pending || readOnly || !numberValid || !submit)
            return;
        Navigation.commitComposing(true);
        var lpcn = number;
        var expected = lpcn === opened.lpcn ? opened : channels.channel(lpcn);
        message = "";
        stale = false;
        sent = submit(lpcn, op, hz, expected, generation, session);
        if (sent <= 0) {
            say(qsTr("Couldn't send the change. Try again."));
            return;
        }
        Qt.inputMethod.hide();
    }
    // The Channels sheet's call for a refusal of this editor's edit.
    function refused(result) {
        var key = String(result.result);
        if (key === "stale") {
            reread();
            stale = true;
        }
        say(refusalText(key));
    }

    ReadableText {
        id: heading
        heading: true
        objectName: "channelEditHeading"
        width: parent.width
        text: !sheet.adding ? qsTr("Channel %1").arg(sheet.opened.lpcn) : sheet.numberValid ? qsTr("Channel %1").arg(sheet.number) : qsTr("Add channel")
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
        // The number of a row is the row's.
        visible: sheet.adding
        label: qsTr("Channel number")
        mono: true
        inputMethodHints: Qt.ImhDigitsOnly
        input.validator: RegularExpressionValidator {
            regularExpression: /^\d{0,4}$/
        }
        error: text.length > 0 && !sheet.numberValid ? qsTr("Enter a channel number from 1 to 4094.") : ""
        // The keyboard's action key goes on to the frequency, the keyboard
        // still open.
        nextField: frequencyField
    }
    ReadableText {
        objectName: "channelEditHelp"
        width: parent.width
        visible: sheet.adding
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
        // What was typed goes: asked twice, as a talkgroup's Remove is.
        text: sheet.removeArmed ? qsTr("Tap again to remove") : qsTr("Remove")
        enabled: !sheet.pending && !sheet.readOnly
        onClicked: {
            if (!sheet.removeArmed) {
                sheet.removeArmed = true;
                return;
            }
            sheet.removeArmed = false;
            sheet.send(sheet.channels.opRestore, 0);
        }
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
