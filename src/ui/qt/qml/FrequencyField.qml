// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// A frequency in MHz, typed: the number, its unit, and the rule for what counts
// as one.
//
// One copy because the rule is the part that matters and it is not obvious from
// looking at any single field. inputMethodHints only chooses which soft keyboard
// appears; a hardware keyboard types whatever it likes, and every one of these
// values is spliced into an input spec or a saved system. The validator is what
// keeps "851.375M" out of them, so a screen that grew its own field without it
// would be the one that let a bad frequency through.
Column {
    id: root

    /** The text in the field. */
    property alias text: field.text
    /** Names the input itself, for tests that address it directly. */
    property alias fieldObjectName: field.objectName
    /** Size of the number; the unit stays at its own size beside it. */
    property int pixelSize: 32
    /** Whether the text currently satisfies the validator. */
    readonly property alias acceptableInput: field.acceptableInput

    /** Emitted when the field is committed from the keyboard. */
    signal accepted

    function forceActiveFocus() {
        field.forceActiveFocus();
    }

    spacing: 8

    Row {
        spacing: 8

        TextInput {
            id: field

            width: Math.max(implicitWidth, 60)
            font.family: Theme.mono
            font.pixelSize: root.pixelSize
            font.weight: Font.Medium
            color: Theme.textPrimary
            inputMethodHints: Qt.ImhFormattedNumbersOnly
            validator: RegularExpressionValidator {
                regularExpression: /^\d{1,5}(\.\d{0,6})?$/
            }
            selectionColor: Qt.alpha(Theme.cyan, 0.35)
            selectedTextColor: Theme.textPrimary
            onAccepted: root.accepted()
        }

        Text {
            text: "MHz"
            anchors.baseline: field.baseline
            font.family: Theme.mono
            font.pixelSize: 15
            color: Theme.textSubdued
        }
    }

    Rectangle {
        width: root.width
        height: 2
        color: Theme.cyan
    }
}
