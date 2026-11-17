// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest

// The Activity screen with accessibility active (issue #782 contract).
//
// A busy control channel lands unit activity every few hundred milliseconds while the screen sits hidden under the
// monitor: nothing may be built or shown under it, since each delegate made under a hidden parent raises an event
// that costs Android's main thread blocking calls into the Qt thread. Open, a message merged into a row and a name
// learned later rename that row in place (a name or description change), never hide, show or move a row a screen
// reader is reading; and each filter pill says whether it is the checked one. IDs and names are made up.
Item {
    width: 420
    height: 900
    Loader {
        id: screenLoader
        anchors.fill: parent
        source: uiDir + "/ActivityScreen.qml"
    }
    // A delegate is parented into the list's contentItem as it is created, so this counts every row built.
    SignalSpy {
        id: rowBuildSpy
        signalName: "childrenChanged"
    }
    TestCase {
        name: "A11yActivity"
        when: windowShown
        readonly property var screen: screenLoader.item

        function item(name) {
            var found = findChild(screen, name);
            verify(found !== null, name + " is missing");
            return found;
        }
        function count(scope, type) {
            return testContext.accessibilityCountWithin(scope, type);
        }
        function showHide(scope) {
            return count(scope, "ObjectShow") + count(scope, "ObjectHide");
        }
        function churn() {
            var counts = testContext.accessibilityCount();
            return JSON.stringify(Object.keys(counts.chains).filter(function (chain) {
                return /^(ObjectShow|ObjectHide|LocationChanged) /.test(chain);
            }).map(function (chain) {
                return counts.chains[chain] + "x " + chain;
            }));
        }

        function initTestCase() {
            verify(testContext.accessibilityActive(), "run with DSD_QML_TEST_ACCESSIBILITY=1, or nothing is counted");
            verify(screen !== null, "ActivityScreen.qml failed to load");
        }
        function init() {
            testContext.clearUnitActivity();
            screen.visible = true;
        }
        function cleanup() {
            testContext.stopAccessibilityCount();
            rowBuildSpy.target = null;
            testContext.clearUnitActivity();
            screen.visible = true;
        }

        // Hidden, a busy site's messages land (new rows, merges, a loss and a name learned): no row is built and
        // nothing under the screen shows or hides. Shown again, it lists them all.
        function test_hidden_screen_builds_no_rows() {
            var view = item("activityList");
            verify(testContext.pushUnitActivity({kind: "registration", subject: 1}));
            tryCompare(view, "count", 1);
            screen.visible = false;
            rowBuildSpy.target = view.contentItem;
            rowBuildSpy.clear();
            testContext.startAccessibilityCount();
            for (var i = 0; i < 40; ++i) {
                verify(testContext.pushUnitActivity({kind: "affiliation", subject: 1000 + (i % 8), group: 100}));
                verify(testContext.pushUnitActivity({kind: "kill", subject: 2000 + i}));
            }
            for (i = 0; i < 1030; ++i)
                verify(testContext.pushUnitActivity({kind: "registration", subject: 100000 + i}, false));
            testContext.readUnitActivity();
            verify(testContext.learnUnitActivityName(1000, "Engine 7"));
            compare(rowBuildSpy.count, 0, "rows were built under the hidden screen");
            verify(view.itemAtIndex(0) === null, "a row exists under the hidden screen");
            compare(showHide(screen), 0, churn());

            screen.visible = true;
            compare(view.count, unitActivity.count + 1, "every row and the gap row");
            tryVerify(function () { return view.itemAtIndex(0) !== null; });
            verify(rowBuildSpy.count > 0, "opening builds the rows, and the spy sees it");
        }

        // Open, a merge and a late name rename the row they are on: no show, hide or move under the screen, no row
        // built, and the row's node carries the new words.
        function test_open_rows_rename_in_place() {
            var view = item("activityList");
            verify(testContext.pushUnitActivity({kind: "affiliation", subject: 1234567, group: 100}));
            verify(testContext.pushUnitActivity({kind: "registration", subject: 7654321}));
            tryCompare(view, "count", 2);
            tryVerify(function () { return view.itemAtIndex(1) !== null; });
            var row = view.itemAtIndex(1);
            rowBuildSpy.target = view.contentItem;
            rowBuildSpy.clear();
            testContext.startAccessibilityCount();
            verify(testContext.pushUnitActivity({kind: "affiliation", subject: 1234567, group: 100}));
            verify(testContext.learnUnitActivityName(1234567, "Engine 7"));
            compare(unitActivity.count, 2, "the message merged into its row");
            wait(20);
            compare(rowBuildSpy.count, 0, "no row was built");
            compare(showHide(screen), 0, churn());
            compare(count(screen, "LocationChanged"), 0, churn());
            compare(view.itemAtIndex(1), row, "the same row");
            var node = testContext.accessibleNode(row);
            verify(node.inTree && node.focusable);
            verify(node.name.indexOf("Engine 7 (1234567)") === 0, node.name);
            verify(node.description.indexOf("heard 2 times") >= 0, node.description);
        }

        // Each pill is a checkable node; only the chosen one is checked, and pressing one as TalkBack does moves it.
        function test_pills_say_which_is_checked() {
            for (var i = 0; i < 6; ++i) {
                var node = testContext.accessibleNode(item("activityFilter" + i));
                verify(node.inTree && node.checkable, "pill " + i);
                compare(node.checked, i === 0, "pill " + i);
            }
            verify(testContext.pressAccessible(item("activityFilter2")));
            compare(activityView.filter, 2);
            compare(testContext.accessibleNode(item("activityFilter2")).checked, true);
            compare(testContext.accessibleNode(item("activityFilter0")).checked, false);
        }
    }
}
