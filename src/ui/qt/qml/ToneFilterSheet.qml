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

    function openEditor() {
        mode = metrics.toneFilterConfiguredMode;
        listField.text = metrics.toneFilterConfiguredList;
        localMessage = "";
        open();
    }
    function apply() {
        if (listError.length > 0)
            return;
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
    // An edit sets the configured policy; a scan row with its own keeps it while on air.
    Text {
        objectName: "toneFilterRowNote"
        width: parent.width
        visible: metrics.toneFilterRowOverride
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
