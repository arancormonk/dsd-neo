import "../../../src/ui/qt/qml" as Ui
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Window
import QtTest

Item {
    width: 360
    height: 900

    Ui.ScanListScreen {
        id: screen

        anchors.fill: parent
    }

    SignalSpy {
        id: closedSpy
        target: screen
        signalName: "closed"
    }

    Component {
        id: numericEditor
        Ui.ScanListScreen {
            width: 360
            height: 900
        }
    }

    Component {
        id: numericClosedSpy
        SignalSpy {
            signalName: "closed"
        }
    }

    TestCase {
        function init() {
            while (scanLists.count)
                scanLists.remove(0);
            screen.openFor(-1);
            closedSpy.clear();
        }

        function cleanup() {
            wait(0);
            screen.Window.window.requestActivate();
            while (scanLists.count)
                scanLists.remove(0);
            decoderHost.keyboardTop = -1;
            testContext.setPrefs("appearance", 0);
        }

        function test_cancel_target_import_preserves_draft_data() {
            return [{tag: "manual-primary"}, {tag: "csv-primary"}, {tag: "manual-sheet"}, {tag: "csv-sheet"}];
        }

        function test_cancel_target_import_preserves_draft(data) {
            screen.addFrequency("Keep unsaved entry", "p25", "851.5");
            if (data.tag.indexOf("csv") === 0) {
                screen.draft = Object.assign({}, screen.draft, {targetSource: "csv", targetsCsvPath: "keep.csv"});
                screen.targetRows = [{id: "Keep preview"}];
            }
            var before = screen.fingerprint();
            var initial = screen.initialDraft;
            var preview = JSON.stringify(screen.targetRows);
            if (data.tag.indexOf("primary") >= 0) {
                screen.importTargets();
                findChild(screen, "csvPrimaryPicker").reject();
            } else {
                var source = testContext.writeFixtureCsv("cancel-targets.csv",
                    "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes\none,p25-trunk,851012500,missing-map.csv,,,\n");
                findChild(screen, "targetCsvImport").begin(source, "Targets.csv", "trunkTargets");
                findChild(screen, "csvCompanionSheet").requestDismiss();
            }
            compare(screen.fingerprint(), before);
            compare(screen.initialDraft, initial);
            compare(JSON.stringify(screen.targetRows), preview);
        }

        function test_edit_entries_and_persist() {
            screen.draft.name = "Local";
            screen.addFrequency("Simplex", "p25", "851.5");
            compare(screen.entries.length, 1);
            screen.save();
            compare(scanLists.count, 1);
            var uid = scanLists.get(0).uid;
            screen.openFor(0);
            compare(screen.entries[0].freqMhz, "851.5");
            screen.setEntry(0, "enabled", false);
            screen.saveDraft();
            compare(scanLists.getByUid(uid).entries[0].enabled, false);
            verify(scanLists.getByUid(uid).isDraft);
        }

        function test_imported_target_preview_and_save() {
            var source = testContext.writeFixtureCsv("scan-targets.csv",
                "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,options\nCSV ID,p25-conventional,851500000,,,,,--scan-max-visit-ms 20000\n");
            var result = importedFiles.importFile(source, "Targets.csv", "trunkTargets");
            verify(result.ok, result.detail || "Import failed");
            try {
                screen.draft.name = "Imported";
                screen.selectTargets(result.path);
                verify(screen.csvMode);
                compare(screen.entries.length, 0);
                compare(screen.targetRows.length, 1);
                compare(screen.targetRows[0].id, "CSV ID");
                compare(screen.targetRows[0].dwellMs, -1);
                verify(visualChild(control("scanTargetCsvBadge"), function (item) {
                    return item.visible && item.text === "TARGET CSV";
                }) !== null);
                verify(screen.validate(), screen.validationText);
                screen.save();
                compare(scanLists.get(0).targetSource, "csv");
                compare(scanLists.get(0).targetsCsvPath, result.path);
                screen.openFor(0);
                compare(screen.targetRows.length, 1);
                var preview = visualChild(screen, "scanTargetPreview");
                verify(preview !== null);
                tryVerify(function() { return preview.height > 0; });
            } finally {
                importedFiles.remove(importedFiles.rowForPath(result.path));
            }
        }

        function test_reselect_manual_targets_keeps_entries() {
            screen.addFrequency("First unsaved target", "dmr", "461");
            screen.addFrequency("Second unsaved target", "nxdn", "462");
            var entries = JSON.stringify(screen.entries);
            var source = visualChild(screen, function (item) {
                return item.Accessible.name === "Scan targets";
            });
            verify(source !== null);
            compare(source.currentIndex, 0);
            compare(screen.entries.length, 2);
            compare(screen.draft.targetSource, "entries");

            source.selected(0);

            compare(screen.entries.length, 2);
            compare(JSON.stringify(screen.entries), entries);
            compare(screen.draft.targetSource, "entries");
        }

        function test_reselect_csv_targets_keeps_preview() {
            var fixture = testContext.writeFixtureCsv("scan-targets.csv",
                "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,options\nCSV ID,p25-conventional,851500000,,,,,--scan-max-visit-ms 20000\n");
            var result = importedFiles.importFile(fixture, "Targets.csv", "trunkTargets");
            verify(result.ok, result.detail || "Import failed");
            try {
                screen.selectTargets(result.path);
                compare(screen.targetRows.length, 1);
                compare(screen.targetRows[0].id, "CSV ID");
                var rows = JSON.stringify(screen.targetRows);
                var source = visualChild(screen, function (item) {
                    return item.Accessible.name === "Scan targets";
                });
                verify(source !== null);
                compare(source.currentIndex, 1);
                compare(screen.draft.targetsCsvPath, result.path);

                source.selected(1);

                compare(screen.draft.targetSource, "csv");
                compare(screen.draft.targetsCsvPath, result.path);
                compare(JSON.stringify(screen.targetRows), rows);

                source.selected(0);
                compare(screen.draft.targetSource, "entries");
                compare(screen.draft.targetsCsvPath, result.path);
                compare(JSON.stringify(screen.targetRows), rows);
                compare(screen.entries.length, 0);
            } finally {
                importedFiles.remove(importedFiles.rowForPath(result.path));
            }
        }

        function test_switch_to_csv_preserves_manual_entries() {
            screen.addFrequency("First unsaved target", "dmr", "461");
            screen.addFrequency("Second unsaved target", "nxdn", "462");
            var source = visualChild(screen, function (item) {
                return item.Accessible.name === "Scan targets";
            });
            verify(source !== null);
            compare(source.currentIndex, 0);
            compare(screen.entries.length, 2);

            source.selected(1);

            compare(screen.draft.targetSource, "csv");
            compare(screen.entries.length, 2);
            source.selected(0);
            compare(screen.entries[0].name, "First unsaved target");
            compare(screen.entries[1].name, "Second unsaved target");
            compare(screen.draft.targetsCsvPath, "");
            compare(screen.targetRows.length, 0);
        }

        function visualChild(item, name) {
            if (typeof name === "function" ? name(item) : item.objectName === name)
                return item;

            var kids = item.contentItem ? [item.contentItem] : item.children || [];
            for (var i = 0; i < kids.length; ++i) {
                var found = visualChild(kids[i], name);
                if (found)
                    return found;
            }
            return null;
        }

        function test_edit_keeps_row_and_focus() {
            screen.addFrequency("First", "dmr", "461");
            wait(20);
            var row = visualChild(screen, "scanEntryRow");
            verify(row !== null);
            row.forceActiveFocus();
            screen.setEntry(0, "name", "Second");
            wait(20);
            compare(visualChild(screen, "scanEntryRow"), row);
            verify(row.activeFocus);
        }

        function test_typing_keeps_cursor_and_persists_each_edit() {
            screen.draft.name = "Typing";
            screen.addFrequency("First", "dmr", "461");
            var field = null;
            tryVerify(function () {
                var control = visualChild(screen, "scanFrequencyName");
                field = control ? control.input : null;
                return field !== null;
            });
            field.forceActiveFocus();
            field.cursorPosition = 2;
            keyClick(Qt.Key_X, Qt.ShiftModifier);
            compare(screen.entries[0].name, "Fixrst");
            compare(field.cursorPosition, 3);
            verify(field.activeFocus);
            keyClick(Qt.Key_Y, Qt.ShiftModifier);
            compare(screen.entries[0].name, "Fixyrst");
            compare(field.cursorPosition, 4);
            verify(field.activeFocus);
            compare(visualChild(screen, "scanFrequencyName").input, field);
            screen.save();
            compare(scanLists.get(0).entries[0].name, "Fixyrst");
        }

        function test_reorder_remove() {
            screen.addFrequency("First", "dmr", "461");
            screen.addFrequency("Second", "nxdn", "462");
            screen.moveEntry(1, -1);
            compare(screen.entries[0].name, "Second");
            screen.removeEntry(0);
            compare(screen.entries.length, 1);
        }

        function test_validate_never_persists() {
            screen.draft.name = "Unsaved";
            screen.addFrequency("Simplex", "p25", "851.5");
            verify(screen.validate(), screen.validationText);
            compare(scanLists.count, 0);
            screen.closed();
            compare(scanLists.count, 0);
        }

        function test_invalid_requires_explicit_draft() {
            screen.draft.name = "Incomplete";
            screen.save();
            compare(scanLists.count, 0);
            verify(screen.validationText.length > 0);
            screen.saveDraft();
            compare(scanLists.count, 1);
            verify(scanLists.get(0).isDraft);
            verify(!scanListStarter.start(scanLists.get(0), null).ok);
        }

        function control(name) {
            var item = visualChild(screen, name);
            verify(item !== null, name);
            return item;
        }

        function edit(name, value) {
            var field = control(name);
            field.input.forceActiveFocus();
            field.input.selectAll();
            keyClick(Qt.Key_Backspace);
            for (var i = 0; i < value.length; ++i)
                keyClick(value.charAt(i));
            compare(field.text, value, name);
        }

        function choose(name, index) {
            var picker = control(name);
            while (picker.currentIndex < index)
                picker.incrementCurrentIndex();
            while (picker.currentIndex > index)
                picker.decrementCurrentIndex();
            picker.activated(index);
        }

        function test_header_back_and_discard() {
            var back = control("scanListBack");
            back.Accessible.pressAction();
            compare(closedSpy.count, 1);
            closedSpy.clear();
            edit("scanListName", "Unsaved");
            back.Accessible.pressAction();
            compare(closedSpy.count, 0);
            var dialog = findChild(screen, "scanListDiscardDialog");
            verify(dialog.visible);
            var discard = visualChild(dialog, function(item) { return item.text === "Discard changes" && item.activate; });
            verify(discard !== null);
            discard.Accessible.pressAction();
            compare(closedSpy.count, 1);
        }

        function test_advanced_hidden_by_default_and_keyboard_accessible() {
            verify(!control("scanAdvancedBody").visible);
            var toggle = control("scanAdvancedToggle");
            toggle.forceActiveFocus();
            keyClick(Qt.Key_Space);
            verify(control("scanAdvancedBody").visible);
            toggle.Accessible.pressAction();
            verify(!control("scanAdvancedBody").visible);
        }

        function test_advanced_navigation_uses_its_own_surface() {
            var toggle = control("scanAdvancedToggle");
            toggle.focus = false;
            var layer = {surface: toggle};
            Ui.Navigation.addLayer(layer);
            try {
                verify(toggle.activeFocusOnTab);
                verify(!toggle.Accessible.ignored);
                toggle.Accessible.pressAction();
                verify(screen.advancedOpen);
                toggle.forceActiveFocus();
                keyClick(Qt.Key_Space);
                verify(!screen.advancedOpen);
            } finally {
                Ui.Navigation.removeLayer(layer);
            }
        }

        function test_tuner_modes_have_accessible_names() {
            var names = {
                scanGainMode: "Gain",
                scanPpmMode: "Frequency correction",
                scanBandwidthMode: "Bandwidth",
                scanBiasTee: "Bias tee"
            };
            for (var name in names)
                compare(control(name).Accessible.name, names[name], name);
        }

        function test_tuner_object_names_identify_text_fields() {
            for (var key of ["gainDb", "ppm", "bandwidthKhz", "defaultDwellMs", "defaultHoldMs"]) {
                var field = control("scanTuner_" + key);
                verify(field.input !== undefined, key);
                verify(field.label.length > 0, key);
            }
        }

        function test_legacy_numeric_fields_round_trip_data() {
            var cases = [
                {tag: "automatic", gain: 0, bw: -1, bias: 1, dwell: 2500, hold: 0,
                 gainMode: 1, gainLabel: "Automatic", bwMode: 0, biasMode: 1, biasLabel: "On"},
                {tag: "inherited", gain: -1, bw: -1, bias: -1, dwell: 0, hold: 0,
                 gainMode: 0, gainLabel: "App default", bwMode: 0, biasMode: 0, biasLabel: "App default"},
                {tag: "explicit", gain: 25, bw: 24, bias: 0, dwell: 1000, hold: 2000,
                 gainMode: 2, gainLabel: "Set", bwMode: 1, biasMode: 2, biasLabel: "Off"}
            ];
            var rows = [];
            for (var row of cases) {
                rows.push(Object.assign({}, row, {tag: row.tag + "-numbers", strings: false}));
                rows.push(Object.assign({}, row, {tag: row.tag + "-strings", strings: true}));
            }
            return rows;
        }

        function test_legacy_numeric_fields_round_trip(data) {
            function stored(value) {
                return data.strings ? String(value) : value;
            }
            verify(scanLists.add({
                name: "Legacy numeric fields", isDraft: true,
                gainDb: stored(data.gain), bandwidthKhz: stored(data.bw), biasTee: stored(data.bias),
                defaultDwellMs: stored(data.dwell), defaultHoldMs: stored(data.hold), ppm: "0",
                entries: [{kind: "freq", name: "Simplex", protocol: "p25", freqMhz: "851.5",
                           gainDb: stored(data.gain), dwellMs: stored(data.dwell), holdMs: stored(data.hold)}]
            }));
            screen.openFor(0);
            control("scanAdvancedToggle").Accessible.pressAction();
            var gain = control("scanGainMode");
            compare(gain.currentIndex, data.gainMode);
            compare(gain.model[gain.currentIndex], data.gainLabel);
            compare(control("scanTuner_gainDb").text, data.gain > 0 ? String(data.gain) : "");
            var bandwidth = control("scanBandwidthMode");
            compare(bandwidth.currentIndex, data.bwMode);
            compare(bandwidth.model[bandwidth.currentIndex], data.bwMode ? "Set" : "App default");
            compare(control("scanTuner_bandwidthKhz").text, data.bw > 0 ? String(data.bw) : "");
            var bias = control("scanBiasTee");
            compare(bias.currentIndex, data.biasMode);
            compare(bias.model[bias.currentIndex], data.biasLabel);
            if (data.bias === -1) {
                verify(visualChild(screen, function (item) {
                    return item.visible && item.text === "App default · Off";
                }) !== null);
            }
            compare(control("scanPpmMode").currentIndex, 1);
            compare(control("scanTuner_ppm").text, "0");
            compare(control("scanTuner_defaultDwellMs").text, data.dwell > 0 ? String(data.dwell) : "");
            compare(control("scanTuner_defaultHoldMs").text, data.hold > 0 ? String(data.hold) : "");
            compare(control("scanTuner_defaultDwellMs").hint, "Scan default when empty");
            compare(control("scanEntryGain").text, data.gain >= 0 ? String(data.gain) : "");
            compare(control("scanEntryDwell").text, data.dwell > 0 ? String(data.dwell) : "");
            compare(control("scanEntryHold").text, data.hold > 0 ? String(data.hold) : "");
            compare(control("scanEntryGain").hint, "From saved system when empty");
            compare(control("scanEntryDwell").hint, "List default when empty");

            control("scanSaveDraft").Accessible.pressAction();
            compare(closedSpy.count, 1);
            var saved = scanLists.get(0);
            compare(Number(saved.gainDb), data.gain);
            compare(Number(saved.bandwidthKhz), data.bw);
            compare(Number(saved.biasTee), data.bias);
            compare(Number(saved.defaultDwellMs), data.dwell);
            compare(Number(saved.defaultHoldMs), data.hold);
            compare(saved.ppm, "0");
            compare(Number(saved.entries[0].gainDb), data.gain);
            compare(Number(saved.entries[0].dwellMs), data.dwell);
            compare(Number(saved.entries[0].holdMs), data.hold);
            screen.openFor(0);
            compare(control("scanGainMode").currentIndex, data.gainMode);
            compare(control("scanBandwidthMode").currentIndex, data.bwMode);
            compare(control("scanBiasTee").currentIndex, data.biasMode);
        }

        function test_save_draft_rejects_invalid_set_fields_data() {
            return [
                {tag: "gain-empty", mode: "scanGainMode", index: 2, field: "scanTuner_gainDb", value: "", valid: "25"},
                {tag: "gain-zero", mode: "scanGainMode", index: 2, field: "scanTuner_gainDb", value: "0", valid: "25"},
                {tag: "gain-range", mode: "scanGainMode", index: 2, field: "scanTuner_gainDb", value: "50", valid: "25"},
                {tag: "ppm-empty", mode: "scanPpmMode", index: 1, field: "scanTuner_ppm", value: "", valid: "0"},
                {tag: "ppm-sign", mode: "scanPpmMode", index: 1, field: "scanTuner_ppm", value: "-", valid: "-1"},
                {tag: "bandwidth-empty", mode: "scanBandwidthMode", index: 1, field: "scanTuner_bandwidthKhz", value: "", valid: "24"},
                {tag: "bandwidth-zero", mode: "scanBandwidthMode", index: 1, field: "scanTuner_bandwidthKhz", value: "0", valid: "24"}
            ];
        }

        function test_save_draft_rejects_invalid_set_fields(data) {
            edit("scanListName", "Incomplete override");
            screen.advancedOpen = true;
            control(data.mode).selected(data.index);
            edit(data.field, data.value);
            if (data.mode === "scanGainMode") {
                compare(screen.draft.gainDb, -1);
                control(data.mode).selected(0);
                control(data.mode).selected(data.index);
                compare(screen.draft.gainDb, -1);
            }
            screen.advancedOpen = false;
            control("scanSaveDraft").Accessible.pressAction();
            compare(closedSpy.count, 0);
            compare(scanLists.count, 0);
            verify(screen.validationText.length > 0);
            verify(control(data.field).error.length > 0);
            verify(control(data.field).visible);
            compare(control(data.mode).currentIndex, data.index);
            edit(data.field, data.valid);
            control("scanSaveDraft").Accessible.pressAction();
            compare(closedSpy.count, 1);
            verify(scanLists.get(0).isDraft);
            screen.openFor(0);
            compare(control(data.mode).currentIndex, data.index);
            compare(control(data.field).text, data.valid);
        }

        function test_numeric_input_rejects_localized_and_grouped_values_data() {
            var fields = [
                {field: "scanTuner_gainDb", key: "gainDb", fallback: -1, valid: "25", mode: "scanGainMode", index: 2},
                {field: "scanTuner_bandwidthKhz", key: "bandwidthKhz", fallback: -1, valid: "24", mode: "scanBandwidthMode", index: 1},
                {field: "scanTuner_ppm", key: "ppm", fallback: "", valid: "-1", mode: "scanPpmMode", index: 1},
                {field: "scanTuner_defaultDwellMs", key: "defaultDwellMs", fallback: 0, valid: "1000"},
                {field: "scanTuner_defaultHoldMs", key: "defaultHoldMs", fallback: 0, valid: "2000"},
                {field: "scanPort", key: "port", fallback: 1234, valid: "1234"},
                {field: "scanEntryDwell", key: "dwellMs", fallback: 0, valid: "1000", entry: true},
                {field: "scanEntryHold", key: "holdMs", fallback: 0, valid: "2000", entry: true},
                {field: "scanEntryGain", key: "gainDb", fallback: -1, valid: "25", entry: true},
                {field: "scanEntryFrequency", key: "freqMhz", fallback: "", valid: "851.5", entry: true}
            ];
            var rows = [];
            for (var field of fields) {
                for (var value of ["٢٥", "1,024", "0,025", "1,000"])
                    rows.push(Object.assign({}, field, {tag: field.field + "-" + value, value: value}));
            }
            for (var fixture of [
                {field: 0, value: "1e1"}, {field: 0, value: "25.0"}, {field: 0, value: " 25"},
                {field: 1, value: "9999999999999999999999999999999999999999"},
                {field: 2, value: "2147483648"}, {field: 3, value: "600001"}, {field: 4, value: "-1"},
                {field: 6, value: "600001"}, {field: 7, value: "-1"}, {field: 8, value: "50"},
                {field: 9, value: "NaN"}, {field: 9, value: "Infinity"}
            ]) {
                var rangedField = fields[fixture.field];
                rows.push(Object.assign({}, rangedField, {tag: rangedField.field + "-" + fixture.value, value: fixture.value}));
            }
            return rows;
        }

        function test_numeric_input_rejects_localized_and_grouped_values(data) {
            verify(scanLists.add({
                name: "Explicit numeric settings", isDraft: true, sourceType: "rtltcp", host: "127.0.0.1", port: 1234,
                gainDb: 25, ppm: "-1", bandwidthKhz: 24, defaultDwellMs: 1000, defaultHoldMs: 2000,
                entries: [{kind: "freq", name: "Simplex", protocol: "p25", freqMhz: "851.5",
                           gainDb: 25, dwellMs: 1000, holdMs: 2000}]
            }));
            var before = JSON.stringify(scanLists.get(0));
            // A fresh editor lets this test exercise the commit handler directly
            // without replacing the shared screen's text bindings for later tests.
            var editor = createTemporaryObject(numericEditor, screen.parent);
            var spy = createTemporaryObject(numericClosedSpy, editor, {target: editor});
            editor.openFor(0);
            editor.advancedOpen = true;
            var field = visualChild(editor, data.field);
            verify(field !== null);
            field.input.text = data.value;
            field.input.textEdited();
            compare(field.text, data.value);
            verify(field.error.length > 0, data.field + " must show an error");
            compare((data.entry ? editor.entries[0] : editor.draft)[data.key], data.fallback);
            if (data.mode) {
                var mode = visualChild(editor, data.mode);
                mode.selected(0);
                mode.selected(data.index);
                compare(editor.draft[data.key], data.fallback);
                verify(field.error.length > 0);
            }
            for (var button of ["scanSaveDraft", "scanSave"]) {
                editor.advancedOpen = false;
                visualChild(editor, button).Accessible.pressAction();
                compare(spy.count, 0);
                compare(scanLists.count, 1);
                compare(JSON.stringify(scanLists.get(0)), before);
                verify(editor.validationText.length > 0);
                verify(field.visible);
                verify(field.error.length > 0);
            }
            verify(JSON.stringify(editor.selectedDraft()).indexOf(":null") < 0);

            field.input.text = data.valid;
            field.input.textEdited();
            compare(field.error, "");
            visualChild(editor, "scanSaveDraft").Accessible.pressAction();
            compare(spy.count, 1);
            var saved = scanLists.get(0);
            compare(String((data.entry ? saved.entries[0] : saved)[data.key]), data.valid);
            var json = JSON.stringify(saved);
            verify(json.indexOf(":null") < 0);
            verify(scanLists.update(0, JSON.parse(json)));
            editor.openFor(0);
            compare(visualChild(editor, "scanGainMode").currentIndex, 2);
            compare(visualChild(editor, "scanTuner_gainDb").text, "25");
            compare(visualChild(editor, data.field).text, data.valid);
            compare(scanLists.get(0).gainDb, 25);
        }

        function test_invalid_entry_input_survives_rebuild() {
            edit("scanListName", "Retain entry error");
            screen.addFrequency("First", "p25", "851.5");
            screen.addFrequency("Second", "dmr", "461");
            edit("scanEntryGain", "50");
            verify(control("scanEntryGain").error.length > 0);
            screen.moveEntry(0, 1);
            screen.moveEntry(1, -1);
            compare(control("scanEntryGain").text, "50");
            verify(control("scanEntryGain").error.length > 0);
            screen.saveDraft();
            compare(closedSpy.count, 0);
            compare(scanLists.count, 0);
            edit("scanEntryGain", "25");
            screen.saveDraft();
            compare(closedSpy.count, 1);
            compare(scanLists.get(0).entries[0].gainDb, 25);
        }

        function test_add_frequency_rejects_localized_input() {
            var editor = createTemporaryObject(numericEditor, screen.parent);
            editor.openFor(-1);
            var field = visualChild(editor, "scanAddFrequency");
            var add = visualChild(editor, "scanAddFrequencyButton");
            for (var value of ["٢٥", "1,024", "0,025", "1,000", "NaN", "Infinity"]) {
                field.input.text = value;
                field.input.textEdited();
                verify(field.error.length > 0);
                verify(!add.enabled);
                compare(editor.entries.length, 0);
            }
            field.input.text = "851.5";
            field.input.textEdited();
            compare(field.error, "");
            verify(add.enabled);
            add.Accessible.pressAction();
            compare(editor.entries[0].freqMhz, "851.5");
        }

        function test_list_inheritance_round_trip_data() {
            return [
                {tag: "inherited", gainMode: 0, gain: "", ppmMode: 0, ppm: "", bwMode: 0, bw: "", bias: 0,
                 dwell: "", hold: "", expectedGain: -1, expectedPpm: "", expectedBw: -1, expectedBias: -1},
                {tag: "automatic-zero-ppm", gainMode: 1, gain: "", ppmMode: 1, ppm: "0", bwMode: 1, bw: "24", bias: 1,
                 dwell: "1500", hold: "3000", expectedGain: 0, expectedPpm: "0", expectedBw: 24, expectedBias: 1},
                {tag: "explicit-negative-ppm", gainMode: 2, gain: "23", ppmMode: 1, ppm: "-1", bwMode: 1, bw: "48", bias: 2,
                 dwell: "2000", hold: "4000", expectedGain: 23, expectedPpm: "-1", expectedBw: 48, expectedBias: 0}
            ];
        }

        function test_list_inheritance_round_trip(data) {
            edit("scanListName", "Sentinels");
            control("scanAdvancedToggle").Accessible.pressAction();
            // Populate explicit values first, then change inheritance states. App
            // values are hints only and must never replace the saved sentinels.
            control("scanGainMode").selected(2);
            edit("scanTuner_gainDb", "19");
            control("scanPpmMode").selected(1);
            edit("scanTuner_ppm", "12");
            control("scanBandwidthMode").selected(1);
            edit("scanTuner_bandwidthKhz", "16");
            control("scanGainMode").selected(data.gainMode);
            if (data.gainMode === 2) edit("scanTuner_gainDb", data.gain);
            control("scanPpmMode").selected(data.ppmMode);
            if (data.ppmMode === 1) edit("scanTuner_ppm", data.ppm);
            control("scanBandwidthMode").selected(data.bwMode);
            if (data.bwMode === 1) edit("scanTuner_bandwidthKhz", data.bw);
            control("scanBiasTee").selected(data.bias);
            edit("scanTuner_defaultDwellMs", "100");
            edit("scanTuner_defaultHoldMs", "200");
            edit("scanTuner_defaultDwellMs", data.dwell);
            edit("scanTuner_defaultHoldMs", data.hold);
            control("scanSaveDraft").Accessible.pressAction();
            compare(closedSpy.count, 1);
            var saved = scanLists.get(0);
            verify(saved.isDraft);
            compare(saved.gainDb, data.expectedGain);
            compare(saved.ppm, data.expectedPpm);
            compare(saved.bandwidthKhz, data.expectedBw);
            compare(saved.biasTee, data.expectedBias);
            compare(saved.defaultDwellMs, Number(data.dwell));
            compare(saved.defaultHoldMs, Number(data.hold));
            screen.openFor(0);
            compare(control("scanGainMode").currentIndex, data.gainMode);
            compare(control("scanPpmMode").currentIndex, data.ppmMode);
            compare(control("scanBandwidthMode").currentIndex, data.bwMode);
            compare(control("scanBiasTee").currentIndex, data.bias);
            compare(control("scanTuner_defaultDwellMs").text, data.dwell);
            compare(control("scanTuner_defaultHoldMs").text, data.hold);
        }

        function test_entry_inheritance_round_trip_data() {
            return [
                {tag: "inherit", gain: "", dwell: "", hold: "", modulation: 0, expectedGain: -1, expectedModulation: ""},
                {tag: "automatic", gain: "0", dwell: "100", hold: "200", modulation: 1, expectedGain: 0, expectedModulation: "c4fm"},
                {tag: "simulcast", gain: "30", dwell: "300", hold: "400", modulation: 2, expectedGain: 30, expectedModulation: "cqpsk"},
                {tag: "gfsk", gain: "12", dwell: "500", hold: "600", modulation: 3, expectedGain: 12, expectedModulation: "gfsk"}
            ];
        }

        function test_entry_inheritance_round_trip(data) {
            edit("scanListName", "Entry defaults");
            screen.addFrequency("Simplex", "p25", "851.5");
            edit("scanEntryGain", "20");
            edit("scanEntryDwell", "1000");
            edit("scanEntryHold", "2000");
            edit("scanEntryGain", data.gain);
            edit("scanEntryDwell", data.dwell);
            edit("scanEntryHold", data.hold);
            choose("scanEntryModulation", data.modulation);
            control("scanSaveDraft").Accessible.pressAction();
            var entry = scanLists.get(0).entries[0];
            compare(entry.gainDb, data.expectedGain);
            compare(entry.dwellMs, Number(data.dwell));
            compare(entry.holdMs, Number(data.hold));
            compare(entry.modulation, data.expectedModulation);
            screen.openFor(0);
            compare(control("scanEntryGain").text, data.gain);
            compare(control("scanEntryDwell").text, data.dwell);
            compare(control("scanEntryHold").text, data.hold);
            compare(control("scanEntryModulation").currentIndex, data.modulation);
        }

        function test_human_protocol_labels_map_to_ids() {
            edit("scanListName", "Protocols");
            var names = ["P25", "DMR", "NXDN48", "NXDN96"];
            var ids = ["p25", "dmr", "nxdn48", "nxdn"];
            for (var i = 0; i < ids.length; ++i) {
                var picker = control("scanAddProtocol");
                compare(picker.textAt(i), names[i]);
                choose("scanAddProtocol", i);
                edit("scanAddFrequencyName", names[i]);
                edit("scanAddFrequency", "851.5");
                control("scanAddFrequencyButton").Accessible.pressAction();
                compare(screen.entries[i].protocol, ids[i]);
            }
            for (var j = 0; j < ids.length; ++j) {
                compare(control("scanEntryProtocol").textAt(j), names[j]);
                choose("scanEntryProtocol", j);
                compare(screen.entries[0].protocol, ids[j]);
            }
            screen.saveDraft();
            compare(scanLists.get(0).entries[0].protocol, "nxdn");
        }

        function test_save_empty_name_stays_open_data() {
            return [{tag: "save", button: "scanSave", entry: true},
                    {tag: "empty-list", button: "scanSave", entry: false},
                    {tag: "draft", button: "scanSaveDraft", entry: true}];
        }

        function test_save_empty_name_stays_open(data) {
            if (data.entry)
                screen.addFrequency("Simplex", "p25", "851.5");
            control(data.button).Accessible.pressAction();
            compare(closedSpy.count, 0);
            compare(scanLists.count, 0);
            compare(screen.validationText, "Name this scan list.");
            compare(control("scanListName").error, "Name this scan list.");
            verify(fieldErrorMessage(control("scanListName")).visible);
            verify(!control("scanValidation").visible);
        }

        function test_modes_preserved_until_selected_mode_is_saved_data() {
            return [{tag: "manual", mode: 0}, {tag: "csv", mode: 1}];
        }

        function test_modes_preserved_until_selected_mode_is_saved(data) {
            var fixture = testContext.writeFixtureCsv("both-targets.csv",
                "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,options\nCSV ID,p25-conventional,851500000,,,,,\n");
            var result = importedFiles.importFile(fixture, "Both.csv", "trunkTargets");
            verify(result.ok, result.detail);
            try {
                edit("scanListName", "Both modes");
                screen.addFrequency("Manual", "dmr", "461");
                screen.selectTargets(result.path);
                compare(screen.entries.length, 1);
                var modes = control("scanTargetMode");
                modes.selected(0);
                compare(screen.entries[0].name, "Manual");
                modes.selected(1);
                compare(screen.draft.targetsCsvPath, result.path);
                compare(screen.targetRows.length, 1);
                modes.selected(data.mode);
                screen.save();
                compare(closedSpy.count, 1);
                var saved = scanLists.get(0);
                compare(saved.targetSource, data.mode ? "csv" : "entries");
                compare(saved.entries.length, data.mode ? 0 : 1);
                compare(saved.targetsCsvPath, data.mode ? result.path : "");
            } finally {
                importedFiles.remove(importedFiles.rowForPath(result.path));
            }
        }

        function test_narrow_themes_and_keyboard_reveal_data() {
            return [{tag: "light", appearance: 1}, {tag: "dark", appearance: 2}];
        }

        function test_narrow_themes_and_keyboard_reveal(data) {
            testContext.setPrefs("appearance", data.appearance);
            compare(screen.width, 360);
            control("scanAdvancedToggle").Accessible.pressAction();
            var field = control("scanTuner_defaultHoldMs");
            field.input.forceActiveFocus();
            decoderHost.keyboardTop = 420;
            var scroll = control("scanListScroll");
            tryVerify(function() {
                var point = field.mapToItem(screen, 0, 0);
                return point.y >= scroll.y && point.y + field.height <= 420;
            });
            for (var name of ["scanTargetMode", "scanGainMode", "scanBiasTee", "scanSave", "scanSaveDraft", "validateScanDraft"]) {
                var item = control(name);
                var pos = item.mapToItem(screen, 0, 0);
                verify(pos.x >= 0 && pos.x + item.width <= 360, name);
            }
        }

        function test_app_default_hints_do_not_change_the_draft() {
            control("scanAdvancedToggle").Accessible.pressAction();
            testContext.setPrefs("gainDb", 37);
            testContext.setPrefs("ppm", -7);
            testContext.setPrefs("bandwidthKhz", 24);
            testContext.setPrefs("biasTee", true);
            try {
                for (var label of ["App default · 37 dB", "App default · -7 ppm", "App default · 24 kHz", "App default · On"])
                    verify(visualChild(screen, function(item) { return item.text === label; }) !== null, label);
                edit("scanListName", "Inherited");
                screen.saveDraft();
                var saved = scanLists.get(0);
                compare(saved.gainDb, -1);
                compare(saved.ppm, "");
                compare(saved.bandwidthKhz, -1);
                compare(saved.biasTee, -1);
            } finally {
                testContext.setPrefs("gainDb", 30);
                testContext.setPrefs("ppm", 0);
                testContext.setPrefs("bandwidthKhz", 48);
                testContext.setPrefs("biasTee", false);
            }
        }

        function test_saved_system_entry_actions_and_scope() {
            var savedRow = savedSystems.count;
            verify(savedSystems.add({name: "Local system", sourceType: "usb", freqMhz: "851.5", decodeFlag: "-fp"}));
            try {
                edit("scanListName", "System actions");
                choose("scanSystemPicker", savedRow);
                control("scanAddSystem").Accessible.pressAction();
                compare(screen.entries[0].systemUid, savedSystems.get(savedRow).uid);
                verify(!control("scanFrequencyName").visible);
                control("scanEntryEnabled").toggle();
                control("scanEntryEnabled").toggled();
                compare(screen.entries[0].enabled, false);
                choose("scanEntryDecryptionScope", 1);
                verify(control("scanEntryDecryptionProfile").visible);
                choose("scanEntryDecryptionScope", 2);
                compare(screen.entries[0].decryptionMode, "none");
                screen.addFrequency("Second", "dmr", "461");
                verify(!control("scanEntryUp").enabled);
                control("scanEntryDown").Accessible.pressAction();
                compare(screen.entries[0].name, "Second");
                control("scanEntryRemove").Accessible.pressAction();
                compare(screen.entries.length, 1);
                screen.saveDraft();
                var entry = scanLists.get(0).entries[0];
                compare(entry.decryptionMode, "none");
                compare(entry.enabled, false);
                compare(entry.gainDb, -1);
            } finally {
                savedSystems.remove(savedRow);
            }
        }

        function test_tuner_sources_and_network_errors() {
            edit("scanListName", "Network");
            screen.addFrequency("Simplex", "p25", "851.5");
            control("scanSource_rtltcp").Accessible.pressAction();
            compare(screen.draft.sourceType, "rtltcp");
            verify(control("scanHost").visible);
            compare(control("scanHost").error, "");
            compare(control("scanHost").text, "192.168.1.10");
            edit("scanHost", "bad host");
            edit("scanPort", "0");
            verify(control("scanHost").error.length > 0);
            verify(control("scanPort").error.length > 0);
            screen.save();
            compare(closedSpy.count, 0);
            compare(scanLists.count, 0);
            edit("scanHost", "127.0.0.1");
            edit("scanPort", "1234");
            control("scanSource_usb").Accessible.pressAction();
            control("scanSource_rtltcp").Accessible.pressAction();
            compare(control("scanHost").text, "127.0.0.1");
            compare(control("scanHost").error, "");
            compare(control("scanPort").error, "");
            screen.save();
            compare(closedSpy.count, 1);
            compare(scanLists.get(0).host, "127.0.0.1");
            compare(scanLists.get(0).port, 1234);
        }

        function test_failed_save_keeps_message_in_view_data() {
            var rows = [];
            for (var keyboard of [false, true]) {
                for (var override of [false, true]) {
                    for (var button of ["scanSave", "scanSaveDraft"]) {
                        rows.push({tag: button + (override ? "-gain" : "-name") + (keyboard ? "-keyboard" : "-no-keyboard"),
                            button: button, override: override, keyboard: keyboard});
                    }
                }
            }
            return rows;
        }

        function renderEditor() {
            screen.Window.window.update();
            verify(waitForRendering(screen));
        }

        function waitForEditorLayout() {
            // Render the newly visible items before reading their Column positions,
            // and wait for the disclosure's final geometry rather than a timer.
            renderEditor();
            var body = control("scanAdvancedBody");
            var header = control("scanAdvancedToggle");
            var height = header.height + (screen.advancedOpen ? body.height + Ui.Theme.cardPadding : 0);
            tryCompare(body.parent, "height", height);
            renderEditor();
            compare(body.parent.height, height);
        }

        function prepareFailedValidation(data) {
            screen.addFrequency("Simplex", "p25", "851.5");
            waitForEditorLayout();
            if (data.override) {
                edit("scanListName", "Incomplete gain");
                control("scanAdvancedToggle").Accessible.pressAction();
                control("scanGainMode").selected(2);
                waitForEditorLayout();
                control("scanAdvancedToggle").Accessible.pressAction();
            }
            control("scanListName").input.forceActiveFocus();
            decoderHost.keyboardTop = data.keyboard ? 420 : -1;
            waitForEditorLayout();
            verify(!screen.advancedOpen);
        }

        function insideVisibleArea(item) {
            var scroll = control("scanListScroll");
            var visibleHeight = Math.min(scroll.height, Ui.Theme.keyboardTop(scroll));
            var pos = item.mapToItem(scroll.contentItem, 0, 0);
            return item.visible && pos.y >= scroll.contentY && pos.y < scroll.contentY + visibleHeight
                && pos.y + item.height <= scroll.contentY + visibleHeight;
        }

        function verifyFailedValidation(data) {
            compare(closedSpy.count, 0);
            compare(scanLists.count, 0);
            compare(screen.advancedOpen, data.override);
            var field = control(data.override ? "scanTuner_gainDb" : "scanListName");
            compare(field.error, data.override ? "Enter a gain from 1 to 49 dB." : "Name this scan list.");
            var message = fieldErrorMessage(field);
            compare(field.border.color, Ui.Theme.alert);
            verify(!control("scanValidation").visible);
            waitForEditorLayout();
            tryVerify(function() { return insideVisibleArea(message); }, 5000, "Settled validation message is visible");
            tryVerify(function() { return insideVisibleArea(field); }, 5000, "Invalid field is visible with the message");
            renderEditor();
            verify(insideVisibleArea(message));
            verify(insideVisibleArea(field));
            verifyMessagePainted(message, field.color);
        }

        function fieldErrorMessage(field) {
            var message = visualChild(field, function(item) { return item.text === field.error; });
            verify(message !== null, "Field must contain its error message");
            return message;
        }

        function test_failed_save_keeps_message_in_view(data) {
            prepareFailedValidation(data);
            var scroll = control("scanListScroll");
            scroll.contentY = scroll.contentHeight - scroll.height;
            renderEditor();
            var save = control(data.button);
            mouseClick(save, save.width / 2, save.height / 2);
            verifyFailedValidation(data);
        }

        function verifyMessagePainted(message, background) {
            verify(message.contentWidth > 0 && message.implicitHeight > 0);
            for (var ancestor = message; ancestor; ancestor = ancestor.parent) {
                verify(ancestor.visible && ancestor.opacity > 0, "Validation ancestor must be visible: " + ancestor);
            }
            // Grab the screen so clipping, occlusion and the actual background are
            // included. Only inspect the text rectangle, excluding the field border.
            var image = grabImage(screen);
            var pos = message.mapToItem(screen, 0, 0);
            var scaleX = image.width / screen.width;
            var scaleY = image.height / screen.height;
            verify(pos.x >= 0 && pos.y >= 0 && pos.x + message.width <= screen.width
                && pos.y + message.height <= screen.height, "Paint sample must be inside the screen");
            var painted = 0;
            for (var y = Math.ceil(pos.y * scaleY); y < Math.floor((pos.y + message.height) * scaleY); ++y) {
                for (var x = Math.ceil(pos.x * scaleX); x < Math.floor((pos.x + message.width) * scaleX); ++x) {
                    var pixel = image.pixel(x, y);
                    if (Math.abs(pixel.r - background.r) + Math.abs(pixel.g - background.g)
                            + Math.abs(pixel.b - background.b) > 0.1)
                        ++painted;
                }
            }
            verify(painted > 20, "Validation must paint glyphs; found " + painted + " contrasting pixels");
        }

        function test_failed_save_paints_name_error_data() {
            return [{tag: "light", appearance: 1, keyboard: false},
                    {tag: "light-keyboard", appearance: 1, keyboard: true},
                    {tag: "dark", appearance: 2, keyboard: false},
                    {tag: "dark-keyboard", appearance: 2, keyboard: true}];
        }

        function test_failed_save_paints_name_error(data) {
            testContext.setPrefs("appearance", data.appearance);
            data.override = false;
            prepareFailedValidation(data);
            var scroll = control("scanListScroll");
            scroll.contentY = scroll.contentHeight - scroll.height;
            renderEditor();
            var save = control("scanSave");
            mouseClick(save, save.width / 2, save.height / 2);
            verifyFailedValidation(data);
            var field = control("scanListName");
            edit("scanListName", "Local");
            compare(field.error, "");
            compare(screen.validationText, "");
            compare(field.border.color, Ui.Theme.cyan);
            screen.openFor(-1);
            compare(field.error, "");
            compare(field.text, "");
        }

        function test_repeated_validation_reveals_message_data() {
            return [{tag: "name", override: false, keyboard: false},
                    {tag: "name-keyboard", override: false, keyboard: true},
                    {tag: "gain", override: true, keyboard: false},
                    {tag: "gain-keyboard", override: true, keyboard: true}];
        }

        function test_non_field_validation_paints_in_footer() {
            control("scanSave").Accessible.pressAction();
            compare(control("scanListName").error, "Name this scan list.");
            edit("scanListName", "No entries");
            control("scanSave").Accessible.pressAction();
            compare(closedSpy.count, 0);
            compare(scanLists.count, 0);
            compare(control("scanListName").error, "");
            compare(screen.validationField, null);
            var message = control("scanValidation");
            verify(message.text.length > 0);
            waitForEditorLayout();
            tryVerify(function() { return insideVisibleArea(message); });
            renderEditor();
            verify(insideVisibleArea(message));
            verifyMessagePainted(message, Ui.Theme.bg);
        }

        function test_repeated_validation_reveals_message(data) {
            prepareFailedValidation(data);
            var scroll = control("scanListScroll");
            for (var i = 0; i < 2; ++i) {
                scroll.contentY = scroll.contentHeight - scroll.height;
                renderEditor();
                control("validateScanDraft").Accessible.pressAction();
                verifyFailedValidation(data);
            }
        }

        function test_explicit_values_survive_inheritance_toggle() {
            edit("scanListName", "Remember overrides");
            control("scanAdvancedToggle").Accessible.pressAction();
            var fields = [
                {mode: "scanGainMode", set: 2, field: "scanTuner_gainDb", value: "23", key: "gainDb", inherited: -1},
                {mode: "scanPpmMode", set: 1, field: "scanTuner_ppm", value: "-1", key: "ppm", inherited: ""},
                {mode: "scanBandwidthMode", set: 1, field: "scanTuner_bandwidthKhz", value: "24", key: "bandwidthKhz", inherited: -1}
            ];
            for (var field of fields) {
                control(field.mode).selected(field.set);
                edit(field.field, field.value);
                control(field.mode).selected(0);
                compare(screen.draft[field.key], field.inherited);
                verify(!control(field.field).visible);
                control(field.mode).selected(field.set);
                compare(control(field.field).text, field.value);
                compare(String(screen.draft[field.key]), field.value);
            }
            screen.saveDraft();
            screen.openFor(-1);
            compare(control("scanListName").text, "");
            compare(control("scanTuner_gainDb").text, "");
            compare(control("scanTuner_ppm").text, "");
            compare(control("scanTuner_bandwidthKhz").text, "");
            screen.openFor(0);
            compare(control("scanListName").text, "Remember overrides");
            for (var savedField of fields) {
                compare(control(savedField.mode).currentIndex, savedField.set);
                compare(control(savedField.field).text, savedField.value);
            }
        }

        function test_inherited_automatic_gain_hint() {
            testContext.setPrefs("gainDb", 0);
            try {
                control("scanAdvancedToggle").Accessible.pressAction();
                verify(visualChild(screen, function (item) {
                    return item.visible && item.text === "App default · Automatic";
                }) !== null);
                edit("scanListName", "Automatic app gain");
                screen.saveDraft();
                compare(scanLists.get(0).gainDb, -1);
            } finally {
                testContext.setPrefs("gainDb", 30);
            }
        }

        function test_voice_and_optional_files_round_trip_data() {
            return [{tag: "files", selected: true}, {tag: "none", selected: false}];
        }

        function test_voice_and_optional_files_round_trip(data) {
            var fixtures = [
                {key: "groupCsvPath", type: "group", csv: "id,mode,name\n1001,A,Dispatch\n"},
                {key: "srcCsvPath", type: "src", csv: "id,name\n1234,Portable\n"}
            ];
            var paths = [];
            try {
                edit("scanListName", "Optional files");
                control("scanAdvancedToggle").Accessible.pressAction();
                for (var fixture of fixtures) {
                    var source = testContext.writeFixtureCsv("scan-" + fixture.type + ".csv", fixture.csv);
                    var result = importedFiles.importFile(source, "scan-" + fixture.type + ".csv", fixture.type);
                    verify(result.ok, result.detail);
                    paths.push(result.path);
                    var picker = control("scanFile_" + fixture.key);
                    compare(picker.textAt(0), "None");
                    var index = picker.model.findIndex(function (file) { return file.path === result.path; });
                    verify(index > 0);
                    choose(picker.objectName, index);
                    if (!data.selected)
                        choose(picker.objectName, 0);
                }
                var voice = control("scanVoiceOnly");
                voice.forceActiveFocus();
                keyClick(Qt.Key_Space);
                screen.saveDraft();
                var saved = scanLists.get(0);
                verify(saved.voiceOnly);
                compare(saved.groupCsvPath, data.selected ? paths[0] : "");
                compare(saved.srcCsvPath, data.selected ? paths[1] : "");
            } finally {
                for (var path of paths)
                    importedFiles.remove(importedFiles.rowForPath(path));
            }
        }

        function test_remove_list_requires_confirmation() {
            edit("scanListName", "Remove me");
            screen.saveDraft();
            screen.openFor(0);
            closedSpy.clear();
            control("scanRemoveList").Accessible.pressAction();
            var dialog = findChild(screen, "scanListRemoveDialog");
            verify(dialog.visible);
            visualChild(dialog, "scanRemoveCancel").Accessible.pressAction();
            compare(scanLists.count, 1);
            compare(closedSpy.count, 0);
            control("scanRemoveList").Accessible.pressAction();
            visualChild(dialog, "scanRemoveConfirm").Accessible.pressAction();
            compare(scanLists.count, 0);
            compare(closedSpy.count, 1);
        }

        name: "ScanListScreen"
        when: windowShown
    }
}
