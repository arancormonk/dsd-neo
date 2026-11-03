// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// The channels or trunk-scan targets avoided this session (issue #672), opened
// from the monitor's SCAN AVOIDS row: each with Put back, which returns that
// one entry to the rotation and tunes nothing, and Put all back (the row's
// Clear). A live-apply sheet, so an outside tap closes it.
//
// Every row follows the decoder: a put back is sent by the entry's key, never
// its position, and the row leaves when the next snapshot no longer lists it,
// never sooner. The list is MetricsModel's ScanAvoidListModel, which turns one
// list into the next by key, shown in a ListView of bounded height that makes
// only the rows in view; the sheet hands it the model only while it is open,
// so a closed sheet builds no rows (issue #782).
ModalSheet {
    id: sheet
    objectName: "scanAvoidSheet"
    panelObjectName: "scanAvoidSheetPanel"
    accessibleName: qsTr("Avoided channels")

    readonly property bool sessionRunning: decoderHost.running
    onSessionRunningChanged: {
        if (!sessionRunning)
            visible = false;
    }
    // Nothing left to list or clear (the scan stopped with no avoids, or the
    // session ended): the monitor's row is gone as well.
    readonly property bool manageable: metrics.scanAvoidsManageable === true
    onManageableChanged: {
        if (!manageable)
            visible = false;
    }
    // A list the decoder could not copy whole has no entries to put back
    // until it can; Put all back still works.
    readonly property bool available: metrics.scanAvoidsAvailable === true
    readonly property int count: available ? metrics.scanAvoidCount : 0

    ReadableText {
        objectName: "scanAvoidTitle"
        heading: true
        width: parent.width
        // The count while there is one to give; "No avoided channels." says 0.
        text: sheet.count > 0 ? qsTr("Avoided channels (%1)").arg(sheet.count) : qsTr("Avoided channels")
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        color: Theme.textPrimary
        font.pixelSize: Theme.fontSize(20)
    }
    // What a put back does, before the first is pressed: it does not tune,
    // though it can let a scan that owes a departure move on (issue #697): one
    // parked on an avoided channel, or on a channel whose list was switched off.
    ReadableText {
        objectName: "scanAvoidHint"
        width: parent.width
        visible: sheet.count > 0
        text: qsTr("Put back returns a channel to the rotation without retuning. If the scan is waiting to leave a channel, it may move on at once.")
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        color: Theme.textSecondary
        font.pixelSize: Theme.fontSize(13)
    }
    ReadableText {
        objectName: "scanAvoidStatus"
        width: parent.width
        visible: sheet.count === 0
        text: sheet.available ? qsTr("No avoided channels.") : qsTr("List not ready yet. Put all back still works.")
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        color: Theme.textSecondary
        font.pixelSize: Theme.fontSize(14)
    }

    ListView {
        id: list
        objectName: "scanAvoidList"
        width: parent.width
        // At most a little over half the panel, so Put all back and Done stay
        // in view under a long list; the rows scroll inside it.
        readonly property real maximumHeight: Math.max(Theme.minimumTouchSize * 2, Math.floor(sheet.maximumHeight * 0.55))
        height: Math.min(contentHeight, maximumHeight)
        visible: sheet.count > 0
        clip: true
        interactive: contentHeight > height
        boundsBehavior: Flickable.StopAtBounds
        // Wider than the 8 px between a label and its own button, so a
        // stacked row's button reads as its label's, not the next one's.
        spacing: 16
        // No current item: a ListView is a focus scope, and a current item
        // would take the focus a button should have.
        currentIndex: -1
        // Only while open and on top: a ListView's delegates stay in the
        // accessibility tree under a sheet that covers this one, where a
        // swipe would reach them (issue #782). Nothing opens over this sheet
        // today, so the rows are not rebuilt in use.
        model: sheet.visible && sheet.isTopModal ? metrics.scanAvoidModel : null
        // Out of the tree with the sheet when covered or closed (issue #782); gate first, see OutlineButton.
        readonly property bool navigationAllowed: Navigation.allows(list)
        Accessible.ignored: !navigationAllowed || !visible

        delegate: Item {
            id: entryRow
            required property int index
            required property string key
            required property string label
            // No remove transition, so a row whose entry left is destroyed at
            // once: no held row is left for a press or a swipe to reach.
            // The label beside its button while both fit on one line, else
            // the button under it.
            readonly property bool stacked: labelMetrics.advanceWidth + 12 + putBack.implicitWidth > width

            width: ListView.view ? ListView.view.width : 0
            height: stacked ? entryLabel.height + 8 + putBack.height : Math.max(entryLabel.height, putBack.height)

            TextMetrics {
                id: labelMetrics
                font: entryLabel.font
                text: entryRow.label
            }
            // Read in the button's name, which says what it puts back.
            Text {
                id: entryLabel
                Accessible.ignored: true
                y: entryRow.stacked ? 0 : Math.round((entryRow.height - height) / 2)
                width: entryRow.stacked ? entryRow.width : entryRow.width - putBack.width - 12
                text: entryRow.label
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                font.family: Theme.mono
                font.pixelSize: Theme.fontSize(12)
                color: Theme.textPrimary
            }
            OutlineButton {
                id: putBack
                objectName: "scanAvoidPutBack"
                x: entryRow.stacked ? 0 : entryRow.width - width
                y: entryRow.stacked ? entryLabel.height + 8 : Math.round((entryRow.height - height) / 2)
                text: qsTr("Put back")
                accessibleName: qsTr("Put back %1").arg(entryRow.label)
                enabled: sheet.available && sheet.sessionRunning
                onClicked: commands.restoreScanAvoid(entryRow.key)
                // Tabbed to below the fold, the row scrolls into view.
                onActiveFocusChanged: {
                    if (activeFocus)
                        list.positionViewAtIndex(entryRow.index, ListView.Contain);
                }
            }
        }
    }

    OutlineButton {
        objectName: "scanAvoidPutAllBack"
        width: parent.width
        text: qsTr("Put all back")
        accessibleName: qsTr("Put all avoided channels back")
        enabled: sheet.sessionRunning && sheet.manageable && (sheet.count > 0 || !sheet.available)
        onClicked: commands.clearScanAvoids()
    }
    OutlineButton {
        objectName: "scanAvoidDone"
        width: parent.width
        text: qsTr("Done")
        onClicked: sheet.visible = false
    }
}
