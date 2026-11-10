// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// What kind of file is being imported: one pill per library kind, and for
// keys whether they are written in decimal or hex. Imported files' Import file
// sheet and the sheet for a file opened from another app (#831) both ask it
// here, so the two read alike. The consumer sets the width; a pill wraps its
// label inside it (#831 B8) rather than being cut off at a narrow sheet or
// large text.
Column {
    id: picker

    // The chosen kind, "keys" for either key format; empty until one is chosen.
    property string kind: ""
    property bool keyHex: false
    // The library type to import as (ImportedFilesModel::knownTypes()).
    readonly property string type: kind === "keys" ? (keyHex ? "keysHex" : "keysDec") : kind

    spacing: 12

    Flow {
        objectName: "importKindPicker"
        width: parent.width
        spacing: 8

        Repeater {
            model: [qsTr("Channel map"), qsTr("Talkgroups"), qsTr("Keys"), qsTr("P25 band plan"), qsTr("Radio IDs"), qsTr("DMR key mappings"), qsTr("Vertex keystreams"), qsTr("Target CSV")]
            delegate: FilterPill {
                required property int index
                required property string modelData
                readonly property string kind: ["chan", "group", "keys", "p25Bandplan", "src", "dmrTgKeys", "vertexKeys", "trunkTargets"][index]
                objectName: "importKind_" + kind
                width: Math.min(implicitWidth, parent ? parent.width : implicitWidth)
                wrapLabel: true
                text: modelData
                caret: false
                active: picker.kind === kind
                onClicked: picker.kind = kind
            }
        }
    }

    SegmentedControl {
        objectName: "importKeyFormat"
        width: parent.width
        visible: picker.kind === "keys"
        model: [qsTr("Decimal keys"), qsTr("Hex keys")]
        currentIndex: picker.keyHex ? 1 : 0
        onSelected: function (index) {
            picker.keyHex = index === 1;
        }
    }
}
