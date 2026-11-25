// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// A tuner gain choice: Automatic or a set 1-49 dB, and App default first where
// the value can follow the app (`inheritable`). Settings, the system editor and
// the scan list editor share it. `mode` is 0 App default, 1 Automatic, 2 Set
// in either layout. The caller owns the state: this never writes `mode` or
// `manualText` (the value the caller puts in the field), it reports a choice
// (`modeSelected`), each keystroke (`draftEdited`) and the end of an edit
// (`committed`, once per edit: Return and the focus loss after it are one
// end, and an end with nothing changed since the last is none). A caller
// keeps `manualText` in step with the typing, so that putting a value back
// in the field is a change. `valid` and `storedValue` read the field as
// shown. None of the signals fire while the caller loads or resyncs
// (`syncing`). Whenever the field hides (Set left, after the choice has
// committed a composing word, or the whole editor hidden), it gives up the
// focus and the keyboard.
Column {
    id: editor

    property string title: qsTr("Gain")
    property bool inheritable: false
    // What App default runs, shown under the choice while it is chosen.
    property string inheritNote: ""
    // What the choice means, under it whatever is chosen.
    property string hint: ""
    property int mode: 1
    property string manualText: ""
    property bool syncing: false
    property string modeObjectName: ""
    property string fieldObjectName: ""
    property string hintObjectName: ""
    property string accessibleName: title
    // The 1-49 dB field, for a caller that reveals or focuses it on an error.
    readonly property PlexTextField field: gainField

    readonly property bool valid: mode !== 2 || !isNaN(parseGain(gainField.text))
    // What the caller stores: -1 App default, 0 Automatic, the set dB, NaN
    // while Set holds no valid gain.
    readonly property var storedValue: mode === 0 ? -1 : mode === 1 ? 0 : parseGain(gainField.text)

    signal draftEdited(string text)
    signal modeSelected(int mode)
    signal committed

    // A whole number of dB from 1 to 49, else NaN (ASCII digits only: a
    // locale-aware validator lets through text Number cannot read).
    function parseGain(text) {
        if (!/^-?[0-9]+$/.test(text))
            return NaN;
        var value = Number(text);
        return value >= 1 && value <= 49 ? value : NaN;
    }

    spacing: 6

    MicroLabel {
        text: editor.title
    }
    SegmentedControl {
        objectName: editor.modeObjectName
        accessibleName: editor.accessibleName
        width: parent.width
        model: editor.inheritable ? [qsTr("App default"), qsTr("Automatic"), qsTr("Set")] : [qsTr("Automatic"), qsTr("Set")]
        currentIndex: editor.inheritable ? editor.mode : Math.max(0, editor.mode - 1)
        onSelected: function (index) {
            if (!editor.syncing)
                editor.modeSelected(editor.inheritable ? index : index + 1);
        }
    }
    ReadableText {
        objectName: editor.hintObjectName
        width: parent.width
        visible: editor.hint.length > 0
        text: editor.hint
        wrapMode: Text.Wrap
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(12)
        color: Theme.textSecondary
    }
    ReadableText {
        width: parent.width
        visible: editor.inheritable && editor.mode === 0
        text: editor.inheritNote
        wrapMode: Text.Wrap
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(12)
        color: Theme.textSecondary
    }
    PlexTextField {
        id: gainField
        objectName: editor.fieldObjectName
        width: parent.width
        visible: editor.mode === 2
        label: editor.title
        unit: qsTr("dB")
        mono: true
        text: editor.manualText
        inputMethodHints: Qt.ImhDigitsOnly
        input.validator: RegularExpressionValidator {
            regularExpression: /^-?[0-9]*$/
        }
        error: isNaN(editor.parseGain(text)) ? qsTr("Enter a gain from 1 to 49 dB.") : ""
        input.onTextEdited: {
            if (!editor.syncing)
                editor.draftEdited(text);
        }
        // The end of an edit already reported, until the text changes.
        property bool commitReported: false
        onTextChanged: commitReported = false
        onEditingFinished: {
            if (editor.syncing || commitReported)
                return;
            commitReported = true;
            editor.committed();
        }
        // A hidden field keeps no focus and takes no key: the field's own
        // gate does that (PressGate, from the field's inReach), handing
        // the keyboard to the app's Back owner once the hide has settled
        // (issue #902). Its word still composing is committed here at once (a
        // collapsed pane, a source switch), so what reads the value as the
        // field hides (the wizard's Continue) has all of it.
        onVisibleChanged: {
            if (!visible && input.activeFocus)
                Navigation.commitComposing(false);
        }
    }
}
