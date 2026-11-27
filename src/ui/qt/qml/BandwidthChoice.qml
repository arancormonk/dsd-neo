// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// A DSP bandwidth choice: the eight widths every radio runs (4 to 72 kHz), and
// App default first where the value can follow the app (`inheritable`).
// Settings, the system editor and the scan list editor share it. The caller
// owns `value` (kHz; when `inheritable`, 0 or less is App default) and
// reports a pick (`chosen`, -1 for App default). A stored value that is not
// one of the eight (an older build took any number) is shown as it is, as an
// entry of its own with what it runs at, and nothing is rewritten until
// another width is picked. That entry stays in the list after such a pick,
// so the list never changes under the reader (a new model would reset the
// choice and rebuild every row); `reload()` takes the stored value afresh
// when the caller loads another one. With `meaningShown` a line under the
// choice says what the bandwidth is (issue #899: it was taken for the NFM
// channel width).
Column {
    id: choice

    property string title: qsTr("Bandwidth")
    property bool inheritable: false
    // What App default runs, shown under the choice while it is chosen.
    property string inheritNote: ""
    property int value: 48
    property string comboObjectName: ""
    property string noteObjectName: ""
    // Settings and the system editor: the line saying the bandwidth is the
    // spectrum the decoder processes, and where the NFM channel width is set.
    property bool meaningShown: false
    property string meaningObjectName: ""
    property string accessibleName: title
    readonly property var widths: [4, 6, 8, 12, 16, 24, 48, 72]
    // A stored value that is not one of the widths, offered as its own entry
    // after them (`oddOffered`).
    property bool oddOffered: false
    property int oddKhz: 0
    readonly property bool inherited: inheritable && value <= 0
    readonly property bool odd: !inherited && widths.indexOf(value) < 0
    // Set while this reports its own pick, so the value that comes back from
    // the caller does not reload the list.
    property bool picking: false

    signal chosen(int khz)

    onValueChanged: {
        if (!picking)
            reload();
    }
    Component.onCompleted: reload()

    // Whether @a khz is shown as an entry of its own.
    function isOdd(khz) {
        return !(inheritable && khz <= 0) && widths.indexOf(khz) < 0;
    }
    function reload() {
        oddOffered = isOdd(value);
        oddKhz = oddOffered ? value : 0;
    }
    // The entries in list order: App default (-1), the widths, the stored odd value.
    function entries() {
        var list = inheritable ? [-1] : [];
        list = list.concat(widths);
        if (oddOffered)
            list.push(oddKhz);
        return list;
    }
    function indexFor(khz) {
        var list = entries();
        return inheritable && khz <= 0 ? 0 : list.indexOf(khz);
    }

    spacing: 6

    MicroLabel {
        text: choice.title
    }
    PlexComboBox {
        id: widthCombo
        objectName: choice.comboObjectName
        width: parent.width
        accessibleName: choice.accessibleName
        model: choice.entries().map(function (khz, i) {
            return choice.inheritable && i === 0 ? qsTr("App default") : qsTr("%1 kHz").arg(khz);
        })
        // Read through the model: the combo resets its index when the model
        // changes, and this runs again after that.
        currentIndex: model.length > 0 ? choice.indexFor(choice.value) : -1
        onActivated: function (index) {
            var khz = choice.entries()[index];
            choice.picking = true;
            choice.chosen(khz);
            choice.picking = false;
        }
    }
    ReadableText {
        objectName: choice.noteObjectName
        width: parent.width
        visible: text.length > 0
        text: choice.inherited ? choice.inheritNote : choice.odd ? qsTr("Runs at %1 kHz on an RTL-SDR, %2 kHz on an Airspy.").arg(sessionArgs.effectiveBandwidthKhz("", "usb", choice.value)).arg(sessionArgs.effectiveBandwidthKhz("", "airspy", choice.value)) : ""
        wrapMode: Text.Wrap
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(12)
        color: Theme.textSecondary
    }
    ReadableText {
        objectName: choice.meaningObjectName
        width: parent.width
        visible: choice.meaningShown
        text: qsTr("The slice of spectrum the decoder processes. The NFM channel width is set in the Radio sheet.")
        wrapMode: Text.Wrap
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(12)
        color: Theme.textSecondary
    }
}
