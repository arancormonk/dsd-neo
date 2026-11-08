// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// Issue #646: the Listen tabs strip on its own. It is one Tab stop, the
// current tab; Left, Right, Home and End move the focus to a tab and choose
// it, held at the ends; Return, keypad Enter and Space choose the focused tab;
// a pointer choice made while a tab has the focus takes the focus along. Every
// way of choosing goes through one guard (navigation, enabled, visible), each
// tab is a checkable radio button, and the labels keep whole words at every
// text size with the strip's height following the tallest.
Item {
    id: root

    width: 360
    height: 400

    // Stands in for an open sheet: the top modal, which the strip is not in.
    Item {
        id: blocker

        y: 360
        width: 100
        height: 30

        Item {
            id: sheetStop

            anchors.fill: parent
            activeFocusOnTab: true
        }
    }

    Column {
        width: root.width

        Item {
            id: before

            objectName: "before"
            width: parent.width
            height: 20
            activeFocusOnTab: true
        }
        Ui.ListenTabs {
            id: strip

            x: Ui.Theme.screenPadding
            width: parent.width - 2 * Ui.Theme.screenPadding
            model: [{id: "systems", label: "Systems"}, {id: "scanLists", label: "Scan lists"}, {id: "explore", label: "Explore"}]
            currentId: "systems"
        }
        Item {
            id: after

            objectName: "after"
            width: parent.width
            height: 20
            activeFocusOnTab: true
        }
    }

    // The caller's side: it makes the chosen tab current, and records
    // whether the chosen tab already had the focus when it was chosen.
    property var chosen: []
    property bool follow: true
    Connections {
        target: strip
        function onSelected(id) {
            root.chosen.push({id: id, focused: tc.tabOf(id).activeFocus});
            if (root.follow)
                strip.currentId = id;
        }
    }

    Component {
        id: freshStrip

        Ui.ListenTabs {
            width: 300
            model: [{id: "systems", label: "Systems"}, {id: "scanLists", label: "Scan lists"}, {id: "explore", label: "Explore"}]
            currentId: "explore"
        }
    }

    // The oracle for a label's height in the selected weight.
    Text {
        id: probe

        visible: false
        font.weight: Font.DemiBold
    }
    TextMetrics {
        id: wordMetrics
    }

    TestCase {
        id: tc

        name: "ListenTabs"
        when: windowShown

        readonly property var ids: ["systems", "scanLists", "explore"]

        function init() {
            failOnWarning(/activeFocusOnTab/);
        }
        function cleanup() {
            Ui.Navigation.removeModal(blocker);
            Ui.Theme.resetFontScale();
            root.width = 360;
            strip.enabled = true;
            strip.visible = true;
            root.follow = true;
            strip.currentId = "systems";
            root.chosen = [];
            before.forceActiveFocus();
        }

        function tabOf(id) {
            return findChild(strip, "listenTab_" + id);
        }
        function tabs() {
            return tc.ids.map(tabOf);
        }
        function chosenIds() {
            return root.chosen.map(function (entry) {
                return entry.id;
            });
        }
        function tabStops() {
            return tabs().filter(function (tab) {
                return tab.activeFocusOnTab;
            }).map(function (tab) {
                return tab.tabId;
            });
        }
        function labelOf(tab) {
            return findChild(tab, "listenTabLabel");
        }
        function indicatorOf(tab) {
            return findChild(tab, "listenTabIndicator");
        }
        function focusTab(id) {
            var tab = tabOf(id);
            tab.forceActiveFocus();
            verify(tab.activeFocus, id + " takes the focus");
            return tab;
        }

        // A strip built once the app is running (the Theme already made) fits
        // its labels without a script error: a label's change handlers that
        // ran while it was still being built found Math undefined.
        function test_a_strip_builds_without_script_errors() {
            failOnWarning(/TypeError/);
            Ui.Theme.fontScale = 1.0;
            var built = createTemporaryObject(freshStrip, root);
            verify(built !== null);
            wait(0);
            compare(built.labelPixelSize, 14, "the labels are fitted once built");
            compare(built.implicitHeight, Ui.Theme.minimumTouchSize);
        }

        function test_tabs_are_named_checkable_radio_buttons() {
            var items = tabs();
            for (var i = 0; i < items.length; ++i)
                verify(items[i] !== null, "listenTab_" + tc.ids[i] + " exists");
            compare(strip.objectName, "listenTabs");
            compare(items.map(function (tab) {
                return tab.Accessible.name;
            }).join(), "Systems,Scan lists,Explore", "names carry no role or position words");
            for (var current of tc.ids) {
                strip.currentId = current;
                for (var tab of items) {
                    var tag = tab.tabId + " with " + current + " current";
                    compare(tab.Accessible.role, Accessible.RadioButton, tag);
                    verify(tab.Accessible.checkable, tag + " is checkable");
                    compare(tab.Accessible.checked, tab.tabId === current, tag + ": checked");
                    compare(tab.Accessible.selected, tab.tabId === current, tag + ": selected");
                    verify(!tab.Accessible.ignored, tag + " is in the tree");
                    verify(tab.Accessible.focusable, tag + " is focusable");
                }
            }
        }

        // R-2: nothing in the strip scrolls (a Flickable is a sticky TalkBack stop).
        function test_the_strip_has_no_scrolling_container() {
            (function walk(node) {
                for (var child of node.children) {
                    verify(child.contentY === undefined, "a scrolling item in the strip: " + child);
                    walk(child);
                }
            })(strip);
        }

        function test_one_tab_stop_the_current_tab() {
            for (var current of tc.ids) {
                strip.currentId = current;
                compare(tabStops(), [current], "only the current tab is a Tab stop");
                before.forceActiveFocus();
                keyClick(Qt.Key_Tab);
                verify(tabOf(current).activeFocus, "Tab enters the strip at " + current);
                keyClick(Qt.Key_Tab);
                verify(after.activeFocus, "the next Tab leaves the strip from " + current);
                keyClick(Qt.Key_Backtab);
                verify(tabOf(current).activeFocus, "Backtab from below lands on " + current);
                keyClick(Qt.Key_Backtab);
                verify(before.activeFocus, "the next Backtab leaves the strip from " + current);
            }
        }

        // Each arrow moves the focus to the tab, then chooses it; no wrap.
        function test_arrows_home_and_end_move_then_choose() {
            focusTab("systems");
            keyClick(Qt.Key_Left);
            compare(chosenIds(), [], "Left on the first tab stays");
            verify(tabOf("systems").activeFocus);
            keyClick(Qt.Key_Right);
            keyClick(Qt.Key_Right);
            compare(chosenIds(), ["scanLists", "explore"]);
            keyClick(Qt.Key_Right);
            compare(chosenIds(), ["scanLists", "explore"], "Right on the last tab stays");
            verify(tabOf("explore").activeFocus);
            keyClick(Qt.Key_Home);
            compare(strip.currentId, "systems");
            verify(tabOf("systems").activeFocus, "Home moves to the first tab");
            keyClick(Qt.Key_End);
            compare(strip.currentId, "explore");
            verify(tabOf("explore").activeFocus, "End moves to the last tab");
            keyClick(Qt.Key_Left);
            compare(chosenIds(), ["scanLists", "explore", "systems", "explore", "scanLists"]);
            for (var entry of root.chosen)
                verify(entry.focused, entry.id + " had the focus before it was chosen");
            compare(tabStops(), ["scanLists"], "one Tab stop after the moves");
        }

        function test_return_enter_and_space_choose_the_focused_tab() {
            root.follow = false;
            focusTab("explore");
            keyClick(Qt.Key_Return);
            keyClick(Qt.Key_Enter, Qt.KeypadModifier);
            keyClick(Qt.Key_Space);
            compare(chosenIds(), ["explore", "explore", "explore"], "Return, keypad Enter and Space each choose it");
            root.follow = true;
            root.chosen = [];
            focusTab("systems");
            keyClick(Qt.Key_Return);
            keyClick(Qt.Key_Enter, Qt.KeypadModifier);
            keyClick(Qt.Key_Space);
            tabOf("systems").Accessible.pressAction();
            mouseClick(tabOf("systems"));
            compare(chosenIds(), [], "choosing the current tab again does nothing");
        }

        function test_pointer_choice_takes_the_focus_along() {
            focusTab("systems");
            mouseClick(tabOf("explore"));
            compare(chosenIds(), ["explore"]);
            verify(root.chosen[0].focused, "the focus moved before the choice");
            verify(tabOf("explore").activeFocus, "the chosen tab has the focus");
            compare(tabStops(), ["explore"], "exactly one Tab stop after a pointer choice");
            tabOf("scanLists").Accessible.pressAction();
            verify(tabOf("scanLists").activeFocus, "an accessibility press takes the focus along too");
            compare(tabStops(), ["scanLists"]);

            // With the focus elsewhere, a tap leaves it there.
            before.forceActiveFocus();
            mouseClick(tabOf("systems"));
            compare(strip.currentId, "systems");
            for (var tab of tabs())
                verify(!tab.activeFocus, tab.tabId + " took no focus from a tap");
        }

        // The caller changing the tab keeps the focus on the current one.
        function test_focus_follows_a_tab_the_caller_sets() {
            focusTab("systems");
            strip.currentId = "explore";
            verify(tabOf("explore").activeFocus);
            compare(tabStops(), ["explore"]);
            before.forceActiveFocus();
            strip.currentId = "scanLists";
            verify(before.activeFocus, "a change made with the focus elsewhere leaves it there");
        }

        function test_focus_current() {
            strip.currentId = "scanLists";
            verify(strip.focusCurrent());
            verify(tabOf("scanLists").activeFocus);
            before.forceActiveFocus();
            Ui.Navigation.addModal(blocker);
            verify(!strip.focusCurrent(), "no focus under a sheet");
            verify(before.activeFocus);
        }

        function test_choosing_is_blocked_under_a_sheet() {
            var held = focusTab("systems");
            Ui.Navigation.addModal(blocker);
            verify(held.activeFocus, "the tab keeps the focus as the sheet opens");
            keyClick(Qt.Key_Right);
            keyClick(Qt.Key_End);
            keyClick(Qt.Key_Return);
            verify(held.activeFocus, "the arrows move nothing under a sheet");
            tabOf("explore").Accessible.pressAction();
            mouseClick(tabOf("scanLists"));
            compare(chosenIds(), [], "no key, press or tap chooses under a sheet");
            for (var tab of tabs()) {
                verify(tab.Accessible.ignored, tab.tabId + " leaves the tree under a sheet");
                verify(!tab.Accessible.focusable, tab.tabId + " is no swipe stop under a sheet");
            }
            sheetStop.forceActiveFocus();
            compare(tabStops(), [], "no Tab stop under a sheet once the focus leaves");
            Ui.Navigation.removeModal(blocker);
            compare(tabStops(), ["systems"]);
        }

        function test_choosing_is_blocked_while_disabled() {
            strip.enabled = false;
            tabOf("explore").Accessible.pressAction();
            mouseClick(tabOf("scanLists"));
            tabOf("scanLists").choose();
            compare(chosenIds(), [], "a disabled strip chooses nothing");
            compare(tabStops(), [], "a disabled strip is no Tab stop");
        }

        function test_choosing_is_blocked_while_hidden() {
            var held = focusTab("systems");
            strip.visible = false;
            keyClick(Qt.Key_Right);
            keyClick(Qt.Key_Return);
            tabOf("explore").Accessible.pressAction();
            compare(chosenIds(), [], "a hidden strip chooses nothing, by key or press");
            for (var tab of tabs())
                verify(tab.Accessible.ignored, tab.tabId + " leaves the tree while hidden");
            before.forceActiveFocus();
            compare(tabStops(), [], "a hidden strip is no Tab stop");
            verify(!held.activeFocus);
        }

        function test_current_tab_look_and_disabled_look() {
            for (var current of tc.ids) {
                strip.currentId = current;
                for (var tab of tabs()) {
                    var tag = tab.tabId + " with " + current + " current";
                    var label = labelOf(tab);
                    var bar = indicatorOf(tab);
                    var isCurrent = tab.tabId === current;
                    compare(bar.visible, isCurrent, tag + ": the bar marks the current tab only");
                    verify(Qt.colorEqual(label.color, isCurrent ? Ui.Theme.cyan : Ui.Theme.textSecondary), tag + ": label colour");
                    compare(label.font.weight, isCurrent ? Font.DemiBold : Font.Normal, tag + ": label weight");
                    if (!isCurrent)
                        continue;
                    verify(Qt.colorEqual(bar.color, Ui.Theme.cyan), tag + ": the bar is cyan");
                    compare(bar.height, 3);
                    var at = bar.mapToItem(tab, 0, 0);
                    fuzzyCompare(at.y + bar.height, tab.height, 0.01, tag + ": the bar sits on the strip's bottom edge");
                    var labelAt = label.mapToItem(tab, 0, 0);
                    fuzzyCompare(at.x + bar.width / 2, labelAt.x + label.width / 2, 0.5, tag + ": the bar is under the label");
                    verify(bar.width >= 24 - 0.01 && bar.width <= label.width + 0.01, tag + ": the bar is as wide as the text");
                    verify(bar.width >= label.contentWidth - 0.5 || bar.width >= label.width - 0.01, tag + ": the bar spans the text");
                }
            }
            strip.currentId = "scanLists";
            strip.enabled = false;
            for (tab of tabs())
                fuzzyCompare(labelOf(tab).opacity, 0.5, 0.01, tab.tabId + ": a disabled tab's label is subdued");
            var bar2 = indicatorOf(tabOf("scanLists"));
            verify(bar2.visible, "the bar still marks the current tab while disabled");
            fuzzyCompare(bar2.opacity, 1.0, 0.01, "the bar is unchanged while disabled");
            verify(Qt.colorEqual(bar2.color, Ui.Theme.cyan));
            strip.enabled = true;
            for (tab of tabs())
                fuzzyCompare(labelOf(tab).opacity, 1.0, 0.01, tab.tabId + ": enabled again");
        }

        function widestWord(label, weight) {
            wordMetrics.font.family = label.font.family;
            wordMetrics.font.pixelSize = label.font.pixelSize;
            wordMetrics.font.weight = weight;
            var widest = {word: "", width: 0};
            for (var word of String(label.text).split(/\s+/)) {
                if (word.length === 0)
                    continue;
                wordMetrics.text = word;
                if (wordMetrics.advanceWidth > widest.width)
                    widest = {word: word, width: wordMetrics.advanceWidth};
            }
            return widest;
        }
        // The label's height laid out in the selected weight at its width and size.
        function demiBoldHeight(label) {
            probe.font.family = label.font.family;
            probe.font.pixelSize = label.font.pixelSize;
            probe.width = label.width;
            probe.wrapMode = label.wrapMode;
            probe.elide = label.elide;
            probe.text = label.text;
            return probe.height;
        }

        function test_whole_words_and_the_height_follows_the_labels_data() {
            var rows = [];
            for (var width of [320, 360]) {
                for (var scale of [1.0, 1.6, 2.0])
                    rows.push({tag: width + " x " + scale, width: width, scale: scale});
            }
            return rows;
        }
        function test_whole_words_and_the_height_follows_the_labels(data) {
            root.width = data.width;
            Ui.Theme.fontScale = data.scale;
            wait(0);
            var heights = [];
            for (var current of tc.ids) {
                strip.currentId = current;
                wait(0);
                var size = labelOf(tabOf("systems")).font.pixelSize;
                verify(size >= 14, data.tag + ": never below the normal-text size");
                var tallest = 0;
                for (var tab of tabs()) {
                    var label = labelOf(tab);
                    var tag = data.tag + " \"" + label.text + "\" at " + label.font.pixelSize + " px in " + label.width.toFixed(1) + " (" + current + " current)";
                    compare(label.font.pixelSize, size, tag + ": every label is drawn at the strip's one size");
                    for (var weight of [Font.Normal, Font.DemiBold]) {
                        var widest = widestWord(label, weight);
                        verify(widest.width <= label.width + 0.01, tag + ": \"" + widest.word + "\" is " + widest.width.toFixed(1) + " px at weight " + weight + ", so it breaks mid-word");
                    }
                    verify(label.contentWidth <= label.width + 0.5, tag + ": the text is wider than its label");
                    var at = label.mapToItem(tab, 0, 0);
                    verify(at.x >= -0.5 && at.x + label.width <= tab.width + 0.5, tag + ": the label leaves its tab sideways");
                    verify(at.y >= 11.5 && at.y + label.height <= tab.height - 11.5, tag + ": the label leaves its padding (" + at.y.toFixed(1) + " + " + label.height.toFixed(1) + " of " + tab.height.toFixed(1) + ")");
                    tallest = Math.max(tallest, demiBoldHeight(label));
                }
                fuzzyCompare(strip.implicitHeight, Math.max(Ui.Theme.minimumTouchSize, tallest + 24), 0.01, data.tag + ": the strip's height follows its tallest label");
                fuzzyCompare(strip.height, strip.implicitHeight, 0.01);
                heights.push(strip.implicitHeight);
            }
            compare(heights[1], heights[0], data.tag + ": choosing a tab never changes the height");
            compare(heights[2], heights[0], data.tag + ": choosing a tab never changes the height");
        }

        // A width where "Scan lists" fits on one line in the regular weight
        // but wraps in the selected one: the strip is as tall as the wrapped
        // label whichever tab is current.
        function test_the_height_holds_where_only_the_selected_weight_wraps() {
            Ui.Theme.fontScale = 1.0;
            var label = labelOf(tabOf("scanLists"));
            wordMetrics.font.family = label.font.family;
            wordMetrics.font.pixelSize = label.font.pixelSize;
            wordMetrics.text = label.text;
            wordMetrics.font.weight = Font.Normal;
            var regular = wordMetrics.advanceWidth;
            wordMetrics.font.weight = Font.DemiBold;
            var selected = wordMetrics.advanceWidth;
            verify(selected > regular + 2, "the selected weight is wider (" + regular + ", " + selected + ")");
            // Each tab's label is its tab's width less 16.
            strip.width = 3 * ((regular + selected) / 2 + 16);
            wait(0);
            compare(label.font.pixelSize, 14);
            strip.currentId = "systems";
            wait(0);
            compare(label.lineCount, 1, "one line in the regular weight");
            var resting = strip.implicitHeight;
            strip.currentId = "scanLists";
            wait(0);
            compare(label.lineCount, 2, "two lines in the selected weight");
            verify(strip.implicitHeight >= label.height + 24 - 0.01, "the strip holds the wrapped label");
            compare(resting, strip.implicitHeight, "the strip was that tall before the tab was chosen");
            strip.width = Qt.binding(function () {
                return root.width - 2 * Ui.Theme.screenPadding;
            });
        }

        function test_the_strip_grows_with_the_text() {
            root.width = 320;
            Ui.Theme.fontScale = 1.0;
            wait(0);
            var normal = strip.implicitHeight;
            compare(normal, Ui.Theme.minimumTouchSize, "one touch target tall at normal text");
            Ui.Theme.fontScale = 2.0;
            wait(0);
            verify(strip.implicitHeight > normal, "taller at large text (" + strip.implicitHeight + ")");
        }
    }
}

