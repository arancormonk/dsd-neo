// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest

// The layers over a running session — the wizard, and the spectrum — cover the
// monitor completely, and both carry a full-width button along the bottom. So
// does the monitor: "Stop listening" sits at the same rect underneath them.
//
// TapHandlers never take exclusive grabs, so covering the monitor is not enough
// to stop a tap reaching it; only `enabled` does that. Without it one tap on the
// layer above runs both handlers, and the button the user actually pressed
// finishes by ending the session. That shipped: "Explore from here" — the one
// way out of a view-only spectrum — stopped the session instead of offering to
// hand the tuner over.
//
// This asserts the guard, not the geometry: the rects are free to move apart,
// but nothing underneath an opaque layer should be taking taps regardless.
Item {
    id: root

    width: 420
    height: 900
    readonly property bool buildHasAppKey: radioReference.buildHasAppKey

    FakeRadioReference {
        id: fakeImport
        credentialsReady: true
    }

    Loader {
        id: appLoader

        anchors.fill: parent
        source: uiDir + "/Main.qml"
    }

    TestCase {
        id: tc

        name: "MainOverlayLayering"
        when: windowShown

        property var app: null
        property var monitor: null
        property var imports: null
        property var radioReference: null
        property var wizard: null
        property bool previousBuildHasAppKey: false
        property var accountPrefs: null
        property string previousRrUsername: ""
        property string previousRrAppKey: ""

        function initTestCase() {
            tc.app = appLoader.item
            verify(tc.app !== null, "Main.qml failed to load")
            tc.monitor = findChild(tc.app, "monitorScreen")
            verify(tc.monitor !== null, "the monitor screen is missing")
            tc.imports = findChild(tc.app, "importsScreen")
            verify(tc.imports !== null, "the imports screen is missing")
            tc.radioReference = findChild(tc.app, "radioReferenceScreen")
            verify(tc.radioReference !== null, "the RadioReference screen is missing")
            tc.wizard = findChild(tc.app, "wizardScreen")
            verify(tc.wizard !== null, "the wizard screen is missing")
            // Keep the production prefs for capture/restore; the default map cannot persist edits.
            testContext.useLifecycleHost(true);
            accountPrefs = prefs;
            testContext.useLifecycleHost(false);
        }

        function init() {
            previousBuildHasAppKey = root.buildHasAppKey;
            previousRrUsername = accountPrefs.rrUsername;
            previousRrAppKey = accountPrefs.rrAppKey;
            tc.app.sessionDestination = "";
            tc.app.licensesOpen = false
            tc.app.radioReferenceAccountOpen = false
            tc.app.diagnosticsOpen = false
            tc.app.spectrumOpen = false
            tc.app.wizardOpen = false
            tc.app.importsOpen = false
            tc.app.radioReferenceOpen = false
        }

        function cleanup() {
            testContext.useRadioReferenceModel(null);
            tc.app.licensesOpen = false;
            tc.app.radioReferenceAccountOpen = false;
            tc.app.diagnosticsOpen = false;
            tc.app.sessionDestination = "";
            testContext.setRadioReference("buildHasAppKey", previousBuildHasAppKey);
            accountPrefs.rrUsername = previousRrUsername;
            accountPrefs.rrAppKey = previousRrAppKey;
            testContext.useLifecycleHost(false);
        }

        function test_session_settings_import_returns_to_wizard_then_monitor() {
            testContext.useLifecycleHost(true, false, true);
            testContext.useRadioReferenceModel(fakeImport);
            fakeImport.imports = 0;
            tc.app.startWithMap(tc.app.exploreSystem("rtltcp", "127.0.0.1", 1234, "851.0125"), -1, false);
            tryCompare(decoderHost, "running", true);
            tryCompare(tc.monitor, "enabled", true);
            tc.monitor.openSessionMenu();
            var menu = findChild(tc.app, "sessionMenu");
            tryCompare(menu, "visible", true);
            findChild(menu, "sessionMenuSettings").activate();
            compare(tc.app.sessionDestination, "settings");
            var settings = findChild(tc.app, "sessionSettingsScreen");
            verify(settings !== null && settings.visible);
            findChild(settings, "settingsImportsRow").activate();
            tryCompare(tc.imports, "enabled", true);
            findChild(tc.imports, "importFromRadioReferenceButton").activate();
            tryCompare(tc.radioReference, "enabled", true);
            tc.radioReference.openSystem(6673);
            compare(tc.radioReference.selectedSites.length, 1);
            var importButton = findChild(tc.radioReference, "radioReferenceImportButton");
            verify(importButton.enabled);
            importButton.activate();
            var confirm = findChild(tc.radioReference, "radioReferenceConfirmImport");
            verify(confirm !== null && confirm.visible && confirm.enabled);
            confirm.activate();
            compare(fakeImport.imports, 1);
            compare(fakeImport.savedRow, -1, "the wizard must remain the saved-system writer");
            tryCompare(tc.wizard, "enabled", true);
            compare(tc.app.radioReferenceOpen, false);
            compare(tc.app.importsOpen, false);
            compare(tc.app.sessionDestination, "");
            verify(!settings.visible, "session Settings covers the imported wizard");
            verify(!tc.monitor.enabled);
            compare(tc.wizard.freqText, "851.0125");
            compare(tc.wizard.importedSite.rrSid, 6673);
            compare(tc.wizard.importedSite.rrSiteId, 1);
            compare(tc.wizard.step, 1);
            tc.app.requestBack();
            compare(tc.wizard.step, 0, "Back must reach the visible wizard");
            tc.app.requestBack();
            var discard = findChild(tc.wizard, "wizardDiscardDialog");
            verify(discard !== null && discard.visible);
            var discardButton = findChild(discard, "discardDialogDiscard");
            verify(discardButton !== null);
            discardButton.activate();
            tryCompare(tc.app, "wizardOpen", false);
            tryCompare(tc.monitor, "enabled", true);
            verify(tc.monitor.visible);
            compare(decoderHost.running, true, "leaving the wizard must keep listening");
        }

        function test_support_escape_data() {
            return [
                {tag: "diagnostics-root", flag: "diagnosticsOpen", screen: "diagnosticsScreen", field: ""},
                {tag: "licenses-root", flag: "licensesOpen", screen: "licensesScreen", field: ""},
                {tag: "account-root", flag: "radioReferenceAccountOpen", screen: "radioReferenceAccountScreen", field: ""},
                {tag: "account-username", flag: "radioReferenceAccountOpen", screen: "radioReferenceAccountScreen", field: "radioReferenceUsernameField", pref: "rrUsername", value: "escape-user"},
                {tag: "account-app-key", flag: "radioReferenceAccountOpen", screen: "radioReferenceAccountScreen", field: "radioReferenceAppKeyField", pref: "rrAppKey", value: "escape-key"}
            ];
        }

        function test_support_escape(data) {
            if (data.field.length)
                testContext.useLifecycleHost(true);
            tc.app.requestActivate();
            tryCompare(tc.app, "active", true);
            testContext.setRadioReference("buildHasAppKey", false);
            tc.app.sessionDestination = "settings";
            tc.app[data.flag] = true;
            var screen = findChild(tc.app, data.screen);
            verify(screen !== null);
            tryCompare(tc.app, "activeFocusItem", screen);
            if (data.field.length) {
                var field = findChild(screen, data.field);
                verify(field !== null && field.visible);
                field.input.forceActiveFocus();
                tryCompare(tc.app, "activeFocusItem", field.input);
                field.input.selectAll();
                for (var i = 0; i < data.value.length; ++i)
                    keyClick(data.value.charAt(i));
                keyClick(Qt.Key_Return);
                compare(prefs[data.pref], data.value);
            }
            keyClick(Qt.Key_Escape);
            tryCompare(tc.app, data.flag, false);
            compare(tc.app.sessionDestination, "settings");
        }

        function test_support_layers_and_back_order() {
            tc.app.requestActivate();
            tryCompare(tc.app, "active", true);
            tc.app.sessionDestination = "settings";
            tc.app.licensesOpen = true;
            tryCompare(tc.app, "activeFocusItem", findChild(tc.app, "licensesScreen"));
            compare(uiController.autoStartBlocked, true);
            verify(!tc.monitor.enabled);
            keyClick(Qt.Key_Escape);
            compare(tc.app.licensesOpen, false);
            compare(tc.app.sessionDestination, "settings");
            tc.app.radioReferenceAccountOpen = true;
            tryCompare(tc.app, "activeFocusItem", findChild(tc.app, "radioReferenceAccountScreen"));
            compare(uiController.autoStartBlocked, true);
            verify(!tc.monitor.enabled);
            keyClick(Qt.Key_Escape);
            compare(tc.app.radioReferenceAccountOpen, false);
            compare(tc.app.sessionDestination, "settings");
            tc.app.diagnosticsOpen = true;
            var diagnostics = findChild(tc.app, "diagnosticsScreen");
            findChild(diagnostics, "diagnosticsClear").activate();
            var confirm = findChild(diagnostics, "diagnosticsClearConfirm");
            verify(confirm.visible);
            tryVerify(function () { return confirm.activeFocus; });
            keyClick(Qt.Key_Escape);
            tryCompare(confirm, "visible", false);
            compare(tc.app.diagnosticsOpen, true);
            keyClick(Qt.Key_Escape);
            compare(tc.app.diagnosticsOpen, false);
            compare(tc.app.sessionDestination, "settings");
            tc.app.requestBack();
            compare(tc.app.sessionDestination, "");
            compare(uiController.autoStartBlocked, false);
        }

        // The baseline the other cases are measured against: with a live session
        // and nothing over it, the monitor is the screen and takes its own taps.
        function test_01_the_monitor_takes_taps_when_it_is_the_top_layer() {
            tryVerify(function () { return tc.monitor.enabled },
                      2000, "the monitor was inert with nothing covering it")
        }

        function test_02_the_spectrum_stops_taps_reaching_the_monitor() {
            tc.app.spectrumOpen = true
            tryVerify(function () { return !tc.monitor.enabled },
                      2000, "a tap on the spectrum also reaches the monitor underneath")

            // And closing it hands the screen back, or the monitor would be dead
            // to touch for the rest of the session.
            tc.app.spectrumOpen = false
            tryVerify(function () { return tc.monitor.enabled },
                      2000, "the monitor stayed inert after the spectrum closed")
        }

        function test_03_the_wizard_stops_taps_reaching_the_monitor() {
            tc.app.wizardOpen = true
            tryVerify(function () { return !tc.monitor.enabled },
                      2000, "a tap on the wizard also reaches the monitor underneath")

            tc.app.wizardOpen = false
            tryVerify(function () { return tc.monitor.enabled },
                      2000, "the monitor stayed inert after the wizard closed")
        }

        // The imports library goes the other way: it is reached from Settings
        // rather than opened over a session, so the monitor keeps the screen and
        // the library stands down. Either direction is fine — what is not is
        // both being lit and enabled at once, and here the library's bottom
        // "Import file" button sits exactly over "Stop listening".
        function test_05_the_imports_library_stands_down_for_the_monitor() {
            tc.app.importsOpen = true
            // Waited out rather than tryVerify'd: `enabled` follows an animated
            // opacity, so "it is inert" is true on the fade's first frame no
            // matter what the binding says. What is under test is where the
            // layer settles, which is only knowable after the 150ms fade.
            wait(400)
            verify(!tc.imports.enabled,
                   "a tap on the imports library also reaches the monitor underneath")
            verify(tc.monitor.enabled,
                   "the monitor gave up its taps to a layer that is standing down")

            // Both inert would leave a live session with nothing taking taps.
            tc.app.importsOpen = false
            tryVerify(function () { return tc.monitor.enabled },
                      2000, "the monitor stayed inert after the imports library closed")
        }

        // The RadioReference import screen goes the wizard's way, not the
        // library's: the wizard opens over a running session ("Save as a
        // system") and pushes this screen from its tune step, so it has to stay
        // lit over the monitor and take the taps itself. Its bottom "Import this
        // system" button sits at the same rect as "Stop listening", so the
        // monitor is what stands down.
        function test_06_the_radioreference_screen_takes_taps_over_the_monitor() {
            tc.app.radioReferenceOpen = true
            // Waited out rather than tryVerify'd, for the reason test_05 gives:
            // `enabled` follows a 150ms animated opacity, so a reading taken on
            // the fade's first frame says nothing about what the binding settles
            // on either way.
            wait(400)
            verify(tc.radioReference.enabled,
                   "the RadioReference screen stood down and cannot be operated")
            verify(!tc.monitor.enabled,
                   "a tap on the RadioReference screen also reaches the monitor underneath")

            tc.app.radioReferenceOpen = false
            tryVerify(function () { return tc.monitor.enabled },
                      2000, "the monitor stayed inert after the RadioReference screen closed")
        }

        // The deadlock this layering exists to prevent. "Save as a system" from
        // a live session opens the wizard over the monitor (openForFound sets
        // step = 1), and the tune step leads with the RadioReference row — so
        // all three layers are up at once over an active session. Each one is
        // disabled by the layer above it, and there is no Keys.onBackPressed or
        // Shortcut anywhere in the QML tree, so if the top layer also stands
        // down nothing on screen takes a tap and the app cannot be recovered
        // short of killing the process.
        function test_07_the_wizard_pushing_radioreference_leaves_a_live_layer() {
            tc.app.wizardOpen = true
            tc.app.radioReferenceOpen = true
            wait(400)

            verify(tc.monitor.enabled || tc.wizard.enabled || tc.radioReference.enabled,
                   "monitor, wizard and RadioReference screen are all inert at once: "
                   + "the session is unrecoverable with no back key to escape it")
            // And specifically the top layer is the live one — anything else
            // means taps are landing on a screen the user cannot see.
            verify(tc.radioReference.enabled,
                   "the top layer is not the one taking taps")

            tc.app.radioReferenceOpen = false
            tryVerify(function () { return tc.wizard.enabled },
                      2000, "the wizard stayed inert after the RadioReference screen closed")
        }

        // The wizard opens over the spectrum ("Save as a system"), so both are up
        // at once. Whichever closes first must not re-arm the monitor while the
        // other is still covering it.
        function test_04_closing_one_layer_leaves_the_other_still_covering() {
            tc.app.spectrumOpen = true
            tc.app.wizardOpen = true
            tryVerify(function () { return !tc.monitor.enabled }, 2000)

            tc.app.wizardOpen = false
            tryVerify(function () { return !tc.monitor.enabled },
                      2000, "closing the wizard re-armed the monitor under the spectrum")

            tc.app.spectrumOpen = false
            tryVerify(function () { return tc.monitor.enabled }, 2000)
        }
    }
}
