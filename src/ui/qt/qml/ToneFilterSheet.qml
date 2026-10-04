// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// The live tone-filter editor (#527), opened from the monitor's Tone filter row: the
// configured CTCSS/DCS receive policy, a mode and its list, sent to the decoder as one
// edit. It opens on the configured policy, never on a scan row's own, and the list is
// checked as it is typed with the decoder's own parser, so the message beside the field
// is the one the decoder would refuse with and a list it would refuse is never sent.
// Off keeps the list in the field, and so in the edit: turning the filter off does not
// throw the list away.
ModalSheet {
    id: sheet
    objectName: "toneFilterSheet"
    accessibleName: qsTr("Tone filter")

    property var bridge: commands
    // 0 off, 1 allow, 2 block (dsd_tone_filter_mode), as the segments are ordered.
    property int mode: 0
    property string localMessage: ""
    readonly property string listError: sheet.bridge.toneFilterError(sheet.mode, listField.text)
    // Issue #518: while a scan row on air takes a "this channel" tone edit, the
    // editor can set that row's own policy for the rest of the session instead
    // of the configured one. The row is captured when the scope is chosen.
    readonly property bool rowTone: metrics.scanRowActive === true
        && (metrics.scanRowEditable & sheet.bridge.scanRowFieldTone) !== 0
    property bool rowScopeChosen: false
    property var rowContext: ({})
    // The row chosen stays the destination until the scope is changed, even after
    // the scan moves on: the edit then waits for that row's next visit, and the
    // decoder refuses it once that scan has ended. Applying never falls back to
    // the configured policy behind the operator's back.
    readonly property bool rowScope: rowScopeChosen && rowContext.active === true

    function openEditor() {
        mode = metrics.toneFilterConfiguredMode;
        listField.text = metrics.toneFilterConfiguredList;
        localMessage = "";
        chooseScope(false);
        open();
    }
    /** Edit the configured policy (false) or "this channel" (true), capturing the row on air for the latter. */
    function chooseScope(thisChannel) {
        rowContext = thisChannel ? sheet.bridge.scanRowContext() : ({});
        rowScopeChosen = thisChannel && rowContext.active === true;
        // The editor opens on what it now edits: the row's policy, or the configured one.
        if (rowScopeChosen) {
            mode = rowContext.toneMode;
            listField.text = rowContext.toneList;
        } else {
            mode = metrics.toneFilterConfiguredMode;
            listField.text = metrics.toneFilterConfiguredList;
        }
    }
    function rowEdit(action) {
        if (sheet.bridge.editScanRow(rowContext, sheet.bridge.scanRowFieldTone, action,
                                     {"toneMode": mode, "toneList": listField.text})) {
            visible = false;
            Qt.inputMethod.hide();
        } else {
            localMessage = qsTr("Not sent: the decoder is not running.");
        }
    }
    function apply() {
        if (listError.length > 0)
            return;
        if (rowScope) {
            rowEdit(sheet.bridge.scanRowEditSet);
            return;
        }
        if (bridge.setToneFilter(mode, listField.text)) {
            visible = false;
            Qt.inputMethod.hide();
        } else {
            localMessage = qsTr("Not sent: the decoder is not running.");
        }
    }

    Text {
        width: parent.width
        text: qsTr("Tone filter")
        wrapMode: Text.Wrap
        color: Theme.textPrimary
        font.pixelSize: Theme.fontSize(20)
    }
    Text {
        width: parent.width
        text: qsTr("Allow hears only traffic carrying a listed CTCSS tone or DCS code; block mutes it and hears the rest.")
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        color: Theme.textSecondary
        font.pixelSize: Theme.fontSize(13)
    }
    // What Apply edits while a scan row on air takes "this channel" edits.
    SegmentedControl {
        objectName: "toneFilterScope"
        visible: sheet.rowTone || sheet.rowScope
        width: parent.width
        model: [qsTr("All channels"), qsTr("This channel")]
        currentIndex: sheet.rowScope ? 1 : 0
        onSelected: function (index) {
            sheet.chooseScope(index === 1);
        }
    }
    Text {
        objectName: "toneFilterScopeNote"
        width: parent.width
        visible: sheet.rowScope
        text: qsTr("Applies to %1 for this session, not to the list or the default.").arg(sheet.rowContext.label || "")
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        color: Theme.textSecondary
        font.pixelSize: Theme.fontSize(13)
    }
    // An edit sets the configured policy; a scan row with its own keeps it while on air.
    Text {
        objectName: "toneFilterRowNote"
        width: parent.width
        visible: metrics.toneFilterRowOverride && !sheet.rowScope
        text: qsTr("This channel sets its own tone filter. Changes here apply to the default, from the next channel without one.")
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        color: Theme.textSecondary
        font.pixelSize: Theme.fontSize(13)
    }
    SegmentedControl {
        objectName: "toneFilterMode"
        width: parent.width
        model: [qsTr("Off"), qsTr("Allow"), qsTr("Block")]
        currentIndex: sheet.mode
        onSelected: function (index) {
            sheet.mode = index;
        }
    }
    PlexTextField {
        id: listField
        objectName: "toneFilterList"
        width: parent.width
        mono: true
        label: qsTr("Tones and codes")
        hint: qsTr("Separate with /, e.g. 67.0/100.0/D023N")
        error: sheet.listError
        inputMethodHints: Qt.ImhNoPredictiveText
        onAccepted: sheet.apply()
    }
    // "This channel": follow the default, offered when the list sets a policy,
    // or go back to the list's, offered while the row runs an edit.
    Row {
        objectName: "toneFilterRowActions"
        // What the list sets and the row runs is known for the row on air only.
        visible: sheet.rowScope && metrics.scanRowKey === sheet.rowContext.key
            && ((metrics.scanRowListed | metrics.scanRowEdited) & sheet.bridge.scanRowFieldTone) !== 0
        spacing: 8
        OutlineButton {
            objectName: "toneFilterRowInherit"
            visible: (metrics.scanRowListed & sheet.bridge.scanRowFieldTone) !== 0
            text: qsTr("Use default")
            accessibleName: qsTr("This Channel Uses the Default Tone Filter")
            onClicked: sheet.rowEdit(sheet.bridge.scanRowEditInherit)
        }
        OutlineButton {
            objectName: "toneFilterRowReset"
            visible: (metrics.scanRowEdited & sheet.bridge.scanRowFieldTone) !== 0
            text: qsTr("List value")
            accessibleName: qsTr("This Channel Back to Its List Tone Filter")
            onClicked: sheet.rowEdit(sheet.bridge.scanRowEditReset)
        }
    }
    GradientButton {
        objectName: "toneFilterApply"
        width: parent.width
        text: qsTr("Apply")
        enabled: sheet.listError.length === 0
        onClicked: sheet.apply()
    }
    Text {
        width: parent.width
        visible: text.length > 0
        text: sheet.localMessage
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        color: Theme.textSecondary
    }
    OutlineButton {
        objectName: "toneFilterClose"
        width: parent.width
        height: Math.max(48, implicitHeight)
        text: qsTr("Close")
        onClicked: {
            sheet.visible = false;
            Qt.inputMethod.hide();
        }
    }
}
