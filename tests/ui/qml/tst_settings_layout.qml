// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest

Item {
    width: 420
    height: 900
    Loader { id: appLoader; anchors.fill: parent }
    FakeRadioReference { id: fakeAccount }
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
            testContext.useRadioReferenceModel(null);
            testContext.setRadioReference("accountState", 0);
            testContext.setRadioReference("accountMessage", "");
            testContext.setRadioReference("statusText", "");
            testContext.setRadioReference("hasPassword", false);
            testContext.setRadioReference("buildHasAppKey", previousBuildHasAppKey);
            testContext.setRadioReference("available", false);
            testContext.setRadioReference("credentialsReady", false);
            testContext.useLifecycleHost(false);
        }

        function test_group_order_and_disclosure() {
            var names = ["Appearance", "Units", "Listening", "Decoding · next start",
                         "Radio defaults · next start", "Libraries", "Account", "Backup", "About & support"];
            var headers = ["Appearance", "Units", "Listening", "Decoding", "RadioDefaults", "Libraries", "Account", "Backup", "Support"];
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
                {tag: "licenses", row: "settingsLicensesRow", flag: "licensesOpen", screen: "licensesScreen"},
                {tag: "call-recordings", row: "manageCallRecordingsRow", flag: "callRecordingsOpen", screen: "callRecordingsScreen"}
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
            var password = findChild(account, "radioReferencePasswordField");
            verify(password !== null && password.visible, "Settings must ask for the password it signs in with");
            compare(username.label, "Username");
            compare(password.label, "Password");
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

        function openAccount() {
            findChild(settings, "settingsAccountRow").activate();
            var account = findChild(app, "radioReferenceAccountScreen");
            tryVerify(function () { return account.visible; });
            return account;
        }
        // Typed in, as a keyboard does, so the field keeps its prefs binding.
        function typeInto(field, value) {
            field.input.forceActiveFocus();
            field.input.remove(0, field.input.text.length);
            field.input.insert(0, value);
        }

        // Regression (B1, B2): the row named an application key a keyed build
        // never asks for, and the screen behind it was a sign-in form with no
        // password. Both now follow the build.
        function test_account_subtitle_and_fields_follow_the_build_data() {
            return [{tag: "keyed", keyed: true, subtitle: "Username and password"},
                    {tag: "keyless", keyed: false, subtitle: "Username, password and application key"}];
        }
        function test_account_subtitle_and_fields_follow_the_build(data) {
            testContext.setRadioReference("buildHasAppKey", data.keyed);
            var row = findChild(settings, "settingsAccountRow");
            tryCompare(row, "subtitle", data.subtitle);
            var account = openAccount();
            verify(findChild(account, "radioReferenceUsernameField").visible);
            verify(findChild(account, "radioReferencePasswordField").visible);
            verify(findChild(account, "radioReferencePasswordReveal").visible);
            compare(findChild(account, "radioReferenceAppKeyField").visible, !data.keyed);
            compare(findChild(account, "radioReferenceAppKeyLink").visible, !data.keyed);
            verify(findChild(account, "radioReferenceCheckAccountButton").visible);
            compare(findChild(account, "radioReferencePasswordHint").text,
                    "DSD-neo keeps your password only until the app closes and never saves it. A RadioReference premium subscription is required.");
        }

        // The Settings form's result line reads the account check alone: an
        // import screen status ("Loading states…") never lands there, and
        // every verdict is worded, a failure with no message included.
        function test_account_result_line_reads_only_the_account_data() {
            return [
                {tag: "none", state: 0, text: ""},
                {tag: "checking", state: 1, text: "Checking your RadioReference account…"},
                {tag: "verified", state: 2, text: "Account verified."},
                {tag: "rejected keyed", state: 3, keyed: true, text: "RadioReference did not accept that username or password."},
                {tag: "rejected keyless", state: 3, keyed: false, text: "RadioReference did not accept that username, password or application key."},
                {tag: "expired", state: 4, text: "This account's RadioReference premium subscription has expired."},
                {tag: "failed", state: 5, message: "RadioReference could not be reached.", text: "RadioReference could not be reached."},
                {tag: "failed, no message", state: 5, message: "", text: "The account could not be checked. Try again."}
            ];
        }
        function test_account_result_line_reads_only_the_account(data) {
            testContext.setRadioReference("buildHasAppKey", data.keyed !== false);
            testContext.setRadioReference("statusText", "Loading states…");
            testContext.setRadioReference("accountMessage", data.message !== undefined ? data.message : "");
            testContext.setRadioReference("accountState", data.state);
            var account = openAccount();
            var result = findChild(account, "radioReferenceAccountResult");
            verify(result !== null, "the Settings form has no result line");
            compare(result.text, data.text);
            compare(result.visible, data.text.length > 0);
            verify(result.text.indexOf("Loading") < 0);
        }

        // One password-manager session per visible form: Settings begins on
        // show and ends on Back, and the import screen then begins its own.
        // A fill there lands in prefs, which both forms' username fields keep
        // following after a Settings edit (the binding survives).
        function test_sign_in_token_moves_with_the_visible_form() {
            testContext.setRadioReference("buildHasAppKey", true);
            signInAutofill.clear();
            var account = openAccount();
            var begins = signInAutofill.calls().filter(function (c) { return c.op === "begin"; });
            compare(begins.length, 1, "Settings did not begin a session");
            var settingsToken = begins[0].token;
            compare(signInAutofill.liveToken(), settingsToken);
            var username = findChild(account, "radioReferenceUsernameField");
            var oldUsername = prefs.rrUsername;
            typeInto(username, "settings-user");
            username.input.focus = false;
            compare(prefs.rrUsername, "settings-user");
            app.requestBack();
            tryVerify(function () { return !account.visible; });
            var ends = signInAutofill.calls().filter(function (c) { return c.op === "end"; });
            compare(ends.length, 1);
            compare(ends[0].token, settingsToken);
            compare(signInAutofill.liveToken(), 0);

            app.openRadioReference(false);
            var rr = findChild(app, "radioReferenceScreen");
            tryVerify(function () { return rr.visible; });
            var importToken = signInAutofill.liveToken();
            verify(importToken > settingsToken, "the import screen took no token of its own");
            var importUsername = findChild(rr, "radioReferenceUsernameField");
            compare(importUsername.text, "settings-user");
            // The relay applies the fill to prefs, then says so.
            prefs.rrUsername = "filled-user";
            signInAutofill.fill(importToken, true, false);
            compare(importUsername.text, "filled-user");
            app.radioReferenceOpen = false;
            tryVerify(function () { return !rr.visible; });
            compare(signInAutofill.liveToken(), 0);
            account = openAccount();
            compare(username.text, "filled-user", "the Settings username stopped following prefs");
            prefs.rrUsername = oldUsername;
            compare(username.text, oldUsername);
        }

        // A password entered on Settings is the session's: the import screen
        // opens past its credentials card, and Change shows an empty draft
        // that says a password is held.
        function test_settings_password_serves_the_import_screen() {
            fakeAccount.resetAccount();
            testContext.useRadioReferenceModel(fakeAccount);
            var account = openAccount();
            var password = findChild(account, "radioReferencePasswordField");
            typeInto(password, "session-secret");
            password.input.focus = false;
            compare(fakeAccount.sessionPassword, "session-secret");
            compare(fakeAccount.accountChecks, 1);
            var result = findChild(account, "radioReferenceAccountResult");
            compare(result.text, "Checking your RadioReference account…");
            fakeAccount.settle(2);
            compare(result.text, "Account verified.");
            app.requestBack();
            tryVerify(function () { return !account.visible; });
            compare(password.text, "", "the Settings draft outlived its screen");

            app.openRadioReference(false);
            var rr = findChild(app, "radioReferenceScreen");
            tryVerify(function () { return rr.visible; });
            var card = findChild(rr, "radioReferenceCredentials");
            verify(!card.visible, "the import screen asked again for credentials Settings already has");
            findChild(rr, "radioReferenceChangeAccount").activate();
            tryCompare(card, "visible", true);
            var importPassword = findChild(rr, "radioReferencePasswordField");
            compare(importPassword.text, "");
            compare(importPassword.placeholderText, "Entered for this session");
            app.radioReferenceOpen = false;
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
