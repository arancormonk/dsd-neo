// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest

Item {
    width: 480
    height: 900
    Loader { id: loader; width: 440; source: uiDir + "/AirspyControls.qml" }
    SignalSpy { id: edits; target: loader.item; signalName: "edited" }
    TestCase {
        name: "AirspyControls"
        when: windowShown
        function init() {
            verify(loader.item !== null);
            loader.item.settings = ({});
            edits.clear();
        }
        function test_native_gain_modes() {
            var mode = findChild(loader.item, "airspy_gain_mode");
            verify(mode !== null);
            compare(mode.currentIndex, 0);
            loader.item.settings = {gain_mode: "manual", lna_gain: 12, bias_tee: 0};
            compare(mode.currentIndex, 2);
            var gain = findChild(loader.item, "airspy_lna_gain");
            compare(gain.text, "12");
            gain.text = "15";
            gain.editingFinished();
            compare(edits.count, 1);
            compare(edits.signalArguments[0][0], "lna_gain");
            compare(edits.signalArguments[0][1], "15");
            // The display changes only when the owner publishes an applied setting.
            compare(loader.item.settings.lna_gain, 12);
        }
        function test_rate_and_serial_persistence() {
            var native = {serial: "0123456789ABCDEF", sample_rate: "3000000", gain_mode: "linearity", linearity_gain: 17};
            var row = savedSystems.count;
            verify(savedSystems.add({name: "Airspy test", sourceType: "airspy", freqMhz: "851.375", airspy: native}));
            var saved = savedSystems.get(row);
            compare(saved.sourceType, "airspy");
            compare(saved.airspy.serial, native.serial);
            compare(saved.airspy.sample_rate, "3000000");
            compare(saved.airspy.linearity_gain, 17);
            savedSystems.remove(row);
        }
    }
}
