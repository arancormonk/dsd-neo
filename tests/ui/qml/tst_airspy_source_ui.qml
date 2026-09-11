// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest

Item {
    width: 480
    height: 900

    Loader {
        id: screenLoader
        anchors.fill: parent
    }

    TestCase {
        name: "AirspySourceUi"
        when: windowShown

        function cleanup() {
            screenLoader.source = "";
        }

        function test_wizard_shared_tuner_visibility() {
            screenLoader.source = uiDir + "/WizardScreen.qml";
            var wizard = screenLoader.item;
            verify(wizard !== null);
            wizard.openForAdd(false);
            wizard.step = 1;
            wizard.advancedOpen = true;
            var controls = ["systemGainField", "systemPpmField", "systemBiasTee"];
            for (var source of ["airspy", "usb", "rtltcp", "airspy"]) {
                wizard.sourceType = source;
                for (var name of controls) {
                    var control = findChild(wizard, name);
                    verify(control !== null, name);
                    compare(control.visible, source !== "airspy", source + ": " + name);
                }
            }
        }

        function test_scan_list_shared_tuner_visibility() {
            screenLoader.source = uiDir + "/ScanListScreen.qml";
            var screen = screenLoader.item;
            verify(screen !== null);
            screen.openFor(-1);
            for (var source of ["airspy", "usb", "rtltcp", "airspy"]) {
                screen.draft = Object.assign({}, screen.draft, {sourceType: source});
                for (var name of ["scanTuner_gainDb", "scanTuner_ppm", "scanBiasTee"]) {
                    var control = findChild(screen, name);
                    verify(control !== null, name);
                    compare(control.visible, source !== "airspy", source + ": " + name);
                }
                // Bandwidth and scan timing still apply to Airspy.
                for (var key of ["bandwidthKhz", "defaultDwellMs", "defaultHoldMs"])
                    verify(findChild(screen, "scanTuner_" + key).visible, key);
            }
        }

        function test_failure_source_label() {
            screenLoader.source = uiDir + "/Main.qml";
            var app = screenLoader.item;
            verify(app !== null);
            app.attemptedSource = {system: {sourceType: "airspy"}};
            compare(app.sourceLabel(), "Airspy R2 / Mini");
            app.attemptedSource = {system: {sourceType: "airspy", extraArgs: "-F"}};
            compare(app.sourceLabel(), "Configured source: Airspy R2 / Mini (Advanced arguments are also in use)");
            app.attemptedSource = {system: {sourceType: "usb"}};
            compare(app.sourceLabel(), "USB RTL-SDR");
        }
    }
}
