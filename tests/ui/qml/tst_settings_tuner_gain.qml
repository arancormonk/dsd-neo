// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest

// Settings > Radio defaults > Tuner gain: Automatic or a set 1-49 dB. Only a
// valid gain is ever written; Automatic writes 0, and Set brings back the last
// valid gain, never typed text that was refused. A change to the preference
// from elsewhere (a restore) shows here, after the field loses the focus if it
// has it.
Item {
    width: 360
    height: 900

    Loader {
        id: screenLoader
        anchors.fill: parent
    }

    // What the editor reported while the case typed and chose, in order.
    SignalSpy {
        id: draftSpy
        signalName: "draftEdited"
    }
    SignalSpy {
        id: modeSpy
        signalName: "modeSelected"
    }

    TestCase {
        name: "SettingsTunerGain"
        when: windowShown

        function init() {
            testContext.useLifecycleHost(true);
            prefs.gainDb = 30;
            load();
        }

        function cleanup() {
            draftSpy.target = null;
            modeSpy.target = null;
            verify(testContext.composeText(""));
            screenLoader.source = "";
            prefs.gainDb = 30;
            testContext.useLifecycleHost(false);
        }

        function load() {
            screenLoader.source = "";
            screenLoader.source = uiDir + "/SettingsScreen.qml";
            verify(screenLoader.item !== null);
            screenLoader.item.advancedOpen = true;
            waitForRendering(screenLoader.item);
        }

        function modeControl() {
            var control = findChild(screenLoader.item, "settingsGainMode");
            verify(control !== null, "settingsGainMode");
            return control;
        }
        function field() {
            var found = findChild(screenLoader.item, "settingsGainDb");
            verify(found !== null, "settingsGainDb");
            return found;
        }
        function segments(control) {
            var found = [];
            (function walk(node) {
                for (var child of node.children) {
                    if (child.modelData !== undefined && child.active !== undefined)
                        found.push(child);
                    else
                        walk(child);
                }
            })(control);
            return found;
        }
        // A segment chosen as a tap chooses it: the composing word is committed first.
        function choose(label) {
            var control = modeControl();
            var index = control.model.indexOf(label);
            verify(index >= 0, label);
            segments(control)[index].choose();
        }
        function shownMode() {
            var control = modeControl();
            return control.model[control.currentIndex];
        }
        function type(text) {
            var input = field().input;
            input.forceActiveFocus();
            input.selectAll();
            keyClick(Qt.Key_Backspace);
            for (var i = 0; i < text.length; ++i)
                keyClick(text.charAt(i));
            compare(field().text, text);
        }
        function leaveField() {
            screenLoader.item.forceActiveFocus();
            verify(!field().input.activeFocus);
            wait(0);
        }

        function test_settingsLayoutAndHint() {
            compare(JSON.stringify(modeControl().model), JSON.stringify(["Automatic", "Set"]));
            compare(modeControl().Accessible.name, "Tuner gain");
            compare(field().label, "Tuner gain");
            compare(field().unit, "dB");
            var hint = findChild(screenLoader.item, "settingsGainHint");
            verify(hint !== null && hint.visible);
            compare(hint.text, "For RTL-SDR and RTL-TCP. Automatic adjusts the receiver gain for you.");
            compare(hint.Accessible.name, hint.text, "the hint reads by itself");
            // It explains the choice: right under the strip, above the field.
            var strip = modeControl().mapToItem(null, 0, 0).y + modeControl().height;
            var at = hint.mapToItem(null, 0, 0).y;
            verify(at >= strip && at < field().mapToItem(null, 0, 0).y, "hint between the strip and the field");
            choose("Automatic");
            verify(hint.visible, "and under Automatic too");
        }

        function test_settingsAutomaticWritesZero() {
            compare(shownMode(), "Set");
            verify(field().visible);
            compare(field().text, "30");
            choose("Automatic");
            compare(prefs.gainDb, 0);
            compare(shownMode(), "Automatic");
            verify(!field().visible, "no gain to type under Automatic");
        }

        function test_settingsInvalidThenModes() {
            type("50");
            keyClick(Qt.Key_Return);
            compare(field().error, "Enter a gain from 1 to 49 dB.");
            compare(prefs.gainDb, 30, "a gain out of range is never written");
            type("0");
            keyClick(Qt.Key_Return);
            compare(prefs.gainDb, 30, "0 typed is not a way to Automatic");
            type("");
            keyClick(Qt.Key_Return);
            compare(prefs.gainDb, 30, "nor is an empty field");
            choose("Automatic");
            compare(prefs.gainDb, 0);
            choose("Set");
            compare(prefs.gainDb, 30, "Set comes back with the last valid gain");
            compare(field().text, "30", "not the refused text");
            compare(field().error, "");

            // Each valid commit is the new last valid gain.
            type("25");
            keyClick(Qt.Key_Return);
            compare(prefs.gainDb, 25);
            type("99");
            keyClick(Qt.Key_Return);
            compare(prefs.gainDb, 25);
            choose("Automatic");
            compare(prefs.gainDb, 0);
            choose("Set");
            compare(prefs.gainDb, 25);
            compare(field().text, "25");
        }

        function test_settingsExternalPrefsChange() {
            prefs.gainDb = 0;
            compare(shownMode(), "Automatic");
            prefs.gainDb = 41;
            compare(shownMode(), "Set");
            compare(field().text, "41");

            // While the field has the focus the change waits; a refused edit then gives way to it.
            type("50");
            prefs.gainDb = 12;
            compare(field().text, "50", "the typing is not replaced under the user's fingers");
            leaveField();
            compare(prefs.gainDb, 12, "the refused text was not written");
            compare(field().text, "12");
            compare(field().error, "");

            type("5");
            prefs.gainDb = 0;
            compare(shownMode(), "Set", "the mode waits too");
            type("50");
            leaveField();
            compare(shownMode(), "Automatic");
            compare(prefs.gainDb, 0);
            choose("Set");
            compare(prefs.gainDb, 12, "the last valid gain is the one the preference last held");
        }

        function test_settingsImeCommitBeforeAutomatic() {
            // Committed with Return, then Automatic: Automatic wins.
            type("25");
            keyClick(Qt.Key_Return);
            compare(prefs.gainDb, 25);
            choose("Automatic");
            compare(prefs.gainDb, 0);

            // A word still composing when Automatic is chosen is committed first, and its commit
            // (the field letting go of the focus as it hides) cannot undo Automatic.
            choose("Set");
            compare(prefs.gainDb, 25);
            type("");
            verify(testContext.composeText("33"), "the field holds the word as composing text");
            var editor = field().parent;
            verify(editor.draftEdited !== undefined, "the field's parent is the GainEditor");
            draftSpy.target = editor;
            modeSpy.target = editor;
            draftSpy.clear();
            modeSpy.clear();
            var order = [];
            editor.draftEdited.connect(function (text) { order.push("draft " + text); });
            editor.modeSelected.connect(function (mode) { order.push("mode " + mode); });
            choose("Automatic");
            compare(draftSpy.count, 1);
            compare(draftSpy.signalArguments[0][0], "33", "the composing word was committed as an edit");
            compare(modeSpy.count, 1);
            compare(JSON.stringify(order), JSON.stringify(["draft 33", "mode 1"]), "the word was committed before the choice");
            tryVerify(function () { return !field().input.activeFocus; }, 1000, "the hidden field gave up the focus");
            compare(prefs.gainDb, 0, "a commit after Automatic writes nothing");
            compare(shownMode(), "Automatic");
            choose("Set");
            compare(prefs.gainDb, 25, "the composed 33 was never a valid commit");
        }

        // Keys typed after Automatic go nowhere, and a change from elsewhere shows at once: no hidden
        // field holds it back.
        function test_settingsAutomaticLeavesNoHiddenField() {
            type("2");
            choose("Automatic");
            keyClick("4");
            keyClick(Qt.Key_Return);
            compare(prefs.gainDb, 0);
            tryVerify(function () { return !field().input.activeFocus; }, 1000, "the hidden field gave up the focus");
            compare(shownMode(), "Automatic");
            prefs.gainDb = 17;
            compare(shownMode(), "Set", "applied at once");
            compare(field().text, "17");
            choose("Automatic");
            choose("Set");
            compare(prefs.gainDb, 17);
        }

        // Set chosen again commits what was typed first: a valid gain is kept, composing or not.
        function test_settingsSetAgainKeepsValidText() {
            type("");
            verify(testContext.composeText("33"));
            choose("Set");
            compare(prefs.gainDb, 33);
            compare(field().text, "33");
            type("21");
            choose("Set");
            compare(prefs.gainDb, 21);
            compare(field().text, "21");
        }

        // Set chosen again over a refused edit goes back to the last valid gain.
        function test_settingsSetAgainOverRefusedText() {
            type("50");
            choose("Set");
            compare(prefs.gainDb, 30);
            compare(field().text, "30");
            compare(field().error, "");
            // The same for a refused word still composing when Set is chosen.
            type("");
            verify(testContext.composeText("77"));
            choose("Set");
            compare(prefs.gainDb, 30);
            compare(field().text, "30");
        }

        // A change from elsewhere during a valid edit waits; on focus loss the user's own edit, the
        // latest action, wins.
        function test_settingsValidEditWinsOverExternalChange() {
            type("33");
            prefs.gainDb = 12;
            compare(field().text, "33");
            leaveField();
            compare(prefs.gainDb, 33);
            compare(field().text, "33");
            compare(shownMode(), "Set");
        }

        // A field only focused, with nothing typed since it showed the preference, commits nothing when it
        // lets go: the change made elsewhere meanwhile stands.
        function test_settingsUntouchedFieldYieldsToExternalChange() {
            field().input.forceActiveFocus();
            prefs.gainDb = 0;
            compare(shownMode(), "Set", "the change waits for the focus");
            leaveField();
            compare(prefs.gainDb, 0, "the loaded 30 was not written back");
            compare(shownMode(), "Automatic");

            prefs.gainDb = 22;
            field().input.forceActiveFocus();
            prefs.gainDb = 41;
            leaveField();
            compare(prefs.gainDb, 41);
            compare(field().text, "41");

            // Return with nothing typed is no edit either.
            field().input.forceActiveFocus();
            prefs.gainDb = 9;
            keyClick(Qt.Key_Return);
            wait(0);
            compare(prefs.gainDb, 9);
            compare(field().text, "9");
        }

        function test_settingsReopen() {
            choose("Automatic");
            load();
            compare(shownMode(), "Automatic");
            verify(!field().visible);
            choose("Set");
            compare(prefs.gainDb, 30, "with no set gain to go back to, Set starts at 30");

            type("18");
            keyClick(Qt.Key_Return);
            load();
            compare(shownMode(), "Set");
            compare(field().text, "18");
        }
    }
}
