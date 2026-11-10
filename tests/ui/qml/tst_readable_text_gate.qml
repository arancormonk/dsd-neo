// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// #830: a text a screen reader reaches carries its name whenever it is in the
// tree. Its name follows its own navigation gate, never `Accessible.ignored`
// read back: a ListView writes that flag on each delegate it lays out
// (QQuickItemViewFxItem::setVisible sets the item's accessible bit with no
// change signal), so a row made while a sheet covered it was put back in the
// tree with the empty name it had then, and kept it once the sheet closed,
// since its gate then set the flag to what it already was.
Item {
    id: root

    width: 360
    height: 640

    ListModel {
        id: rows
    }

    // What a sheet over the screen leaves reachable: only itself.
    Item {
        id: cover
    }

    Column {
        id: page

        anchors.fill: parent

        Ui.ReadableText {
            id: lone
            text: "A line on its own"
        }

        ListView {
            id: list

            width: parent.width
            height: 300
            model: rows
            delegate: Ui.ReadableText {
                required property string line
                width: ListView.view.width
                text: line
            }
        }
    }

    Loader {
        id: diagnostics

        width: root.width
        height: root.height
        active: false
        source: uiDir + "/DiagnosticsScreen.qml"
    }

    TestCase {
        id: tc

        name: "ReadableTextGate"
        when: windowShown

        function cleanup() {
            Ui.Navigation.modals = [];
            rows.clear();
            diagnostics.active = false;
        }
        function delegates(view) {
            var made = [];
            var kids = view.contentItem.children;
            for (var i = 0; i < kids.length; ++i) {
                if (kids[i].text !== undefined && kids[i].visible)
                    made.push(kids[i]);
            }
            return made;
        }

        // Outside a list the gate's own change renames the text both ways.
        function test_a_lone_text_is_named_again_once_uncovered() {
            compare(lone.Accessible.name, "A line on its own");
            Ui.Navigation.modals = [cover];
            compare(lone.Accessible.name, "");
            Ui.Navigation.modals = [];
            compare(lone.Accessible.name, "A line on its own");
        }

        // Rows made while a sheet covers the list, then laid out again: once
        // the sheet closes each one is named by its text.
        function test_rows_made_while_covered_are_named_once_uncovered() {
            Ui.Navigation.modals = [cover];
            for (var i = 0; i < 4; ++i)
                rows.append({line: "Row " + i});
            waitForRendering(list);
            rows.append({line: "Row 4"});
            waitForRendering(list);
            Ui.Navigation.modals = [];
            var made = delegates(list);
            compare(made.length, 5);
            for (var r = 0; r < made.length; ++r)
                compare(made[r].Accessible.name, made[r].text, "row " + r + " has no name");
        }

        // Diagnostics: lines that came in while the log streamed hold no name
        // then (issue #782) and are named once Pause holds the log.
        function test_diagnostics_lines_are_named_once_paused() {
            diagnosticsLog.paused = false;
            diagnostics.active = true;
            diagnostics.item.visible = true;
            for (var i = 0; i < 3; ++i)
                testContext.pushDiagnostic("gate line " + i);
            diagnosticsLog.refresh();
            var view = null;
            (function look(item) {
                for (var k = 0; k < item.children.length && view === null; ++k) {
                    if (item.children[k].contentItem !== undefined && item.children[k].model === diagnosticsLog)
                        view = item.children[k];
                    else
                        look(item.children[k]);
                }
            })(diagnostics.item);
            verify(view !== null, "the log list is missing");
            tryVerify(function () { return delegates(view).length > 0; });
            waitForRendering(view);
            var streaming = delegates(view);
            for (var s = 0; s < streaming.length; ++s)
                compare(streaming[s].Accessible.name, "", "a streaming line " + s + " is named");
            diagnosticsLog.paused = true;
            var made = delegates(view);
            verify(made.length > 0);
            for (var r = 0; r < made.length; ++r) {
                verify(!made[r].Accessible.ignored, "line " + r + " is out of the tree while paused");
                compare(made[r].Accessible.name, made[r].text, "line " + r + " has no name");
            }
            diagnosticsLog.paused = false;
        }
    }
}
