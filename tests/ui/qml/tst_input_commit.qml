// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Window
import QtTest
import "../../../src/ui/qt/qml" as Ui

// Issue #718: an Android keyboard holds the word being typed as composing
// (preedit) text, which `text` leaves out. Every control activation finishes
// that word before the action reads the form; buttons also end the field's
// editing. testContext.composeText() sends the focused field exactly what the
// keyboard sends mid-word, and the production inputCommit runs its fallback.
Item {
    id: root
    width: 420
    height: 900

    property string seenOnClick: ""
    property int clicks: 0
    property int finished: 0
    property int chosen: -1

    Column {
        width: parent.width
        spacing: 8
        Ui.PlexTextField {
            id: field
            objectName: "field"
            width: parent.width
            label: "Name"
            onEditingFinished: root.finished++
        }
        Ui.GradientButton {
            id: save
            objectName: "save"
            width: parent.width
            text: "Save"
            onClicked: {
                root.seenOnClick = field.text;
                root.clicks++;
            }
        }
        // Gated only on the field's committed text.
        Ui.OutlineButton {
            id: gated
            objectName: "gated"
            width: parent.width
            text: "Use this"
            inputValid: /^[0-9a-f]+$/i.test(field.text)
            onClicked: {
                root.seenOnClick = field.text;
                root.clicks++;
            }
        }
        // A real guard: no typing makes it act.
        Ui.OutlineButton {
            id: blocked
            objectName: "blocked"
            width: parent.width
            text: "Busy"
            enabled: false
            onClicked: root.clicks++
        }
        Ui.SegmentedControl {
            id: segments
            width: parent.width
            model: ["One", "Two"]
            onSelected: function (index) {
                root.chosen = index;
                root.seenOnClick = field.text;
            }
        }
        Ui.DisclosureRow {
            id: row
            title: "More"
            onTapped: root.seenOnClick = field.text
        }
    }

    TestCase {
        name: "InputCommit"
        when: windowShown

        function init() {
            root.seenOnClick = "";
            root.clicks = 0;
            root.chosen = -1;
            field.input.forceActiveFocus();
            // Drop any word a case left composing, then start empty.
            verify(testContext.composeText(""));
            field.text = "";
            root.finished = 0;
            verify(field.input.activeFocus);
            verify(!Ui.Navigation.composing);
        }
        function compose(word) {
            var before = field.text;
            verify(testContext.composeText(word), "the field holds the word as composing text");
            compare(field.text, before, "composing text is not part of text yet");
            verify(Ui.Navigation.composing);
        }
        function tap(item) {
            mouseClick(item, item.width / 2, item.height / 2);
        }

        function test_button_tap_commits_and_finishes_editing() {
            compose("Weekend");
            tap(save);
            compare(root.clicks, 1);
            compare(root.seenOnClick, "Weekend", "the action reads the whole word");
            compare(field.input.preeditText, "");
            verify(!field.input.activeFocus, "a button ends the field's editing");
            compare(root.finished, 1, "editingFinished ran before the action finished");
            verify(!Ui.Navigation.composing);
        }
        function test_keyboard_and_accessibility_activation_commit() {
            // Keys.onReturnPressed and Accessible.onPressAction both call activate().
            compose("Week");
            save.activate();
            compare(root.seenOnClick, "Week");
            field.input.forceActiveFocus();
            field.text = "Week";
            field.input.cursorPosition = 4;
            compose("end");
            save.activate();
            compare(root.seenOnClick, "Weekend");
            compare(root.clicks, 2);
        }
        function test_validity_gate_lets_a_composing_value_through() {
            verify(!gated.enabled, "empty: the gated button is disabled");
            compose("AB");
            verify(gated.enabled, "composing: live, so a tap can commit first");
            verify(!gated.inputValid, "but it still looks unavailable");
            compare(gated.opacity, 0.5);
            tap(gated);
            compare(root.clicks, 1, "the committed value is valid, so it acts");
            compare(root.seenOnClick, "AB");
        }
        function test_validity_gate_still_refuses_an_invalid_value() {
            compose("XYZ");
            tap(gated);
            compare(root.clicks, 0, "the committed value is invalid, so nothing happens");
            compare(field.text, "XYZ", "the word is kept, not lost");
            verify(!gated.enabled, "and the button is disabled again");
        }
        function test_genuinely_disabled_button_stays_inert() {
            compose("AB");
            verify(!blocked.enabled);
            tap(blocked);
            blocked.activate();
            compare(root.clicks, 0);
        }
        function test_choices_commit_but_keep_focus() {
            compose("Wee");
            mouseClick(segments, segments.width * 0.75, segments.height / 2);
            compare(root.chosen, 1);
            compare(root.seenOnClick, "Wee");
            verify(field.input.activeFocus, "a choice made mid-word leaves the field focused");
            compose("k");
            compare(field.input.preeditText, "k");
            row.activate();
            compare(root.seenOnClick, "Week");
            verify(field.input.activeFocus);
        }
        function test_tap_inside_the_field_does_not_commit() {
            compose("We");
            mouseClick(field.input, 4, field.input.height / 2);
            compare(field.input.preeditText, "We", "the keyboard keeps its word");
            compare(field.text, "");
            verify(field.input.activeFocus);
        }
        function test_back_commits_first() {
            compose("Weekend");
            Ui.Navigation.back(field.Window.window);
            compare(field.text, "Weekend");
        }
    }
}
