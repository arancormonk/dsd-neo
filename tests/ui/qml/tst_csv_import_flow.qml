// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

Item {
    width: 420
    height: 900
    Ui.CsvImportFlow {
        id: flow
        anchors.fill: parent
    }
    SignalSpy {
        id: completion
        target: flow
        signalName: "finished"
    }
    TestCase {
        name: "CsvImportFlow"
        when: windowShown
        property string source: ""

        function visualChild(parent, name) {
            if (parent.objectName === name)
                return parent;
            for (var child of parent.children || []) {
                var found = visualChild(child, name);
                if (found)
                    return found;
            }
            return null;
        }
        function item(name) {
            var result = null;
            tryVerify(function () { result = findChild(flow, name) || visualChild(findChild(flow, "csvCompanionSheet"), name); return result !== null; }, 2000, name);
            return result;
        }
        function init() {
            completion.clear();
            source = testContext.writeFixtureCsv("flow-primary.csv",
                "channel,frequency,mode,keys_dec_csv\n1,851012500,p25,unavailable/wanted.csv\n");
        }
        function cleanup() {
            flow.cancel();
            while (importedFiles.count)
                importedFiles.remove(0);
        }
        function openSheet() {
            flow.begin(source, "Map.csv", "chan");
            tryCompare(item("csvCompanionSheet"), "visible", true);
            compare(completion.count, 0);
        }
        function test_cancel_data() {
            return [{tag: "button"}, {tag: "back"}, {tag: "escape"}, {tag: "scrim"}];
        }
        function test_cancel(data) {
            openSheet();
            var sheet = item("csvCompanionSheet");
            if (data.tag === "button")
                mouseClick(item("csvCancel"));
            else if (data.tag === "scrim")
                mouseClick(sheet, 2, 2);
            else {
                sheet.forceActiveFocus();
                keyClick(data.tag === "back" ? Qt.Key_Back : Qt.Key_Escape);
            }
            tryCompare(completion, "count", 1);
            compare(completion.signalArguments[0][0].error, "cancelled");
            compare(importedFiles.count, 0);
            flow.cancel();
            sheet.dismissed();
            compare(completion.count, 1);
        }
        function test_primary_picker_success_and_reuse() {
            var path = testContext.writeFixtureCsv("standalone-map.csv", "channel,frequency\n1,851012500\n");
            flow.pick("chan");
            var picker = item("csvPrimaryPicker");
            picker.selectedFile = "file://" + path;
            picker.accepted();
            picker.close();
            compare(completion.count, 1);
            verify(completion.signalArguments[0][0].ok);
            flow.pick("chan");
            picker.reject();
            compare(completion.count, 2);
            compare(completion.signalArguments[1][0].error, "cancelled");
        }
        function test_failed_import_finishes_once() {
            flow.begin("/missing/primary.csv", "Map.csv", "chan");
            compare(completion.count, 1);
            verify(!completion.signalArguments[0][0].ok);
            flow.cancel();
            compare(completion.count, 1);
        }
        function test_picked_companion_commits() {
            openSheet();
            item("csvChooseFile_0").clicked();
            var path = testContext.writeFixtureCsv("picked-keys.csv", "id,value\n1,12345\n");
            var picker = item("csvCompanionPicker");
            picker.selectedFile = "file://" + path;
            picker.accepted();
            picker.close();
            compare(completion.count, 0);
            item("csvCompleteBundle").clicked();
            compare(completion.count, 1);
            verify(completion.signalArguments[0][0].ok);
        }
        function test_primary_picker_cancel() {
            flow.pick("chan");
            item("csvPrimaryPicker").reject();
            tryCompare(completion, "count", 1);
            compare(completion.signalArguments[0][0].error, "cancelled");
            flow.cancel();
            compare(completion.count, 1);
        }
        function test_new_picker_clears_previous_document() {
            flow.reference = "/previous/document.csv";
            flow.fileName = "Previous.csv";
            flow.pick("chan");
            compare(flow.reference, "");
            compare(flow.fileName, "");
            item("csvPrimaryPicker").reject();
            compare(completion.count, 1);
        }
        function test_companion_picker_cancel_resumes_unresolved() {
            openSheet();
            item("csvChooseFile_0").clicked();
            item("csvCompanionPicker").reject();
            verify(item("csvCompanionSheet").visible);
            compare(completion.count, 0);
            verify(!flow.companions[flow.requiredFiles[0]]);
            flow.cancel();
            compare(completion.count, 1);
        }
        function addKeys(name, type) {
            var path = testContext.writeFixtureCsv("flow-keys.csv", "id,value\n1,12345\n");
            var result = importedFiles.importFile(path, name, type);
            verify(result.ok);
            return result.path;
        }
        function test_unique_candidate_visible_changeable_and_commits_once() {
            var stored = addKeys("wanted.csv", "keysDec");
            openSheet();
            var choice = item("csvLibraryChoice_0_0");
            verify(choice.text.indexOf("wanted.csv") >= 0);
            compare(choice.Accessible.name, "Use imported wanted.csv (decimal keys, 1 row)");
            compare(choice.Accessible.role, Accessible.RadioButton);
            verify(choice.Accessible.checkable);
            verify(choice.Accessible.checked);
            verify(choice.checked);
            compare(flow.companions[flow.requiredFiles[0]], stored);
            item("csvChooseFile_0").clicked();
            item("csvCompanionPicker").reject();
            verify(!flow.companions[flow.requiredFiles[0]]);
            verify(!choice.Accessible.checked);
            verify(!/[●○]/.test(choice.Accessible.name));
            choice.clicked();
            verify(choice.Accessible.checked);
            compare(flow.companions[flow.requiredFiles[0]], stored);
            item("csvCompleteBundle").clicked();
            compare(completion.count, 1);
            verify(completion.signalArguments[0][0].ok);
            flow.cancel();
            compare(completion.count, 1);
        }
        function test_multiple_candidates_require_choice() {
            addKeys("other.csv", "keysDec");
            var stored = addKeys("wanted.csv", "keysDec");
            addKeys("wrong.csv", "keysHex");
            openSheet();
            verify(!flow.companions[flow.requiredFiles[0]]);
            verify(!item("csvCompleteBundle").enabled);
            item("csvLibraryChoice_0_0").clicked();
            compare(flow.companions[flow.requiredFiles[0]], stored);
            verify(item("csvCompleteBundle").enabled);
        }
        function test_nonmatching_basename_not_preselected() {
            addKeys("other.csv", "keysDec");
            openSheet();
            verify(!flow.companions[flow.requiredFiles[0]]);
            verify(!item("csvLibraryChoice_0_0").checked);
        }
        function test_mismatched_key_type_explained() {
            addKeys("wanted.csv", "keysHex");
            openSheet();
            verify(!flow.companions[flow.requiredFiles[0]]);
            verify(item("csvCompanionWarning_0").text.indexOf("expects decimal keys") >= 0);
        }
        function companionRolesData() {
            return [
                {tag: "channel", kind: "chan", csv: "channel,frequency,mode,options\n1,851012500,p25,-k unavailable/shared.csv\n2,852012500,p25,-K unavailable/shared.csv\n"},
                {tag: "targets", kind: "trunkTargets", csv: "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,keys_dec_csv,keys_hex_csv\none,p25-conventional,851012500,,,,,unavailable/shared.csv,unavailable/shared.csv\n"}
            ];
        }
        function test_same_path_roles_have_distinct_headers_data() {
            return companionRolesData();
        }
        function test_same_path_roles_have_distinct_headers(data) {
            var path = testContext.writeFixtureCsv("flow-roles.csv", data.csv);
            flow.begin(path, "Roles.csv", data.kind);
            compare(flow.requiredFiles.length, 2);
            var headers = [item("csvCompanionLabel_0").text, item("csvCompanionLabel_1").text];
            verify(headers.indexOf("Decimal keys · unavailable/shared.csv") >= 0);
            verify(headers.indexOf("Hex keys · unavailable/shared.csv") >= 0);
            verify(headers[0] !== headers[1]);
        }
        function test_wrong_kind_pick_preserves_other_selections_data() {
            return companionRolesData();
        }
        function test_wrong_kind_pick_preserves_other_selections(data) {
            var hex = addKeys("hex.csv", "keysHex");
            var dec = addKeys("decimal.csv", "keysDec");
            var path = testContext.writeFixtureCsv("flow-retry.csv", data.csv);
            flow.begin(path, "Retry.csv", data.kind);
            compare(flow.requiredFiles.length, 2);
            var decimalIndex = flow.requiredDetails[flow.requiredFiles[0]].type === "keysDec" ? 0 : 1;
            var hexIndex = 1 - decimalIndex;
            var decimalKey = flow.requiredFiles[decimalIndex];
            var hexKey = flow.requiredFiles[hexIndex];
            item("csvLibraryChoice_" + hexIndex + "_0").clicked();
            item("csvChooseFile_" + decimalIndex).clicked();
            var picker = item("csvCompanionPicker");
            // Desktop file:// selections can identify a known library kind.
            picker.selectedFile = "file://" + hex;
            picker.accepted();
            picker.close();
            verify(item("csvCompleteBundle").enabled);
            item("csvCompleteBundle").clicked();
            compare(completion.count, 0);
            verify(flow.active);
            verify(item("csvCompanionSheet").visible);
            compare(flow.requiredFiles.length, 2);
            compare(flow.companions[hexKey], hex);
            verify(!flow.companions[decimalKey]);
            verify(!item("csvCompleteBundle").enabled);
            verify(item("csvCompanionWarning_" + decimalIndex).text.indexOf("hex.csv is a hex key file; this bundle expects decimal keys") >= 0);
            compare(importedFiles.count, 2);
            item("csvChooseFile_" + decimalIndex).clicked();
            picker.selectedFile = "file://" + dec;
            picker.accepted();
            picker.close();
            compare(flow.companions[hexKey], hex);
            item("csvCompleteBundle").clicked();
            compare(completion.count, 1);
            verify(completion.signalArguments[0][0].ok);
            compare(importedFiles.count, 3);
            flow.cancel();
            compare(completion.count, 1);
        }
    }
}
