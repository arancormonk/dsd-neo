// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// Issue #801: the Pinned strip above Listen's body and the Pinned sheet, on Home alone (tst_home_pinned.qml covers
// what Main does with a pin). The strip is one line at every width and text size: the found pins that fit, in order,
// then More, which opens the sheet with every pin.
Item {
    id: root
    QtObject {
        id: mainRoot
        property bool transitioning: false
        property bool showFailure: false
        property string failureText: ""
        property string dismissedFailure: ""
    }
    width: 700
    height: 900
    Ui.HomeScreen {
        id: home
        anchors.fill: parent
    }
    // Changed once per frame wanted, so the window draws while Home is hidden.
    Rectangle {
        id: ticker
        width: 1
        height: 1
        z: 10
        color: "black"
    }
    SignalSpy {
        id: started
        target: home
        signalName: "startPin"
    }
    TestCase {
        id: tc

        name: "HomePinnedStrip"
        when: windowShown

        readonly property string missingUid: "6f1c2a1e-0000-4000-8000-000000000000"

        function init() {
            // The brokered capability is CONSTANT: set it before publishing the host (see tst_home_dongle_status).
            testContext.setDongleStatus(false, 0, "");
            testContext.useLifecycleHost(true);
            testContext.resetPins();
            emptyModels();
            started.clear();
        }
        function cleanup() {
            root.forceActiveFocus();
            var sheet = findChild(home, "pinnedSheet");
            if (sheet.visible)
                sheet.visible = false;
            home.visible = true;
            testContext.resetPins();
            emptyModels();
            testContext.setDongleStatus(false, 0, "");
            testContext.useLifecycleHost(false);
            root.width = 700;
            root.height = 900;
            Ui.Theme.resetFontScale();
        }
        // Draws @a count frames while Home stays as it is: every polish a hidden Home would see runs in them.
        function waitFrames(count) {
            for (var i = 0; i < count; ++i) {
                ticker.color = ticker.color === Qt.color("black") ? "white" : "black";
                verify(waitForRendering(ticker), "frame " + i);
            }
        }
        function emptyModels() {
            while (savedSystems.count)
                savedSystems.remove(0);
            while (scanLists.count)
                scanLists.remove(0);
        }

        function addSystem(name, fields) {
            verify(savedSystems.add(Object.assign({name: name, sourceType: "rtltcp", host: "127.0.0.1", port: 1234,
                freqMhz: "851.5"}, fields || {})), "could not add " + name);
            return savedSystems.get(savedSystems.count - 1).uid;
        }
        function addList(name) {
            verify(scanLists.add({name: name, sourceType: "rtltcp", host: "127.0.0.1", port: 1234,
                entries: [{kind: "freq", name: "Simplex", protocol: "p25", freqMhz: "851.5", enabled: true}]}));
            return scanLists.get(scanLists.count - 1).uid;
        }
        function pin(kind, uid) {
            compare(listenPins.pin(kind, uid), "", "pin " + kind);
        }
        function strip() {
            return findChild(home, "pinnedStrip");
        }
        function body() {
            return findChild(home, "dongleScrollBody");
        }
        function tabs() {
            return findChild(home, "listenTabs");
        }
        // The strip's pins on screen, left to right.
        function stripPins() {
            var out = [];
            for (var child of strip().children) {
                if (child.objectName === "pinnedStripPin" && child.opacity > 0)
                    out.push(child);
            }
            out.sort(function (a, b) {
                return a.x - b.x;
            });
            return out;
        }
        function stripKeys() {
            return stripPins().map(function (button) {
                return button.pinKey;
            });
        }
        function stripTexts() {
            return stripPins().map(function (button) {
                return button.text;
            });
        }
        function labelOf(button) {
            return findChild(button, "pinButtonLabel");
        }
        function rectOf(item) {
            var p = item.mapToItem(home, 0, 0);
            return {x: p.x, y: p.y, w: item.width, h: item.height};
        }
        function descends(item, scope) {
            for (var at = item; at; at = at.parent) {
                if (at === scope)
                    return true;
            }
            return false;
        }
        function sameButtons(a, b, message) {
            compare(a.length, b.length, message);
            for (var i = 0; i < a.length; ++i)
                verify(a[i] === b[i], message + ": button " + i);
        }
        function sheetRows(name) {
            var out = [];
            (function look(item) {
                for (var child of item.children) {
                    if (child.objectName === name && child.parent.opacity > 0)
                        out.push(child);
                    look(child);
                }
            })(findChild(home, "pinnedSheet"));
            return out;
        }
        function openSheet() {
            waitForRendering(home);
            mouseClick(findChild(home, "pinnedStripMore"));
            var sheet = findChild(home, "pinnedSheet");
            tryCompare(sheet, "visible", true);
            waitForRendering(sheet);
            return sheet;
        }

        // §6: nothing pinned, no strip: on both hosts the Listen tabs sit 8 px under the fixed title and the body
        // starts under them (issue #646). A pin brings the strip between the title and the tabs, which move below it.
        function test_strip_hidden_without_pins_data() {
            return [{tag: "desktop", brokered: false}, {tag: "brokered", brokered: true}];
        }
        function test_strip_hidden_without_pins(data) {
            if (!data.brokered)
                testContext.useLifecycleHost(false);
            var header = findChild(home, "listenHeader");
            verify(header.visible, "the title is fixed on both hosts");
            var restingTabs = header.y + header.height + 8;
            verify(!strip().visible, "no strip");
            compare(strip().height, 0);
            compare(home.pinnedStripHeight, 0);
            compare(rectOf(tabs()).y, restingTabs, "the tabs under the title");
            compare(body().y, restingTabs + tabs().height, "the body under the tabs");
            compare(body().height, home.height - body().y);
            var title = findChild(home, "listenTitle");
            verify(!title.visible, "the body's own title row is not shown");

            var uid = addSystem("Alpha");
            pin("system", uid);
            tryCompare(strip(), "visible", true);
            verify(strip().height >= Ui.Theme.minimumTouchSize);
            compare(home.pinnedStripHeight, strip().height);
            verify(header.visible, "the title is fixed above the strip");
            compare(strip().y, header.y + header.height + 8);
            compare(rectOf(tabs()).y, strip().y + strip().height + 8, "the tabs under the strip");
            compare(body().y, rectOf(tabs()).y + tabs().height, "the body starts under the tabs");
            compare(JSON.stringify(stripTexts()), JSON.stringify(["Alpha"]));
            verify(!title.visible, "the body's own title row is not shown as well");

            listenPins.unpin("system", uid);
            tryCompare(strip(), "visible", false);
            compare(rectOf(tabs()).y, restingTabs, "unpinned, the tabs are back where they were");
            compare(body().y, restingTabs + tabs().height, "and the body");
        }

        // §6 [Review focus 3]: six pins with long names stay on one line at every phone width and text size, portrait
        // and landscape, and on the desktop beside the supporting pane: the pins that fit, More, and the body below.
        function test_strip_one_line_overflow_data() {
            var rows = [];
            for (var width of [320, 360, 411]) {
                for (var scale of [1.0, 1.3, 1.6]) {
                    rows.push({tag: width + " px portrait at " + scale, width: width, height: 690, scale: scale, brokered: true});
                    rows.push({tag: width + " px landscape at " + scale, width: Math.round(width * 2.05), height: width,
                        scale: scale, brokered: true});
                }
            }
            for (var desktopScale of [1.0, 1.6])
                rows.push({tag: "desktop 900 px at " + desktopScale, width: 900, height: 690, scale: desktopScale, brokered: false});
            return rows;
        }
        function test_strip_one_line_overflow(data) {
            if (!data.brokered)
                testContext.useLifecycleHost(false);
            root.width = data.width;
            root.height = data.height;
            Ui.Theme.fontScale = data.scale;
            var names = ["Countywide Public Safety Trunked Radio System", "Fire", "Metro Transit Operations",
                "Harbor Patrol", "Statewide Interoperability Network", "EMS"];
            for (var name of names)
                pin("system", addSystem(name));
            compare(listenPins.count, 6);
            waitForRendering(home);
            var s = rectOf(strip());
            var more = findChild(home, "pinnedStripMore");
            var m = rectOf(more);
            var pins = stripPins();
            verify(pins.length >= 1 && pins.length < 6, pins.length + " pins on the line");
            compare(pins[0].text, names[0], "in order, starting with the first");
            for (var i = 0; i < pins.length; ++i) {
                var p = rectOf(pins[i]);
                var label = labelOf(pins[i]);
                compare(pins[i].text, names[i], "pin " + i + " in order");
                compare(p.y, s.y, "pin " + i + " on the strip's line");
                compare(p.h, s.h, "pin " + i + " as tall as the strip");
                verify(p.w >= Ui.Theme.minimumTouchSize && p.h >= Ui.Theme.minimumTouchSize, "pin " + i + " touch size");
                verify(p.x >= s.x - 0.5 && p.x + p.w <= m.x + 0.5, "pin " + i + " left of More: " + JSON.stringify(p));
                if (i > 0) {
                    var before = rectOf(pins[i - 1]);
                    verify(before.x + before.w <= p.x, "pin " + i + " overlaps the one before it");
                }
                compare(label.lineCount, 1, "pin " + i + " label on one line");
                // Every pin but the last shows its whole name; only the last may be elided.
                if (i < pins.length - 1)
                    verify(!label.truncated, "pin " + i + " shows whole");
                verify(label.height + 24 <= p.h + 0.5, "pin " + i + " label fits its pin");
            }
            // One line of label, not two.
            verify(s.h <= Math.max(Ui.Theme.minimumTouchSize, labelOf(pins[0]).height + 24) + 0.5, "strip " + s.h + " is one line");
            verify(more.visible, "More is always there");
            compare(m.x + m.w, s.x + s.w, "More ends the strip");
            verify(m.y >= s.y && m.y + m.h <= s.y + s.h + 0.5, "More inside the strip");
            var right = home.supportingPane ? home.width - 280 - 2 * Ui.Theme.screenPadding : home.width - Ui.Theme.screenPadding;
            verify(s.x >= Ui.Theme.screenPadding - 0.5 && s.x + s.w <= right + 0.5, "strip inside the column");
            var b = rectOf(body());
            var t = rectOf(tabs());
            compare(t.y, s.y + s.h + 8, "the tabs 8 px below the strip");
            // Issue #646: the Listen tabs are fixed while that leaves the body half of Home at least; these landscape
            // phones are shorter, and there the tabs lead the body instead, at rest just where fixed ones would be.
            var short = ["320 px landscape at 1", "320 px landscape at 1.3", "320 px landscape at 1.6",
                "360 px landscape at 1.6"].indexOf(data.tag) >= 0;
            compare(home.tabsFixed, !short, "the tabs fixed");
            compare(home.tabsFixed, home.fixedExtent <= home.height / 2, "by the short-screen rule");
            if (home.tabsFixed) {
                compare(b.y, t.y + t.h, "body below the tabs");
            } else {
                compare(body().contentY, body().originY);
                verify(descends(tabs(), body()), "the tabs in the body");
                verify(t.y >= b.y && t.y + t.h <= b.y + b.h, "the tabs at the top of the body, in view");
            }
            compare(b.y + b.h, home.height, "the body runs to the bottom");
            verify(b.h >= home.height / 2, "the body keeps most of the screen: " + b.h + " of " + home.height);
            var header = rectOf(findChild(home, "listenHeader"));
            verify(s.y >= header.y + header.h, "strip under the header");
        }

        // Strip fill: after the pins that fit whole, the next one is shown elided in the room left before More, when
        // that room is at least the strip's minimum; its name stays whole for a screen reader. Pins after it are in the
        // sheet.
        function test_strip_fills_with_one_elided_pin() {
            root.width = 360;
            var names = ["SARA Network", "Countywide Public Safety Trunked Radio System", "Fire", "EMS", "Roads"];
            for (var name of names)
                pin("system", addSystem(name));
            waitForRendering(home);
            var pins = stripPins();
            compare(pins.length, 2, "the whole first pin and one elided");
            compare(pins[0].text, names[0]);
            verify(!labelOf(pins[0]).truncated, "the first shows whole");
            compare(pins[1].text, names[1], "in order");
            verify(labelOf(pins[1]).truncated, "the second is elided");
            verify(pins[1].width >= strip().elidedMinimum, "at no less than the minimum");
            compare(pins[1].Accessible.name, "Start " + names[1], "read whole");
            var more = findChild(home, "pinnedStripMore");
            verify(pins[0].x + pins[0].width <= pins[1].x, "no overlap");
            verify(pins[1].x + pins[1].width <= more.x, "More is last");
            compare(more.x + more.width, strip().width);
        }
        // Too little room left for the minimum: the next pin is not shown, and the line ends with the whole ones.
        function test_strip_leaves_out_a_pin_below_the_minimum() {
            root.width = 360;
            var names = ["Countywide Public Safety", "Metro Transit Operations Center", "EMS"];
            for (var name of names)
                pin("system", addSystem(name));
            waitForRendering(home);
            var pins = stripPins();
            var more = findChild(home, "pinnedStripMore");
            var left = more.x - strip().pinGap - (pins[0].x + pins[0].width + strip().pinGap);
            verify(left > 0 && left < strip().elidedMinimum, "the case leaves less than the minimum: " + left);
            compare(pins.length, 1, "only the first fits");
            verify(!labelOf(pins[0]).truncated);
        }

        // §6: the strip shows found pins in stored order, and a tap, Return, Enter, Space or a screen reader's press on
        // each asks for that kind and uid; a pin not found is left out of the strip.
        function test_strip_order_and_activation() {
            var solo = addSystem("Solo");
            addSystem("County", {siteName: "North", rrSid: 12, rrSiteId: 100});
            var south = addSystem("County", {siteName: "South", freqMhz: "852.5", rrSid: 12, rrSiteId: 101});
            var list = addList("Patrol");
            pin("explore", "");
            pin("site", south);
            pin("scan", list);
            prefs.pinned = prefs.pinned.concat([{kind: "system", uid: missingUid}]);
            pin("search", "");
            pin("system", solo);
            compare(listenPins.count, 6);
            tryCompare(strip(), "visible", true);
            waitForRendering(home);
            var expected = [["explore", ""], ["site", south], ["scan", list], ["search", ""], ["system", solo]];
            compare(JSON.stringify(stripKeys()), JSON.stringify(expected.map(function (pair) {
                return pair[0] + ":" + pair[1];
            })), "found pins in stored order, the missing one left out");
            compare(JSON.stringify(stripTexts()), JSON.stringify(["Explore", "County · South", "Patrol", "Signal search", "Solo"]));
            for (var whole of stripPins())
                verify(!labelOf(whole).truncated, whole.text + " shows whole");
            compare(stripPins()[4].accessibleName, "Start Solo");
            compare(stripPins()[2].accessibleName, "Start scan list Patrol");
            var pins = stripPins();
            for (var i = 0; i < pins.length; ++i) {
                var ways = ["tap", Qt.Key_Return, Qt.Key_Enter, Qt.Key_Space, "press"];
                for (var way of ways) {
                    started.clear();
                    if (way === "tap") {
                        mouseClick(pins[i]);
                    } else if (way === "press") {
                        verify(testContext.pressAccessible(pins[i]), "the pin offers a press action");
                    } else {
                        pins[i].forceActiveFocus();
                        keyClick(way);
                    }
                    compare(started.count, 1, "pin " + i + " by " + way);
                    compare(started.signalArguments[0][0], expected[i][0]);
                    compare(started.signalArguments[0][1], expected[i][1]);
                }
            }
        }

        // §6: Tab goes from the receiver pill through the pins to More, then the current Listen tab (issue #646), then
        // into the body. On the desktop the fixed title is no stop, and there is no pill: pins, More, tab, body.
        function test_keyboard_order_data() {
            return [{tag: "brokered", brokered: true}, {tag: "desktop", brokered: false}];
        }
        function test_keyboard_order(data) {
            if (!data.brokered)
                testContext.useLifecycleHost(false);
            pin("system", addSystem("Alpha"));
            pin("system", addSystem("Bravo"));
            waitForRendering(home);
            var pill = findChild(home, "dongleStatusPill");
            compare(pill.visible, data.brokered);
            verify(findChild(home, "listenHeaderTitle").visible, "the title leads");
            var pins = stripPins();
            compare(pins.length, 2);
            var order = (data.brokered ? [pill] : []).concat([pins[0], pins[1], findChild(home, "pinnedStripMore"),
                findChild(home, "listenTab_" + home.section)]);
            root.forceActiveFocus();
            for (var i = 0; i < order.length; ++i) {
                keyClick(Qt.Key_Tab);
                verify(order[i].activeFocus, "Tab " + (i + 1) + " reaches " + (order[i].objectName || order[i].text));
            }
            keyClick(Qt.Key_Tab);
            var focused = root.Window.window.activeFocusItem;
            var inBody = false;
            for (var at = focused; at; at = at.parent)
                inBody = inBody || at === body();
            verify(inBody, "then the body: " + focused);
        }

        // D9: the sheet lists every stored pin, a missing one disabled and named as missing; Unpin removes each one,
        // and a start closes the sheet before it asks.
        function test_sheet_lists_all_and_unpins() {
            var solo = addSystem("Solo");
            var list = addList("Patrol");
            pin("system", solo);
            prefs.pinned = prefs.pinned.concat([{kind: "system", uid: missingUid}]);
            pin("scan", list);
            pin("search", "");
            tryCompare(listenPins, "count", 4);
            var sheet = openSheet();
            compare(findChild(sheet, "pinnedSheetTitle").text, "Pinned");
            var starts = sheetRows("pinnedSheetStart");
            compare(JSON.stringify(starts.map(function (b) { return b.text; })),
                    JSON.stringify(["Solo", "Saved system not found", "Patrol", "Signal search"]));
            compare(JSON.stringify(starts.map(function (b) { return b.enabled; })), JSON.stringify([true, false, true, true]));
            compare(starts[0].accessibleName, "Start Solo");
            compare(starts[1].accessibleName, "Saved system not found");
            var unpins = sheetRows("pinnedSheetUnpin");
            compare(unpins.length, 4);
            compare(unpins[0].accessibleName, "Unpin Solo");
            compare(unpins[1].accessibleName, "Unpin the missing saved system");
            compare(sheet.missingText({label: "County · North", kind: "site"}), "County · North (not found)");
            compare(sheet.missingText({label: "", kind: "scan"}), "Scan list not found");

            // A missing pin can only be unpinned.
            started.clear();
            mouseClick(starts[1]);
            compare(started.count, 0);
            verify(sheet.visible);
            mouseClick(unpins[1]);
            tryCompare(listenPins, "count", 3);
            tryVerify(function () {
                return sheetRows("pinnedSheetStart").length === 3;
            });
            compare(JSON.stringify(sheetRows("pinnedSheetStart").map(function (b) { return b.text; })),
                    JSON.stringify(["Solo", "Patrol", "Signal search"]));
            verify(sheet.visible, "the sheet stays open");
            // The strip had left it out all along.
            compare(JSON.stringify(stripTexts()), JSON.stringify(["Solo", "Patrol", "Signal search"]));

            waitForRendering(sheet);
            mouseClick(sheetRows("pinnedSheetStart")[1]);
            verify(!sheet.visible, "a start closes the sheet");
            compare(started.count, 1);
            compare(started.signalArguments[0][0], "scan");
            compare(started.signalArguments[0][1], list);

            // Everything unpinned from the sheet: it says so, and the strip goes.
            sheet = openSheet();
            for (var n = 3; n > 0; --n) {
                waitForRendering(sheet);
                mouseClick(sheetRows("pinnedSheetUnpin")[0]);
                tryCompare(listenPins, "count", n - 1);
            }
            tryVerify(function () {
                return sheetRows("pinnedSheetStart").length === 0;
            });
            verify(findChild(sheet, "pinnedSheetEmpty").visible);
            verify(!strip().visible);
            mouseClick(findChild(sheet, "pinnedSheetDone"));
            verify(!sheet.visible);
        }

        // Unpinned from the keyboard, an Unpin hands its focus to the next row's Unpin, else the one before, else Done:
        // the reader stays in the sheet's list.
        function test_keyboard_unpin_keeps_focus_in_sheet() {
            var names = ["Alpha", "Bravo", "Charlie"];
            for (var name of names)
                pin("system", addSystem(name));
            var sheet = openSheet();
            var panel = findChild(home, "pinnedSheetPanel");
            function within(item, scope) {
                for (var at = item; at; at = at.parent) {
                    if (at === scope)
                        return true;
                }
                return false;
            }
            function unpinOf(label) {
                return sheetRows("pinnedSheetUnpin").filter(function (button) {
                    return button.accessibleName === "Unpin " + label;
                })[0];
            }
            // The middle row: focus goes on to the next.
            unpinOf("Bravo").forceActiveFocus();
            keyClick(Qt.Key_Space);
            compare(listenPins.count, 2);
            var focused = root.Window.window.activeFocusItem;
            verify(within(focused, panel), "focus stays in the sheet: " + focused);
            compare(focused, unpinOf("Charlie"), "on the next row's Unpin");
            // The last row: back to the one before.
            keyClick(Qt.Key_Space);
            compare(listenPins.count, 1);
            tryVerify(function () {
                return sheetRows("pinnedSheetUnpin").length === 1;
            });
            compare(root.Window.window.activeFocusItem, unpinOf("Alpha"), "on the row before");
            // The only row: Done.
            keyClick(Qt.Key_Space);
            compare(listenPins.count, 0);
            compare(root.Window.window.activeFocusItem, findChild(sheet, "pinnedSheetDone"), "on Done");
            verify(sheet.visible);
        }

        // StableRows lets a departing row go at the end of the turn; until then its buttons, in the strip and in the
        // sheet, are not drawn, read as nothing, leave the tree and take no press.
        function test_departing_rows_read_as_nothing() {
            var alpha = addSystem("Alpha");
            pin("system", alpha);
            pin("system", addSystem("Bravo"));
            var sheet = openSheet();
            var stripButtons = stripPins();
            var starts = sheetRows("pinnedSheetStart");
            var unpins = sheetRows("pinnedSheetUnpin");
            compare(stripButtons.length, 2);
            compare(starts.length, 2);
            var departing = [stripButtons[0], starts[0], unpins[0]];
            started.clear();
            listenPins.unpin("system", alpha);
            // Still in this turn: the rows are there, the departing ones not live.
            for (var button of departing) {
                verify(button.parent !== null, "the row is let go of at the end of the turn");
                compare(button.Accessible.name, "", button.objectName + " reads as nothing");
                verify(button.Accessible.ignored, button.objectName + " leaves the tree");
                verify(!button.activeFocusOnTab, button.objectName + " takes no focus");
                button.activate();
            }
            compare(started.count, 0, "a departing strip pin or sheet start starts nothing");
            compare(listenPins.count, 1, "a departing Unpin unpins nothing more");
            compare(stripButtons[1].Accessible.name, "Start Bravo", "the staying pin keeps its name");
            sheet.visible = false;
        }

        // [r1-7] and the strip's layout: a rotation or a text-size change while Home is hidden adds or removes no pin
        // there; shown again, the line is laid out for the new width and size.
        function test_hidden_home_holds_strip_layout_data() {
            return [{tag: "rotation", width: 700}, {tag: "text size", width: 420}];
        }
        function test_hidden_home_holds_strip_layout(data) {
            root.width = data.width;
            for (var name of ["Fire", "EMS", "Police", "Roads"])
                pin("system", addSystem(name));
            waitForRendering(home);
            var buttons = stripPins();
            compare(buttons.length, 4, "all four fit at " + data.width + " px");
            var widths = buttons.map(function (b) { return b.width; });
            home.visible = false;
            if (data.tag === "rotation")
                root.width = 320;
            else
                Ui.Theme.fontScale = 1.6;
            sameButtons(stripPins(), buttons, "the hidden strip keeps its pins");
            compare(JSON.stringify(stripPins().map(function (b) { return b.width; })), JSON.stringify(widths),
                    "and their places");
            home.visible = true;
            tryVerify(function () {
                return stripPins().length < 4;
            }, 1000, "shown again, fewer pins fit");
            verify(stripPins()[0] === buttons[0], "the first pin kept its button");
        }

        // A sheet row's start keeps the whole name readable at 320 px and the largest text size Theme allows: a word
        // wider than the room beside Unpin breaks inside the word rather than running past the button's edge.
        function test_sheet_start_label_fits_its_button() {
            root.width = 320;
            root.height = 690;
            Ui.Theme.fontScale = 1.6;
            var name = "Countywide Public Safety Trunked Radio System";
            pin("system", addSystem(name));
            var sheet = openSheet();
            var start = sheetRows("pinnedSheetStart")[0];
            var label = null;
            for (var child of start.children) {
                if (child.text === name)
                    label = child;
            }
            verify(label !== null, "the start's label");
            verify(label.contentWidth > 0);
            verify(label.contentWidth <= label.width + 0.5, "label " + label.contentWidth + " wider than its room " + label.width);
            var r = label.mapToItem(start, (label.width - label.contentWidth) / 2, 0);
            verify(r.x >= -0.5 && r.x + label.contentWidth <= start.width + 0.5, "the label runs past the button");
            verify(label.contentHeight <= start.height, "the button is as tall as its lines");
            verify(!label.truncated, "the whole name shows");
            sheet.visible = false;
        }

        // The sheet is a management sheet: open, nothing on the strip or the body behind it acts.
        function test_sheet_blocks_background() {
            pin("system", addSystem("Alpha"));
            var sheet = openSheet();
            verify(home.managementSheetOpen);
            verify(!strip().enabled);
            verify(!body().enabled);
            var pinButton = stripPins()[0];
            verify(pinButton.Accessible.ignored, "the covered pin leaves the tree");
            verify(!pinButton.activeFocusOnTab);
            started.clear();
            pinButton.forceActiveFocus();
            verify(!pinButton.activeFocus, "the covered pin takes no focus");
            keyClick(Qt.Key_Return);
            pinButton.activate();
            compare(started.count, 0, "neither a key nor the pin's own activation starts anything");
            // A tap over the pin is a tap outside the panel: it closes the sheet and goes no further.
            var at = pinButton.mapToItem(root, pinButton.width / 2, pinButton.height / 2);
            mouseClick(root, at.x, at.y);
            verify(!sheet.visible);
            compare(started.count, 0, "a tap over the pin does not reach it");
            verify(!home.managementSheetOpen);
        }

        // [r1-7, Review focus 5]: hidden, Home's strip holds what it showed through a rename, a removal and an unpin,
        // keeping its pins and height; shown again, it shows the pins as they are.
        function test_hidden_home_holds_strip() {
            var alpha = addSystem("Alpha");
            var bravo = addSystem("Bravo");
            var charlie = addSystem("Charlie");
            pin("system", alpha);
            pin("system", bravo);
            pin("system", charlie);
            waitForRendering(home);
            var buttons = stripPins();
            var height = strip().height;
            home.visible = false;
            verify(savedSystems.update(savedSystems.rowForUid(alpha), {name: "Alpha Prime"}));
            verify(savedSystems.remove(savedSystems.rowForUid(bravo)));
            listenPins.unpin("system", charlie);
            compare(listenPins.count, 1, "the store moved on");
            compare(JSON.stringify(stripTexts()), JSON.stringify(["Alpha", "Bravo", "Charlie"]), "the hidden strip holds");
            compare(strip().height, height);
            sameButtons(stripPins(), buttons, "the same buttons");
            home.visible = true;
            tryCompare(stripPins()[0], "text", "Alpha Prime");
            compare(stripPins().length, 1);
            compare(stripPins()[0], buttons[0], "the pin kept its button");

            // Everything unpinned while hidden: the strip and its height stay until Home shows again.
            home.visible = false;
            listenPins.unpin("system", alpha);
            compare(strip().height, height);
            home.visible = true;
            tryCompare(strip(), "visible", false);
        }

        // Controller ruling on D2: on the desktop with pins, the title leaves the body for a fixed header above the
        // strip (no receiver pill), so the order is title, strip, body as on Android; shown once, and kept across tabs.
        function test_desktop_title_leads_with_pins() {
            testContext.useLifecycleHost(false);
            root.width = 900;
            root.height = 400;
            for (var i = 0; i < 8; ++i) {
                var uid = addSystem("System " + i);
                if (i < 2)
                    pin("system", uid);
            }
            waitForRendering(home);
            var header = findChild(home, "listenHeader");
            var headerTitle = findChild(home, "listenHeaderTitle");
            var bodyTitle = findChild(home, "listenTitle");
            verify(header.visible && headerTitle.visible, "the title is in the fixed header");
            verify(!bodyTitle.visible, "and not in the body as well");
            verify(!findChild(home, "dongleStatusPill").visible, "no receiver pill on the desktop");
            compare(header.y, Ui.Theme.screenPadding);
            var h = rectOf(header);
            var t = rectOf(headerTitle);
            verify(t.y >= h.y - 0.5 && t.y + t.h <= h.y + h.h + 0.5, "the title inside its header");
            var s = rectOf(strip());
            verify(s.y >= h.y + h.h, "the strip under the title");
            verify(body().y >= s.y + s.h, "the body under the strip");
            var headerHeight = header.height;
            var stripHeight = strip().height;
            var scrolled = body().contentHeight - body().height;
            verify(scrolled > 40, "the body scrolls");
            body().contentY = 40;
            home.visible = false;
            // Everything unpinned on another tab: the hidden title and strip stay where they were.
            while (listenPins.count > 0)
                listenPins.unpinKey(listenPins.entries[0].key);
            compare(header.height, headerHeight, "the hidden header keeps its height");
            compare(strip().height, stripHeight, "the hidden strip keeps its height");
            compare(body().contentY, 40);
            home.visible = true;
            verify(waitForPolish(home));
            compare(body().contentY, 40, "scroll position after a tab switch");
            // Back on Home with nothing pinned: the strip goes, and the title stays fixed above the Listen tabs
            // (issue #646), still once.
            tryCompare(strip(), "visible", false);
            verify(header.visible && headerTitle.visible);
            compare(header.height, Math.max(44, headerTitle.implicitHeight));
            verify(!bodyTitle.visible);
            compare(rectOf(tabs()).y, header.y + header.height + 8);
            compare(body().y, rectOf(tabs()).y + tabs().height);
        }

        // Hidden on another tab with pins, Home keeps its scroll range and position, also when the pins change there.
        function test_scroll_kept_across_tabs_with_pins() {
            root.width = 360;
            root.height = 400;
            testContext.setAttachedDeviceSource("usb");
            var uids = [];
            for (var i = 0; i < 4; ++i)
                uids.push(addSystem("System " + i));
            tryCompare(findChild(home, "dongleRetry"), "visible", true);
            for (var scrolled of [40, 1e9]) {
                for (var uid of uids)
                    pin("system", uid);
                // The whole window's layout, so the body's range is measured with the cards laid out.
                verify(waitForPolish(root.Window.window));
                waitForRendering(home);
                var stripHeight = strip().height;
                verify(stripHeight > 0);
                scrolled = Math.min(scrolled, body().contentHeight - body().height);
                verify(scrolled > 0);
                body().contentY = scrolled;
                home.visible = false;
                // Everything unpinned on another tab: the hidden strip keeps its place all the same.
                for (uid of uids)
                    listenPins.unpin("system", uid);
                compare(listenPins.count, 0);
                compare(strip().height, stripHeight, "the hidden strip keeps its height");
                compare(body().contentHeight - body().height >= scrolled, true, "the hidden body still scrolls that far");
                compare(body().contentY, scrolled, "the hidden body keeps its place");
                // And still after the hidden body's next layout passes: the cards keep their heights (issue #646 B1).
                waitFrames(3);
                compare(strip().height, stripHeight, "the hidden strip keeps its height through a layout");
                compare(body().contentHeight - body().height >= scrolled, true, "the hidden body still scrolls that far after a layout");
                compare(body().contentY, scrolled, "the hidden body keeps its place through a layout");
                home.visible = true;
                verify(waitForPolish(home));
                // Back on Home the strip goes and the body grows by its height: the place is kept, short of the end.
                tryCompare(strip(), "visible", false);
                compare(body().contentY, Math.min(scrolled, body().contentHeight - body().height), "scroll position after a tab switch");
            }
            testContext.setAttachedDeviceSource("");
        }

        // B5: a group's pin reads as its card, the name an import generated without its site, and keeps that name
        // when the lead goes; a site pin names its site once.
        function test_strip_group_title() {
            var lead = addSystem("SARA — Johnson Simulcast", {siteName: "Johnson Simulcast", rrSid: 9, rrSiteId: 1});
            var polk = addSystem("SARA — Polk", {siteName: "Polk", freqMhz: "852.5", rrSid: 9, rrSiteId: 2});
            pin("system", polk);
            pin("site", polk);
            waitForRendering(home);
            compare(JSON.stringify(stripTexts()), JSON.stringify(["SARA", "SARA · Polk"]));
            compare(stripPins()[0].accessibleName, "Choose a site of SARA");
            var buttons = stripPins();
            verify(savedSystems.remove(savedSystems.rowForUid(lead)));
            tryCompare(buttons[0], "accessibleName", "Start SARA");
            compare(buttons[0].text, "SARA", "the group keeps its name");
        }

        // A rename on screen renames the pin's own button; it is not made again.
        function test_strip_follows_rename() {
            var uid = addSystem("Alpha");
            var list = addList("Patrol");
            pin("system", uid);
            pin("scan", list);
            waitForRendering(home);
            var buttons = stripPins();
            verify(savedSystems.update(savedSystems.rowForUid(uid), {name: "Alpha Prime"}));
            tryCompare(buttons[0], "text", "Alpha Prime");
            compare(buttons[0].accessibleName, "Start Alpha Prime");
            verify(scanLists.update(scanLists.rowForUid(list), {name: "Night patrol"}));
            tryCompare(buttons[1], "text", "Night patrol");
            sameButtons(stripPins(), buttons, "the same buttons");
        }
    }
}
