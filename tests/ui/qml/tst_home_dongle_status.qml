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
    TestCase {
        name: "HomeDongleStatus"
        when: windowShown
        function init() {
            // The brokered capability is CONSTANT: set it before publishing the host.
            testContext.setDongleStatus(false, 0, "");
            testContext.useLifecycleHost(true);
        }
        function cleanup() {
            testContext.setDongleStatus(false, 0, "");
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
    }
}
