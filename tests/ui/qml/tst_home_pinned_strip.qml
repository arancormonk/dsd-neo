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

        // §6: nothing pinned, no strip: the desktop's layout is exactly as before, and the brokered body starts under
        // the header as before. A pin brings the strip, and the body moves below it.
        function test_strip_hidden_without_pins_data() {
            return [{tag: "desktop", brokered: false}, {tag: "brokered", brokered: true}];
        }
        function test_strip_hidden_without_pins(data) {
            if (!data.brokered)
                testContext.useLifecycleHost(false);
            var header = findChild(home, "listenHeader");
            var restingTop = data.brokered ? header.y + header.height + home.headerClearance : 0;
            verify(!strip().visible, "no strip");
            compare(strip().height, 0);
            compare(home.pinnedStripHeight, 0);
            compare(body().y, restingTop);
            compare(body().height, home.height - restingTop);
            var title = findChild(home, "listenTitle");
            compare(title.visible, !data.brokered);
            if (!data.brokered)
                compare(title.parent.mapToItem(body().contentItem, 0, 0).y, Ui.Theme.screenPadding);

            var uid = addSystem("Alpha");
            pin("system", uid);
            tryCompare(strip(), "visible", true);
            verify(strip().height >= Ui.Theme.minimumTouchSize);
            compare(home.pinnedStripHeight, strip().height);
            compare(strip().y, data.brokered ? header.y + header.height + 8 : Ui.Theme.screenPadding);
            compare(body().y, strip().y + strip().height + home.headerClearance, "the body starts under the strip");
            compare(JSON.stringify(stripTexts()), JSON.stringify(["Alpha"]));
            if (!data.brokered)
                verify(title.visible, "the desktop title stays in the body");

            listenPins.unpin("system", uid);
            tryCompare(strip(), "visible", false);
            compare(body().y, restingTop, "unpinned, the body is back where it was");
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
            verify(b.y >= s.y + s.h, "body below the strip");
            compare(b.y + b.h, home.height, "the body runs to the bottom");
            verify(b.h >= home.height / 2, "the body keeps most of the screen: " + b.h + " of " + home.height);
            if (data.brokered) {
                var header = rectOf(findChild(home, "listenHeader"));
                verify(s.y >= header.y + header.h, "strip under the header");
            }
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

        // §6: Tab goes from the receiver pill through the pins to More, then into the body.
        function test_keyboard_order() {
            pin("system", addSystem("Alpha"));
            pin("system", addSystem("Bravo"));
            waitForRendering(home);
            var pill = findChild(home, "dongleStatusPill");
            verify(pill.visible);
            var pins = stripPins();
            compare(pins.length, 2);
            var order = [pill, pins[0], pins[1], findChild(home, "pinnedStripMore")];
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
            pinButton.activate();
            keyClick(Qt.Key_Return);
            compare(started.count, 0, "the pin's own activation does nothing");
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
                verify(waitForPolish(home));
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
                home.visible = true;
                verify(waitForPolish(home));
                // Back on Home the strip goes and the body grows by its height: the place is kept, short of the end.
                tryCompare(strip(), "visible", false);
                compare(body().contentY, Math.min(scrolled, body().contentHeight - body().height), "scroll position after a tab switch");
            }
            testContext.setAttachedDeviceSource("");
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
