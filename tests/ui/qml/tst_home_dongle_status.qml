// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

Item {
    id: root
    QtObject {
        id: mainRoot
        property bool transitioning: false
        property bool showFailure: false
        property string failureText: ""
        property string dismissedFailure: ""
    }
    width: 390
    height: 900
    Ui.HomeScreen { id: home; anchors.fill: parent }
    Ui.OnboardingScreen { id: onboarding; anchors.fill: parent; visible: false }
    // Changed once per frame wanted, so the window draws while Home is hidden.
    Rectangle {
        id: ticker
        width: 1
        height: 1
        z: 10
        color: "black"
    }
    SignalSpy { id: exploreStarted; target: home; signalName: "explore" }
    SignalSpy { id: exploreEdited; target: home; signalName: "exploreSetup" }
    TestCase {
        name: "HomeDongleStatus"
        when: windowShown
        function init() {
            // The brokered capability is CONSTANT: set it before publishing the host.
            testContext.setDongleStatus(false, 0, "");
            testContext.useLifecycleHost(true);
            home.showSection("systems");
        }
        function cleanup() {
            // Leave no focus on the pill for the next case's state changes.
            root.forceActiveFocus();
            var sheet = findChild(home, "addSystemMenu");
            if (sheet && sheet.visible)
                sheet.requestDismiss();
            testContext.setDeviceRequestPends(false);
            testContext.setDongleStatus(false, 0, "");
            testContext.setLocalDeviceRecord("usb", false, "");
            testContext.setReceiverHealth("ok", "");
            testContext.setAttachedDeviceSource("");
            testContext.setScanTestUsb(false, false);
            testContext.useLifecycleHost(false);
            root.width = 390;
            root.height = 900;
            Ui.Theme.resetFontScale();
            home.visible = true;
            onboarding.visible = false;
            home.showSection("systems");
            while (savedSystems.count)
                savedSystems.remove(0);
        }
        // Issue #646: saved systems enough that Listen's Systems tab scrolls at 360 x 400.
        function fillSystems() {
            for (var i = 0; i < 4; ++i)
                verify(savedSystems.add({name: "System " + i, sourceType: "rtltcp", host: "127.0.0.1", port: 1234, freqMhz: "851.5"}));
        }
        // Draws @a count frames while Home stays as it is: every polish a hidden Home would see runs in them.
        function waitFrames(count) {
            for (var i = 0; i < count; ++i) {
                ticker.color = ticker.color === Qt.color("black") ? "white" : "black";
                verify(waitForRendering(ticker), "frame " + i);
            }
        }
        function test_cleanup_restores_platform_font_binding() {
            var previousFont = testContext.applicationFont();
            try {
                // Qt.application.font is CONSTANT: change the font before cleanup
                // so the restored binding evaluates the platform's current value.
                testContext.setApplicationFont(Qt.font({pixelSize: 24}));
                Ui.Theme.fontScale = 1.6;
                cleanup();
                compare(Ui.Theme.fontScale, 1.5);
            } finally {
                testContext.setApplicationFont(previousFont);
                Ui.Theme.resetFontScale();
            }
        }
        function test_failure_data() {
            return [
                {tag: "busy", kind: 1, ready: true, text: "Android could not claim RTL-SDR. It may be held by another SDR app, or the OTG port may be under-powered — try a powered hub."},
                {tag: "open_failed", kind: 2, ready: false, text: "Android could not open RTL-SDR (open_failed)."},
                {tag: "native_other", kind: 2, ready: true, text: "Android could not open or claim RTL-SDR (code -3)."},
                {tag: "detached", kind: 3, ready: false, text: "Device detached: RTL-SDR"},
                {tag: "permission", kind: 4, ready: false, text: "No permission for RTL-SDR"}
            ];
        }
        function test_failure(data) {
            testContext.setDongleStatus(data.ready, data.kind, data.text);
            for (var page of [home, onboarding]) {
                home.visible = page === home;
                onboarding.visible = page === onboarding;
                var label = findChild(page, "dongleStatusText");
                verify(label !== null);
                tryCompare(label, "text", data.text);
                tryCompare(label, "visible", true);
                var retry = findChild(page, "dongleRetry");
                verify(retry !== null);
                tryCompare(retry, "visible", true);
                compare(retry.text, "Retry");
                var before = testContext.dongleRetryRequests();
                waitForRendering(page);
                mouseClick(retry);
                compare(testContext.dongleRetryRequests(), before + 1);
            }
        }
        // Issue #802 B12: the record's text names the USB device as the device
        // names itself, drawn and measured as written, never as markup.
        function test_device_name_draws_as_written() {
            var text = "Android could not open <b>RTL2838</b> (open_failed).";
            testContext.setDongleStatus(false, 2, text);
            for (var page of [home, onboarding]) {
                home.visible = page === home;
                onboarding.visible = page === onboarding;
                var label = findChild(page, "dongleStatusText");
                tryCompare(label, "text", text);
                var mirror = Qt.createQmlObject("import QtQuick; Text { visible: false; textFormat: Text.PlainText }", root);
                mirror.font = label.font;
                mirror.text = label.text;
                var plainWidth = mirror.implicitWidth;
                mirror.destroy();
                verify(Math.abs(label.implicitWidth - plainWidth) <= 0.5, (page === home ? "Home" : "Onboarding") + " draws as written: " + label.implicitWidth + " vs " + plainWidth);
                compare(label.textFormat, Text.PlainText, (page === home ? "Home" : "Onboarding") + " is plain text");
            }
        }
        function test_short_screen_retry_is_reachable() {
            root.width = 320;
            root.height = 360;
            Ui.Theme.fontScale = 1.4;
            testContext.setDongleStatus(true, 1, test_failure_data()[0].text);
            for (var page of [home, onboarding]) {
                home.visible = page === home;
                onboarding.visible = page === onboarding;
                var body = findChild(page, "dongleScrollBody");
                var retry = findChild(page, "dongleRetry");
                var label = findChild(page, "dongleStatusText");
                tryCompare(retry, "visible", true);
                waitForRendering(page);
                verify(label.width <= page.width);
                verify(label.height >= label.contentHeight);
                body.contentY = retry.mapToItem(body.contentItem, 0, 0).y;
                verify(retry.mapToItem(body, 0, 0).y >= -1);
                verify(retry.mapToItem(body, 0, retry.height).y <= body.height + 1);
                var before = testContext.dongleRetryRequests();
                mouseClick(retry);
                compare(testContext.dongleRetryRequests(), before + 1);
            }
        }
        function test_home_routes_explain_all_sources() {
            var add = findChild(home, "addSystemButton");
            verify(add !== null);
            compare(add.text, "+ Add a system");
            compare(findChild(home, "addSystemHint").text, "USB, Airspy, network or file");
            compare(typeof home.networkSource, "undefined");
            compare(typeof home.importScanList, "undefined");
            verify(typeof onboarding.networkSource === "function", "onboarding keeps its shortcut");
        }
        // D5: Explore's More, and a long press on the card, open its menu; More never starts Explore. The menu's Edit
        // connection opens the setup, and a tap on the card's face starts.
        function test_explore_more_opens_menu() {
            var oldSource = prefs.exploreSourceType;
            prefs.exploreSourceType = "rtltcp";
            try {
                // Issue #646: on the Explore tab.
                home.showSection("explore");
                var card = findChild(home, "exploreCard");
                var more = findChild(home, "exploreManageButton");
                var face = findChild(home, "exploreStart");
                var menu = findChild(home, "exploreManageSheet");
                compare(more.accessibleName, "More options for Explore");
                compare(more.y, 0);
                compare(card.width - (more.x + more.width / 2), Ui.Theme.cardPadding + 26);
                compare(face.parent, more.parent, "the face is beside More, not around it");
                verify(face.x + face.width <= more.x, "the face, and so its focus ring, stops short of More's column");
                var scroll = findChild(home, "dongleScrollBody");
                verify(waitForPolish(home));
                scroll.contentY = Math.max(0, scroll.contentHeight - scroll.height);
                exploreStarted.clear();
                exploreEdited.clear();
                waitForRendering(home);
                mouseClick(more);
                tryCompare(menu, "visible", true);
                compare(exploreStarted.count, 0, "More never starts");
                compare(exploreEdited.count, 0, "More opens the menu, not the setup");
                var edit = findChild(menu, "editExploreConnectionButton");
                waitForRendering(edit);
                mouseClick(edit);
                tryCompare(menu, "visible", false);
                compare(exploreEdited.count, 1, "Edit connection opens the setup");
                compare(exploreStarted.count, 0);

                mousePress(face, face.width / 3, face.height / 2);
                wait(Qt.styleHints.mousePressAndHoldInterval + 300);
                mouseRelease(face, face.width / 3, face.height / 2);
                tryCompare(menu, "visible", true);
                compare(exploreStarted.count, 0, "a long press opens the menu");
                menu.cancel();
                tryCompare(menu, "visible", false);
                waitForRendering(home);
                mouseClick(card, card.width / 3, card.height / 2);
                compare(exploreStarted.count, 1);
                // The caret, in More's column under More, is still the card's to tap.
                mouseClick(card, card.width - Ui.Theme.cardPadding - 12, card.height - Ui.Theme.cardPadding - 4);
                compare(exploreStarted.count, 2, "a tap on the caret starts");
                compare(exploreEdited.count, 1);
            } finally {
                prefs.exploreSourceType = oldSource;
            }
        }
        // At large text the subtitle wraps rather than cutting the frequency a tap starts on, and the card grows
        // to hold it.
        function test_explore_subtitle_whole_at_large_text_data() {
            return [{tag: "320", width: 320}, {tag: "360", width: 360}];
        }
        function test_explore_subtitle_whole_at_large_text(data) {
            var saved = {source: prefs.exploreSourceType, freq: prefs.exploreFreqMhz};
            try {
                prefs.exploreSourceType = "rtltcp";
                prefs.exploreFreqMhz = "851.0125";
                root.width = data.width;
                Ui.Theme.fontScale = 1.6;
                home.showSection("explore");
                var card = findChild(home, "exploreCard");
                var subtitle = findChild(home, "homeExploreSubtitle");
                verify(waitForPolish(home));
                compare(subtitle.text, "RTL-TCP · from 851.0125 MHz");
                verify(!subtitle.truncated, "the whole frequency shows");
                verify(subtitle.lineCount <= 2);
                tryVerify(function () {
                    return subtitle.mapToItem(card, 0, subtitle.height).y <= card.height - Ui.Theme.cardPadding + 1;
                }, 1000, "the card holds both lines");
            } finally {
                prefs.exploreSourceType = saved.source;
                prefs.exploreFreqMhz = saved.freq;
            }
        }
        // B3: the face is a button: Return, Enter, Space and a screen reader's press start Explore (or, with no radio
        // chosen, open its setup). The same on More open the menu at most and never start.
        function test_explore_keyboard_and_press_start_data() {
            return [{tag: "configured", source: "rtltcp"}, {tag: "not set up", source: ""}];
        }
        function test_explore_keyboard_and_press_start(data) {
            var oldSource = prefs.exploreSourceType;
            prefs.exploreSourceType = data.source;
            try {
                home.showSection("explore");
                var face = findChild(home, "exploreStart");
                var more = findChild(home, "exploreManageButton");
                var menu = findChild(home, "exploreManageSheet");
                var search = findChild(home, "signalSearchManageButton");
                compare(face.Accessible.role, Accessible.Button);
                compare(face.Accessible.name, data.source
                    ? "Explore the band, start on RTL-TCP from " + prefs.exploreFreqMhz + " MHz"
                    : "Explore the band, set up a radio to tune around and find what is on the air");
                var node = testContext.accessibleNode(face);
                verify(node.inTree, "the face is a node a screen reader reaches");
                for (var label of [findChild(face, "exploreTitle"), findChild(home, "homeExploreSubtitle")])
                    verify(label.Accessible.ignored, "its texts are read in the face's name, once");
                // Explore by touch: More's spot finds More, the rest of the card the face.
                var scroll = findChild(home, "dongleScrollBody");
                scroll.contentY = Math.max(0, scroll.contentHeight - scroll.height);
                waitForRendering(home);
                compare(testContext.accessibleNodeAt(more, more.width / 2, more.height / 2).object, more);
                compare(testContext.accessibleNodeAt(face, face.width / 3, face.height / 2).object, face);
                search.forceActiveFocus();
                keyClick(Qt.Key_Tab);
                verify(face.activeFocus, "Tab from Signal search's More reaches the face");
                keyClick(Qt.Key_Tab);
                verify(more.activeFocus, "then More");

                var spy = data.source ? exploreStarted : exploreEdited;
                var other = data.source ? exploreEdited : exploreStarted;
                spy.clear();
                other.clear();
                var count = 0;
                for (var key of [Qt.Key_Return, Qt.Key_Enter, Qt.Key_Space]) {
                    face.forceActiveFocus();
                    keyClick(key);
                    compare(spy.count, ++count, "key " + key + " on the face");
                }
                verify(testContext.pressAccessible(face), "the face offers a press");
                compare(spy.count, ++count, "a screen reader's press on the face");
                compare(other.count, 0);

                exploreStarted.clear();
                exploreEdited.clear();
                for (key of [Qt.Key_Return, Qt.Key_Enter, Qt.Key_Space]) {
                    more.forceActiveFocus();
                    keyClick(key);
                    tryCompare(menu, "visible", true);
                    menu.visible = false;
                }
                verify(testContext.pressAccessible(more), "More offers a press");
                tryCompare(menu, "visible", true);
                menu.visible = false;
                compare(exploreStarted.count, 0, "keys and presses on More never start");
                compare(exploreEdited.count, 0, "nor open the setup");
            } finally {
                prefs.exploreSourceType = oldSource;
            }
        }
        function test_retry_disabled_during_session() {
            testContext.setDongleStatus(true, 1, "Claim failed");
            testContext.setLifecyclePhase(3);
            var retry = findChild(home, "dongleRetry");
            tryCompare(retry, "enabled", false);
        }
        function test_recovery() {
            testContext.setDongleStatus(true, 1, "Claim failed");
            testContext.setDongleStatus(true, 0, "Ready: RTL-SDR");
            var retry = findChild(home, "dongleRetry");
            verify(retry !== null);
            tryCompare(retry, "visible", false);
            verify(home.dongleReady);
        }

        // #647 WP6 helpers: geometry in the screen's coordinates.
        function within(item, ancestor) {
            for (var node = item; node; node = node.parent) {
                if (node === ancestor)
                    return true;
            }
            return false;
        }
        function rectOf(item) {
            var at = item.mapToItem(home, 0, 0);
            return {x: at.x, y: at.y, w: item.width, h: item.height};
        }
        function contains(outer, inner) {
            return inner.x >= outer.x - 0.5 && inner.y >= outer.y - 0.5 && inner.x + inner.w <= outer.x + outer.w + 0.5 && inner.y + inner.h <= outer.y + outer.h + 0.5;
        }
        function overlaps(a, b) {
            return a.x < b.x + b.w - 0.5 && b.x < a.x + a.w - 0.5 && a.y < b.y + b.h - 0.5 && b.y < a.y + a.h - 0.5;
        }
        function requestsAfter(action) {
            var before = testContext.dongleRetryRequests();
            action();
            return testContext.dongleRetryRequests() - before;
        }

        function test_header_stays_pinned() {
            root.width = 360;
            root.height = 400;
            fillSystems();
            testContext.setAttachedDeviceSource("usb");
            var header = findChild(home, "listenHeader");
            var body = findChild(home, "dongleScrollBody");
            var pill = findChild(home, "dongleStatusPill");
            verify(header !== null, "brokered hosts get a pinned header");
            tryCompare(header, "visible", true);
            verify(!within(header, body), "the header is not part of the scrolling body");
            verify(within(pill, header));
            waitForRendering(home);
            var top = rectOf(header).y;
            verify(body.contentHeight > body.height, "the case needs a body that scrolls");
            verify(rectOf(body).y >= top + header.height - 0.5, "nothing scrolls under the header");
            body.contentY = body.contentHeight - body.height;
            waitForRendering(home);
            compare(rectOf(header).y, top);
            verify(pill.visible);
            verify(contains({x: 0, y: 0, w: home.width, h: home.height}, rectOf(pill)), "the pill stays on screen");
        }

        // Issue #646 B3: revealTop() wins over Home's other deferred scroll writers. A tab's place still owed is
        // dropped (a focus move while it is owed arms nothing), and so is a focus reveal still pending; the top becomes
        // the tab's place, and a focus handed back later (a sheet restoring its opener) does not scroll it away.
        function test_reveal_top() {
            root.width = 360;
            root.height = 400;
            fillSystems();
            var body = findChild(home, "dongleScrollBody");
            verify(waitForPolish(home));
            waitForRendering(home);
            var plays = [];
            (function look(node) {
                for (var child of node.children) {
                    if (child.objectName === "savedSystemPlay" && child.visible)
                        plays.push(child);
                    look(child);
                }
            })(findChild(home, "listenSystemsSection"));
            compare(plays.length, 4);
            // A place still owed.
            body.contentY = body.contentHeight - body.height;
            verify(body.contentY > 0);
            home.showSection("explore");
            home.showSection("systems");
            verify(home.restorePending, "Systems' place is owed");
            home.revealTop();
            compare(body.contentY, body.originY);
            verify(!home.restorePending, "the owed place is dropped");
            waitFrames(3);
            compare(body.contentY, body.originY, "nothing owed scrolled the top away");
            verify(plays[2].mapToItem(body, 0, 0).y > body.height, "the control handed the focus is out of view at the top");
            plays[2].forceActiveFocus();
            waitFrames(3);
            compare(body.contentY, body.originY, "a focus handed back later scrolls nothing");
            home.showSection("explore");
            home.showSection("systems");
            waitFrames(3);
            compare(body.contentY, body.originY, "the top is Systems' place now");

            // A focus reveal still pending: the keyboard reached a control out of view, and the refusal came first.
            verify(!home.restorePending && !home.focusRevealHeld, "nothing owed, nothing held");
            verify(plays[3].mapToItem(body, 0, 0).y > body.height, "the focused control is out of view at the top");
            plays[3].forceActiveFocus();
            home.revealTop();
            waitFrames(3);
            compare(body.contentY, body.originY, "the pending reveal was dropped");
            root.forceActiveFocus();
        }

        function test_receiver_on_the_bus_is_not_missing() {
            // B3: plugged in but not yet opened reads as something to connect, not as absent.
            testContext.setAttachedDeviceSource("usb");
            var label = findChild(home, "dongleStatusLabel");
            tryCompare(label, "text", "CONNECT RTL-SDR");
            var detail = findChild(home, "dongleStatusText");
            verify(detail.text.indexOf("Plug in") < 0, detail.text);
            compare(findChild(home, "dongleRetry").text, "Connect");
            testContext.setAttachedDeviceSource("airspy");
            tryCompare(label, "text", "CONNECT AIRSPY");
            testContext.setAttachedDeviceSource("");
            tryCompare(label, "text", "NO RECEIVER");
        }

        function test_pill_follows_every_source() {
            // B1: the pill belongs to the brokered host, not to whichever source just failed.
            home.localDeviceRelevant = false;
            try {
                var pill = findChild(home, "dongleStatusPill");
                verify(pill.visible);
                testContext.setDongleStatus(false, 2, "Android could not open Airspy (open_failed).");
                testContext.setLocalDeviceRecord("", false, "airspy");
                tryCompare(findChild(home, "dongleStatusLabel"), "text", "AIRSPY ERROR");
                verify(pill.visible);
            } finally {
                home.localDeviceRelevant = true;
            }
        }

        function test_pill_activation_data() {
            return [
                {tag: "connect", ready: false, pending: false, actionable: true},
                {tag: "ready", ready: true, pending: false, actionable: false},
                {tag: "waiting", ready: false, pending: true, actionable: false}
            ];
        }
        function test_pill_activation(data) {
            testContext.setAttachedDeviceSource("usb");
            testContext.setDongleStatus(data.ready, 0, data.ready ? "Ready: RTL-SDR" : "");
            testContext.setLocalDeviceRecord(data.ready ? "usb" : "", data.pending, data.pending ? "usb" : "");
            var pill = findChild(home, "dongleStatusPill");
            tryCompare(pill, "visible", true);
            waitForRendering(home);
            var expected = data.actionable ? 1 : 0;
            compare(pill.Accessible.role, data.actionable ? Accessible.Button : Accessible.StaticText);
            compare(requestsAfter(function () {
                mouseClick(pill);
            }), expected, "tap");
            pill.forceActiveFocus();
            compare(requestsAfter(function () {
                keyClick(Qt.Key_Return);
            }), expected, "Return");
            compare(requestsAfter(function () {
                keyClick(Qt.Key_Space);
            }), expected, "Space");
            compare(requestsAfter(function () {
                pill.Accessible.pressAction();
            }), expected, "accessibility press");
        }

        function test_pill_inert_behind_a_manage_sheet() {
            testContext.setAttachedDeviceSource("usb");
            var pill = findChild(home, "dongleStatusPill");
            tryCompare(pill, "visible", true);
            pill.forceActiveFocus();
            var sheet = findChild(home, "addSystemMenu");
            sheet.open();
            tryCompare(home, "managementSheetOpen", true);
            compare(requestsAfter(function () {
                pill.Accessible.pressAction();
            }), 0, "accessibility press");
            // The header is disabled under the sheet, so the pill cannot take
            // focus; Return and the tap below are belt-and-braces checks.
            pill.forceActiveFocus();
            verify(!pill.activeFocus, "the pill takes no focus behind the sheet");
            compare(requestsAfter(function () {
                keyClick(Qt.Key_Return);
            }), 0, "Return");
            compare(requestsAfter(function () {
                mouseClick(pill);
            }), 0, "tap");
        }

        function test_pill_keeps_focus_when_the_prompt_opens() {
            // Android leaves the permission prompt pending after the press, so
            // the focused pill turns inert under the keyboard user. It keeps
            // focus and its tab stop, quietly, like the other controls.
            failOnWarning(/activeFocusOnTab/);
            testContext.setDeviceRequestPends(true);
            testContext.setAttachedDeviceSource("usb");
            var pill = findChild(home, "dongleStatusPill");
            var label = findChild(home, "dongleStatusLabel");
            tryCompare(label, "text", "CONNECT RTL-SDR");
            pill.forceActiveFocus();
            verify(pill.activeFocus);
            compare(requestsAfter(function () {
                keyClick(Qt.Key_Return);
            }), 1, "Return asks once");
            tryCompare(label, "text", "RTL-SDR WAITING");
            verify(!pill.actionable);
            compare(pill.Accessible.role, Accessible.StaticText);
            verify(pill.activeFocus, "focus stays where the user left it");
            verify(pill.activeFocusOnTab);
            compare(requestsAfter(function () {
                keyClick(Qt.Key_Return);
            }), 0, "the waiting pill asks nothing");
            // The prompt expires: the same focused pill acts again.
            testContext.setLocalDeviceRecord("", false, "usb");
            tryCompare(label, "text", "CONNECT RTL-SDR");
            compare(requestsAfter(function () {
                keyClick(Qt.Key_Return);
            }), 1);
        }

        function test_scroll_kept_across_tabs() {
            // Main hides Home on the other tabs; nothing on it may collapse
            // then, or the body loses the place the user scrolled to.
            root.width = 360;
            root.height = 400;
            // Cards enough that the height set below keeps the Listen tabs fixed (issue #646): a shorter Home would
            // move them into the body and change the range being set.
            fillSystems();
            testContext.setAttachedDeviceSource("usb");
            var body = findChild(home, "dongleScrollBody");
            var header = findChild(home, "listenHeader");
            tryCompare(findChild(home, "dongleRetry"), "visible", true);
            verify(waitForPolish(home));
            waitForRendering(home);
            var headerHeight = header.height;
            verify(headerHeight > 0);
            // A body that scrolls by less than the details panel is tall, so
            // the panel collapsing would leave nothing to scroll.
            root.height += body.contentHeight - body.height - 60;
            verify(waitForPolish(home));
            verify(home.tabsFixed, "the Listen tabs stay fixed at this height");
            var panel = findChild(home, "dongleDetailsPanel");
            var panelHeight = panel.height;
            verify(panelHeight > 60);
            for (var scrolled of [40, body.contentHeight - body.height]) {
                verify(scrolled > 0 && scrolled <= body.contentHeight - body.height);
                body.contentY = scrolled;
                home.visible = false;
                compare(header.height, headerHeight, "the hidden header keeps its height");
                compare(panel.height, panelHeight, "the hidden details panel keeps its height");
                compare(body.contentHeight - body.height >= scrolled, true, "the hidden body still scrolls that far");
                // And still after the hidden body's next layout passes (issue #646 B1).
                waitFrames(3);
                compare(header.height, headerHeight, "the header keeps its height through a layout while hidden");
                compare(panel.height, panelHeight, "the details panel keeps its height through a layout while hidden");
                compare(body.contentY, scrolled, "the hidden body keeps its place through a layout");
                home.visible = true;
                verify(waitForPolish(home));
                compare(body.contentY, scrolled, "scroll position after a tab switch");
            }
        }

        function test_reflow_data() {
            var rows = [];
            for (var width of [320, 360, 411]) {
                for (var scale of [1.0, 1.6, 2.0])
                    rows.push({tag: width + " px at " + scale, width: width, scale: scale});
            }
            rows.push({tag: "supporting pane at 2.0", width: 900, scale: 2.0});
            return rows;
        }
        function test_reflow(data) {
            root.width = data.width;
            root.height = 690;
            Ui.Theme.fontScale = data.scale;
            // The widest label the pill shows.
            testContext.setAttachedDeviceSource("usb");
            testContext.setDongleStatus(false, 4, "USB permission denied");
            testContext.setLocalDeviceRecord("", false, "usb");
            var header = findChild(home, "listenHeader");
            var title = findChild(home, "listenHeaderTitle");
            var pill = findChild(home, "dongleStatusPill");
            var label = findChild(home, "dongleStatusLabel");
            tryCompare(label, "text", "RTL-SDR NO ACCESS");
            waitForRendering(home);
            var h = rectOf(header);
            var p = rectOf(pill);
            var t = {x: rectOf(title).x, y: rectOf(title).y, w: title.contentWidth, h: title.contentHeight};
            verify(h.x >= Ui.Theme.screenPadding - 0.5, "header x " + h.x);
            var right = home.supportingPane ? home.width - 280 - 2 * Ui.Theme.screenPadding : home.width - Ui.Theme.screenPadding;
            verify(h.x + h.w <= right + 0.5, "header right " + (h.x + h.w) + " > " + right);
            verify(contains(h, p), "pill " + JSON.stringify(p) + " outside header " + JSON.stringify(h));
            verify(contains(h, t), "title outside header");
            verify(!overlaps(p, t), "pill " + JSON.stringify(p) + " overlaps title " + JSON.stringify(t));
            verify(!label.truncated, "label truncated");
            verify(contains(p, {x: rectOf(label).x, y: rectOf(label).y, w: label.contentWidth, h: label.contentHeight}), "label spills out of the pill");
            verify(pill.height >= Ui.Theme.minimumTouchSize);
            verify(pill.width >= Ui.Theme.minimumTouchSize);
            verify(rectOf(findChild(home, "dongleScrollBody")).y >= h.y + h.h - 0.5, "body under the header");
            // The panel's action keeps its whole word at every text size.
            var action = findChild(home, "dongleRetry");
            for (var word of ["Retry", "Connect"]) {
                if (word === "Connect") {
                    testContext.setDongleStatus(false, 0, "");
                    testContext.setLocalDeviceRecord("", false, "");
                }
                tryCompare(action, "text", word);
                verify(waitForPolish(home));
                var found = 0;
                for (var child of action.children) {
                    if (child.text === word) {
                        ++found;
                        compare(child.lineCount, 1, word + " wraps");
                        verify(!child.truncated, word + " elided");
                    }
                }
                compare(found, 1, "the button's label is where the check looks");
            }
        }

        // Issue #646: the desktop's title is fixed above the Listen tabs as the brokered host's is, shown once, with
        // no receiver pill; the body starts under the tabs and runs to the bottom.
        function test_non_brokered_title_above_the_tabs() {
            testContext.useLifecycleHost(false);
            var body = findChild(home, "dongleScrollBody");
            var header = findChild(home, "listenHeader");
            var title = findChild(home, "listenHeaderTitle");
            tryCompare(header, "visible", true);
            verify(title.visible);
            verify(!within(header, body), "the title stays put above the body");
            compare(title.text, "Listen");
            compare(header.y, Ui.Theme.screenPadding);
            compare(header.height, Math.max(44, title.implicitHeight));
            verify(!findChild(home, "listenTitle").visible, "the body's own title row is not shown as well");
            var pill = findChild(home, "dongleStatusPill");
            verify(pill === null || !pill.visible, "no pill");
            var tabs = findChild(home, "listenTabs");
            verify(rectOf(tabs).y >= rectOf(header).y + header.height, "the tabs under the title");
            compare(body.y, rectOf(tabs).y + tabs.height);
            compare(body.height, home.height - body.y);
        }

        function test_onboarding_names_the_receiver_data() {
            return [
                {tag: "Airspy connected", ready: true, kind: 0, text: "Ready: Airspy Mini", source: "airspy", requested: "airspy", attached: "airspy", title: "Airspy connected", button: ""},
                {tag: "Airspy plugged in", ready: false, kind: 0, text: "", source: "", requested: "", attached: "airspy", title: "Airspy detected", button: "Connect"},
                {tag: "Airspy not found", ready: false, kind: 3, text: "No Airspy attached", source: "", requested: "airspy", attached: "", title: "No Airspy detected", button: "Retry"},
                {tag: "Airspy no access", ready: false, kind: 4, text: "USB permission denied", source: "", requested: "airspy", attached: "airspy", title: "Airspy needs attention", button: "Retry"},
                {tag: "RTL-SDR connected", ready: true, kind: 0, text: "Ready: RTL-SDR", source: "usb", requested: "usb", attached: "usb", title: "RTL-SDR connected", button: ""},
                {tag: "nothing plugged in", ready: false, kind: 0, text: "", source: "", requested: "", attached: "", title: "No receiver detected", button: "Connect"}
            ];
        }
        function test_onboarding_names_the_receiver(data) {
            home.visible = false;
            onboarding.visible = true;
            testContext.setAttachedDeviceSource(data.attached);
            testContext.setDongleStatus(data.ready, data.kind, data.text);
            testContext.setLocalDeviceRecord(data.source, false, data.requested);
            var title = findChild(onboarding, "dongleStatusTitle");
            verify(title !== null);
            tryCompare(title, "text", data.title);
            var detail = findChild(onboarding, "dongleStatusText");
            if (data.text.length)
                compare(detail.text, data.text);
            else
                verify(detail.text.length > 0);
            var retry = findChild(onboarding, "dongleRetry");
            compare(retry.visible, data.button.length > 0);
            if (data.button.length) {
                compare(retry.text, data.button);
                // Its whole word at the largest text size.
                Ui.Theme.fontScale = 2.0;
                verify(waitForPolish(onboarding));
                var found = 0;
                for (var child of retry.children) {
                    if (child.text === data.button) {
                        ++found;
                        compare(child.lineCount, 1, data.button + " wraps");
                    }
                }
                compare(found, 1, "the button's label is where the check looks");
            }
        }

        function test_onboarding_desktop_wording_unchanged() {
            testContext.useLifecycleHost(false);
            home.visible = false;
            onboarding.visible = true;
            tryCompare(findChild(onboarding, "dongleStatusTitle"), "text", "RTL-SDR dongle connected");
            compare(findChild(onboarding, "dongleStatusText").text, "RTL2832U · USB-OTG · ready");
            compare(findChild(onboarding, "dongleRetry").visible, false);
        }
    }
}
