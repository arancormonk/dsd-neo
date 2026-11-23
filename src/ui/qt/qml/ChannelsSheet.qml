// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtQuick.Window
import "Util.js" as Util

// Discussion #900 F3: a DMR Tier III site's channels, opened from the Site
// sheet's Channels button and the Monitor's NO FREQ reading. "No frequency"
// lists the channels the site granted calls on that have no frequency here,
// so dsd-neo cannot follow those calls; "Channels" lists the ones that have
// one, and where it came from, a page at a time. A row opens the channel
// editor; an edit is kept on the saved system the session runs (the bridge
// does that once the decoder applied it), and a save that failed can be tried
// again. Under a trunk scan the channels are read-only; on another system's
// carrier, or in a session no longer decoding DMR, none is listed or changed;
// and while a save is owed for the session's system nothing more is changed
// until Try saving again saves it, so the saved list never takes the edits in
// another order than the live one.
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
            return;
        }
        // The editor and its question go with the sheet: hidden only through
        // it, the editor keeps its own visible flag and would stand over the
        // sheet, perhaps on another session, the next time it opens. An edit
        // already sent stays the sheet's to answer (request).
        editSheet.visible = false;
        firstPages();
        // A save still owed keeps the words that say so.
        if (statusSeen && owedSaveCount === 0)
            setStatus("", false);
    }
    // The session ended: so does the edit, as the Talkgroups editor's does.
    readonly property bool hostRunning: decoderHost.running
    onHostRunningChanged: {
        if (!hostRunning)
            editSheet.visible = false;
    }
    function announceKept() {
        if (visible && status.length > 0)
            Navigation.announce(sheet, statusLine, status);
    }
    Component.onDestruction: channels.active = false
    function open() {
        visible = true;
    }

    // Why no edit can be sent now, "" when one can: the editor shows the same
    // words (lockText). A save owed for the session's system comes first to
    // be fixed (fail closed): an edit made after it could be saved before it.
    readonly property bool otherSystem: channels.otherSystemHere === true
    // A live mode change left DMR off: no Tier III lookup reads the channels.
    readonly property bool notDecoding: channels.dmrDecoding === false
    // Either way no Tier III row is listed.
    readonly property bool noTier3Here: otherSystem || notDecoding
    readonly property bool saveOwedHere: owedSaveCount > 0 && owedFor(String(channels.sessionSystemUid))
    readonly property string lockNote: notDecoding
        ? qsTr("This session isn't decoding DMR now, so channels can't be changed.")
        : otherSystem ? qsTr("dsd-neo is on another system now, so Tier III channels can't be changed here.")
        : channels.trunkScanActive === true ? qsTr("Channel editing is available when one system is running.")
        : ""
    readonly property string saveFirstText: qsTr("Save the earlier change first: Try saving again.")
    readonly property string lockText: lockNote.length ? lockNote : saveOwedHere ? saveFirstText : ""
    readonly property bool readOnly: lockText.length > 0
    onNoTier3HereChanged: {
        if (noTier3Here)
            Qt.callLater(sheet.noTier3Arrived);
    }
    // The carrier now names another system, or DMR is no longer decoded: the
    // rows went. Said once, and the focus moves to the words that say why
    // when it was on a row that went.
    function noTier3Arrived() {
        if (!visible || !noTier3Here)
            return;
        followLpcn = 0;
        Navigation.announce(sheet, readOnlyNote, lockNote);
        var window = sheet.Window.window;
        var focused = window ? window.activeFocusItem : null;
        if (editSheet.visible || !focused)
            return;
        if (!Navigation.presented(focused) || focused.live === false || focused.opacity < 0.5)
            readOnlyNote.forceActiveFocus();
    }
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
    // the row the focus was on now holds another channel, or none. Set only
    // for an answer that came while the editor that sent it was open.
    property int followLpcn: 0
    property double followRevision: 0
    // The channels with a frequency a page at a time, by number: a channel
    // map can list thousands, and each row is built when it is listed. Show
    // more channels adds the next page; the sheet opens on the first again,
    // and holds no rows past it. The channels with no frequency are paged the
    // same way: the ledger can hold every channel number (4094), and the
    // model builds only the rows of the pages shown (missingLimit).
    readonly property int channelPageSize: 50
    property int channelLimit: channelPageSize
    readonly property var allChannels: channels.channels
    readonly property var channelPage: allChannels.length > channelLimit ? allChannels.slice(0, channelLimit) : allChannels
    property int missingLimit: channelPageSize
    readonly property var missingPage: channels.missingPage
    Binding {
        target: sheet.channels
        property: "missingLimit"
        value: sheet.missingLimit
    }
    // Edits the decoder made whose save failed (the saved system could not be
    // written), the latest for each system and channel: Try saving again saves
    // them. Kept while the sheet is closed; a save that cannot work (the system
    // was removed, say) is not kept.
    property var owedSaves: ({})
    property int owedSaveCount: 0
    function owedFor(uid) {
        for (var key in owedSaves) {
            if (owedSaves[key].systemUid === uid)
                return true;
        }
        return false;
    }
    // An edit sent and still unanswered after a moment: the decoder applies
    // an edit made during a DMR voice call when the call ends.
    property bool requestSlow: false
    property int slowRequestMs: 2000
    onRequestChanged: requestSlow = false
    Timer {
        interval: sheet.slowRequestMs
        running: sheet.request > 0
        onTriggered: {
            sheet.requestSlow = true;
            sheet.sayWaiting();
        }
    }
    readonly property string waitingText: qsTr("Saved when the current call ends.")
    // Said once where it shows: in the editor that sent the edit, else here.
    function sayWaiting() {
        if (editSheet.visible && editSheet.sent === request)
            editSheet.sayWaiting(waitingText);
        else if (visible)
            Navigation.announce(sheet, waitingLine, waitingText);
    }

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
    Binding {
        target: sheet.channels
        property: "missingInChannelOrder"
        value: screenReader.active
    }

    function openEditor(lpcn, listed) {
        if (readOnly)
            return;
        editSheet.openFor(lpcn, listed);
    }
    function showMoreChannels() {
        var first = channelLimit;
        var focusHere = moreButton.activeFocus;
        channelLimit += channelPageSize;
        // The button goes with the last page: the focus moves on to the first
        // channel it added rather than out of the sheet.
        if (focusHere && allChannels.length <= channelLimit)
            Qt.callLater(sheet.focusChannelAt, first);
    }
    function focusChannelAt(at) {
        var row = at < channelRows.live ? channelRepeater.itemAt(at) : null;
        if (row)
            row.forceActiveFocus();
        else
            addButton.forceActiveFocus();
    }
    function showMoreMissing() {
        var first = missingLimit;
        var focusHere = moreMissingButton.activeFocus;
        missingLimit += channelPageSize;
        if (focusHere && channels.missingCount <= missingLimit)
            Qt.callLater(sheet.focusMissingAt, first);
    }
    function focusMissingAt(at) {
        var row = at < missingRows.live ? missingRepeater.itemAt(at) : null;
        if (row)
            row.forceActiveFocus();
        else
            addButton.forceActiveFocus();
    }
    function firstPages() {
        channelLimit = channelPageSize;
        missingLimit = channelPageSize;
        // Closed, no screen reader is told of rows that go.
        if (channelRows.count > channelPageSize)
            channelRows.remove(channelPageSize, channelRows.count - channelPageSize);
        if (missingRows.count > channelPageSize)
            missingRows.remove(channelPageSize, missingRows.count - channelPageSize);
    }
    function owe(result) {
        var key = result.systemUid + "|" + result.lpcn;
        var owed = Object.assign({}, owedSaves);
        if (result.saved !== true && String(result.saveError) === "not-saved")
            owed[key] = {systemUid: String(result.systemUid), lpcn: Number(result.lpcn), op: Number(result.op), hz: Number(result.hz) || 0};
        else
            delete owed[key];
        owedSaves = owed;
        owedSaveCount = Object.keys(owed).length;
    }
    function retrySaves() {
        var left = {};
        var problem = "";
        for (var key in owedSaves) {
            var edit = owedSaves[key];
            var kept = bridge.saveDmrChannelEdit(edit.systemUid, edit.lpcn, edit.op, edit.hz);
            if (kept.saved === true)
                continue;
            if (String(kept.saveError) === "not-saved")
                left[key] = edit;
            else if (!problem.length)
                problem = String(kept.saveError);
        }
        var hadFocus = retryButton.activeFocus;
        owedSaves = left;
        owedSaveCount = Object.keys(left).length;
        if (owedSaveCount > 0)
            setStatus(saveErrorText("not-saved"), true);
        else if (problem.length)
            setStatus(saveErrorText(problem), true);
        else
            setStatus(qsTr("Saved."), false);
        // The button went: the focus stays in the sheet.
        if (hadFocus && owedSaveCount === 0)
            addButton.forceActiveFocus();
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
        // A new edit says its own outcome; a save still owed keeps its words.
        if (owedSaveCount === 0)
            setStatus("", false);
        return request;
    }
    function answered(result) {
        if (request <= 0 || Number(result.request) !== request)
            return;
        var editorSent = editSheet.visible && editSheet.sent === request;
        var key = String(result.result);
        request = 0;
        if (key === "ok") {
            if (editorSent)
                editSheet.visible = false;
            applied(result, editorSent);
            return;
        }
        // A channel that changed meanwhile is opened again to be seen as it
        // is now: the editor that sent the edit closes, and the sheet says so.
        if (editorSent && key !== "stale") {
            editSheet.refused(result);
            return;
        }
        if (editorSent)
            editSheet.visible = false;
        setStatus(editSheet.refusalText(key), true);
    }
    Connections {
        target: sheet.bridge
        ignoreUnknownSignals: true
        function onDmrChannelResultReceived(result) {
            sheet.answered(result);
        }
    }
    // @p editorSent: the editor that sent the edit was open when its answer
    // came; only then does the focus follow the channel (a later answer is
    // handled and saved, but the focus stays where the user has it now).
    function applied(result, editorSent) {
        var kept = result.systemUid && String(result.systemUid).length > 0;
        if (kept)
            owe(result);
        if (!kept)
            setStatus(qsTr("Not kept: this session is not a saved system."), false);
        else if (result.saved !== true)
            setStatus(saveErrorText(String(result.saveError || "")), true);
        else if (owedSaveCount > 0)
            setStatus(saveErrorText("not-saved"), true);
        else
            setStatus("", false);
        followLpcn = visible && editorSent ? Number(result.lpcn) || 0 : 0;
        followRevision = Number(result.revision) || 0;
        Qt.callLater(sheet.follow);
    }
    function saveErrorText(error) {
        var reason = error === "no-system" ? qsTr("this system is no longer in your saved systems.")
            : error === "full" ? qsTr("this system already keeps %1 channels of yours.").arg(channels.maxEntries)
            : qsTr("dsd-neo could not save this system.");
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
    function channelIndexOf(lpcn) {
        for (var i = 0; i < allChannels.length; ++i) {
            if (Number(allChannels[i].lpcn) === lpcn)
                return i;
        }
        return -1;
    }
    function follow() {
        if (followLpcn <= 0 || !visible)
            return;
        // Another system's carrier lists no rows to follow, nor does a session
        // not decoding DMR.
        if (noTier3Here) {
            followLpcn = 0;
            return;
        }
        // Only an answer the sending editor was open for is followed, and that
        // editor closed with it: an editor open now was opened since, and
        // keeps the focus.
        if (editSheet.visible) {
            followLpcn = 0;
            return;
        }
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
        // Listed on the page after the ones shown: show that page, then focus
        // its row once built. Further on, the focus waits on the Channels
        // heading: showing every page before it would build thousands of
        // rows at once on a large channel map.
        var at = channelIndexOf(followLpcn);
        if (at >= 0) {
            if (at >= channelLimit + channelPageSize) {
                followLpcn = 0;
                channelsLabel.forceActiveFocus();
            } else if (at >= channelLimit) {
                channelLimit += channelPageSize;
                Qt.callLater(sheet.follow);
            }
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
        id: readOnlyNote
        objectName: "channelsReadOnlyNote"
        width: parent.width
        visible: text.length > 0
        text: sheet.lockNote
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
    // An edit sent a while ago and still unanswered, with the editor closed.
    ReadableText {
        id: waitingLine
        objectName: "channelsWaiting"
        width: parent.width
        visible: sheet.requestSlow && sheet.request > 0 && !editSheet.visible
        text: sheet.waitingText
        wrapMode: Text.Wrap
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(13)
        color: Theme.textSecondary
    }
    // Nothing more is changed until the save owed is made.
    ReadableText {
        objectName: "channelsSaveFirst"
        width: parent.width
        visible: sheet.saveOwedHere && sheet.lockNote.length === 0
        text: sheet.saveFirstText
        wrapMode: Text.Wrap
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(13)
        color: Theme.textSecondary
    }
    OutlineButton {
        id: retryButton
        objectName: "channelsRetrySave"
        width: parent.width
        visible: sheet.owedSaveCount > 0
        text: qsTr("Try saving again")
        onClicked: sheet.retrySaves()
    }

    // Another system's carrier, or a session not decoding DMR, has no Tier
    // III channels to list.
    MicroLabel {
        objectName: "channelsMissingLabel"
        topPadding: 6
        visible: !sheet.noTier3Here
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
        visible: missingRows.live === 0 && !sheet.noTier3Here
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
    // with fewer channels raises no hide. A page at a time, as the Channels
    // list below: the rows held are bounded by the pages shown.
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
                entries: sheet.missingPage
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
    // The next page of channels with no frequency (channelPageSize at a time).
    OutlineButton {
        id: moreMissingButton
        objectName: "channelsMissingShowMore"
        width: parent.width
        visible: sheet.channels.missingCount > sheet.missingLimit
        text: qsTr("Show more channels without a frequency")
        onClicked: sheet.showMoreMissing()
    }

    // Where the focus waits after an edit to a channel listed past the next
    // page (follow()).
    MicroLabel {
        id: channelsLabel
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
                entries: sheet.channelPage
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
    // The next page of channels (channelPageSize at a time).
    OutlineButton {
        id: moreButton
        objectName: "channelsShowMore"
        width: parent.width
        visible: sheet.allChannels.length > sheet.channelLimit
        text: qsTr("Show more channels")
        onClicked: sheet.showMoreChannels()
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
        sentPending: sent > 0 && sent === sheet.request
        lockedText: sheet.lockText
        waiting: sheet.requestSlow && sent > 0 && sent === sheet.request
        waitingText: sheet.waitingText
        submit: function (lpcn, op, hz, expected, generation, session) {
            return sheet.submit(lpcn, op, hz, expected, generation, session);
        }
    }
}
