// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest

// The Map screen with accessibility active (issue #782 contract, issue #800 screen).
//
// A busy system reports positions every few seconds per radio while the map shows its canvas, or sits closed under
// the monitor: the List behind the canvas, or under a closed screen, builds no row and nothing under it shows or
// hides, since each delegate made under a hidden parent raises an event that costs Android's main thread blocking
// calls into the Qt thread. Open, a new fix for a radio and the ages moving update its row in place (a name or
// description change), never hide, show or move a row a screen reader is reading. The canvas is not in the tree.
// IDs, names and places are made up.
Item {
    width: 420
    height: 900
    Loader {
        id: screenLoader
        anchors.fill: parent
        source: uiDir + "/MapScreen.qml"
    }
    // A delegate is parented into the list's contentItem as it is created, so this counts every row built.
    SignalSpy {
        id: rowBuildSpy
        signalName: "childrenChanged"
    }
    TestCase {
        name: "A11yMap"
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
        // A busy minute: every radio moves, one's fix may be invalid, the clock runs on (ages move), and with
        // @p strangers each round an unconfirmed source sends too (a new single-point row each time).
        function busy(rounds, strangers) {
            for (var r = 0; r < rounds; ++r) {
                for (var i = 0; i < 6; ++i)
                    testContext.pushPosition({source: 3000 + i, label: "Unit " + i, lat: 10.0 + 0.001 * r, lon: 20.0 + 0.001 * i}, false);
                if (strangers)
                    testContext.pushPosition({anonymous: true, lat: 10.01, lon: 20.01 + 0.001 * r}, false);
                testContext.pushPosition({source: 3010, lat: 10.02, lon: 20.02, validityUnknown: true}, false);
                testContext.readPositions();
                testContext.setPositionWallSeconds(testContext.positionWallStart() + 5 * (r + 1));
                testContext.refreshPositionAges();
            }
        }

        function initTestCase() {
            verify(testContext.accessibilityActive(), "run with DSD_QML_TEST_ACCESSIBILITY=1, or nothing is counted");
            verify(screen !== null, "MapScreen.qml failed to load");
        }
        function init() {
            testContext.resetPositions();
            testContext.setPositionWallSeconds(testContext.positionWallStart());
            screen.visible = true;
            screen.mapShown = true;
        }
        function cleanup() {
            testContext.stopAccessibilityCount();
            rowBuildSpy.target = null;
            testContext.resetPositions();
            screen.visible = true;
            screen.mapShown = true;
        }

        // The canvas is never a node; its words are the List's.
        function test_canvas_is_not_in_the_tree() {
            verify(!testContext.accessibleNode(item("mapCanvas")).inTree);
        }

        // Behind the canvas the List holds no rows: fixes land, ages move, a filter comes and goes, and nothing is
        // built or shown. Closed, likewise. Shown, it lists them all.
        function test_hidden_list_builds_no_rows() {
            var view = item("mapList");
            // Read once, as a listener does before going back to the map: a view that kept its model would now keep
            // its rows and build more under the canvas.
            busy(1, true);
            screen.mapShown = false;
            tryVerify(function () { return view.itemAtIndex(0) !== null; });
            screen.mapShown = true;
            rowBuildSpy.target = view.contentItem;
            rowBuildSpy.clear();
            testContext.startAccessibilityCount();
            busy(10, true);
            positions.ageFilterSeconds = 900;
            positions.ageFilterSeconds = 0;
            screen.visible = false;
            busy(10, true);
            compare(rowBuildSpy.count, 0, "rows were built under the canvas or the closed screen");
            verify(view.itemAtIndex(0) === null, "a row exists while hidden");
            compare(showHide(view), 0, churn());

            screen.visible = true;
            screen.mapShown = false;
            compare(view.count, positionView.count, "every radio the filter shows");
            tryVerify(function () { return view.itemAtIndex(0) !== null; });
            verify(rowBuildSpy.count > 0, "opening builds the rows, and the spy sees it");
        }

        // Open, new fixes for the radios listed and the ages moving rename rows in place: no row built, no show, hide
        // or move under the screen, the same row object, and its node carries the words.
        function test_open_rows_update_in_place() {
            busy(1, true);
            screen.mapShown = false;
            var view = item("mapList");
            tryVerify(function () { return view.itemAtIndex(1) !== null; });
            var row = view.itemAtIndex(1);
            var key = row.rowTrack;
            rowBuildSpy.target = view.contentItem;
            rowBuildSpy.clear();
            testContext.startAccessibilityCount();
            busy(8, false);
            wait(20);
            compare(rowBuildSpy.count, 0, "no row was built");
            compare(showHide(screen), 0, churn());
            compare(count(screen, "LocationChanged"), 0, churn());
            compare(view.itemAtIndex(1), row, "the same row");
            compare(row.rowTrack, key, "for the same radio");
            var node = testContext.accessibleNode(row.node);
            verify(node.inTree && node.focusable);
            verify(node.name.indexOf("Unit") === 0 || node.name.indexOf("Radio") === 0 || node.name.indexOf("Unconfirmed") === 0, node.name);
        }

        // Review P7 #1: an open detail sheet changes in place. The clock moving (every second while the map shows)
        // and a new fix for its radio rename lines; no line is built or destroyed, nothing under the sheet shows or
        // hides, and each line is the same object it was.
        function test_open_sheet_updates_in_place() {
            testContext.pushPosition({source: 4001, label: "Engine 7", lat: 10.0, lon: 20.0, errM: 20, speedDkmh: 300,
                                      headingDdeg: 900, altitudeDm: 100, targetId: 100, group: true, fixAgeSeconds: 3});
            var id = 0;
            for (var t = 1; t < 20000 && id === 0; t++) {
                if (positions.trackFor(t).label === "Engine 7")
                    id = t;
            }
            verify(id > 0);
            screen.openTrack(id, false);
            var sheet = item("mapPositionSheet");
            verify(sheet.visible);
            // Every text the sheet holds, by object, and the Speed line among them.
            var texts = function () {
                var found = [];
                var walk = function (node) {
                    if (!node)
                        return;
                    if (node.text !== undefined && typeof node.text === "string")
                        found.push(node);
                    for (var c = 0; c < node.children.length; c++)
                        walk(node.children[c]);
                    if (node.contentItem)
                        walk(node.contentItem);
                };
                walk(sheet);
                return found;
            };
            var lines = texts();
            var speed = lines.filter(function (line) { return line.visible && line.text.indexOf("Speed") === 0; })[0];
            verify(speed !== undefined, "the Speed line shows");
            testContext.startAccessibilityCount();
            for (var r = 1; r <= 10; r++) {
                testContext.setPositionWallSeconds(testContext.positionWallStart() + r);
                testContext.refreshPositionAges();
                if (r % 3 === 0)
                    testContext.pushPosition({source: 4001, label: "Engine 7", lat: 10.0 + 0.001 * r, lon: 20.0, errM: 20,
                                              speedDkmh: 300 + r, headingDdeg: 900, altitudeDm: 100, targetId: 100,
                                              group: true, fixAgeSeconds: 3});
            }
            wait(20);
            compare(showHide(sheet), 0, churn());
            var after = texts();
            compare(after.length, lines.length, "no line built or destroyed");
            for (var i = 0; i < lines.length; i++)
                verify(after[i] === lines[i], "line " + i + " is the same object");
            verify(speed.text.indexOf("Speed") === 0, "renamed in place: " + speed.text);
            var node = testContext.accessibleNode(speed);
            verify(node.inTree);
            sheet.visible = false;
        }

        // Review P7 #4: the History pin is a List row a screen reader reaches, named and pressable.
        function test_pin_row_is_a_node() {
            screen.showPin({uid: 9, name: "CALL 009", when: 1700000000, position: {latitude: 10.0, longitude: 20.0,
                            kindText: "LIP", validityUnknown: true}});
            screen.mapShown = false;
            var row = item("mapPinRow");
            var node = testContext.accessibleNode(row);
            verify(node.inTree && node.focusable);
            compare(node.name, "Saved position from History: CALL 009");
            verify(testContext.pressAccessible(row));
            verify(item("mapPositionSheet").visible);
            item("mapPositionSheet").visible = false;
            var chip = item("mapPinChipAction");
            screen.mapShown = true;
            verify(testContext.accessibleNode(chip).inTree, "on the map, its chip's button is the node");
            screen.clearPin();
        }

        // Each age pill is a checkable node; only the chosen one is checked.
        function test_age_pills_say_which_is_checked() {
            for (var i = 0; i < 3; ++i) {
                var node = testContext.accessibleNode(item("mapAge" + i));
                verify(node.inTree && node.checkable, "pill " + i);
                compare(node.checked, i === 2, "pill " + i);
            }
            verify(testContext.pressAccessible(item("mapAge0")));
            compare(positions.ageFilterSeconds, 900);
            compare(testContext.accessibleNode(item("mapAge0")).checked, true);
            positions.ageFilterSeconds = 0;
        }
    }
}
