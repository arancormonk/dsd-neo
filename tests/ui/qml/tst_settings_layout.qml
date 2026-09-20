// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest

Item {
    width: 420
    height: 900
    Loader { id: appLoader; anchors.fill: parent }
    TextEdit { id: clipboardProbe; visible: false }

    TestCase {
        name: "SettingsLayout"
        when: windowShown
        property var app
        property var settings
        property bool previousBuildHasAppKey: false

        function init() {
            previousBuildHasAppKey = radioReference.buildHasAppKey;
            testContext.useLifecycleHost(true);
            prefs.onboardingDone = true;
            testContext.setRadioReference("available", true);
            appLoader.source = uiDir + "/Main.qml";
            app = appLoader.item;
            verify(app !== null);
            app.currentTab = 2;
            settings = findChild(app, "idleSettingsScreen");
            verify(settings !== null);
            tryVerify(function () { return settings.visible && settings.opacity > 0.9; });
        }

        function cleanup() {
            appLoader.source = "";
            testContext.setRadioReference("buildHasAppKey", previousBuildHasAppKey);
            testContext.setRadioReference("available", false);
            testContext.setRadioReference("credentialsReady", false);
            testContext.useLifecycleHost(false);
        }

        function test_group_order_and_disclosure() {
            var names = ["Appearance", "Units", "Listening", "Decoding · next start",
                         "Radio defaults · next start", "Libraries", "Account", "About & support"];
            var headers = ["Appearance", "Units", "Listening", "Decoding", "RadioDefaults", "Libraries", "Account", "Support"];
            var previousY = -1;
            for (var i = 0; i < names.length; ++i) {
                var header = findChild(settings, "settings" + headers[i] + "Header");
                verify(header !== null, names[i]);
                compare(header.text, names[i]);
                var y = header.mapToItem(settings, 0, 0).y;
                verify(y > previousY, names[i] + " is out of order");
                previousY = y;
            }
            compare(settings.advancedOpen, false);
            var scroll = findChild(settings, "settingsScroll");
            var header = findChild(settings, "settingsRadioDefaultsHeader");
            scroll.contentY += header.mapToItem(scroll, 0, 0).y - 100;
            waitForRendering(settings);
            mouseClick(header);
            compare(settings.advancedOpen, true);
            compare(findChild(settings, "settingsImportsRow").subtitle,
                    "Channel maps, talkgroups, keys, band plans, radio IDs");
        }

        function test_destinations_data() {
            return [
                {tag: "imports", row: "settingsImportsRow", flag: "importsOpen", screen: "importsScreen"},
                {tag: "account", row: "settingsAccountRow", flag: "radioReferenceAccountOpen", screen: "radioReferenceAccountScreen"},
                {tag: "diagnostics", row: "settingsDiagnosticsRow", flag: "diagnosticsOpen", screen: "diagnosticsScreen"},
                {tag: "licenses", row: "settingsLicensesRow", flag: "licensesOpen", screen: "licensesScreen"}
            ];
        }

        function test_destinations(data) {
            // Account management remains reachable after login.
            testContext.setRadioReference("credentialsReady", true);
            var row = findChild(settings, data.row);
            verify(row !== null);
            row.activate();
            compare(app[data.flag], true);
            tryVerify(function () { return findChild(app, data.screen).visible; });
            compare(uiController.autoStartBlocked, true);
            app.requestBack();
            compare(app[data.flag], false);
            compare(app.currentTab, 2);
        }

        function test_account_availability() {
            var row = findChild(settings, "settingsAccountRow");
            verify(row.visible);
            testContext.setRadioReference("available", false);
            tryCompare(row, "visible", false);
        }

        function test_import_companion_back_and_radioreference_route() {
            app.requestActivate();
            tryCompare(app, "active", true);
            findChild(settings, "settingsImportsRow").activate();
            var imports = findChild(app, "importsScreen");
            tryCompare(imports, "enabled", true);
            var flow = findChild(imports, "importsCsvImport");
            var source = testContext.writeFixtureCsv("settings-companion.csv",
                    "channel,frequency,mode,keys_dec_csv\n1,851012500,p25,missing.csv\n");
            verify(source.length > 0);
            var count = importedFiles.count;
            flow.begin(source, "Map.csv", "chan");
            var sheet = findChild(flow, "csvCompanionSheet");
            tryCompare(sheet, "visible", true);
            tryVerify(function () { return sheet.activeFocus; });
            compare(uiController.autoStartBlocked, true);
            keyClick(Qt.Key_Escape);
            tryCompare(sheet, "visible", false);
            compare(flow.active, false);
            compare(imports.pendingRow, -1);
            compare(imports.notice, "");
            compare(importedFiles.count, count);
            compare(app.importsOpen, true);
            findChild(imports, "importFromRadioReferenceButton").activate();
            tryCompare(app, "radioReferenceOpen", true);
            compare(app.radioReferenceAccountOpen, false);
            app.requestBack();
            compare(app.radioReferenceOpen, false);
            compare(app.importsOpen, true);
            app.requestBack();
            compare(app.importsOpen, false);
            compare(app.currentTab, 2);
        }

        function test_account_edits_persist() {
            var oldUsername = prefs.rrUsername;
            var oldKey = prefs.rrAppKey;
            testContext.setRadioReference("buildHasAppKey", false);
            findChild(settings, "settingsAccountRow").activate();
            var account = findChild(app, "radioReferenceAccountScreen");
            var username = findChild(account, "radioReferenceUsernameField");
            var key = findChild(account, "radioReferenceAppKeyField");
            verify(findChild(account, "radioReferencePasswordField") === null);
            compare(username.label, "Username");
            compare(key.label, "Application key");
            username.text = "account-fixture";
            username.editingFinished();
            key.text = "fixture-key";
            key.editingFinished();
            app.requestBack();
            compare(prefs.rrUsername, "account-fixture");
            compare(prefs.rrAppKey, "fixture-key");
            findChild(settings, "settingsAccountRow").activate();
            compare(username.text, "account-fixture");
            compare(key.text, "fixture-key");
            prefs.rrUsername = oldUsername;
            prefs.rrAppKey = oldKey;
        }

        function test_version_copy() {
            var row = findChild(settings, "settingsVersionRow");
            verify(row !== null);
            compare(row.showCaret, false);
            row.activate();
            clipboardProbe.text = "";
            clipboardProbe.paste();
            compare(clipboardProbe.text, "DSD-neo " + appVersionText.replace(/^v/, ""));
            compare(row.subtitle, "Version copied");
            tryCompare(row, "subtitle", "DSD-neo " + appVersionText.replace(/^v/, "") + " · GPL-3.0", 4000);
        }
    }
}
