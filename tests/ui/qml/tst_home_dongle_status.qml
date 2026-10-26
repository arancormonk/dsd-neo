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
    SignalSpy { id: exploreStarted; target: home; signalName: "explore" }
    SignalSpy { id: exploreEdited; target: home; signalName: "exploreSetup" }
    TestCase {
        name: "HomeDongleStatus"
        when: windowShown
        function init() {
            // The brokered capability is CONSTANT: set it before publishing the host.
            testContext.setDongleStatus(false, 0, "");
            testContext.useLifecycleHost(true);
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
        function test_explore_more_only_edits_connection() {
            var oldSource = prefs.exploreSourceType;
            prefs.exploreSourceType = "rtltcp";
            try {
                var card = findChild(home, "exploreCard");
                var more = findChild(home, "exploreManageButton");
                compare(more.accessibleName, "Edit Explore connection");
                compare(more.y, 0);
                compare(card.width - (more.x + more.width / 2), Ui.Theme.cardPadding + 26);
                var scroll = findChild(home, "dongleScrollBody");
                verify(waitForPolish(home));
                scroll.contentY = Math.max(0, scroll.contentHeight - scroll.height);
                exploreStarted.clear();
                exploreEdited.clear();
                waitForRendering(home);
                mouseClick(more);
                compare(exploreEdited.count, 1);
                compare(exploreStarted.count, 0);
                mouseClick(card, card.width / 3, card.height / 2);
                compare(exploreStarted.count, 1);
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

        function test_reveal_top() {
            root.width = 360;
            root.height = 400;
            var body = findChild(home, "dongleScrollBody");
            waitForRendering(home);
            body.contentY = body.contentHeight - body.height;
            verify(body.contentY > 0);
            home.revealTop();
            compare(body.contentY, body.originY);
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
            testContext.setAttachedDeviceSource("usb");
            var body = findChild(home, "dongleScrollBody");
            var header = findChild(home, "listenHeader");
            tryCompare(findChild(home, "dongleRetry"), "visible", true);
            verify(waitForPolish(home));
            var headerHeight = header.height;
            verify(headerHeight > 0);
            // A body that scrolls by less than the details panel is tall, so
            // the panel collapsing would leave nothing to scroll.
            root.height += body.contentHeight - body.height - 60;
            verify(waitForPolish(home));
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

        function test_non_brokered_layout_unchanged() {
            testContext.useLifecycleHost(false);
            var body = findChild(home, "dongleScrollBody");
            var title = findChild(home, "listenTitle");
            verify(title !== null);
            tryCompare(title, "visible", true);
            verify(within(title, body), "the title scrolls with the body, as before");
            compare(title.text, "Listen");
            compare(title.parent.height, 44);
            compare(title.parent.mapToItem(body.contentItem, 0, 0).y, Ui.Theme.screenPadding);
            var header = findChild(home, "listenHeader");
            verify(header === null || !header.visible, "no pinned header");
            var pill = findChild(home, "dongleStatusPill");
            verify(pill === null || !pill.visible, "no pill");
            compare(body.y, 0);
            compare(body.height, home.height);
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
