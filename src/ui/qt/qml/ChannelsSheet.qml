// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtQuick.Window
import "Util.js" as Util

// Discussion #900 F3: a DMR Tier III site's channels, opened from the Site
// sheet's Channels button and the Monitor's NO FREQ reading. "No frequency"
// lists the channels the site granted calls on that have no frequency here,
// so dsd-neo cannot follow those calls; "Channels" lists the ones that have
// one, and where it came from. A row opens the channel editor; an edit is
// kept on the saved system the session runs (the bridge does that once the
// decoder applied it). Under a trunk scan the channels are read-only.
ModalSheet {
    id: sheet
    objectName: "channelsSheet"
    panelObjectName: "channelsSheetPanel"
    accessibleName: qsTr("Tier III channels")
    property var channels: dmrChannels
    property var bridge: commands
    onVisibleChanged: {
        channels.active = visible;
        followLpcn = 0;
        // A status is kept until it was on screen: one that came while the
        // sheet was closed is still there when it opens again, and is read
        // out once then, as it would have been had the sheet been open.
        if (visible) {
            statusSeen = status.length > 0;
            if (statusSeen)
                Qt.callLater(sheet.announceKept);
        } else if (statusSeen) {
            setStatus("", false);
        }
    }
    function announceKept() {
        if (visible && status.length > 0)
            Navigation.announce(sheet, statusLine, status);
    }
    Component.onDestruction: channels.active = false
    function open() {
        visible = true;
    }

    readonly property bool readOnly: channels.trunkScanActive === true
    // What the last edit leaves to say: not kept, not saved, or refused after
    // the editor closed.
    property string status: ""
    property bool statusProblem: false
    property bool statusSeen: false
    // The edit sent and not yet answered (CommandBridge's request id), 0 for
    // none. Held here, not in the editor, so its answer is handled however the
    // editor was closed or reopened meanwhile.
    property double request: 0
    // Navigation.announce()'s record of what was read out.
    property string announced: ""
    property int announcements: 0
    // The channel the focus goes to once the record lists it (#782): an edit
    // applied from a "No frequency" row moves the channel to "Channels", and
    // the row the focus was on now holds another channel, or none.
    property int followLpcn: 0
    property double followRevision: 0

    function named(name) {
        return name ? " (" + Util.oneLine(name) + ")" : "";
    }
    function heardText(ms) {
        if (!ms)
            return "";
        var delta = Math.max(0, channels.decodeNowMs() - ms);
        if (delta < 60000)
            return qsTr("last heard just now");
        if (delta < 3600000)
            return qsTr("last heard %1 min ago").arg(Math.floor(delta / 60000));
        if (delta < 86400000)
            return qsTr("last heard %1 h ago").arg(Math.floor(delta / 3600000));
        return qsTr("last heard %1 d ago").arg(Math.floor(delta / 86400000));
    }
    // A row's words: [title, details as drawn, the whole row as read]. A
    // private call is drawn caller → called radio and read "caller to called".
    function missingWords(m) {
        var title = qsTr("Channel %1").arg(m.lpcn) + (m.unverified ? " " + qsTr("(unchecked)") : "");
        var shown = [];
        var spoken = [];
        if (m.detailed) {
            if (m.slot > 0)
                shown.push(qsTr("slot %1").arg(m.slot));
            if (m.data)
                shown.push(qsTr("data"));
            spoken = shown.slice();
            if (m.group) {
                shown.push(qsTr("TG %1").arg(m.recipient) + named(m.recipientName));
                spoken.push(shown[shown.length - 1]);
            } else {
                var caller = m.caller + named(m.callerName);
                var called = m.recipient + named(m.recipientName);
                shown.push(caller + " → " + called);
                spoken.push(qsTr("%1 to %2").arg(caller).arg(called));
            }
            // Not with a screen reader on: an age would rename the row as it
            // moves on, or freeze between renames (issue #782).
            var heard = screenReader.active ? "" : heardText(m.lastHeardMs);
            if (heard.length) {
                shown.push(heard);
                spoken.push(heard);
            }
        }
        return [title, shown.join(" · "), [title].concat(spoken).join(" · ")];
    }
    function originText(origin) {
        switch (origin) {
        case "file":
            return qsTr("from file");
        case "user":
            return qsTr("yours");
        case "site":
            return qsTr("heard from site");
        case "unchecked":
            return qsTr("unchecked");
        case "neighbour":
            return qsTr("from a neighbour site");
        case "estimated":
            return qsTr("estimated");
        }
        return "";
    }
    function channelWords(c) {
        var title = qsTr("Channel %1").arg(c.lpcn);
        var details = c.hidden ? qsTr("hidden") : [(c.hz / 1000000).toFixed(6) + " MHz", originText(c.origin)].filter(function (s) {
            return s.length > 0;
        }).join(" · ");
        return [title, details, title + " · " + details];
    }
    // Issue #782: with a screen reader on, the channels without a frequency
    // in channel order; most recently heard first moves a row with each grant
    // heard again, and Qt's bridge tells Android nothing of a move.
    readonly property var missingEntries: screenReader.active ? channels.missingByChannel : channels.missing

    function openEditor(lpcn, listed) {
        if (readOnly)
            return;
        editSheet.openFor(lpcn, listed);
    }
    function setStatus(text, problem) {
        status = text;
        statusProblem = problem;
        statusSeen = visible && text.length > 0;
        if (statusSeen)
            Navigation.announce(sheet, statusLine, text);
    }
    // Send an edit for the editor: the expectations are the row as it read
    // it. Returns the request id, 0 when nothing was sent.
    function submit(lpcn, op, hz, expected, generation, session) {
        if (request > 0 || readOnly)
            return 0;
        if (!bridge.setDmrChannel(lpcn, op, hz, expected.hz || 0, expected.originCode || 0, generation,
                                  channels.sessionSystemUid, session))
            return 0;
        request = bridge.lastDmrChannelRequest;
        // A new edit says its own outcome.
        setStatus("", false);
        return request;
    }
    function answered(result) {
        if (request <= 0 || Number(result.request) !== request)
            return;
        var editorSent = editSheet.visible && editSheet.sent === request;
        request = 0;
        if (result.result === "ok") {
            if (editorSent)
                editSheet.visible = false;
            applied(result);
            return;
        }
        if (editorSent)
            editSheet.refused(result);
        else
            setStatus(editSheet.refusalText(String(result.result)), true);
    }
    Connections {
        target: sheet.bridge
        ignoreUnknownSignals: true
        function onDmrChannelResultReceived(result) {
            sheet.answered(result);
        }
    }
    function applied(result) {
        if (!result.systemUid || String(result.systemUid).length === 0)
            setStatus(qsTr("Not kept: this session is not a saved system."), false);
        else if (result.saved !== true)
            setStatus(saveErrorText(String(result.saveError || "")), true);
        else
            setStatus("", false);
        followLpcn = visible ? Number(result.lpcn) || 0 : 0;
        followRevision = Number(result.revision) || 0;
        Qt.callLater(sheet.follow);
    }
    function saveErrorText(error) {
        var reason = error === "no-system" ? qsTr("this system is no longer in your saved systems.")
            : error === "not-dmr" ? qsTr("this saved system is not set to decode DMR.")
            : error === "full" ? qsTr("this system already keeps %1 channels of yours.").arg(channels.maxEntries)
            : qsTr("dsd-neo could not save this system. Open the channel and apply it again.");
        return qsTr("Changed until you stop listening, but not saved: ") + reason;
    }
    function channelRowFor(lpcn) {
        for (var i = 0; i < channelRows.live; ++i) {
            var row = channelRepeater.itemAt(i);
            if (row && row.channel === lpcn)
                return row;
        }
        return null;
    }
    function follow() {
        if (followLpcn <= 0 || !visible)
            return;
        // Not before the record has the edit: until then a row of the channel
        // can be one about to go (a channel of yours with no base frequency,
        // removed).
        if (channels.revision < followRevision)
            return;
        var row = channelRowFor(followLpcn);
        if (row !== null) {
            followLpcn = 0;
            row.forceActiveFocus();
            return;
        }
        // The record has the edit and no row for it (a channel of yours with
        // no base frequency, removed): the place stays in the sheet, on Add
        // channel when the focus was on the row that went.
        followLpcn = 0;
        var window = sheet.Window.window;
        var focused = window ? window.activeFocusItem : null;
        // A row whose channel went is held: not drawn at once, out of the tree
        // by the end of the turn.
        if (focused && (!Navigation.presented(focused) || focused.live === false || focused.opacity < 0.5))
            addButton.forceActiveFocus();
    }
    Connections {
        target: sheet.channels
        function onChannelsChanged() {
            Qt.callLater(sheet.follow);
        }
        function onStateChanged() {
            Qt.callLater(sheet.follow);
        }
    }
    // The ages ("last heard 2 min ago") move on while the sheet is open. A
    // screen reader is given no ages (missingWords()).
    Timer {
        interval: 30000
        repeat: true
        running: sheet.visible && !screenReader.active
        onTriggered: missingRows.sync()
    }

    // A channel's row: its number, then what is known of it, read as one
    // line. Tapping it opens the editor (not under a trunk scan).
    component ChannelRow: Rectangle {
        id: row
        required property int index
        required property string key
        required property string line
        property var rows: null
        property Item list: null
        signal activated(int lpcn)
        readonly property var parts: JSON.parse(line)
        readonly property int channel: Number(key)
        readonly property string words: parts[2]
        readonly property string shownText: parts[0] + (parts[1].length ? " · " + parts[1] : "")
        readonly property bool live: index >= 0 && rows !== null && index < rows.live
        readonly property bool acts: live && !sheet.readOnly
        width: parent ? parent.width : 0
        height: Math.max(Theme.minimumTouchSize, texts.height + 20)
        // A row being removed has index -1 for an instant, and a row past the
        // end of a shorter list is held (see the lists below): neither is
        // drawn, named or tapped.
        opacity: index >= 0 && rows !== null && index < rows.shown ? 1 : 0
        radius: Theme.radiusButton
        color: tap.pressed && acts ? Qt.alpha(Theme.cyan, 0.08) : Theme.panel
        border.width: 1
        border.color: Theme.controlBorder
        onYChanged: {
            if (list)
                list.place();
        }
        onHeightChanged: {
            if (list)
                list.place();
        }
        activeFocusOnTab: (navigationAllowed && visible && acts) || activeFocus
        Accessible.role: sheet.readOnly ? Accessible.StaticText : Accessible.Button
        Accessible.name: navigationAllowed && visible && live ? words : ""
        Accessible.focusable: navigationAllowed && visible && live
        readonly property bool navigationAllowed: Navigation.allows(row)
        Accessible.ignored: !navigationAllowed || !visible || !live
        Accessible.onPressAction: activate()
        function activate() {
            if (acts && Navigation.allows(row))
                activated(channel);
        }
        Keys.onPressed: function (event) {
            if (Navigation.activationPress(event, Window.activeFocusItem))
                activate();
        }
        Keys.onReleased: function (event) {
            Navigation.activationRelease(event);
        }
        FocusFrame {}
        GatedTapHandler {
            id: tap
            enabled: row.acts
            onAllowedTap: row.activate()
        }
        // Read in the row's own name, so these stay out of the tree.
        Column {
            id: texts
            x: 14
            width: parent.width - 28 - (rowCaret.visible ? rowCaret.width + 12 : 0)
            anchors.verticalCenter: parent.verticalCenter
            spacing: 3
            Text {
                Accessible.ignored: true
                width: parent.width
                text: row.parts[0]
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: Theme.textPrimary
                font.family: Theme.mono
                font.pixelSize: Theme.fontSize(15)
            }
            // Talkgroup and radio names are what the lists or the air gave.
            Text {
                Accessible.ignored: true
                width: parent.width
                visible: text.length > 0
                text: row.parts[1]
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: Theme.textSecondary
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(13)
            }
        }
        Caret {
            id: rowCaret
            visible: row.acts
            anchors.right: parent.right
            anchors.rightMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            rotation: -90
        }
    }

    ReadableText {
        heading: true
        objectName: "channelsHeading"
        width: parent.width
        text: qsTr("Tier III channels")
        wrapMode: Text.Wrap
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(20)
        color: Theme.textPrimary
    }
    ReadableText {
        objectName: "channelsReadOnlyNote"
        width: parent.width
        visible: sheet.readOnly
        text: qsTr("Channel editing is available when one system is running.")
        wrapMode: Text.Wrap
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(13)
        color: Theme.textSecondary
    }
    ReadableText {
        id: statusLine
        objectName: "channelsStatus"
        width: parent.width
        visible: text.length > 0
        text: sheet.status
        Accessible.role: sheet.statusProblem ? Accessible.AlertMessage : Accessible.StaticText
        wrapMode: Text.Wrap
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(13)
        color: sheet.statusProblem ? Theme.alert : Theme.textSecondary
    }

    MicroLabel {
        objectName: "channelsMissingLabel"
        topPadding: 6
        text: qsTr("No frequency")
    }
    ReadableText {
        objectName: "channelsMissingNote"
        width: parent.width
        visible: missingRows.live > 0
        text: qsTr("dsd-neo cannot retune to calls on these channels until each has a frequency.")
        wrapMode: Text.Wrap
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(13)
        color: Theme.textSecondary
    }
    // In the tree while hidden (issue #782, as the Network sheet's), so that
    // its show when the list empties makes Android's bridge read the sheet
    // again.
    ReadableText {
        objectName: "channelsMissingEmpty"
        width: parent.width
        visible: missingRows.live === 0
        text: qsTr("No unresolved grants recorded.")
        wrapMode: Text.Wrap
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(13)
        color: Theme.textSubdued
    }
    // Issue #782: a row per channel, kept across refreshes (StableRows), and
    // rows past the end of a shorter list held out of sight and out of the
    // tree rather than removed, as the Network sheet's are: a grant heard
    // again renames or moves nothing but its own row, and a hop to a target
    // with fewer channels raises no hide.
    Column {
        id: missingList
        objectName: "channelsMissingList"
        property real shownBottom: 0
        function place() {
            var last = missingRows.shown > 0 ? missingRepeater.itemAt(missingRows.shown - 1) : null;
            shownBottom = last ? last.y + last.height : 0;
        }
        width: parent.width
        spacing: 8
        height: shownBottom
        Connections {
            target: missingRows
            function onShownChanged() {
                missingList.place();
            }
        }
        Repeater {
            id: missingRepeater
            model: StableRows {
                id: missingRows
                entries: sheet.missingEntries
                holdRows: true
                keyOf: function (m) {
                    return String(m.lpcn);
                }
                lineOf: function (m) {
                    return JSON.stringify(sheet.missingWords(m));
                }
            }
            ChannelRow {
                objectName: "channelsMissingRow"
                rows: missingRows
                list: missingList
                onActivated: function (lpcn) {
                    sheet.openEditor(lpcn, false);
                }
            }
        }
    }

    MicroLabel {
        objectName: "channelsChannelsLabel"
        topPadding: 6
        visible: channelRows.live > 0
        text: qsTr("Channels")
    }
    Column {
        id: channelList
        objectName: "channelsChannelList"
        property real shownBottom: 0
        function place() {
            var last = channelRows.shown > 0 ? channelRepeater.itemAt(channelRows.shown - 1) : null;
            shownBottom = last ? last.y + last.height : 0;
        }
        width: parent.width
        spacing: 8
        height: shownBottom
        Connections {
            target: channelRows
            function onShownChanged() {
                channelList.place();
            }
        }
        Repeater {
            id: channelRepeater
            model: StableRows {
                id: channelRows
                entries: sheet.channels.channels
                holdRows: true
                keyOf: function (c) {
                    return String(c.lpcn);
                }
                lineOf: function (c) {
                    return JSON.stringify(sheet.channelWords(c));
                }
            }
            ChannelRow {
                objectName: "channelsChannelRow"
                rows: channelRows
                list: channelList
                onActivated: function (lpcn) {
                    sheet.openEditor(lpcn, true);
                }
            }
        }
    }

    OutlineButton {
        id: addButton
        objectName: "channelsAddButton"
        width: parent.width
        text: qsTr("Add channel")
        enabled: !sheet.readOnly
        onClicked: sheet.openEditor(0, false)
    }
    OutlineButton {
        objectName: "channelsDone"
        width: parent.width
        text: qsTr("Done")
        onClicked: sheet.visible = false
    }

    ChannelEditSheet {
        id: editSheet
        parent: sheet
        channels: sheet.channels
        pending: sheet.request > 0
        submit: function (lpcn, op, hz, expected, generation, session) {
            return sheet.submit(lpcn, op, hz, expected, generation, session);
        }
    }
}
