// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// Issue #782: what a screen reader finds in the key sheet.
//
// TalkBack builds its nodes from the accessibility tree: an item missing from its parent's children is never read,
// by touch or by swiping. The key sheet's target line, the note that the scan has left that target and the direct
// override paragraph are text a user needs before applying, and the scope choice has to say what it is set to, not
// only what it is for.
Item {
    width: 420
    height: 900
    Loader {
        id: loader
        anchors.fill: parent
        source: uiDir + "/KeySheet.qml"
    }
    Ui.PlexComboBox {
        id: unnamedChoice
        y: 860
        width: 240
        model: ["Use received identifiers", "Force privacy"]
    }
    SignalSpy {
        id: descriptionSpy
        target: unnamedChoice.Accessible
        signalName: "descriptionChanged"
    }
    TestCase {
        name: "A11yKeySheetNodes"
        when: windowShown
        readonly property var sheet: loader.item

        function item(name) {
            var found = findChild(sheet, name);
            verify(found !== null, name + " is missing");
            return found;
        }
        // Reachable, visible, read as static text, with @p name.
        function verifyRead(target, name, what) {
            var node = testContext.accessibleNode(target);
            verify(node.exists, what + " has no accessible interface");
            verify(node.inTree, what + " is not among its parent's accessible children");
            compare(node.role, Accessible.StaticText, what + " role");
            compare(node.name, name, what + " name");
            verify(!node.invisible, what + " is marked invisible");
            verify(!target.Accessible.ignored, what + " is ignored");
        }

        function initTestCase() {
            verify(testContext.accessibilityActive(), "run with DSD_QML_TEST_ACCESSIBILITY=1, or there is no tree");
            verify(sheet !== null, "KeySheet must load");
        }
        function init() {
            testContext.setHostRunning(true);
            testContext.resetCommands();
            sheet.visible = false;
            testContext.setMetric("scanTargetId", "target-2");
            sheet.targetName = "North site";
            sheet.open();
            verify(sheet.visible);
            waitForRendering(sheet);
        }
        function cleanup() {
            sheet.visible = false;
            sheet.targetName = "";
            testContext.setMetric("scanTargetId", "");
            testContext.setHostRunning(false);
        }

        function test_target_line_is_read() {
            var line = item("keySheetTargetName");
            verify(line.visible);
            verifyRead(line, "Applies to North site", "the target line");
            // Under session defaults it is hidden, and out of the reading order.
            var scope = item("keySheetScope");
            scope.currentIndex = 0;
            scope.activated(0);
            verify(!line.visible);
            verify(line.Accessible.ignored, "a hidden target line is skipped");
        }

        function test_scan_left_note_is_read_only_while_shown() {
            var note = item("keySheetTargetLeft");
            verify(!note.visible);
            verify(note.Accessible.ignored, "the hidden note is skipped");
            testContext.setMetric("scanTargetId", "target-3");
            verify(note.visible);
            verifyRead(note, "The scan has left North site. Reopen the editor before applying.", "the note");
            // Focusing Apply says it too, so a user who swipes straight to the button hears it before applying.
            var buttons = [];
            var walk = function (at) {
                for (var c of at.children) {
                    if (c.text === "Apply profile to this target" && typeof c.activate === "function")
                        buttons.push(c);
                    walk(c);
                }
            };
            walk(sheet);
            compare(buttons.length, 1);
            compare(testContext.accessibleNode(buttons[0]).description, note.text, "Apply carries the note");
        }

        function test_direct_override_note_is_read() {
            var note = item("directOverrideNote");
            verifyRead(note, note.text, "the direct override note");
        }

        // The scope choice is read by what it is for and what it is set to.
        function test_scope_choice_reads_its_value() {
            var scope = item("keySheetScope");
            var node = testContext.accessibleNode(scope);
            verify(node.exists && node.inTree, "the scope choice is not in the tree");
            compare(node.role, Accessible.ComboBox);
            compare(node.name, "Profile application scope");
            compare(node.description, "Active scan target");
            scope.currentIndex = 0;
            scope.activated(0);
            compare(testContext.accessibleNode(scope).description, "Session defaults");
        }

        // A choice with no name of its own is read by its value already: it gains nothing twice.
        function test_unnamed_choice_reads_as_before() {
            var node = testContext.accessibleNode(unnamedChoice);
            verify(node.exists);
            compare(node.name, "Use received identifiers");
            compare(node.description, "");
            descriptionSpy.clear();
            unnamedChoice.currentIndex = 1;
            node = testContext.accessibleNode(unnamedChoice);
            compare(node.name, "Force privacy");
            compare(node.description, "");
            compare(descriptionSpy.count, 0, "a value change never passes through a description");
            unnamedChoice.currentIndex = 0;
        }

        // The sheet's notices are read too, and only while shown.
        function test_notice_is_read() {
            sheet.notice = "Session defaults updated. Explicit scan profiles keep their own keys.";
            var notice = null;
            var walk = function (at) {
                for (var c of at.children) {
                    if (c.text === sheet.notice && c.activate === undefined)
                        notice = c;
                    walk(c);
                }
            };
            walk(sheet);
            verify(notice !== null);
            verifyRead(notice, sheet.notice, "the notice");
            sheet.notice = "";
            verify(notice.Accessible.ignored, "an empty notice is skipped");
        }
    }
}
