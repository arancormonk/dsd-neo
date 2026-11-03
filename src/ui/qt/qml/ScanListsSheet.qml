// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// The running scan's lists (issue #697), opened from Session options while the
// session scans more than one: one row per list, switched on or off for this
// session only. A live-apply sheet, so an outside tap closes it.
//
// Every row is MetricsModel's ScanListToggleModel. A switch is sent at once
// and the row waits for the decoder's answer; a refused one goes back and says
// why under the list until the lists change. The model decides all of it, by
// the same rule as the terminal: this sheet only shows it. While the scan is
// paused nothing can be switched, and the rows say how to get back.
ModalSheet {
    id: sheet
    objectName: "scanListsSheet"
    panelObjectName: "scanListsSheetPanel"
    accessibleName: qsTr("Scan lists")

    // The name the session gives list @a number, else the decoder's (the file's).
    property var listName: function (number, fallback) {
        return fallback;
    }
    readonly property var lists: metrics.scanListModel
    readonly property bool sessionRunning: decoderHost.running
    onSessionRunningChanged: {
        if (!sessionRunning)
            visible = false;
    }
    // A session of one list has nothing to switch: the menu item is gone too.
    readonly property int count: lists ? lists.count : 0
    onCountChanged: {
        if (count < 2)
            visible = false;
    }
    readonly property bool paused: !!lists && lists.paused

    // A refusal is said aloud at once, as the failure card says a failure: a
    // screen reader user switched the list and would otherwise not learn it
    // went back (issue #697). The last one said, and how many, for tests.
    property string announced: ""
    property int announcements: 0
    function announceRefusal(name, reason) {
        if (!sheet.visible || reason.length === 0)
            return;
        sheet.announced = qsTr("%1: %2").arg(name).arg(reason);
        sheet.announcements++;
        if (typeof Accessible.announce === "function")
            Accessible.announce(sheet.announced);
    }

    function countText(n) {
        return n === 1 ? qsTr("1 target") : qsTr("%1 targets").arg(n);
    }

    ReadableText {
        objectName: "scanListsTitle"
        heading: true
        width: parent.width
        text: qsTr("Scan lists")
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        color: Theme.textPrimary
        font.pixelSize: Theme.fontSize(20)
    }
    // What is on now, or, paused, the one thing to do first.
    ReadableText {
        objectName: "scanListsStatus"
        width: parent.width
        text: sheet.paused ? qsTr("Resume the scan to change lists.") : !sheet.lists ? "" : qsTr("%1 of %2 on · %3").arg(sheet.lists.listsOn).arg(sheet.count).arg(sheet.countText(sheet.lists.targetsOn))
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        color: Theme.textSecondary
        font.pixelSize: Theme.fontSize(13)
    }

    Column {
        objectName: "scanListsRows"
        width: parent.width

        Repeater {
            // Only while open, so a closed sheet builds no rows (issue #782).
            model: sheet.visible ? sheet.lists : null
            ToggleRow {
                required property int index
                required property int number
                required property string name
                required property bool on
                required property int targets
                required property bool onAir
                required property bool pending
                required property string reason
                objectName: "scanListsRow_" + number
                title: sheet.listName(number, name)
                // Size and whether the target on air is one of its, then what
                // the switch is doing; why one was not applied goes under it.
                subtitle: {
                    var about = sheet.countText(targets) + (onAir ? " · " + qsTr("On air") : "");
                    return pending ? about + "\n" + (on ? qsTr("Switching on…") : qsTr("Switching off…")) : about;
                }
                note: reason
                onReasonChanged: sheet.announceRefusal(title, reason)
                checked: on
                enabled: !!sheet.lists && sheet.lists.switchable && !pending
                showDivider: index < sheet.count - 1
                onToggled: function (checked) {
                    sheet.lists.setEnabled(number, checked);
                }
            }
        }
    }

    OutlineButton {
        objectName: "scanListsDone"
        width: parent.width
        text: qsTr("Done")
        onClicked: sheet.visible = false
    }
}
