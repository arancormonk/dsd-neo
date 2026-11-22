// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import "Util.js" as Util

// One call in a list — monitor's recent panel and the history screen share it.
// Encrypted rows carry the ENC tag while retaining readable text contrast. A
// call with a recording carries a small play glyph before the time or ENC tag
// (issue #708); a row without one is unchanged.
//
// Issue #901: the row's height is its fonts' and the user's density, never
// its call's or its width's: always the two lines, each a fixed line box
// (LineBoxes, D10), so a name in another script does not move the row, and
// every row of a view shares one height, LineBoxes.callRowHeight(), which the
// Monitor's budget reads too. Compact view (D5) halves the padding.
//
// B7: where the row is too narrow for everything at its right, the name keeps
// its place and the extras give way (plan): the name first gets min(its
// natural width, 96); then EMERGENCY, ENC, the time and the recording glyph
// are drawn in that order, each only while it still fits beside that, and the
// first that does not fit ends the run. The name and meta line take what is
// left and elide. What is not drawn is still in the node's accessible text.
Item {
    id: row
    property string accessibleName: name
    property bool interactive: false
    property bool hasRecording: false
    signal activated
    // The keyboard lands on the node, the row's named stop (Tab, a sheet's
    // focus return): a focus given to the row itself is passed on to it.
    onActiveFocusChanged: {
        if (activeFocus)
            rowNode.forceActiveFocus();
    }
    // Issue #800 (F1): the row's accessible node is `node`, a child that
    // fills it, never the row itself. A list view writes Accessible.ignored on
    // the root of every row it lays out, with no change signal
    // (QQuickItemViewFxItem::setVisible): a call landing under a pushed screen
    // re-lays the Monitor's and History's rows, and a row whose own node was
    // the root came back into the tree under that screen, a stop with a press.
    // The view never writes a child's flag, so the node's gate holds: covered,
    // it leaves the tree with its name and its press. The root keeps the
    // keyboard (Tab, keys, the tap) and gates itself too, so a cover with no
    // re-layout leaves nothing of it; re-laid, it is a bare node that offers
    // nothing.
    readonly property bool navigationAllowed: Navigation.allows(row)
    readonly property bool reachable: navigationAllowed && visible
    readonly property Item node: rowNode
    Accessible.ignored: !reachable
    // What the row shows besides its name (issue #782): the meta line, its
    // tags and its time, each once: a tag the meta line already says (History
    // puts EMERGENCY and "encrypted" there) and a time the caller's name
    // already reads are left out. A tag's word comes from the tag itself,
    // drawn or not (issue #901 B7: a narrow row may leave it out). The time
    // is said on an encrypted row too, where ENC takes its place on the row's
    // face (G3).
    readonly property string emergencyWords: emergency ? emergencyTag.label : ""
    function activate() {
        if (interactive && enabled && Navigation.allows(row))
            activated();
    }
    Keys.onPressed: function (event) {
        if (Navigation.activationPress(event, Window.activeFocusItem))
            activate();
    }
    Keys.onReleased: function (event) {
        Navigation.activationRelease(event);
    }
    GatedTapHandler {
        onAllowedTap: row.activate()
    }

    property string name: ""
    property string metaText: ""
    property string rightText: ""
    property bool enc: false
    property bool emergency: false
    property bool showDivider: true
    // Compact view: 6 px above and below the lines instead of 12, floored at
    // a touch target instead of Theme.rowHeight.
    property bool compact: false
    // The widest times the view shows, one per line: the time's slot is that
    // wide whatever the row's own time says, so an age that ticks from "9m"
    // to "10m" never changes what the row draws (plan). "00:00" for HH:mm;
    // the Monitor gives its ages.
    property string timeWidthProbe: "00:00"
    readonly property real timeSlotWidth: LineBoxes.callRowTimeWidth(timeWidthProbe)

    implicitHeight: LineBoxes.callRowHeight(compact)

    // What the right side draws (issue #901 B7), from the room beside the
    // labels and the items' own widths: the name first gets min(its natural
    // width, 96); then EMERGENCY, ENC, the time and the recording glyph, each
    // only while it still fits, the first that does not ending the run, so a
    // lower one is never drawn in place of a higher one. `width` is the drawn
    // right side's, with the gaps the anchors below put between its items.
    // Widths only, never the items' positions, so no binding loops; the time
    // counts as its fixed slot (timeSlotWidth), never its text, so the plan
    // follows the width, the fonts and the call's flags only.
    readonly property var plan: {
        var room = Math.max(0, width - 18 - 12 - 18);
        var least = Math.min(Math.ceil(nameMetrics.advanceWidth), 96);
        var shown = {emergency: false, enc: false, time: false, recording: false, width: 0};
        var parts = [{key: "emergency", on: emergency},
                     {key: "enc", on: enc},
                     {key: "time", on: !enc && rightText.length > 0},
                     {key: "recording", on: hasRecording}];
        for (var part of parts) {
            if (!part.on)
                continue;
            var next = Object.assign({}, shown);
            next[part.key] = true;
            next.width = rightWidth(next);
            if (least + next.width > room)
                break;
            shown = next;
        }
        return shown;
    }
    // The right side's width with @p drawn's items: ENC or the time at its
    // right edge, the glyph 8 px before that, EMERGENCY 8 px before the glyph
    // or 6 before ENC or the time.
    function rightWidth(drawn) {
        var end = drawn.enc ? encTag.implicitWidth : drawn.time ? timeSlotWidth : 0;
        var endDrawn = drawn.enc || drawn.time;
        var glyph = drawn.recording ? recordingGlyph.implicitWidth + (endDrawn ? 8 : 0) : 0;
        var tag = drawn.emergency ? emergencyTag.implicitWidth + (drawn.recording ? 8 : endDrawn ? 6 : 0) : 0;
        return end + glyph + tag;
    }
    TextMetrics {
        id: nameMetrics
        font: LineBoxes.callRowNameFont
        text: Util.oneLine(row.name)
    }

    Item {
        id: rowNode
        // Sized by binding, not anchors: a row the view builds is measured at
        // once (anchored, a list that put its place back found rows 58 high).
        width: row.width
        height: row.height
        Accessible.role: row.interactive ? Accessible.Button : Accessible.StaticText
        Accessible.name: !row.reachable ? "" : row.hasRecording ? qsTr("%1, has a recording").arg(row.accessibleName) : row.accessibleName
        Accessible.description: !row.reachable ? "" : [row.metaText,
            row.emergencyWords.length > 0 && row.metaText.indexOf(row.emergencyWords) < 0 ? row.emergencyWords : "",
            row.enc && row.metaText.indexOf(qsTr("encrypted")) < 0 ? encTag.label : "",
            row.rightText.length > 0 && row.accessibleName.indexOf(row.rightText) < 0 ? row.rightText : ""].filter(function (part) {
            return part.length > 0;
        }).join(", ")
        Accessible.focusable: row.reachable
        Accessible.ignored: !row.reachable
        Accessible.onPressAction: row.activate()
        // The row's Tab stop, held while it has the focus (see OutlineButton);
        // keys reach the row's handlers from here.
        activeFocusOnTab: (row.navigationAllowed && row.interactive && row.enabled && row.visible) || activeFocus
        // The keyboard's and TalkBack's focus both land here: the frame shows it.
        FocusFrame {}

        // The row's text sits inside its node, so the node's words are its
        // own lines' (what a screen reader reaches is checked by that).
        // As wide as the right side leaves, never less than nothing.
        Column {
            id: labels
            x: 18
            width: Math.max(0, parent.width - 18 - 12 - 18 - row.plan.width)
            anchors.verticalCenter: parent.verticalCenter
            spacing: 3

            // Plain text: names and meta lines carry what the air delivered (aliases,
            // SMS bodies), and Qt's automatic rich-text detection would render a tag
            // in them as markup instead of the characters received. One line each:
            // a line break in that text is drawn as a space.
            //
            // Each line is exactly its font's box, its text centred in it. A
            // script that stands taller than the box (Tibetan stacks) is drawn
            // unclipped into the row's padding: the decided limit of D10.
            Text {
                objectName: "callRowName"
                width: parent.width
                height: LineBoxes.callRowNameBox()
                verticalAlignment: Text.AlignVCenter
                text: Util.oneLine(row.name)
                textFormat: Text.PlainText
                font: LineBoxes.callRowNameFont
                color: Theme.textPrimary
                elide: Text.ElideRight
            }

            Text {
                objectName: "callRowMeta"
                width: parent.width
                height: LineBoxes.callRowMetaBox()
                verticalAlignment: Text.AlignVCenter
                text: Util.oneLine(row.metaText)
                textFormat: Text.PlainText
                font: LineBoxes.callRowMetaFont
                color: Theme.textSubdued
                elide: Text.ElideRight
            }
        }

        // The tags and the time the plan draws, laid out from the row's right
        // margin and exactly as wide as they are, centred on the row.
        Item {
            id: right

            x: parent.width - 18 - width
            width: row.plan.width
            height: parent.height

            EmergencyTag {
                id: emergencyTag
                objectName: "rowEmergencyTag"
                visible: row.plan.emergency
                anchors.right: recordingGlyph.visible ? recordingGlyph.left : encTag.visible ? encTag.left : timeText.visible ? timeText.left : parent.right
                anchors.rightMargin: recordingGlyph.visible ? 8 : encTag.visible || timeText.visible ? 6 : 0
                anchors.verticalCenter: parent.verticalCenter
            }

            PlayGlyph {
                id: recordingGlyph
                objectName: "rowRecordingGlyph"
                visible: row.plan.recording
                anchors.right: encTag.visible ? encTag.left : timeText.visible ? timeText.left : parent.right
                anchors.rightMargin: encTag.visible || timeText.visible ? 8 : 0
                anchors.verticalCenter: parent.verticalCenter
            }

            EncTag {
                id: encTag
                objectName: "rowEncTag"
                visible: row.plan.enc
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
            }

            Text {
                id: timeText
                objectName: "callRowTime"
                visible: row.plan.time
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                // Its fixed slot, the time drawn at its right.
                width: row.timeSlotWidth
                horizontalAlignment: Text.AlignRight
                text: row.rightText
                font: LineBoxes.callRowMetaFont
                color: Theme.textSubdued
            }
        }

        Rectangle {
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: 18
            height: 1
            visible: row.showDivider
            color: Theme.divider
        }
    }
}
