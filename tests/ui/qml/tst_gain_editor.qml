// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// The shared tuner gain editor: [Automatic | Set], or [App default | Automatic |
// Set] where the value can follow the app, with one canonical mode either way
// (0 App default, 1 Automatic, 2 Set), the 1-49 dB field under Set, and what a
// caller stores. It reports choices and typing and never changes its own state;
// nothing is reported while the caller syncs it.
Item {
    id: root

    width: 360
    height: 700

    Column {
        width: parent.width
        spacing: 12

        Ui.GainEditor {
            id: plain

            width: parent.width
            title: "Tuner gain"
            modeObjectName: "plainGainMode"
            fieldObjectName: "plainGainField"
            hintObjectName: "plainGainHint"
        }

        Ui.GainEditor {
            id: inherit

            width: parent.width
            title: "Gain"
            inheritable: true
            inheritNote: "App default · Automatic"
            modeObjectName: "inheritGainMode"
            fieldObjectName: "inheritGainField"
            hintObjectName: "inheritGainHint"
        }
    }

    SignalSpy {
        id: draftSpy

        target: plain
        signalName: "draftEdited"
    }

    SignalSpy {
        id: modeSpy

        target: plain
        signalName: "modeSelected"
    }

    SignalSpy {
        id: commitSpy

        target: plain
        signalName: "committed"
    }

    SignalSpy {
        id: inheritModeSpy

        target: inherit
        signalName: "modeSelected"
    }

    TestCase {
        name: "GainEditor"
        when: windowShown

        function init() {
            plain.syncing = false;
            plain.mode = 1;
            plain.manualText = "30";
            inherit.syncing = false;
            inherit.mode = 0;
            inherit.manualText = "";
            draftSpy.clear();
            modeSpy.clear();
            commitSpy.clear();
            inheritModeSpy.clear();
        }

        function cleanup() {
            plain.forceActiveFocus();
        }

        function noteOf(editor) {
            for (var child of editor.children) {
                if (child.text === editor.inheritNote && child.wrapMode !== undefined && child.input === undefined)
                    return child;
            }
            return null;
        }

        function test_layouts() {
            var plainMode = findChild(plain, "plainGainMode");
            var inheritMode = findChild(inherit, "inheritGainMode");
            verify(plainMode !== null && inheritMode !== null);
            compare(JSON.stringify(plainMode.model), JSON.stringify(["Automatic", "Set"]));
            compare(JSON.stringify(inheritMode.model), JSON.stringify(["App default", "Automatic", "Set"]));
            compare(plainMode.Accessible.name, "Tuner gain", "named for the whole choice");
            compare(inheritMode.Accessible.name, "Gain");
            var field = findChild(plain, "plainGainField");
            compare(field.label, "Tuner gain");
            compare(field.unit, "dB");
            verify(plain.field === field, "the field a caller reveals on an error");
        }

        // The mode is canonical: the same number means the same choice in both layouts.
        function test_mode_shows_its_segment_data() {
            return [
                {tag: "automatic", mode: 1, plainIndex: 0, inheritIndex: 1, field: false, note: false},
                {tag: "set", mode: 2, plainIndex: 1, inheritIndex: 2, field: true, note: false},
                {tag: "inherit", mode: 0, plainIndex: -1, inheritIndex: 0, field: false, note: true}
            ];
        }
        function test_mode_shows_its_segment(data) {
            inherit.mode = data.mode;
            compare(findChild(inherit, "inheritGainMode").currentIndex, data.inheritIndex);
            compare(findChild(inherit, "inheritGainField").visible, data.field);
            compare(noteOf(inherit).visible, data.note, "the App default note shows only under App default");
            if (data.plainIndex < 0)
                return;
            plain.mode = data.mode;
            compare(findChild(plain, "plainGainMode").currentIndex, data.plainIndex);
            compare(findChild(plain, "plainGainField").visible, data.field);
        }

        function test_selection_reports_the_canonical_mode() {
            var plainMode = findChild(plain, "plainGainMode");
            plainMode.selected(0);
            plainMode.selected(1);
            compare(modeSpy.count, 2);
            compare(modeSpy.signalArguments[0][0], 1, "Automatic");
            compare(modeSpy.signalArguments[1][0], 2, "Set");
            compare(plain.mode, 1, "the caller sets the mode, not the editor");

            var inheritMode = findChild(inherit, "inheritGainMode");
            for (var i = 0; i < 3; ++i)
                inheritMode.selected(i);
            compare(inheritModeSpy.count, 3);
            for (var j = 0; j < 3; ++j)
                compare(inheritModeSpy.signalArguments[j][0], j);
            compare(inherit.mode, 0);
        }

        function test_valid_and_stored_value_data() {
            return [
                {tag: "empty", text: "", valid: false, stored: NaN},
                {tag: "zero", text: "0", valid: false, stored: NaN},
                {tag: "one", text: "1", valid: true, stored: 1},
                {tag: "forty-nine", text: "49", valid: true, stored: 49},
                {tag: "fifty", text: "50", valid: false, stored: NaN},
                {tag: "negative", text: "-3", valid: false, stored: NaN},
                {tag: "letters", text: "abc", valid: false, stored: NaN}
            ];
        }
        function test_valid_and_stored_value(data) {
            plain.mode = 2;
            plain.manualText = data.text;
            compare(plain.valid, data.valid);
            if (isNaN(data.stored))
                verify(isNaN(plain.storedValue), "an invalid Set stores nothing: " + plain.storedValue);
            else
                compare(plain.storedValue, data.stored);
            compare(plain.field.error, data.valid ? "" : "Enter a gain from 1 to 49 dB.");

            // Whatever the text, Automatic and App default are valid and store their own value.
            plain.mode = 1;
            verify(plain.valid);
            compare(plain.storedValue, 0);
            inherit.manualText = data.text;
            inherit.mode = 0;
            verify(inherit.valid);
            compare(inherit.storedValue, -1);
        }

        function test_typing_reports_each_keystroke_and_the_commit() {
            plain.mode = 2;
            var field = plain.field;
            field.input.forceActiveFocus();
            field.input.selectAll();
            keyClick(Qt.Key_Backspace);
            keyClick("2");
            keyClick("5");
            compare(draftSpy.count, 3);
            compare(draftSpy.signalArguments[0][0], "");
            compare(draftSpy.signalArguments[1][0], "2");
            compare(draftSpy.signalArguments[2][0], "25");
            compare(commitSpy.count, 0, "no commit while typing");
            // Return ends the edit, and the field then lets go of the focus: one end, one commit.
            keyClick(Qt.Key_Return);
            compare(commitSpy.count, 1, "Return commits once");
            verify(!field.input.activeFocus);
            compare(plain.manualText, "30", "the caller keeps manualText; the editor does not write it");
            // Back in and out with nothing changed: no new end.
            field.input.forceActiveFocus();
            plain.forceActiveFocus();
            compare(commitSpy.count, 1, "nothing changed, nothing committed");
            // A new edit ends once again, by focus loss alone.
            field.input.forceActiveFocus();
            keyClick("6");
            plain.forceActiveFocus();
            compare(commitSpy.count, 2);
            // A value the caller puts in is a change too: the next end reports it.
            plain.manualText = "40";
            field.input.forceActiveFocus();
            plain.forceActiveFocus();
            compare(commitSpy.count, 3);
        }

        // valid and storedValue read the field as shown, even when a caller leaves manualText behind.
        function test_valid_follows_the_field() {
            plain.mode = 2;
            var field = plain.field;
            field.input.forceActiveFocus();
            field.input.selectAll();
            keyClick("5");
            keyClick("0");
            compare(plain.manualText, "30");
            verify(!plain.valid, "the error shows, so it is not valid");
            verify(isNaN(plain.storedValue));
            compare(field.error, "Enter a gain from 1 to 49 dB.");
            field.input.selectAll();
            keyClick("9");
            verify(plain.valid);
            compare(plain.storedValue, 9);
        }

        // Leaving Set hides the field, which then gives up the focus: no key goes to a hidden field.
        function test_leaving_set_takes_the_focus_from_the_field() {
            inherit.mode = 2;
            var field = inherit.field;
            field.input.forceActiveFocus();
            verify(field.input.activeFocus);
            inherit.mode = 1;
            verify(!field.input.activeFocus, "Automatic");
            keyClick("7");
            compare(field.text, "", "a key after Automatic reached nothing");
            inherit.mode = 2;
            field.input.forceActiveFocus();
            inherit.mode = 0;
            verify(!field.input.activeFocus, "App default");
        }

        // The same when the caller hides the whole editor with Set still chosen (a source with no RTL gain).
        function test_hiding_the_editor_takes_the_focus_from_the_field() {
            inherit.mode = 2;
            inherit.manualText = "20";
            var field = inherit.field;
            field.input.forceActiveFocus();
            verify(field.input.activeFocus);
            inherit.visible = false;
            verify(!field.input.activeFocus, "a hidden editor's field has no focus");
            keyClick("7");
            compare(field.text, "20", "a key after hiding reached nothing");
            inherit.visible = true;
        }

        // The hint explains the choice: under the strip whatever is chosen, above the field.
        function test_hint_sits_under_the_choice() {
            var hint = findChild(plain, "plainGainHint");
            verify(hint !== null);
            verify(!findChild(inherit, "inheritGainHint").visible, "no hint, no line");
            plain.hint = "What Automatic does.";
            plain.mode = 2;
            verify(hint.visible);
            compare(hint.Accessible.name, "What Automatic does.");
            waitForRendering(plain);
            var strip = findChild(plain, "plainGainMode");
            verify(hint.y >= strip.y + strip.height);
            verify(hint.y + hint.height <= plain.field.y);
            plain.mode = 1;
            verify(hint.visible);
            plain.hint = "";
        }

        function test_nothing_is_reported_while_syncing() {
            plain.mode = 2;
            plain.syncing = true;
            findChild(plain, "plainGainMode").selected(0);
            var field = plain.field;
            field.input.forceActiveFocus();
            keyClick("4");
            keyClick(Qt.Key_Return);
            plain.manualText = "12";
            compare(modeSpy.count, 0);
            compare(draftSpy.count, 0);
            compare(commitSpy.count, 0);
            plain.syncing = false;
            findChild(plain, "plainGainMode").selected(0);
            compare(modeSpy.count, 1, "reported again once the sync is over");
        }
    }
}
