// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest

Item {
    width: 420
    height: 900
    Loader { id: appLoader; anchors.fill: parent }
    TestCase {
        name: "LicensesScreen"
        when: windowShown
        function init() {
            testContext.useLifecycleHost(true);
            prefs.onboardingDone = true;
            appLoader.source = uiDir + "/Main.qml";
            verify(appLoader.item !== null);
            appLoader.item.currentTab = 2;
            appLoader.item.licensesOpen = true;
        }
        function cleanup() {
            appLoader.source = "";
            testContext.useLifecycleHost(false);
        }
        function test_text_and_header_back() {
            var screen = findChild(appLoader.item, "licensesScreen");
            verify(screen.visible);
            var text = findChild(screen, "licenseNoticesText");
            verify(text !== null);
            verify(text.text.indexOf("GNU GENERAL PUBLIC LICENSE") >= 0);
            compare(text.text, decoderHost.licenseNotices());
            verify(text.readOnly && text.selectByMouse);
            compare(text.wrapMode, TextEdit.Wrap);
            var scroll = findChild(screen, "licensesScroll");
            verify(scroll.contentHeight > scroll.height);
            scroll.contentY = scroll.contentHeight - scroll.height;
            var back = findChild(screen, "licensesBack");
            compare(back.icon, "back");
            mouseClick(back);
            compare(appLoader.item.licensesOpen, false);
        }
        function test_system_back() {
            appLoader.item.requestBack();
            compare(appLoader.item.licensesOpen, false);
            compare(appLoader.item.currentTab, 2);
        }
    }
}
