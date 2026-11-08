// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// Issue #646 (B4): TalkBack says which bottom tab is the screen shown. The
// tabs were page tabs marked selected, and Qt 6.11.3's Android bridge passes
// neither on (no tab role, no selected state), so TalkBack said only "Listen",
// "History", "Settings". Each is now a radio button, checkable and checked
// while its screen is shown, which the bridge does pass on ("checked, Listen,
// radio button"). `selected` stays set for platforms that read it.
Item {
    id: root

    width: 360
    height: 200

    Item {
        id: blocker
    }

    Ui.BottomNav {
        id: nav

        width: parent.width
    }

    TestCase {
        name: "BottomNavA11y"
        when: windowShown

        function cleanup() {
            Ui.Navigation.removeModal(blocker);
            nav.currentIndex = 0;
        }
        function tabs() {
            var found = [];
            (function walk(node) {
                for (var child of node.children) {
                    if (child.index !== undefined && child.modelData !== undefined)
                        found.push(child);
                    else
                        walk(child);
                }
            })(nav);
            found.sort(function (a, b) {
                return a.index - b.index;
            });
            return found;
        }

        function test_each_tab_says_whether_it_is_the_screen_shown() {
            var items = tabs();
            compare(items.length, 3);
            compare(items.map(function (tab) {
                return tab.Accessible.name;
            }).join(), "Listen,History,Settings", "the names carry no role or position words");
            for (var current = 0; current < 3; ++current) {
                nav.currentIndex = current;
                for (var tab of items) {
                    var tag = tab.modelData + " with tab " + current + " current";
                    compare(tab.Accessible.role, Accessible.RadioButton, tag);
                    verify(tab.Accessible.checkable, tag + " is checkable");
                    compare(tab.Accessible.checked, tab.index === current, tag + ": checked");
                    compare(tab.Accessible.selected, tab.index === current, tag + ": selected");
                }
            }
        }

        function test_choosing_a_tab_reports_it_and_a_sheet_hides_them() {
            var items = tabs();
            var chosen = [];
            var record = function (index) {
                chosen.push(index);
            };
            nav.selected.connect(record);
            items[2].Accessible.pressAction();
            compare(chosen, [2], "the accessibility press chooses the tab");
            for (var tab of items)
                verify(!tab.Accessible.ignored, tab.modelData + " is in the tree");
            Ui.Navigation.addModal(blocker);
            for (tab of items)
                verify(tab.Accessible.ignored, tab.modelData + " leaves the tree under a sheet");
            items[1].Accessible.pressAction();
            compare(chosen, [2], "a covered tab does nothing");
            nav.selected.disconnect(record);
        }
    }
}
