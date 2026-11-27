// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest
import DsdNeo
import "../../../src/ui/qt/qml" as Ui

// Issue #902 B9: what a screen reader gets from a covered control is Qt's
// accessible interface, not the QML attached flags. A Qt Quick Controls
// wrapper (a combo, a check box, a toggle) and a text field are focusable and
// offer SetFocus, Press or Toggle from their role and native focus policy, so
// under a sheet each must lose those too: a node TalkBack still holds of it is
// then no stop, and an action on it (a cached double tap, an accessibility
// focus) changes nothing behind the sheet. Uncovered, each is a stop again.
Item {
    id: root

    width: 420
    height: 1300

    // What a sheet over the screen leaves reachable: only itself, which takes
    // its Escape and Back as a sheet does (`keyTarget`).
    Item {
        id: cover
        readonly property Item keyTarget: cover
    }
    // The app's one Escape and Back owner, as Main registers its safe area
    // (Navigation.backTarget): where the keyboard goes when nothing on top
    // declares its own.
    Item {
        id: appBack
        property int escapes: 0
        Keys.onEscapePressed: escapes++
    }

    Column {
        width: parent.width

        Ui.PlexComboBox {
            id: combo
            width: parent.width
            accessibleName: "Entry protocol"
            model: ["P25", "DMR"]
        }
        Ui.PlexTextField {
            id: textField
            width: parent.width
            label: "Host"
        }
        Ui.PlexInput {
            id: plexInput
            width: parent.width
            label: "Gain"
        }
        Ui.FrequencyField {
            id: frequency
            width: parent.width
            fieldObjectName: "coveredFrequencyInput"
        }
        Ui.PlexCheckBox {
            id: checkBox
            text: "Skip silent channels"
        }
        Ui.PlexToggle {
            id: toggle
            text: "Bias tee"
        }
        Ui.FileBrowserCommand {
            id: command
            width: parent.width
            objectName: "coveredCommand"
            lines: ["adb push calls.csv /sdcard/Download"]
        }
        Loader {
            id: licenses
            width: parent.width
            height: 200
            source: uiDir + "/LicensesScreen.qml"
        }
        Ui.ReadableText {
            id: readable
            text: "Saved systems follow the radio"
        }
        Ui.IconButton {
            id: iconButton
            icon: "more"
            property int presses: 0
            onClicked: presses++
        }
        Ui.PlayCircle {
            id: playCircle
            accessibleName: "Listen to County"
            property int presses: 0
            onClicked: presses++
        }
        Ui.ToggleRow {
            id: toggleRow
            title: "Record calls"
            property int presses: 0
            onToggled: presses++
        }
        Ui.PinButton {
            id: pinButton
            text: "County"
        }
        Ui.BandRail {
            id: bandRail
            width: parent.width
            property int tunes: 0
            onTuneRequested: tunes++
        }
        Ui.ReadableText {
            id: heldLine
            text: "Downtown simulcast site"
            property bool shown: true
            reachable: navigationAllowed && visible && shown
        }
        // What has the keyboard while the cover is up.
        Ui.OutlineButton {
            id: holder
            text: "Close"
        }
        // A longer list, each option with its own initial, and one with no option at all.
        Ui.PlexComboBox {
            id: longCombo
            width: parent.width
            accessibleName: "Saved system"
            model: ["Alpha", "Bravo", "Charlie", "Delta", "Echo"]
        }
        Ui.PlexComboBox {
            id: emptyCombo
            width: parent.width
            accessibleName: "Saved system to add"
            model: []
        }
        // Opens the sheet and gives its field the keyboard, as Backup's Export does with its passphrase.
        Ui.OutlineButton {
            id: opener
            text: "Export backup"
            onClicked: {
                sheet.visible = true;
                sheetField.input.forceActiveFocus();
            }
        }
    }

    // A second sheet, opened as the first closes, giving its field the keyboard (an ActionMenu row opening its
    // own form).
    Ui.ModalSheet {
        id: nextSheet
        anchors.fill: parent
        visible: false
        Ui.PlexTextField {
            id: nextField
            width: parent.width
            label: "Range name"
        }
    }

    // A modal below a sheet, removed while the sheet's list is open.
    Item {
        id: under
    }
    SignalSpy {
        id: fieldFinished
        target: textField
        signalName: "editingFinished"
    }
    // A check box made for one case and destroyed while Space is held on it (a row Remove takes away).
    Component {
        id: spareCheckBox
        Ui.PlexCheckBox {
            text: "Spare"
        }
    }
    SignalSpy {
        id: pinClicks
        target: pinButton
        signalName: "clicked"
    }

    // A selector made and destroyed while hidden, its list never opened.
    Component {
        id: spareCombo
        Ui.PlexComboBox {
            visible: false
            model: ["A"]
        }
    }

    // A sheet that opens by itself over an open list.
    Ui.ModalSheet {
        id: sheet
        anchors.fill: parent
        visible: false
        Ui.OutlineButton {
            objectName: "sheetButton"
            width: parent.width
            text: "Unlock"
        }
        Ui.PlexComboBox {
            id: sheetCombo
            width: parent.width
            accessibleName: "Profile application scope"
            model: ["Talkgroup", "Radio", "Everywhere"]
        }
        Ui.PlexTextField {
            id: sheetField
            width: parent.width
            label: "Passphrase"
        }
    }

    // A layer whose surface is a Loader, as Main registers the Map's and the
    // Spectrum's: the screen it loads may or may not take Escape itself.
    Loader {
        id: loadedLayer
        width: 200
        height: 100
        sourceComponent: Component {
            Item {
                objectName: "loadedScreen"
                property int escapes: 0
                Keys.onEscapePressed: escapes++
                Ui.OutlineButton {
                    objectName: "loadedButton"
                    text: "Not now"
                }
            }
        }
    }

    TestCase {
        name: "A11yCoveredNativeControls"
        when: windowShown

        function initTestCase() {
            verify(testContext.accessibilityActive(), "run with DSD_QML_TEST_ACCESSIBILITY=1, or there is no tree");
        }
        // The open list's option row (plexComboRow) at @p index.
        function comboRow(index) {
            var found = null;
            (function walk(at) {
                for (var c of at.children) {
                    if (c.objectName === "plexComboRow" && c.parent.index === index)
                        found = c;
                    walk(c);
                }
            })(combo.popup.contentItem);
            verify(found !== null, "option " + index);
            return found;
        }
        function init() {
            Ui.Navigation.backTarget = appBack;
            appBack.escapes = 0;
        }
        function cleanup() {
            nextSheet.visible = false;
            sheet.visible = false;
            Ui.Navigation.modals = [];
            Ui.Navigation.backTarget = null;
            Ui.Navigation.layers = [];
            Ui.Navigation.rootSurfaces = [];
            for (var selector of [combo, longCombo, emptyCombo, sheetCombo])
                selector.popup.close();
            checkBox.checked = false;
            checkBox.enabled = true;
            longCombo.enabled = true;
            sheetField.text = "";
            textField.input.text = "";
            iconButton.enabled = true;
            toggle.checked = false;
            for (var control of [combo, textField, plexInput, frequency, checkBox, toggle, readable, iconButton, playCircle,
                                 toggleRow, bandRail, command, licenses])
                control.visible = true;
            heldLine.shown = true;
            pinButton.live = true;
            testContext.sendWindowFocus(holder, true);
        }

        // Issue #902 B9: hidden, not covered (Navigation.allows() still true): Qt keeps an invisible item's focus and
        // offers SetFocus from its role or Tab stop, so each control drops both itself. A field that had the keyboard
        // gives it up, commits the edit, and typing after that changes nothing.
        function hidden_data() {
            return [
                {tag: "PlexTextField", control: function () { return textField; }, node: function () { return textField.input; }, edits: true},
                {tag: "PlexInput", control: function () { return plexInput; }, node: function () { return plexInput; }, edits: true},
                {tag: "FrequencyField", control: function () { return frequency; },
                    node: function () { return findChild(frequency, "coveredFrequencyInput"); }, edits: true},
                {tag: "PlexComboBox", control: function () { return combo; }, node: function () { return combo; }},
                {tag: "PlexCheckBox", control: function () { return checkBox; }, node: function () { return checkBox; }},
                {tag: "PlexToggle", control: function () { return toggle; }, node: function () { return toggle; }},
                {tag: "ReadableText", control: function () { return readable; }, node: function () { return readable; }},
                {tag: "IconButton", control: function () { return iconButton; }, node: function () { return iconButton; }, presses: true},
                {tag: "PlayCircle", control: function () { return playCircle; }, node: function () { return playCircle; }, presses: true},
                {tag: "ToggleRow", control: function () { return toggleRow; }, node: function () { return toggleRow; }, presses: true}
            ];
        }
        function test_hidden_controls_offer_nothing_data() {
            return hidden_data();
        }
        function test_hidden_controls_offer_nothing(data) {
            var control = data.control();
            var item = data.node();
            verify(item !== null);
            verify(testContext.accessibleNode(item).focusable, "shown: a stop");
            // The focused control itself is hidden; a text takes the keyboard from a screen reader's SetFocus.
            if (data.tag === "ReadableText")
                testContext.accessibleDoAction(item, testContext.setFocusAction);
            else
                item.forceActiveFocus();
            verify(item.activeFocus, "shown: it has the keyboard");
            var text = item.text;
            var index = item.currentIndex;
            control.visible = false;
            verify(Ui.Navigation.allows(item), "nothing covers it");
            // It gives the keyboard up, stored focus too, once the change that hid it has settled.
            tryVerify(function () { return !item.activeFocus && !item.focus; }, 1000, "hidden: the keyboard is given up");
            var node = testContext.accessibleNode(item);
            verify(!node.inTree, "hidden: out of the tree");
            compare(node.name, "", "hidden: nameless");
            verify(!node.focusable, "hidden: no stop, " + JSON.stringify(node.actions));
            verify(node.actions.indexOf(testContext.setFocusAction) < 0, "hidden: no focus action");
            verify(!item.activeFocus, "hidden: no keyboard");
            testContext.accessibleDoAction(item, testContext.setFocusAction);
            verify(!item.activeFocus, "hidden: an accessibility focus takes no keyboard");
            // No key of its own acts unseen: Space toggles nothing, Down opens no list, typing edits nothing.
            keyClick(Qt.Key_Space);
            keyClick(Qt.Key_Down);
            keyClick(Qt.Key_7);
            if (data.edits)
                compare(item.text, text, "hidden: typing edits nothing");
            if (index !== undefined) {
                compare(item.currentIndex, index, "hidden: no key changes the choice");
                verify(!item.popup.visible, "hidden: no key opens the list");
            }
            var checked = item.checked;
            var presses = item.presses;
            testContext.accessibleDoAction(item, "Press");
            testContext.accessibleDoAction(item, "Toggle");
            if (checked !== undefined)
                compare(item.checked, checked, "hidden: a press or toggle changes nothing");
            if (data.presses)
                compare(item.presses, presses, "hidden: a press does nothing");
            control.visible = true;
            verify(testContext.accessibleNode(item).focusable, "shown again: a stop");
            if (data.presses) {
                testContext.accessibleClick(item);
                compare(item.presses, presses + 1, "shown: the same press acts");
            }
        }

        // Issue #902: a native text field's placeholder is its accessible description (Qt writes it there). Covered or
        // hidden, the node describes nothing, as it names nothing; shown, the placeholder reads again.
        function test_native_placeholder_follows_the_gate_data() {
            return [{tag: "covered", how: "covered"}, {tag: "hidden", how: "hidden"}];
        }
        function test_native_placeholder_follows_the_gate(data) {
            var label = plexInput.label;
            plexInput.label = "Gain";
            plexInput.placeholderText = "auto";
            try {
                compare(testContext.accessibleNode(plexInput).description, "auto", "shown, the placeholder describes it");
                if (data.how === "covered")
                    Ui.Navigation.modals = [cover];
                else
                    plexInput.visible = false;
                var node = testContext.accessibleNode(plexInput);
                compare(node.name, "", data.tag + ": nameless");
                compare(node.description, "", data.tag + ": no description");
                Ui.Navigation.modals = [];
                plexInput.visible = true;
                compare(testContext.accessibleNode(plexInput).description, "auto", "shown again, it reads again");
            } finally {
                Ui.Navigation.modals = [];
                plexInput.visible = true;
                plexInput.placeholderText = "";
                plexInput.label = label;
            }
        }

        function controls_data() {
            return [
                {tag: "PlexComboBox", node: function () { return combo; }, name: "Entry protocol"},
                {tag: "PlexTextField", node: function () { return textField.input; }, name: "Host"},
                {tag: "PlexInput", node: function () { return plexInput; }, name: "Gain"},
                {tag: "FrequencyField", node: function () { return findChild(frequency, "coveredFrequencyInput"); },
                    name: "Frequency in MHz"},
                {tag: "PlexCheckBox", node: function () { return checkBox; }, name: "Skip silent channels"},
                {tag: "PlexToggle", node: function () { return toggle; }, name: "Bias tee"},
                {tag: "FileBrowserCommand line", node: function () { return findChild(command, "coveredCommandText0"); },
                    name: "adb push calls.csv /sdcard/Download", text: true},
                {tag: "Licenses Qt source", node: function () { return findChild(licenses.item, "licensesQtSource"); },
                    name: null, text: true},
                {tag: "ReadableText", node: function () { return readable; }, name: "Saved systems follow the radio", text: true},
                {tag: "IconButton", node: function () { return iconButton; }, name: "More options"},
                {tag: "PlayCircle", node: function () { return playCircle; }, name: "Listen to County"}
            ];
        }

        function test_covered_controls_offer_nothing_data() {
            return controls_data();
        }
        function test_covered_controls_offer_nothing(data) {
            var item = data.node();
            verify(item !== null);
            var node = testContext.accessibleNode(item);
            verify(node.inTree, "in the tree before the cover");
            if (data.name !== null)
                compare(node.name, data.name, "named before the cover");
            var named = node.name;
            verify(named.length > 0 || node.value.length > 0, "it reads something before the cover");
            verify(node.focusable, "a stop before the cover");

            // The focused control itself is covered; a text takes the keyboard from a screen reader's SetFocus.
            if (data.text)
                testContext.accessibleDoAction(item, testContext.setFocusAction);
            else
                item.forceActiveFocus();
            verify(item.activeFocus, "before the cover: it has the keyboard");
            var checkedBefore = item.checked;
            Ui.Navigation.modals = [cover];
            tryVerify(function () { return !item.activeFocus && !item.focus; }, 1000, "covered: the keyboard is given up");
            keyClick(Qt.Key_Space);
            keyClick(Qt.Key_Down);
            if (checkedBefore !== undefined)
                compare(item.checked, checkedBefore, "covered: Space toggles nothing");
            holder.forceActiveFocus();
            node = testContext.accessibleNode(item);
            verify(!node.inTree, "covered: out of the tree");
            compare(node.name, "", "covered: nameless");
            compare(node.value, "", "covered: no text to read");
            verify(!node.focusable, "covered: no stop");
            verify(node.actions.indexOf(testContext.setFocusAction) < 0, "covered: no focus action, " + JSON.stringify(node.actions));
            // A cached node's actions, whatever it still lists, act on nothing.
            var checked = item.checked;
            testContext.accessibleDoAction(item, testContext.setFocusAction);
            testContext.accessibleDoAction(item, "Press");
            testContext.accessibleDoAction(item, "Toggle");
            verify(!item.activeFocus, "covered: an accessibility focus leaves the keyboard where it was");
            if (checked !== undefined)
                compare(item.checked, checked, "covered: a press or toggle changes nothing");
            if (item.popup !== undefined)
                verify(!item.popup.visible, "covered: a press opens no list");

            Ui.Navigation.modals = [];
            node = testContext.accessibleNode(item);
            verify(node.inTree, "dismissed: in the tree");
            compare(node.name, named, "dismissed: named");
            verify(node.focusable, "dismissed: a stop");
        }

        // Issue #902: a key already queued when the focused control turns unreachable (delivered in the same turn
        // as the change, before the focus moves on) does nothing: Space toggles nothing, Down changes no choice and
        // opens no list, a digit or an input method's word edits nothing. Hidden and covered alike.
        function queued_data() {
            var rows = [];
            for (var how of ["hidden", "covered"]) {
                rows.push({tag: how + " PlexCheckBox", how: how, control: function () { return checkBox; }, node: function () { return checkBox; }});
                rows.push({tag: how + " PlexToggle", how: how, control: function () { return toggle; }, node: function () { return toggle; }});
                rows.push({tag: how + " PlexComboBox", how: how, control: function () { return combo; }, node: function () { return combo; }});
                rows.push({tag: how + " PlexTextField", how: how, control: function () { return textField; }, node: function () { return textField.input; }});
                rows.push({tag: how + " PlexInput", how: how, control: function () { return plexInput; }, node: function () { return plexInput; }});
                rows.push({tag: how + " FrequencyField", how: how, control: function () { return frequency; },
                    node: function () { return findChild(frequency, "coveredFrequencyInput"); }});
            }
            return rows;
        }
        function test_queued_key_does_nothing_data() {
            return queued_data();
        }
        function test_queued_key_does_nothing(data) {
            var control = data.control();
            var item = data.node();
            item.forceActiveFocus();
            verify(item.activeFocus);
            var checked = item.checked;
            var index = item.currentIndex;
            var text = item.text;
            if (data.how === "hidden")
                control.visible = false;
            else
                Ui.Navigation.modals = [cover];
            verify(item.activeFocus, "the focus has not moved on yet: the keys below are queued in the same turn");
            for (var key of [Qt.Key_Space, Qt.Key_Down, Qt.Key_7]) {
                testContext.sendKey(item, key, 0, true, false);
                testContext.sendKey(item, key, 0, false, false);
            }
            if (checked !== undefined)
                compare(item.checked, checked, "Space toggles nothing");
            if (index !== undefined) {
                compare(item.currentIndex, index, "Down changes no choice");
                verify(!item.popup.visible, "Down opens no list");
            }
            if (text !== undefined)
                compare(item.text, text, "a digit edits nothing");
            if (item.preeditText !== undefined) {
                // An input method's word started after the change is no edit either (a word composing before it is
                // committed: tst_wizard_gain.qml).
                verify(!testContext.composeText("8"), "an input method composes nothing");
                compare(item.text, text, "and edits nothing");
            }
            tryVerify(function () { return !item.activeFocus && !item.focus; }, 1000, "then the keyboard is given up");
            if (data.how === "covered")
                verify(cover.activeFocus, "covered: the keyboard goes to what covers it");
        }

        // Issue #902: Space pressed on a control that hides before the key comes up: the release it would have
        // acted on is taken, and the press is let go of with it, unacted. Shown again before the keyboard moved on,
        // the control is neither left pressed nor toggled, and the next Space acts as ever.
        function press_data() {
            return [
                {tag: "PlexCheckBox", control: function () { return checkBox; }},
                {tag: "PlexToggle", control: function () { return toggle; }},
                {tag: "PlexComboBox", control: function () { return combo; }}
            ];
        }
        function test_release_taken_while_hidden_lets_the_press_go_data() {
            return press_data();
        }
        function test_release_taken_while_hidden_lets_the_press_go(data) {
            var control = data.control();
            control.forceActiveFocus();
            verify(control.activeFocus);
            var checked = control.checked;
            testContext.sendKey(control, Qt.Key_Space, 0, true, false);
            verify(control.pressed, "Space is down on it");
            control.visible = false;
            testContext.sendKey(control, Qt.Key_Space, 0, false, false);
            control.visible = true;
            verify(!control.pressed && !control.down, "the press is let go of with its taken release");
            if (checked !== undefined)
                compare(control.checked, checked, "and toggled nothing");
            if (control.popup !== undefined)
                verify(!control.popup.visible, "and opened nothing");
            wait(50);
            control.forceActiveFocus();
            keyClick(Qt.Key_Space);
            if (checked !== undefined)
                compare(control.checked, !checked, "the next Space acts as ever");
            if (control.popup !== undefined) {
                tryVerify(function () { return control.popup.opened; }, 1000, "the next Space opens the list");
                control.popup.close();
                tryVerify(function () { return !control.popup.visible; });
            } else {
                control.checked = checked;
            }
        }

        // Issue #902: a selectable read-only text (the file browser's command, the licenses' texts) can hold the
        // keyboard (a tap, a screen reader's focus). Hidden with it, a queued Ctrl+A selects nothing unseen, and it
        // gives the keyboard up.
        function selectable_data() {
            return [
                {tag: "FileBrowserCommand line", shown: function () { return command; },
                    node: function () { return findChild(command, "coveredCommandText0"); }},
                {tag: "Licenses Qt source", shown: function () { return licenses; },
                    node: function () { return findChild(licenses.item, "licensesQtSource"); }}
            ];
        }
        function test_selectable_text_gives_up_the_keyboard_data() {
            return selectable_data();
        }
        function test_selectable_text_gives_up_the_keyboard(data) {
            var shown = data.shown();
            var item = data.node();
            verify(item !== null);
            item.forceActiveFocus();
            verify(item.activeFocus);
            item.deselect();
            shown.visible = false;
            testContext.sendKey(item, Qt.Key_A, Qt.ControlModifier, true, false);
            testContext.sendKey(item, Qt.Key_A, Qt.ControlModifier, false, false);
            compare(item.selectedText, "", "hidden: a queued Ctrl+A selects nothing");
            tryVerify(function () { return !item.activeFocus && !item.focus; }, 1000, "hidden: the keyboard is given up");
            shown.visible = true;
        }

        // Issue #902: a field hidden while composing finishes only the word it
        // was composing: a new word or another commit queued after it hid
        // changes nothing, and the release commits the word.
        function test_hidden_field_finishes_only_its_word() {
            var field = textField.input;
            field.text = "";
            field.forceActiveFocus();
            verify(testContext.composeText("before"), "composing");
            textField.visible = false;
            verify(field.activeFocus, "the focus has not moved on yet");
            compare(field.text, "before", "the word composing as it hid is committed as it hides");
            testContext.sendInputMethod("after", "");
            compare(field.preeditText, "", "a new word composes nothing");
            testContext.sendInputMethod("", "other");
            compare(field.text, "before", "another word's commit edits nothing");
            tryVerify(function () { return !field.activeFocus && !field.focus; }, 1000, "the keyboard is given up");
            compare(field.text, "before", "and the word stays as it was");
        }

        // Issue #902: the commit the release asks for is the keyboard's to
        // make, and a converting keyboard finishes the composing word as a
        // candidate unlike the word it showed ("nihao" as its characters):
        // that word is the one committed, not dropped for differing from the
        // composing text, while a commit the field was not asked for still
        // edits nothing.
        function test_hidden_field_takes_the_keyboards_final_word() {
            var field = textField.input;
            field.text = "";
            field.forceActiveFocus();
            verify(testContext.composeText("nihao"), "composing");
            // The keyboard converts as it commits; the commit is asked for as the field hides.
            testContext.convertOnCommit(field, "\u4f60\u597d");
            textField.visible = false;
            compare(field.text, "\u4f60\u597d", "the keyboard's final word is committed");
            compare(field.preeditText, "", "and nothing is left composing");
            testContext.sendInputMethod("", "other");
            compare(field.text, "\u4f60\u597d", "a commit it was not asked for edits nothing");
            tryVerify(function () { return !field.activeFocus && !field.focus; }, 1000, "the keyboard is given up");
        }

        // Issue #902: the one commit a hidden field lets through lands at its
        // composing word, never elsewhere: one offset into the text is
        // refused, and the release commits the word where it was.
        function test_hidden_field_commit_lands_at_its_word() {
            var field = textField.input;
            field.text = "abc";
            field.forceActiveFocus();
            field.cursorPosition = 3;
            verify(testContext.composeText("x"), "composing");
            // The keyboard answers the commit asked for as the field hides with a word two places back first:
            // refused, and the word lands where it was composed.
            testContext.convertOnCommit(field, "y", -2);
            textField.visible = false;
            compare(field.text, "abcx", "the text kept, the word committed where it was composed");
            testContext.sendInputMethod("", "z", -2, 0);
            compare(field.text, "abcx", "a later commit two places back edits nothing");
            tryVerify(function () { return !field.activeFocus && !field.focus; }, 1000, "the keyboard is given up");
            compare(field.text, "abcx");
        }

        // Issue #902: the same while the field is disabled under it (a sheet's panel disabling it before the sheet
        // covers it): still visible and in reach, its word is committed through the closed gate, at the word.
        function test_disabled_field_commit_lands_at_its_word() {
            var field = textField.input;
            field.text = "abc";
            field.forceActiveFocus();
            field.cursorPosition = 3;
            verify(testContext.composeText("x"), "composing");
            testContext.convertOnCommit(field, "y", -2);
            textField.enabled = false;
            try {
                compare(field.text, "abcx", "the text kept, the word committed where it was composed");
                compare(field.preeditText, "", "nothing is left composing");
            } finally {
                textField.enabled = true;
            }
        }

        // Issue #902: the window leaving while a word is composing, the field hidden afterwards: the edit the window's
        // leave ends (editingFinished) holds the whole word, and the field still gives the stored focus up.
        function test_window_leaving_publishes_the_whole_word() {
            var field = textField.input;
            field.text = "foo";
            field.forceActiveFocus();
            field.cursorPosition = 3;
            verify(testContext.composeText("bar"), "composing");
            var finished = [];
            var onFinished = function () {
                finished.push(field.text);
            };
            field.editingFinished.connect(onFinished);
            try {
                testContext.sendWindowFocus(holder, false);
                compare(finished, ["foobar"], "the edit the window's leave ended held the whole word");
                compare(field.preeditText, "", "nothing is left composing");
                textField.visible = false;
                tryVerify(function () { return !field.focus; }, 1000, "the hidden field lets the stored focus go");
                compare(field.text, "foobar");
            } finally {
                field.editingFinished.disconnect(onFinished);
                testContext.sendWindowFocus(holder, true);
            }
        }

        // Issue #902: a pin whose row StableRows lets go of (no longer live)
        // while it has the keyboard gives it up, though still shown.
        function test_pin_no_longer_live_gives_up_the_keyboard() {
            pinButton.forceActiveFocus();
            verify(pinButton.activeFocus);
            pinButton.live = false;
            tryVerify(function () { return !pinButton.activeFocus && !pinButton.focus; }, 1000, "no longer live, no keyboard");
        }

        // Issue #902: a control that takes the focus after it hid (a deferred
        // hand-off landing on it) gives it up too.
        function test_focus_taken_while_hidden_is_given_up() {
            holder.forceActiveFocus();
            checkBox.visible = false;
            wait(0);
            checkBox.forceActiveFocus();
            tryVerify(function () { return !checkBox.activeFocus && !checkBox.focus; }, 1000, "hidden, it lets the focus go");
            var checked = checkBox.checked;
            keyClick(Qt.Key_Space);
            compare(checkBox.checked, checked, "and Space toggles nothing");
        }

        // Issue #902: a layer hands the keyboard of a control that hid to the
        // item that takes its Escape and Back: its own, when it declares one
        // (`keyTarget`), else the app's one Back owner (Navigation.backTarget).
        // Never to the surface itself, which may pass Escape to nothing (a
        // Loader's screen, the Spectrum, a tab Main anchors but does not parent).
        function test_layer_hands_the_keyboard_to_a_back_owner_data() {
            return [{tag: "declared", declared: true}, {tag: "undeclared", declared: false}];
        }
        function test_layer_hands_the_keyboard_to_a_back_owner(data) {
            var screen = findChild(loadedLayer, "loadedScreen");
            var button = findChild(loadedLayer, "loadedButton");
            verify(screen !== null && button !== null);
            Ui.Navigation.layers = [data.declared ? {surface: loadedLayer, keyTarget: screen} : {surface: loadedLayer}];
            var owner = data.declared ? screen : appBack;
            var escapes = owner.escapes;
            button.forceActiveFocus();
            verify(button.activeFocus);
            button.visible = false;
            tryVerify(function () { return !button.activeFocus; }, 1000, "the hidden button gives the keyboard up");
            verify(owner.activeFocus, data.tag + ": to the layer's Back owner");
            keyClick(Qt.Key_Escape);
            compare(owner.escapes, escapes + 1, "Escape reaches it");
            button.visible = true;
        }

        // A list's options, once a sheet covers the open list or the list closes, are nameless and no stop, and a
        // press a screen reader still sends one chooses nothing.
        function test_combo_options_follow_their_own_gate() {
            combo.currentIndex = 0;
            combo.popup.open();
            tryVerify(function () { return combo.popup.opened; });
            var rows = [];
            (function walk(at) {
                for (var c of at.children) {
                    if (c.objectName === "plexComboRow")
                        rows.push(c);
                    walk(c);
                }
            })(combo.popup.contentItem);
            verify(rows.length === 2, "two options");
            var row = rows[1];
            verify(testContext.accessibleNode(row).focusable && row.Accessible.name === "DMR", "open: a named stop");
            // Issue #902: a screen reader's SetFocus gives the option the keyboard.
            testContext.accessibleDoAction(row, testContext.setFocusAction);
            verify(row.activeFocus, "open: SetFocus gives the option the keyboard");
            Ui.Navigation.modals = Ui.Navigation.modals.concat([cover]);
            compare(row.Accessible.name, "", "covered: nameless");
            verify(!testContext.accessibleNode(row).focusable, "covered: no stop");
            testContext.accessibleDoAction(row, "Press");
            compare(combo.currentIndex, 0, "covered: a press chooses nothing");
            // The option's own key handling (the delegate's Space) acts on no key queued as the cover came up.
            testContext.sendKey(row, Qt.Key_Space, 0, true, false);
            testContext.sendKey(row, Qt.Key_Space, 0, false, false);
            compare(combo.currentIndex, 0, "covered: Space chooses nothing");
            tryVerify(function () { return !row.activeFocus && !row.focus; }, 1000, "covered: the option gives the keyboard up");
            Ui.Navigation.modals = Ui.Navigation.modals.filter(function (m) { return m !== cover; });
            combo.popup.close();
            tryVerify(function () { return !combo.popup.visible; });
            compare(row.Accessible.name, "", "closed: nameless");
            testContext.accessibleDoAction(row, "Press");
            compare(combo.currentIndex, 0, "closed: a press chooses nothing");
        }

        // Issue #902: an open list is drawn over everything, a sheet opened after it included, and its options take
        // taps there by their own handling. A sheet over the list closes it, keeping the keyboard, so a tap where an
        // option was chooses nothing behind the sheet; a selector that hides closes its list too.
        function test_combo_list_closes_out_of_reach_data() {
            return [{tag: "sheet over it", how: "sheet"}, {tag: "selector hidden", how: "hidden"}];
        }
        function test_combo_list_closes_out_of_reach(data) {
            combo.currentIndex = 0;
            combo.popup.open();
            tryVerify(function () { return combo.popup.opened; });
            var row = null;
            (function walk(at) {
                for (var c of at.children) {
                    if (c.objectName === "plexComboRow" && c.parent.index === 1)
                        row = c;
                    walk(c);
                }
            })(combo.popup.contentItem);
            verify(row !== null, "the second option");
            if (data.how === "hidden") {
                combo.visible = false;
                verify(!combo.popup.visible, "hidden: the list closes at once");
                return;
            }
            var tap = row.mapToItem(root, row.width / 2, row.height / 2);
            sheet.visible = true;
            verify(!combo.popup.visible, "covered: the list closes at once");
            wait(50);
            verify(Ui.Navigation.contains(sheet, sheet.Window.activeFocusItem), "the sheet keeps the keyboard");
            mouseClick(root, tap.x, tap.y);
            compare(combo.currentIndex, 0, "covered: a tap where the option was chooses nothing");
        }

        // Issue #902: the selector's own open list is no cover. While it is open the selector keeps its name, its
        // stop and the keyboard, and Qt's own combo keys work as ever: End then Return chooses the last option, a
        // letter jumps to the option it starts, Escape closes choosing nothing, Tab leaves.
        function test_open_combo_keeps_its_native_keyboard() {
            longCombo.currentIndex = 2;
            longCombo.forceActiveFocus();
            keyClick(Qt.Key_Space);
            tryVerify(function () { return longCombo.popup.opened; });
            var node = testContext.accessibleNode(longCombo);
            verify(node.inTree && node.focusable, "open: still a stop in the tree");
            compare(node.name, "Saved system", "open: still named");
            verify(longCombo.activeFocus, "open: the selector keeps the keyboard");
            keyClick(Qt.Key_End);
            keyClick(Qt.Key_Return);
            tryVerify(function () { return !longCombo.popup.visible; });
            compare(longCombo.currentIndex, 4, "End then Return chose the last option");
            keyClick(Qt.Key_Space);
            tryVerify(function () { return longCombo.popup.opened; });
            keyClick(Qt.Key_B);
            keyClick(Qt.Key_Return);
            tryVerify(function () { return !longCombo.popup.visible; });
            compare(longCombo.currentIndex, 1, "a letter jumped to its option");
            keyClick(Qt.Key_Space);
            tryVerify(function () { return longCombo.popup.opened; });
            keyClick(Qt.Key_Down);
            keyClick(Qt.Key_Escape);
            tryVerify(function () { return !longCombo.popup.visible; });
            compare(longCombo.currentIndex, 1, "Escape chose nothing");
            verify(longCombo.activeFocus, "and the selector keeps the keyboard");
            keyClick(Qt.Key_Space);
            tryVerify(function () { return longCombo.popup.opened; });
            // Tab is Qt's own: every stop beneath an open list is gated while the list is on top, so it moves
            // nowhere, and the list and the keyboard stay as they were; Escape closes the list.
            keyClick(Qt.Key_Tab);
            keyClick(Qt.Key_Tab, Qt.ShiftModifier);
            wait(50);
            verify(longCombo.popup.visible && longCombo.activeFocus, "Tab and Shift+Tab leave the open list as it was");
            keyClick(Qt.Key_Escape);
            tryVerify(function () { return !longCombo.popup.visible; }, 1000, "Escape closes the list");
            verify(longCombo.activeFocus, "the selector keeps the keyboard");
            keyClick(Qt.Key_Tab);
            verify(emptyCombo.activeFocus, "closed, Tab moves on");
        }

        // Issue #902: Back in a list opened in a sheet closes only the list; the next Back closes the sheet.
        function test_back_closes_only_the_list_in_a_sheet() {
            sheet.visible = true;
            sheetCombo.forceActiveFocus();
            keyClick(Qt.Key_Space);
            tryVerify(function () { return sheetCombo.popup.opened; });
            keyClick(Qt.Key_Back);
            tryVerify(function () { return !sheetCombo.popup.visible; }, 1000, "Back closes the list");
            verify(sheet.visible, "and leaves the sheet open");
            verify(sheetCombo.activeFocus, "the selector has the keyboard");
            keyClick(Qt.Key_Back);
            tryVerify(function () { return !sheet.visible; }, 1000, "the next Back closes the sheet");
        }

        // Issue #902: a sheet that opens over the open list closes it; once the sheet is dismissed the keyboard is
        // the selector's again, to take the choice up where it was.
        function test_sheet_over_the_open_list_returns_to_the_selector() {
            longCombo.forceActiveFocus();
            keyClick(Qt.Key_Space);
            tryVerify(function () { return longCombo.popup.opened; });
            sheet.visible = true;
            verify(!longCombo.popup.visible, "the sheet closes the list");
            sheet.visible = false;
            tryVerify(function () { return longCombo.activeFocus; }, 1000, "dismissed, the keyboard is the selector's");
        }

        // Issue #902: a screen reader's focus on an option, then a sheet over the list: the list closes, and once
        // the sheet is dismissed the keyboard goes back to the selector that owns the option, not to the hidden row.
        function test_sheet_over_a_focused_option_returns_to_the_selector() {
            combo.currentIndex = 0;
            combo.popup.open();
            tryVerify(function () { return combo.popup.opened; });
            var row = comboRow(1);
            testContext.accessibleDoAction(row, testContext.setFocusAction);
            verify(row.activeFocus);
            sheet.visible = true;
            verify(!combo.popup.visible, "the sheet closes the list");
            sheet.visible = false;
            tryVerify(function () { return combo.activeFocus; }, 1000, "dismissed, the keyboard is the selector's");
            compare(combo.currentIndex, 0, "nothing was chosen");
        }

        // Issue #902: the selector is captured as the sheet opens over its focused option: the list's model rebuilt
        // while the sheet is open destroys the option, and the dismissed sheet still hands the keyboard to the
        // selector.
        function test_rebuilt_options_still_return_to_the_selector() {
            var model = combo.model;
            combo.currentIndex = 0;
            combo.popup.open();
            tryVerify(function () { return combo.popup.opened; });
            var row = comboRow(1);
            testContext.accessibleDoAction(row, testContext.setFocusAction);
            verify(row.activeFocus);
            sheet.visible = true;
            verify(!combo.popup.visible, "the sheet closes the list");
            combo.model = ["P25", "DMR", "NXDN"];
            wait(0);
            verify(sheet.previousFocus === null || sheet.previousFocus === undefined, "the option it opened from is gone");
            sheet.visible = false;
            try {
                tryVerify(function () { return combo.activeFocus; }, 1000, "dismissed, the keyboard is the selector's");
            } finally {
                combo.model = model;
            }
        }

        // Issue #902: a selector focused and its list opened after a sheet closed, before the sheet's deferred
        // hand-back runs: the hand-back leaves that newer choice alone, and the list stays open.
        function test_dismissed_sheet_keeps_a_newly_opened_list() {
            checkBox.forceActiveFocus();
            sheet.open();
            sheet.visible = false;
            verify(checkBox.activeFocus, "dismissed: the keyboard goes back to the opener");
            combo.forceActiveFocus();
            combo.popup.open();
            verify(combo.popup.visible);
            wait(50);
            verify(combo.popup.visible, "the newly opened list stays open");
            verify(combo.activeFocus, "and the selector keeps the keyboard");
            combo.popup.close();
            tryVerify(function () { return !combo.popup.visible; });
        }

        // Issue #902, one hand-back (a decided behaviour): a sheet closing while its opener is disabled hands the
        // keyboard, once its dismissal has settled, to the sheet below (the item that takes Escape and Back), and the
        // opener enabled again afterwards does not take it back.
        function test_opener_reachable_only_later_does_not_take_the_keyboard_back() {
            var button = findChild(sheet, "sheetButton");
            sheet.visible = true;
            button.forceActiveFocus();
            verify(button.activeFocus);
            nextSheet.open();
            button.enabled = false;
            nextSheet.visible = false;
            tryVerify(function () { return sheet.activeFocus; }, 1000, "the opener disabled: the sheet below takes the keyboard");
            button.enabled = true;
            wait(50);
            verify(sheet.activeFocus, "enabled again later: the keyboard stays where the hand-back put it");
            verify(!button.activeFocus);
            keyClick(Qt.Key_Escape);
            tryVerify(function () { return !sheet.visible; }, 1000, "Escape reaches the sheet below");
        }

        // Issue #902: a pin captured as a sheet's opener whose row StableRows lets go of (no longer live) while the
        // sheet is open is no place to return to, though still shown and enabled: the keyboard goes to the Back
        // owner, where Escape works.
        function test_opener_no_longer_live_is_not_returned_to() {
            pinButton.live = true;
            pinButton.forceActiveFocus();
            verify(pinButton.activeFocus);
            sheet.open();
            pinButton.live = false;
            try {
                verify(pinButton.visible && pinButton.enabled, "still shown and enabled");
                sheet.visible = false;
                verify(!pinButton.activeFocus, "the hand-back gives the pin let go of no keyboard");
                tryVerify(function () { return appBack.activeFocus; }, 1000, "the keyboard goes to the Back owner");
                verify(!pinButton.activeFocus, "nor afterwards");
                keyClick(Qt.Key_Escape);
                compare(appBack.escapes, 1, "Escape goes back");
            } finally {
                pinButton.live = true;
            }
        }

        // Issue #902: a lower sheet closing while a sheet above it has its field focused (the recordings sheet closing
        // itself as its list empties) takes nothing from that field: not its keyboard, not its input.
        function test_lower_sheet_closing_keeps_the_upper_field() {
            holder.forceActiveFocus();
            sheet.open();
            nextSheet.open();
            nextField.input.forceActiveFocus();
            verify(nextField.input.activeFocus);
            sheet.visible = false;
            verify(nextField.input.activeFocus, "the upper sheet's field keeps the keyboard");
            wait(50);
            verify(nextField.input.activeFocus, "and still has it once the close has settled");
            keyClick(Qt.Key_7);
            compare(nextField.text, "7", "typing reaches it");
            nextField.text = "";
        }

        // Issue #902: the same with the window away as the lower sheet closes: the focus the window returns to is the
        // upper sheet's field, not the lower sheet's opener.
        function test_lower_sheet_closing_away_keeps_the_upper_field() {
            holder.forceActiveFocus();
            sheet.open();
            nextSheet.open();
            nextField.input.forceActiveFocus();
            verify(nextField.input.activeFocus);
            testContext.sendWindowFocus(holder, false);
            try {
                sheet.visible = false;
                wait(50);
            } finally {
                testContext.sendWindowFocus(holder, true);
            }
            verify(nextField.input.activeFocus, "back: the upper sheet's field has the keyboard");
            verify(!holder.activeFocus, "not the lower sheet's opener");
            keyClick(Qt.Key_7);
            compare(nextField.text, "7", "typing reaches it");
            nextField.text = "";
        }

        // Issue #902: Space down on a button that hides before the key comes up: the release the gate takes also ends
        // the button's hold for Navigation, so a later repeat of another Space hold, with the button shown and
        // focused again, presses nothing.
        function test_release_taken_ends_the_held_activation() {
            iconButton.forceActiveFocus();
            testContext.sendKey(iconButton, Qt.Key_Space, 0, true, false);
            compare(Ui.Navigation.heldActivationItem, iconButton, "Space's hold is the button's");
            var presses = iconButton.presses;
            iconButton.visible = false;
            testContext.sendKey(iconButton, Qt.Key_Space, 0, false, false);
            compare(Ui.Navigation.heldActivationItem, null, "the taken release ends the hold");
            iconButton.visible = true;
            checkBox.forceActiveFocus();
            testContext.sendKey(checkBox, Qt.Key_Space, 0, true, false);
            iconButton.forceActiveFocus();
            testContext.sendKey(iconButton, Qt.Key_Space, 0, true, true);
            testContext.sendKey(iconButton, Qt.Key_Space, 0, false, false);
            compare(iconButton.presses, presses, "a repeat of another hold presses nothing");
            checkBox.checked = false;
        }

        // Issue #902: a sheet opened while the window is away records the focus the window returns to (a field) as
        // its opener, though no item has the active focus then: dismissed, it hands the keyboard back to the field.
        function test_sheet_opened_away_returns_to_the_stored_focus() {
            textField.input.forceActiveFocus();
            verify(textField.input.activeFocus);
            testContext.sendWindowFocus(holder, false);
            try {
                sheet.open();
            } finally {
                testContext.sendWindowFocus(holder, true);
            }
            verify(sheet.visible);
            sheet.visible = false;
            tryVerify(function () { return textField.input.activeFocus; }, 1000, "the field has the keyboard back");
        }

        // Issue #902: a sheet closed while the window is away hands the field its stored focus; the field disabled
        // before the window returns is followed (PressGate.followStoredFocus()), so the window comes back with the
        // keyboard on the Back owner, where Escape works. The offscreen platform keeps every window active, so a
        // focus given while away turns active again here: this guards the outcome, not the stored-only path.
        function test_background_hand_back_is_followed() {
            textField.input.forceActiveFocus();
            sheet.open();
            testContext.sendWindowFocus(holder, false);
            try {
                sheet.visible = false;
                textField.enabled = false;
                wait(50);
            } finally {
                testContext.sendWindowFocus(holder, true);
            }
            try {
                verify(!textField.input.activeFocus, "the disabled field has no keyboard");
                tryVerify(function () { return appBack.activeFocus; }, 1000, "the Back owner has it");
                keyClick(Qt.Key_Escape);
                compare(appBack.escapes, 1, "Escape goes back");
            } finally {
                textField.enabled = true;
            }
        }

        // Issue #902: two visits to one sheet in a turn, each opener disabled at its dismissal: only the latest
        // visit's hand-back runs. The first visit's opener enabled again before the turn ends gets nothing; the
        // latest visit's opener still disabled, the keyboard goes to the Back owner.
        function test_only_the_latest_visit_hands_back() {
            checkBox.forceActiveFocus();
            sheet.open();
            checkBox.enabled = false;
            sheet.visible = false;
            holder.forceActiveFocus();
            sheet.open();
            holder.enabled = false;
            sheet.visible = false;
            checkBox.enabled = true;
            try {
                tryVerify(function () { return appBack.activeFocus; }, 1000, "the latest visit hands the keyboard to the Back owner");
                verify(!checkBox.activeFocus, "the first visit's opener gets nothing");
            } finally {
                holder.enabled = true;
            }
        }

        // Issue #902: only a real change of navigation closes an open list. A selector destroyed while hidden, its
        // list never opened, takes nothing out of the modals, and the same entries assigned again (a binding
        // re-evaluated) are no change either: the list stays open.
        function test_open_list_stays_open_across_no_navigation_change() {
            longCombo.forceActiveFocus();
            keyClick(Qt.Key_Space);
            tryVerify(function () { return longCombo.popup.opened; });
            var changes = Ui.Navigation.changes;
            var spare = spareCombo.createObject(root);
            spare.destroy();
            wait(50);
            Ui.Navigation.modals = Ui.Navigation.modals.slice();
            Ui.Navigation.layers = Ui.Navigation.layers.slice();
            Ui.Navigation.rootSurfaces = Ui.Navigation.rootSurfaces.slice();
            compare(Ui.Navigation.changes, changes, "no stack changed");
            verify(longCombo.popup.visible, "the list stays open");
            longCombo.popup.close();
        }

        // Issue #902: a field hidden while composing, its window leaving in the same turn (before the release):
        // the word is committed all the same, into that field, though it is no longer the focus object.
        function test_hidden_field_commits_when_the_window_leaves() {
            var field = textField.input;
            field.text = "";
            field.forceActiveFocus();
            verify(testContext.composeText("final"), "composing");
            textField.visible = false;
            testContext.sendWindowFocus(holder, false);
            tryVerify(function () { return !field.focus; }, 1000, "the hidden field lets the focus go");
            compare(field.text, "final", "the word is committed");
            compare(field.preeditText, "", "nothing is left composing");
            testContext.sendWindowFocus(holder, true);
        }

        // Issue #902: the open list closes on a real change of navigation only: the same root surfaces assigned
        // again keep it open; a modal removed below it closes it.
        function test_combo_list_follows_real_navigation_changes() {
            combo.currentIndex = 0;
            combo.popup.open();
            tryVerify(function () { return combo.popup.opened; });
            Ui.Navigation.rootSurfaces = Ui.Navigation.rootSurfaces.slice();
            verify(combo.popup.visible, "the same root surfaces again: the list stays open");
            combo.popup.close();
            tryVerify(function () { return !combo.popup.visible; });
            Ui.Navigation.modals = [under];
            sheet.visible = true;
            sheetCombo.popup.open();
            tryVerify(function () { return sheetCombo.popup.opened; });
            Ui.Navigation.removeModal(under);
            verify(!sheetCombo.popup.visible, "a modal removed below it: the list closes");
        }

        // Issue #902: a field hidden while composing whose keyboard another field takes before the release runs:
        // the hidden field's word is committed all the same.
        function test_hidden_field_commits_after_focus_moved_on() {
            var field = textField.input;
            field.text = "";
            field.forceActiveFocus();
            verify(testContext.composeText("xyz"), "composing");
            textField.visible = false;
            plexInput.forceActiveFocus();
            wait(50);
            compare(field.text, "xyz", "the hidden field's word is committed");
            compare(field.preeditText, "", "nothing is left composing");
            verify(plexInput.activeFocus, "the keyboard stays where it moved");
        }

        // Issue #902: a field hidden while composing whose keyboard another field takes in the same turn: the edit
        // it publishes as it loses the focus (editingFinished, which a form saves on) already holds the word.
        function test_hidden_field_publishes_its_whole_word() {
            var field = textField.input;
            field.text = "foo";
            field.forceActiveFocus();
            field.cursorPosition = 3;
            verify(testContext.composeText("bar"), "composing");
            fieldFinished.clear();
            var published = [];
            var record = function () { published.push(textField.text); };
            textField.editingFinished.connect(record);
            textField.visible = false;
            plexInput.forceActiveFocus();
            wait(50);
            textField.editingFinished.disconnect(record);
            verify(published.length > 0, "the field finished its edit");
            compare(published[0], "foobar", "and published the whole word");
            compare(field.text, "foobar");
        }

        // Issue #902: a sheet closing hands the keyboard back on the next turn as well; by then a sheet opened as it
        // closed has given its field the keyboard, which it keeps.
        function test_next_sheet_keeps_its_fields_keyboard() {
            holder.forceActiveFocus();
            sheet.visible = true;
            sheet.visible = false;
            nextSheet.visible = true;
            nextField.input.forceActiveFocus();
            verify(nextField.input.activeFocus);
            wait(50);
            verify(nextField.input.activeFocus, "the next sheet's field keeps the keyboard");
            keyClick(Qt.Key_7);
            compare(nextField.text, "7", "and typing reaches it");
            nextField.text = "";
        }

        // Issue #902: the keyboard given to another control in reach as a sheet closes (the user's choice, before the
        // sheet's deferred hand-back runs) stays there; the opener does not take it back.
        function test_dismissed_sheet_keeps_a_newer_focus_choice() {
            combo.forceActiveFocus();
            sheet.open();
            verify(!combo.activeFocus, "the sheet takes the keyboard");
            sheet.visible = false;
            verify(combo.activeFocus, "dismissed: the keyboard goes back to the opener");
            textField.input.forceActiveFocus();
            verify(textField.input.activeFocus);
            wait(50);
            verify(textField.input.activeFocus, "the field given the keyboard keeps it");
            keyClick(Qt.Key_7);
            compare(textField.text, "7", "and typing reaches it");
            textField.text = "";
        }

        // Issue #902: a list with no option closes on Return, choosing nothing.
        function test_empty_list_closes_on_return() {
            emptyCombo.forceActiveFocus();
            keyClick(Qt.Key_Space);
            tryVerify(function () { return emptyCombo.popup.opened; });
            keyClick(Qt.Key_Return);
            tryVerify(function () { return !emptyCombo.popup.visible; }, 1000, "Return closes the empty list");
            compare(emptyCombo.currentIndex, -1, "choosing nothing");
        }

        // Issue #902: any change of navigation while the list is open (a layer over its screen, a new root surface)
        // closes it, so a tap where an option was chooses nothing on a screen no longer on top.
        function test_combo_list_closes_on_any_navigation_change_data() {
            return [{tag: "layer", how: "layer"}, {tag: "root surface", how: "root"}];
        }
        function test_combo_list_closes_on_any_navigation_change(data) {
            combo.currentIndex = 0;
            combo.popup.open();
            tryVerify(function () { return combo.popup.opened; });
            var row = comboRow(1);
            testContext.accessibleDoAction(row, testContext.setFocusAction);
            var tap = row.mapToItem(root, row.width / 2, row.height / 2);
            if (data.how === "layer")
                Ui.Navigation.layers = [{surface: loadedLayer}];
            else
                Ui.Navigation.rootSurfaces = [loadedLayer];
            verify(!combo.popup.visible, data.tag + ": the list closes at once");
            mouseClick(root, tap.x, tap.y);
            compare(combo.currentIndex, 0, data.tag + ": a tap where the option was chooses nothing");
            Ui.Navigation.layers = [];
            Ui.Navigation.rootSurfaces = [];
        }

        // Issue #902: Space down on an option a screen reader focused presses the option's delegate (the key goes up
        // from the row). A sheet before the key comes up lets go of that press, so the list opened again holds no
        // pressed option, and a lone Space release chooses nothing.
        function test_combo_option_press_is_let_go_under_a_sheet() {
            combo.currentIndex = 0;
            combo.popup.open();
            tryVerify(function () { return combo.popup.opened; });
            var row = comboRow(1);
            testContext.accessibleDoAction(row, testContext.setFocusAction);
            testContext.sendKey(row, Qt.Key_Space, 0, true, false);
            verify(row.parent.pressed, "Space is down on the option");
            sheet.visible = true;
            verify(!combo.popup.visible, "the list closes");
            verify(!row.parent.pressed && !row.parent.down, "the option's press is let go of");
            sheet.visible = false;
            combo.popup.open();
            tryVerify(function () { return combo.popup.opened; });
            row = comboRow(1);
            verify(!row.parent.pressed && !row.parent.down, "open again: no option is pressed");
            testContext.accessibleDoAction(row, testContext.setFocusAction);
            testContext.sendKey(row, Qt.Key_Space, 0, false, false);
            compare(combo.currentIndex, 0, "a lone Space release chooses nothing");
        }

        // A hidden coarse-tuning rail retunes nothing, by a screen reader's step or by an arrow key.
        function test_hidden_rail_retunes_nothing() {
            bandRail.forceActiveFocus();
            bandRail.visible = false;
            var tunes = bandRail.tunes;
            testContext.accessibleDoAction(bandRail, "Increase");
            testContext.accessibleDoAction(bandRail, "Decrease");
            bandRail.nudge(1e6);
            keyClick(Qt.Key_Right);
            compare(bandRail.tunes, tunes, "hidden: no step tunes");
            bandRail.visible = true;
            bandRail.nudge(1e6);
            verify(bandRail.tunes > tunes, "shown: the same step tunes");
        }

        // A field that holds the window's stored focus while another window is active, and hides then, has nothing
        // to take the keyboard back to once the window returns.
        function test_field_hidden_while_the_window_is_away() {
            textField.input.forceActiveFocus();
            testContext.sendWindowFocus(holder, false);
            textField.visible = false;
            tryVerify(function () { return !textField.input.focus; }, 1000, "the hidden field lets its stored focus go");
            testContext.sendWindowFocus(holder, true);
            var text = textField.input.text;
            keyClick(Qt.Key_7);
            verify(!textField.input.activeFocus, "the window back, the hidden field has no keyboard");
            compare(textField.input.text, text, "and typing edits nothing");
        }

        // A caller's own shown state goes into a text's reachability: kept but not shown, it is a plain pane with
        // no SetFocus, as hidden.
        function test_text_follows_its_callers_reachability() {
            var node = testContext.accessibleNode(heldLine);
            verify(node.focusable && node.inTree, "shown: a stop in the tree");
            heldLine.shown = false;
            node = testContext.accessibleNode(heldLine);
            verify(!node.inTree, "not shown: out of the tree");
            compare(node.name, "", "not shown: nameless");
            verify(!node.focusable && node.actions.indexOf(testContext.setFocusAction) < 0, "not shown: no stop, " + JSON.stringify(node.actions));
        }

        // Uncovered, a check box and a toggle still take a screen reader's double tap.
        function test_uncovered_press_still_acts() {
            verify(testContext.accessibleClick(checkBox));
            verify(checkBox.checked, "the check box toggled");
            verify(testContext.accessibleClick(toggle));
            verify(toggle.checked, "the toggle toggled");
        }
    }
}
