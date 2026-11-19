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
            testContext.useLicenseNotices(true);
            testContext.useLifecycleHost(false);
        }
        function rowFor(screen, name) {
            var rows = findChild(screen, "licenseFileRows");
            for (var i = 0; i < rows.count; ++i) {
                var row = rows.itemAt(i);
                if (row.title === name)
                    return row;
            }
            return null;
        }
        // Taps the row of @p name once the screen is laid out (its safe area settles a frame after it opens).
        function tapRow(screen, name) {
            tryVerify(function () {
                var row = rowFor(screen, name);
                return row !== null && row.width > 0 && screen.height > 0;
            });
            mouseClick(rowFor(screen, name));
        }
        function test_list_then_one_file_and_header_back() {
            var screen = findChild(appLoader.item, "licensesScreen");
            verify(screen.visible);
            // Where Qt's source code is, then the list of files: no file's text is laid out yet.
            var source = findChild(screen, "licensesQtSource");
            compare(source.text, decoderHost.qtSourceNotice());
            compare(source.text.indexOf("Qt source code\n"), 0);
            verify(source.text.indexOf("https://download.qt.io/archive/qt/") > 0);
            verify(source.text.indexOf("licenses/Qt-SOURCE.txt") > 0);
            verify(source.readOnly && source.selectByMouse);
            var rows = findChild(screen, "licenseFileRows");
            compare(rows.count, decoderHost.licenseFiles().length);
            compare(rows.count, 2);
            compare(rows.itemAt(0).title, "LICENSE");
            compare(rows.itemAt(1).title, "licenses/gone.txt");
            compare(rows.itemAt(0).accessibleName, "LICENSE");
            var fileText = findChild(screen, "licenseFileText");
            verify(!fileText.visible);
            compare(fileText.text, "");
            verify(!findChild(screen, "licensesMissing").visible);
            var title = findChild(screen, "licensesTitle");
            compare(title.text, "Open source licenses");

            // A file opens on its own, under its name; the list goes away until Back.
            tapRow(screen, "LICENSE");
            compare(screen.openFile, "LICENSE");
            compare(title.text, "LICENSE");
            verify(fileText.visible);
            compare(fileText.text, decoderHost.licenseFileText("LICENSE"));
            verify(fileText.text.indexOf("GNU GENERAL PUBLIC LICENSE") >= 0);
            verify(fileText.readOnly && fileText.selectByMouse);
            compare(fileText.wrapMode, TextEdit.Wrap);
            verify(!findChild(screen, "licensesScroll").visible);
            var fileScroll = findChild(screen, "licenseFileScroll");
            verify(fileScroll.contentHeight > fileScroll.height);
            compare(fileScroll.accessibleName, "LICENSE");
            var back = findChild(screen, "licensesBack");
            compare(back.icon, "back");
            mouseClick(back);
            compare(screen.openFile, "");
            compare(fileText.text, "");
            compare(title.text, "Open source licenses");
            verify(findChild(screen, "licensesScroll").visible);
            compare(appLoader.item.licensesOpen, true);

            // A listed file the package lacks opens as missing.
            tapRow(screen, "licenses/gone.txt");
            compare(fileText.text, "This file is missing from the package.");
            mouseClick(back);
            mouseClick(back);
            compare(appLoader.item.licensesOpen, false);
        }
        function test_system_back() {
            var screen = findChild(appLoader.item, "licensesScreen");
            tapRow(screen, "LICENSE");
            compare(screen.openFile, "LICENSE");
            appLoader.item.requestBack();
            compare(screen.openFile, "");
            compare(appLoader.item.licensesOpen, true);
            appLoader.item.requestBack();
            compare(appLoader.item.licensesOpen, false);
            compare(appLoader.item.currentTab, 2);
        }
        function test_closing_forgets_the_open_file() {
            var screen = findChild(appLoader.item, "licensesScreen");
            tapRow(screen, "LICENSE");
            appLoader.item.licensesOpen = false;
            appLoader.item.licensesOpen = true;
            compare(screen.openFile, "");
            verify(findChild(screen, "licensesScroll").visible);
        }
        function test_missing_files() {
            var screen = findChild(appLoader.item, "licensesScreen");
            appLoader.item.licensesOpen = false;
            testContext.useLicenseNotices(false);
            appLoader.item.licensesOpen = true;
            compare(findChild(screen, "licenseFileRows").count, 0);
            var missing = findChild(screen, "licensesMissing");
            verify(missing.visible);
            compare(missing.text, "The license and notice files are missing from this package.");
            compare(findChild(screen, "licensesQtSource").text, decoderHost.qtSourceNotice());
        }
    }
}
