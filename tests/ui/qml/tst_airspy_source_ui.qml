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
                // The DSP bandwidth is every radio's (issue #806): TETRA and
                // AM hold step 1 on an Airspy until it fits, so it is shown.
                verify(findChild(wizard, "systemBandwidthField").visible, source + ": systemBandwidthField");
            }
        }

        // Issue #806: on an Airspy the Advanced pane names only what it shows
        // (its own gain controls and bias tee, no PPM), and the bandwidth field,
        // alone in its row, takes the row and its own label without the
        // caption the three narrow columns share.
        function test_wizard_airspy_advanced_pane() {
            screenLoader.source = uiDir + "/WizardScreen.qml";
            var wizard = screenLoader.item;
            verify(wizard !== null);
            wizard.openForAdd(false);
            wizard.step = 1;
            wizard.advancedOpen = true;
            var subtitle = findChild(wizard, "wizardAdvancedSubtitle");
            var caption = findChild(wizard, "wizardBandwidthCaption");
            var bw = findChild(wizard, "systemBandwidthField");
            verify(subtitle !== null && caption !== null && bw !== null);
            var row = bw.parent.parent;
            for (var source of ["airspy", "usb", "rtltcp", "airspy"]) {
                wizard.sourceType = source;
                var airspy = source === "airspy";
                compare(subtitle.text, airspy ? "Gain, bandwidth, bias tee — defaults work" : "Gain, PPM, bandwidth, bias tee — defaults work", source);
                compare(caption.visible, !airspy, source + ": caption");
                if (airspy)
                    compare(bw.width, row.width, source + ": full row");
                else
                    verify(bw.width < row.width / 2, source + ": a third of the row");
            }
        }

        function test_wizard_network_port_visibility() {
            screenLoader.source = uiDir + "/WizardScreen.qml";
            var wizard = screenLoader.item;
            verify(wizard !== null);
            wizard.openForAdd(false);
            var port = findChild(wizard, "wizardPortField");
            verify(port !== null);
            for (var source of ["airspy", "rtltcp", "udp", "tcp", "usb", "file", "airspy"]) {
                wizard.sourceType = source;
                compare(port.visible, ["rtltcp", "udp", "tcp"].indexOf(source) >= 0, source);
            }
        }

        function test_scan_list_shared_tuner_visibility() {
            screenLoader.source = uiDir + "/ScanListScreen.qml";
            var screen = screenLoader.item;
            verify(screen !== null);
            screen.openFor(-1);
            screen.advancedOpen = true;
            screen.gainMode = 2;
            screen.ppmSet = true;
            screen.bandwidthSet = true;
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
