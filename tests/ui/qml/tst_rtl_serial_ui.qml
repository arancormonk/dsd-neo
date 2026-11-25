// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui
import "../../../src/ui/qt/qml/Util.js" as Util

// A USB dongle chosen by its serial, for several dongles whose indexes change
// between boots: a field under the Airspy's own label in the system editor's
// Advanced pane and in a scan list's tuner, for a USB dongle only. The serial
// is kept exactly as typed (the decoder matches it with case), and one no
// rtl:serial= spec can carry holds the save with the field's reason.
Item {
    width: 420
    height: 900

    Loader {
        id: screenLoader
        anchors.fill: parent
    }

    TestCase {
        id: tc

        name: "RtlSerialUi"
        when: windowShown

        readonly property string label: "Serial (empty selects first device)"
        readonly property string invalidText: "Enter up to 127 printable ASCII characters, with no : or \" and no space at either end."

        function cleanup() {
            screenLoader.source = "";
            Ui.Navigation.rootSurfaces = [];
            while (savedSystems.count)
                savedSystems.remove(0);
            while (scanLists.count)
                scanLists.remove(0);
        }

        function openWizard() {
            screenLoader.source = uiDir + "/WizardScreen.qml";
            var wizard = screenLoader.item;
            verify(wizard !== null);
            wizard.openForAdd(false);
            return wizard;
        }

        function openScanList() {
            screenLoader.source = uiDir + "/ScanListScreen.qml";
            var screen = screenLoader.item;
            verify(screen !== null);
            screen.openFor(-1);
            return screen;
        }

        // The same rule as the decoder's dsd_rtl_serial_is_valid(), which the
        // argv build applies again (UI_QT_SESSION_ARGS).
        function test_rule_data() {
            return [
                {tag: "empty", text: "", valid: true},
                {tag: "digits", text: "00000001", valid: true},
                {tag: "mixed case and inner space", text: "AbC 1", valid: true},
                {tag: "127 characters", text: "x".repeat(127), valid: true},
                {tag: "128 characters", text: "x".repeat(128), valid: false},
                {tag: "colon", text: "A:B", valid: false},
                {tag: "quote", text: "A\"B", valid: false},
                {tag: "leading space", text: " A", valid: false},
                {tag: "trailing space", text: "A ", valid: false},
                {tag: "tab", text: "A\tB", valid: false},
                {tag: "not ASCII", text: "café", valid: false}
            ];
        }
        function test_rule(data) {
            compare(Util.rtlSerialValid(data.text), data.valid);
            compare(Util.rtlSerialError(data.text), data.valid ? "" : tc.invalidText);
        }

        function test_wizard_field_only_for_a_usb_dongle() {
            var wizard = openWizard();
            wizard.step = 1;
            wizard.advancedOpen = true;
            var field = findChild(wizard, "systemRtlSerialField");
            verify(field !== null);
            compare(field.label, tc.label, "the Airspy field's words");
            for (var source of ["usb", "airspy", "rtltcp", "usb"]) {
                wizard.sourceType = source;
                compare(field.visible, source === "usb", source);
            }
        }

        function test_wizard_saves_the_serial_as_typed() {
            var wizard = openWizard();
            wizard.step = 1;
            wizard.advancedOpen = true;
            compare(wizard.rtlSerialText, "", "a new system opens the first device");
            var clean = wizard.draftFingerprint();
            wizard.rtlSerialText = "AbC 01";
            verify(wizard.draftFingerprint() !== clean, "the typed serial is unsaved work");
            verify(wizard.stepValid());
            wizard.nameText = "Dongle B";
            wizard.commit();
            compare(savedSystems.count, 1);
            compare(savedSystems.get(0).rtlSerial, "AbC 01", "case and spaces kept");

            // Edited, the field shows it; Add starts over with none.
            wizard.openForEdit(0);
            compare(wizard.rtlSerialText, "AbC 01");
            verify(!wizard.hasUnsavedDraft());
            wizard.openForAdd(false);
            compare(wizard.rtlSerialText, "");
        }

        function test_wizard_refuses_a_serial_no_spec_can_carry() {
            var wizard = openWizard();
            wizard.step = 1;
            wizard.advancedOpen = true;
            var field = findChild(wizard, "systemRtlSerialField");
            wizard.rtlSerialText = "00000001 ";
            compare(field.error, tc.invalidText);
            verify(!wizard.stepValid(), "the tune step holds");
            wizard.nameText = "Dongle";
            wizard.commit();
            compare(savedSystems.count, 0, "nothing is saved");
            // On another source the field is not the system's, and holds nothing.
            wizard.sourceType = "rtltcp";
            verify(wizard.stepValid());
            wizard.commit();
            compare(savedSystems.count, 1);
            compare(savedSystems.get(0).rtlSerial, "", "only a USB dongle saves a serial");
        }

        // Save as system from a found session (the Spectrum's Save passes the
        // session's own map, no detection): a USB session that named its
        // dongle by serial, such as Explore here on a Signal search hit, saves
        // that serial. Another source saves none, and a detection names its own.
        function test_found_session_keeps_its_rtl_serial_data() {
            return [
                {tag: "spectrum usb", sys: {sourceType: "usb", rtlSerial: "AbC 1"}, serial: "AbC 1"},
                {tag: "spectrum first device", sys: {sourceType: "usb"}, serial: ""},
                {tag: "spectrum rtltcp", sys: {sourceType: "rtltcp", host: "127.0.0.1", port: 1234, rtlSerial: "AbC 1"}, serial: ""},
                {tag: "detection overrides", sys: {sourceType: "usb", rtlSerial: "AbC 1"},
                    detection: {sourceType: "usb", rtlSerial: "00000002"}, serial: "00000002"},
                {tag: "detection on the first device", sys: {sourceType: "usb", rtlSerial: "AbC 1"},
                    detection: {sourceType: "usb"}, serial: ""}
            ];
        }
        function test_found_session_keeps_its_rtl_serial(data) {
            var wizard = openWizard();
            wizard.openForFound(data.sys, "851.5000", data.detection);
            compare(wizard.rtlSerialText, data.serial);
            verify(!wizard.hasUnsavedDraft(), "what the found session names is the draft's start, not an edit");
            wizard.nameText = "Found";
            wizard.decodeFlag = "-fA";
            wizard.commit();
            compare(savedSystems.count, 1);
            compare(savedSystems.get(0).rtlSerial, data.serial);
        }

        // A system saved with a serial the editor refuses (an import, a restored
        // backup) opens on the field's reason, not a held Continue.
        function test_wizard_reveals_a_refused_saved_serial() {
            verify(savedSystems.add({name: "Imported", sourceType: "usb", freqMhz: "851.5", rtlSerial: "A:B"}));
            var wizard = openWizard();
            wizard.openForEdit(0);
            verify(wizard.advancedOpen, "the Advanced pane opens on the error");
            compare(findChild(wizard, "systemRtlSerialField").error, tc.invalidText);
        }

        function test_scan_list_field_only_for_a_usb_dongle() {
            var screen = openScanList();
            var field = findChild(screen, "scanRtlSerial");
            verify(field !== null);
            compare(field.label, tc.label);
            for (var source of ["usb", "airspy", "rtltcp", "usb"]) {
                screen.draft = Object.assign({}, screen.draft, {sourceType: source});
                compare(field.visible, source === "usb", source);
            }
        }

        function test_scan_list_saves_the_serial_as_typed() {
            var screen = openScanList();
            screen.draft.name = "Dongles";
            screen.addFrequency("Simplex", "p25", "851.5");
            var field = findChild(screen, "scanRtlSerial");
            var clean = screen.fingerprint();
            field.input.forceActiveFocus();
            keyClick("a");
            keyClick("B");
            compare(screen.draft.rtlSerial, "aB", "typing reaches the draft as typed");
            verify(screen.fingerprint() !== clean, "the typed serial is unsaved work");
            screen.save();
            compare(scanLists.count, 1);
            compare(scanLists.get(0).rtlSerial, "aB");
            screen.openFor(0);
            compare(field.text, "aB", "the saved list shows it");
        }

        function test_scan_list_refuses_a_serial_no_spec_can_carry() {
            var screen = openScanList();
            screen.draft.name = "Dongles";
            screen.addFrequency("Simplex", "p25", "851.5");
            screen.draft = Object.assign({}, screen.draft, {rtlSerial: " 00000001"});
            var field = findChild(screen, "scanRtlSerial");
            compare(field.error, tc.invalidText);
            verify(!screen.validate());
            compare(screen.validationText, tc.invalidText);
            screen.save();
            compare(scanLists.count, 0, "nothing is saved");
            // Another tuner saves none.
            screen.draft = Object.assign({}, screen.draft, {sourceType: "rtltcp", host: "127.0.0.1"});
            screen.save();
            compare(scanLists.count, 1);
            compare(scanLists.get(0).rtlSerial, "");
        }
    }
}
