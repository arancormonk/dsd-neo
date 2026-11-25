// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

Item {
    id: screen

    property string systemName: ""
    property string saveMessage: ""
    property var bulkRows: []
    property string bulkContext: "0"
    property int bulkGeneration: 0
    property bool bulkListen: true
    property string bulkMessage: ""
    // The engine's answer and this screen's own bulk status share one line,
    // the newer of the two on it. A bulk status is newer until the engine
    // next answers, the same text answered again included (its count moves);
    // an answer that expires leaves the line empty rather than bringing back
    // the status it replaced.
    readonly property string engineMessage: metrics.uiMessage
    readonly property var engineMessageSeq: metrics.uiMessageSeq
    property bool bulkNewer: false
    onEngineMessageChanged: {
        if (engineMessage.length > 0)
            bulkNewer = false;
    }
    onEngineMessageSeqChanged: {
        if (metrics.uiMessage.length > 0)
            bulkNewer = false;
    }
    readonly property string commandMessage: bulkNewer ? bulkMessage : engineMessage
    // An accepted request the engine never answers (decoding stopped) leaves
    // after the life of an engine answer. It stays the newer message, so the
    // line goes empty rather than back to an older answer.
    Timer {
        id: bulkRequestedExpiry
        interval: 4000
        onTriggered: screen.bulkMessage = ""
    }
    function requestBulk(listen) {
        bulkRows = talkgroupView.snapshotSelection();
        bulkContext = talkgroups.policyContext;
        bulkGeneration = talkgroups.policyGeneration;
        bulkListen = listen;
        if (bulkRows.length)
            bulkConfirm.visible = true;
    }
    ModalSheet {
        id: bulkConfirm
        objectName: "talkgroupBulkConfirm"
        accessibleName: qsTr("Change matching talkgroups")
        ReadableText {
            width: parent.width
            text: (screen.bulkListen ? qsTr("Listen to %1 matching rows?") : qsTr("Stop tuning %1 matching rows?")).arg(screen.bulkRows.length) + qsTr(" Both the search and category filters apply.")
            wrapMode: Text.Wrap
            color: Theme.textPrimary
            font.pixelSize: Theme.fontSize(16)
        }
        OutlineButton {
            objectName: "confirmTalkgroupBulk"
            width: parent.width
            text: qsTr("Apply to matching rows")
            onClicked: {
                var accepted = commands.setTalkgroupSelection(screen.bulkListen, screen.bulkContext, screen.bulkGeneration, screen.bulkRows);
                screen.bulkMessage = accepted ? qsTr("Update requested") : qsTr("The update could not be queued. Try again.");
                screen.bulkNewer = true;
                if (accepted)
                    bulkRequestedExpiry.restart();
                else
                    bulkRequestedExpiry.stop();
                bulkConfirm.visible = false;
            }
        }
        OutlineButton {
            width: parent.width
            text: qsTr("Cancel")
            onClicked: bulkConfirm.visible = false
        }
    }

    property bool canSaveList: false
    property string saveListText: qsTr("Save talkgroup list")
    // The decoder refuses an export while a scan row with its own talkgroup list is on air: every Save talkgroup list
    // button on this screen honours this.
    readonly property bool scanRowRefusesSave: talkgroups.persistenceScope === "scan"
    // Where Save talkgroup list is offered: the edit sheet and the footer (issue #903), by one rule.
    readonly property bool saveOffered: canSaveList && !scanRowRefusesSave && decoderHost.running

    // Issue #903: the system whose remembered talkgroups Clear asks about, captured when it asks, and its name.
    property string clearOwner: ""
    property string clearOwnerName: ""
    // A Clear History refused (it could not save its log): said in the confirmation, which stays open.
    property string clearError: ""
    function requestClearRemembered() {
        clearOwner = talkgroups.viewedOwner;
        var system = typeof savedSystems !== "undefined" ? savedSystems.getByUid(clearOwner) : ({});
        clearOwnerName = system && system.name ? system.name : screen.systemName;
        clearError = "";
        clearConfirm.visible = true;
    }
    ModalSheet {
        id: clearConfirm
        objectName: "clearRememberedConfirm"
        accessibleName: qsTr("Forget remembered talkgroups")
        // What the confirmation last read out, and how many (Navigation.announce()).
        property string announced: ""
        property int announcements: 0
        ReadableText {
            objectName: "clearRememberedQuestion"
            width: parent.width
            text: qsTr("Forget the talkgroups remembered for %1?").arg(screen.clearOwnerName)
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            color: Theme.textPrimary
            font.pixelSize: Theme.fontSize(16)
        }
        ReadableText {
            width: parent.width
            text: qsTr("Talkgroups heard after this are remembered again. A saved talkgroup list is not changed.")
            wrapMode: Text.Wrap
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSize(13)
        }
        ReadableText {
            objectName: "clearRememberedError"
            width: parent.width
            visible: text.length > 0
            text: screen.clearError
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            font.pixelSize: Theme.fontSize(13)
            color: Theme.alert
            Accessible.role: Accessible.AlertMessage
        }
        OutlineButton {
            id: confirmClear
            objectName: "confirmClearRemembered"
            width: parent.width
            height: Math.max(48, implicitHeight)
            text: qsTr("Forget remembered talkgroups")
            onClicked: {
                if (!talkgroups.clearRememberedOf(screen.clearOwner)) {
                    // Refused: nothing was cleared. The confirmation stays, and says why; tapping again retries.
                    screen.clearError = qsTr("Call history could not be saved, so these talkgroups are still remembered.");
                    Navigation.announce(clearConfirm, confirmClear, screen.clearError);
                    return;
                }
                clearConfirm.visible = false;
            }
        }
        OutlineButton {
            objectName: "cancelClearRemembered"
            width: parent.width
            height: Math.max(48, implicitHeight)
            text: qsTr("Cancel")
            onClicked: clearConfirm.visible = false
        }
    }
    signal saveListRequested(string context, int generation)
    signal closed
    // Shown, fade included (issue #782). Hidden, the cards and category chips
    // let their models go: a scan brings each target's own table, and every
    // card or chip rebuilt under the hidden screen sends accessibility events.
    // The filters live in talkgroupView and stay set; reopening lists the
    // table as it is then, with the reader back on the card they had at the
    // top (gridPlace), or from the top when the table was replaced meanwhile.
    readonly property bool live: screen.visible
    onVisibleChanged: {
        if (!visible) {
            editSheet.visible = false;
            clearConfirm.visible = false;
        }
    }
    readonly property bool hostRunning: decoderHost.running
    onHostRunningChanged: {
        if (!hostRunning)
            editSheet.visible = false;
    }
    // Ticks the remembered cards' "Last heard" ages once a minute while shown (as MonitorScreen's ageTick).
    property int ageTick: 0
    Timer {
        interval: 60000
        running: screen.visible
        repeat: true
        onTriggered: screen.ageTick++
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.bg
    }

    Item {
        id: header

        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: Theme.screenPadding
        height: 46

        IconButton {
            id: back
            icon: "back"
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            onClicked: screen.closed()
        }

        ReadableText {
            heading: true
            anchors.left: back.right
            anchors.right: parent.right
            anchors.leftMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            text: screen.systemName.length > 0 ? screen.systemName : qsTr("Talk groups")
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(22)
            font.weight: Font.Bold
            font.letterSpacing: -0.22
            color: Theme.textPrimary
            elide: Text.ElideRight
        }
    }

    MicroLabel {
        id: countLabel

        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.topMargin: 6
        anchors.leftMargin: Theme.screenPadding
        anchors.rightMargin: Theme.screenPadding
        text: qsTr("Talk groups · %1").arg(talkgroups.count) + (talkgroups.notTunedCount > 0 ? qsTr(" · %1 not tuned").arg(talkgroups.notTunedCount) : "")
        elide: Text.ElideRight
    }

    PlexTextField {
        id: search
        objectName: "talkgroupSearch"

        anchors.top: countLabel.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.topMargin: Theme.gap
        anchors.leftMargin: Theme.screenPadding
        anchors.rightMargin: Theme.screenPadding
        placeholderText: qsTr("Search talkgroups")
        onTextChanged: talkgroupView.filterText = text
    }

    ListView {
        id: chips
        objectName: "talkgroupChips"

        anchors.top: search.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.topMargin: Theme.gap
        anchors.leftMargin: Theme.screenPadding
        anchors.rightMargin: Theme.screenPadding
        orientation: ListView.Horizontal
        spacing: 8
        clip: true
        implicitHeight: Math.max(48, Theme.fontSize(13) + 24)
        visible: talkgroups.categories.length > 0
        model: screen.live ? [qsTr("All")].concat(talkgroups.categories) : []

        // The pill sits in a plain item (issue #800, F1): the view writes
        // Accessible.ignored on each delegate's root as it lays it out, and a
        // pill that was the root came back into the tree under a sheet.
        delegate: Item {
            id: chip
            required property int index
            required property string modelData
            readonly property alias pill: chipPill
            // The wrapper gates itself too (a cover with no re-layout leaves
            // nothing of it; re-laid, it is a bare node offering nothing).
            Accessible.ignored: !Navigation.allows(chip) || !visible

            width: chipPill.width
            height: chipPill.height

            FilterPill {
                id: chipPill
                caret: false
                text: chip.modelData
                active: chip.index === 0 ? talkgroupView.filterTag === "" : talkgroupView.filterTag === chip.modelData
                onClicked: talkgroupView.filterTag = chip.index === 0 ? "" : chip.modelData
            }
        }
    }

    Row {
        id: bulkActions

        anchors.top: chips.visible ? chips.bottom : search.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.topMargin: Theme.gap
        anchors.leftMargin: Theme.screenPadding
        anchors.rightMargin: Theme.screenPadding
        spacing: 10

        OutlineButton {
            id: noTuneAll
            objectName: "noTuneAllButton"
            width: (parent.width - 10) / 2
            text: qsTr("Do not tune matching")
            enabled: decoderHost.running && talkgroupView.count > 0
            onClicked: screen.requestBulk(false)

            // Category names can be much wider than half a phone screen. Keep
            // the shared button styling while bounding its single-line label.
            TextMetrics {
                id: noTuneLabel
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(15)
                font.weight: Font.DemiBold
                text: talkgroupView.filterTag.length > 0 ? qsTr("Do not tune all · %1").arg(talkgroupView.filterTag) : qsTr("Do not tune all")
                elide: Text.ElideRight
                elideWidth: noTuneAll.width - 16
            }
        }

        GradientButton {
            id: listenAll
            objectName: "listenAllButton"
            width: (parent.width - 10) / 2
            text: qsTr("Listen to matching")
            enabled: decoderHost.running && talkgroupView.count > 0
            onClicked: screen.requestBulk(true)

            TextMetrics {
                id: listenLabel
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(15)
                font.weight: Font.Bold
                text: talkgroupView.filterTag.length > 0 ? qsTr("Listen all · %1").arg(talkgroupView.filterTag) : qsTr("Listen all")
                elide: Text.ElideRight
                elideWidth: listenAll.width - 16
            }
        }
    }

    FontMetrics {
        id: cardIdMetrics
        font.family: Theme.mono
        font.pixelSize: Theme.fontSize(15)
    }
    FontMetrics {
        id: cardNameMetrics
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(12)
    }
    FontMetrics {
        id: cardTagMetrics
        font.family: Theme.mono
        font.pixelSize: Theme.fontSize(11)
    }
    FontMetrics {
        id: cardStatusMetrics
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(11)
    }

    ListView {
        id: grid
        objectName: "talkgroupGrid"

        anchors.top: bulkActions.bottom
        anchors.bottom: footer.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.topMargin: Theme.gap
        anchors.bottomMargin: Theme.gap
        anchors.leftMargin: Theme.screenPadding
        anchors.rightMargin: Theme.screenPadding
        clip: true
        // Set by gridPlace below, only while the grid is visible (issue #782).
        model: null
        spacing: 10
        enabled: decoderHost.running

        delegate: TalkgroupCard {
            required property var idStart
            required property var idEnd
            required property bool record
            required property int tableIndex
            required property string pattern

            width: ListView.view.width
            height: implicitHeight
            ageTick: screen.ageTick
            // Snapshot truth, not a local toggle: the decoder may refuse an edit.
            // The row token (table index, pattern) names this row, so a duplicate or a pattern row is the one edited.
            onClicked: commands.setTalkgroupListening(idStart, idEnd, !listening, talkgroups.policyContext, talkgroups.policyGeneration, tableIndex, pattern)
            // The sheet keeps the system the card is remembered on (issue #903): Forget forgets it there.
            onEditRequested: editSheet.openRow(idStart, idEnd, name, listening, listed, priority, preempt, talkgroups.policyContext, talkgroups.policyGeneration, record, tableIndex, pattern, remembered, talkgroups.viewedOwner)
        }

        ReadableText {
            anchors.centerIn: parent
            width: parent.width - 24
            visible: talkgroups.count === 0
            text: qsTr("No talkgroups yet. Calls heard on this system will appear here.")
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(14)
            color: Theme.textSubdued
            wrapMode: Text.Wrap
            horizontalAlignment: Text.AlignHCenter
        }
    }

    // Hands the grid its model only while it is shown and keeps the reader on
    // the card they had at the top (issue #782; see `live` above). A table
    // replaced while hidden resets the model, and the grid starts from the
    // top, as it does on a reset while shown.
    KeepPlace {
        id: gridPlace
        list: grid
        model: talkgroupView
        // A card is its bounds, pattern and table index, the token its edits
        // carry: an exact row and a range can share a start, every pattern row
        // has the bounds 0-0, and a table can list the same bounds twice.
        keyOf: function (card) {
            return {start: card.idStart, end: card.idEnd, pattern: card.pattern, table: card.tableIndex};
        }
        rowOf: function (key) {
            return talkgroupView.rowForCard(key.start, key.end, key.pattern, key.table);
        }
    }
    // A search or category set while the grid is hidden is a new question:
    // the grid shows its answer from the top, as History does.
    Connections {
        target: talkgroupView

        function onFilterChanged() {
            gridPlace.forget();
        }
    }

    Column {
        id: footer

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: Theme.screenPadding
        spacing: 8

        ReadableText {
            objectName: "talkgroupScopeNote"
            width: parent.width
            // Issue #903: in session scope with remembering on, the session note below says it all.
            visible: !(talkgroups.remembering && !talkgroups.persistent && talkgroups.persistenceScope !== "scan")
            // In scan scope edits are not written, but a learned radio alias still goes to the target's own list.
            text: talkgroups.persistenceScope === "scan" ? qsTr("Edits last for this session; learned names may still be added to this target's list") : talkgroups.persistent ? qsTr("Edits are saved to the attached CSV and affect other systems sharing it") : qsTr("This session · save a talkgroup list to keep edits")
            wrapMode: Text.Wrap
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSize(12)
        }
        ReadableText {
            width: parent.width
            text: screen.saveMessage
            visible: text.length > 0
            color: Theme.textSecondary
            wrapMode: Text.Wrap
        }

        // The one command line (issue #901 B3) keeps the look of the line it
        // replaced, the app's 13 px text in the primary colour, so it grows
        // with the text size like the rest of the screen.
        ReadableText {
            objectName: "talkgroupCommandToast"
            width: parent.width
            text: screen.commandMessage
            visible: text.length > 0
            color: Theme.textPrimary
            font.pixelSize: Theme.fontSize(13)
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
        }

        ReadableText {
            objectName: "sessionOnlyNote"
            width: parent.width
            visible: !talkgroups.persistent
            // Issue #903: a system that remembers what it hears keeps its cards; only names and settings need a list.
            text: talkgroups.remembering ? qsTr("Talkgroups heard on this system are remembered. Save a talkgroup list to keep names and listen settings.") : qsTr("Changes last for this session. Give the system a talkgroup list to keep them.")
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(12)
            color: Theme.textSubdued
            wrapMode: Text.Wrap
        }

        ReadableText {
            width: parent.width
            visible: talkgroups.allowListMode
            text: qsTr("Allow list on: talkgroups not on this list are never tuned.")
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(12)
            color: Theme.textSubdued
            wrapMode: Text.Wrap
        }

        // Issue #903: the list's own actions, side by side as the bulk actions
        // are, so the cards keep their room on a phone; either alone takes the row.
        // With a screen reader on, a scan hop changes neither (#860: the layout
        // holds still): both stay while the decoder runs, each disabled where it
        // does not apply.
        Row {
            id: listActions

            width: parent.width
            spacing: 10
            readonly property bool steady: typeof screenReader !== "undefined" && screenReader !== null && screenReader.active
            // Read from the conditions, not the buttons' visible (false while this row is hidden).
            readonly property bool showSave: steady ? decoderHost.running : screen.saveOffered
            // The system on air has something remembered, remembering or not (what it remembered before stays).
            readonly property bool showClear: steady || talkgroups.hasRemembered
            readonly property bool both: showSave && showClear
            readonly property real cellHeight: Math.max(showSave ? footerSave.implicitHeight : 0, showClear ? clearRemembered.implicitHeight : 0)
            visible: showSave || showClear

            // The edit sheet's Save talkgroup list, for the list on show.
            OutlineButton {
                id: footerSave
                objectName: "footerSaveTalkgroupList"
                width: listActions.both ? (parent.width - 10) / 2 : parent.width
                height: listActions.cellHeight
                visible: listActions.showSave
                enabled: screen.saveOffered
                wrapMode: Text.WordWrap
                text: screen.saveListText
                onClicked: screen.saveListRequested(talkgroups.policyContext, talkgroups.policyGeneration)
            }
            OutlineButton {
                id: clearRemembered
                objectName: "clearRememberedTalkgroups"
                width: listActions.both ? (parent.width - 10) / 2 : parent.width
                height: listActions.cellHeight
                visible: listActions.showClear
                enabled: talkgroups.hasRemembered
                wrapMode: Text.WordWrap
                text: qsTr("Clear remembered talkgroups…")
                onClicked: screen.requestClearRemembered()
            }
        }
    }

    TalkgroupEditSheet {
        id: editSheet
        objectName: "talkgroupEditSheet"
        saveListText: screen.saveListText
        saveMessage: screen.saveMessage
        canSaveList: screen.saveOffered
        onSaveListRequested: function (context, generation) {
            screen.saveListRequested(context, generation);
        }
    }
}
