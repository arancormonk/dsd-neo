// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Window
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
    // A flow made after a case switches the host's picker off, so it takes
    // Qt's FileDialog (the desktop path).
    Component {
        id: flowComponent
        Ui.CsvImportFlow {
            anchors.fill: parent
        }
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
            testContext.setDocumentPickerSupported(true);
            testContext.resetHostPlayback();
            completion.clear();
            source = testContext.writeFixtureCsv("flow-primary.csv",
                "channel,frequency,mode,keys_dec_csv\n1,851012500,p25,unavailable/wanted.csv\n");
        }
        function cleanup() {
            flow.cancel();
            flow.wanted = null;
            testContext.setDocumentPickerSupported(true);
            // A desktop dialog can leave its own window active; the keys the
            // next case sends go to this one.
            flow.Window.window.requestActivate();
            while (importedFiles.count)
                importedFiles.remove(0);
        }
        // The host's answers to the request a picker is waiting on (issue #829).
        function requestId() {
            var request = testContext.lastDocumentRequest();
            compare(request.kind, "open");
            return request.id;
        }
        function answer(reference, name) {
            testContext.answerDocumentPicked(requestId(), reference, name, false);
        }
        function backOut() {
            testContext.answerDocumentCancelled(requestId());
        }
        function cancelledIds() {
            return testContext.documentCalls().filter(function (call) {
                return call.name === "cancelDocumentRequest";
            }).map(function (call) { return call.id; });
        }
        // The sheet's own DiscardDialog.
        function question(sheet) {
            for (var child of sheet.children) {
                if (child.accessibleName === "Discard changes?")
                    return child;
            }
            verify(false, "the companion sheet has a discard question");
            return null;
        }
        function openSheet() {
            flow.begin(source, "Map.csv", "chan");
            tryCompare(item("csvCompanionSheet"), "visible", true);
            compare(completion.count, 0);
        }
        function test_cancel_data() {
            return [{tag: "button"}, {tag: "back"}, {tag: "escape"}];
        }
        function test_cancel(data) {
            openSheet();
            var sheet = item("csvCompanionSheet");
            if (data.tag === "button")
                mouseClick(item("csvCancel"));
            else {
                sheet.forceActiveFocus();
                keyClick(data.tag === "back" ? Qt.Key_Back : Qt.Key_Escape);
            }
            tryCompare(completion, "count", 1);
            compare(completion.signalArguments[0][0].error, "cancelled");
            compare(importedFiles.count, 0);
            flow.cancel();
            compare(completion.count, 1);
        }
        // Issue #718: the companion sheet is a form; a tap beside it keeps it.
        function test_outside_tap_keeps_the_sheet() {
            openSheet();
            var sheet = item("csvCompanionSheet");
            verify(!sheet.hitsPanel(2, sheet.height / 2));
            mouseClick(sheet, 2, sheet.height / 2);
            wait(50);
            verify(sheet.visible, "an outside tap keeps the companion sheet");
            verify(flow.active);
            compare(completion.count, 0);
            verify(!question(sheet).visible, "with nothing chosen there is nothing to ask about");
        }
        // Once a companion was chosen, Cancel, Back and Escape ask first; Keep
        // editing keeps the choice and Discard cancels the import.
        function test_cancel_after_a_choice_asks_data() {
            return [{tag: "button"}, {tag: "back"}, {tag: "escape"}];
        }
        function test_cancel_after_a_choice_asks(data) {
            var stored = addKeys("other.csv", "keysDec");
            openSheet();
            var sheet = item("csvCompanionSheet");
            verify(!flow.companions[flow.requiredFiles[0]], "nothing preselected: the choice below is the user's");
            item("csvLibraryChoice_0_0").clicked();
            compare(flow.companions[flow.requiredFiles[0]], stored);
            function dismiss() {
                if (data.tag === "button")
                    mouseClick(item("csvCancel"));
                else {
                    sheet.forceActiveFocus();
                    keyClick(data.tag === "back" ? Qt.Key_Back : Qt.Key_Escape);
                }
            }
            dismiss();
            var asked = question(sheet);
            verify(asked.visible, "the sheet asks before the choice goes");
            compare(completion.count, 0);
            visualChild(asked, "discardDialogKeep").activate();
            verify(!asked.visible);
            verify(sheet.visible);
            compare(flow.companions[flow.requiredFiles[0]], stored, "keep editing keeps the choice");
            dismiss();
            verify(asked.visible);
            visualChild(asked, "discardDialogDiscard").activate();
            tryCompare(completion, "count", 1);
            compare(completion.signalArguments[0][0].error, "cancelled");
            verify(!sheet.visible);
        }
        function test_primary_picker_success_and_reuse() {
            var path = testContext.writeFixtureCsv("standalone-map.csv", "channel,frequency\n1,851012500\n");
            flow.pick("chan");
            var picker = item("csvPrimaryPicker");
            compare(picker.mode, "open");
            verify(picker.visible);
            answer("file://" + path, "Standalone map.csv");
            compare(completion.count, 1);
            verify(completion.signalArguments[0][0].ok);
            // The name the picker gave, not the reference's last segment.
            compare(completion.signalArguments[0][0].name, "Standalone map.csv");
            verify(!picker.visible);
            flow.pick("chan");
            backOut();
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
            answer("file://" + path, "picked-keys.csv");
            compare(completion.count, 0);
            item("csvCompleteBundle").clicked();
            compare(completion.count, 1);
            verify(completion.signalArguments[0][0].ok);
        }
        function test_primary_picker_cancel() {
            flow.pick("chan");
            backOut();
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
            backOut();
            compare(completion.count, 1);
        }
        function test_companion_picker_cancel_resumes_unresolved() {
            openSheet();
            item("csvChooseFile_0").clicked();
            backOut();
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
            // Issue #718: "Choose a file…" keeps the current companion until the
            // picker hands back a replacement; a rejected picker leaves it.
            item("csvChooseFile_0").clicked();
            compare(flow.companions[flow.requiredFiles[0]], stored, "opening the picker keeps the choice");
            backOut();
            compare(flow.companions[flow.requiredFiles[0]], stored, "a rejected picker keeps the choice");
            verify(choice.Accessible.checked);
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
            // Desktop file:// selections can identify a known library kind.
            answer("file://" + hex, "hex.csv");
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
            answer("file://" + dec, "decimal.csv");
            compare(flow.companions[hexKey], hex);
            item("csvCompleteBundle").clicked();
            compare(completion.count, 1);
            verify(completion.signalArguments[0][0].ok);
            compare(importedFiles.count, 3);
            flow.cancel();
            compare(completion.count, 1);
        }

        // ---- issue #829 ----

        // pick() cancels the import before it, with that import's context,
        // then keeps its own; every result carries the context it was asked with.
        function test_results_carry_their_context() {
            flow.pick("chan", -1, {target: "first"});
            var first = requestId();
            flow.pick("chan", -1, {target: "second"});
            compare(completion.count, 1);
            compare(completion.signalArguments[0][0].error, "cancelled");
            compare(completion.signalArguments[0][0].context.target, "first");
            verify(cancelledIds().indexOf(first) >= 0, "the first pick's request was cancelled");
            var path = testContext.writeFixtureCsv("context-map.csv", "channel,frequency\n1,851012500\n");
            answer("file://" + path, "Context map.csv");
            compare(completion.count, 2);
            verify(completion.signalArguments[1][0].ok);
            compare(completion.signalArguments[1][0].context.target, "second");
            flow.begin("/missing/primary.csv", "Map.csv", "chan", -1, {target: "third"});
            compare(completion.count, 3);
            compare(completion.signalArguments[2][0].context.target, "third");
            flow.pick("chan");
            flow.cancel();
            compare(JSON.stringify(completion.signalArguments[3][0].context), "{}");
        }

        // complete() ends both pickers silently: the request is cancelled and
        // a late answer for it finds nothing waiting.
        function test_a_finished_import_ignores_a_late_answer() {
            flow.pick("chan");
            var id = requestId();
            flow.cancel();
            compare(completion.count, 1);
            verify(cancelledIds().indexOf(id) >= 0);
            verify(!item("csvPrimaryPicker").visible);
            var path = testContext.writeFixtureCsv("late-map.csv", "channel,frequency\n1,851012500\n");
            testContext.answerDocumentPicked(id, "file://" + path, "late-map.csv", false);
            compare(completion.count, 1);
            compare(importedFiles.count, 0, "the late file was not imported");
        }

        function test_primary_picker_unavailable() {
            flow.pick("chan");
            testContext.answerDocumentUnavailable(requestId(), "noPicker");
            compare(completion.count, 1);
            var result = completion.signalArguments[0][0];
            verify(!result.ok);
            compare(result.error, "unavailable");
            compare(result.detail, "No compatible file picker on this device. Install a file manager app, then try again.");
            verify(!flow.active);
            compare(importedFiles.count, 0);
        }

        // An owner whose field took a newer choice drops the pick: the import
        // ends as cancelled, nothing is imported, whatever the picker answered.
        function test_a_pick_the_owner_no_longer_wants_is_dropped_data() {
            return [{tag: "picked"}, {tag: "unavailable"}];
        }
        function test_a_pick_the_owner_no_longer_wants_is_dropped(data) {
            var generation = 4;
            flow.wanted = function (context) {
                return context.generation === generation;
            };
            flow.pick("chan", -1, {generation: generation});
            generation++;
            if (data.tag === "picked") {
                var path = testContext.writeFixtureCsv("unwanted-map.csv", "channel,frequency\n1,851012500\n");
                answer("file://" + path, "unwanted-map.csv");
            } else {
                testContext.answerDocumentUnavailable(requestId(), "noResult");
            }
            compare(completion.count, 1);
            compare(completion.signalArguments[0][0].error, "cancelled");
            verify(!completion.signalArguments[0][0].detail, "no notice for a dropped pick");
            compare(importedFiles.count, 0);
        }

        // An update whose row has no stored path (a row index past the
        // library) is refused like one whose row went: never a new import.
        function test_an_update_without_a_row_is_refused() {
            var path = testContext.writeFixtureCsv("orphan-update.csv", "id,mode,name\n1,A,One\n");
            flow.pick("group", 99);
            answer("file://" + path, "orphan-update.csv");
            compare(completion.count, 1);
            var result = completion.signalArguments[0][0];
            verify(!result.ok);
            compare(result.error, "changed");
            compare(result.detail, "That file changed while you were picking. Try again.");
            compare(importedFiles.count, 0, "an update never adds a row");
        }

        // B3: "Selected:" names the picked file as the picker named it, not the
        // reference's last segment; a library choice then names the library file.
        function test_companion_label_names_the_picked_file() {
            addKeys("other.csv", "keysDec");
            openSheet();
            var label = item("csvCompanionSelected_0");
            compare(label.text, "No file selected");
            item("csvChooseFile_0").clicked();
            answer("content://com.Example.Provider/document/msf%3A1234", "Site keys.csv");
            compare(flow.companions[flow.requiredFiles[0]], "content://com.Example.Provider/document/msf%3A1234");
            compare(label.text, "Selected: Site keys.csv");
            item("csvLibraryChoice_0_0").clicked();
            compare(label.text, "Selected: other.csv");
        }
        function test_preselected_companion_label_names_the_library_file() {
            addKeys("wanted.csv", "keysDec");
            openSheet();
            compare(item("csvCompanionSelected_0").text, "Selected: wanted.csv");
        }

        // A library choice made while the picker is out wins over the pick
        // that answers after it.
        function test_newer_library_choice_survives_a_late_pick() {
            var stored = addKeys("other.csv", "keysDec");
            openSheet();
            item("csvChooseFile_0").clicked();
            var id = requestId();
            item("csvLibraryChoice_0_0").clicked();
            testContext.answerDocumentPicked(id, "content://com.example.documents/document/late.csv", "late.csv", false);
            compare(flow.companions[flow.requiredFiles[0]], stored);
            compare(item("csvCompanionSelected_0").text, "Selected: other.csv");
            verify(flow.active);
        }

        // With no picker the sheet comes back with every choice kept and says
        // why, read out once per failure; the line goes with the next try, a
        // library choice or the end of the import.
        function test_companion_unavailable_keeps_choices() {
            var hex = addKeys("hex.csv", "keysHex");
            var path = testContext.writeFixtureCsv("flow-unavailable.csv", companionRolesData()[0].csv);
            flow.begin(path, "Roles.csv", "chan");
            var decimalIndex = flow.requiredDetails[flow.requiredFiles[0]].type === "keysDec" ? 0 : 1;
            var hexIndex = 1 - decimalIndex;
            item("csvLibraryChoice_" + hexIndex + "_0").clicked();
            var notice = item("csvCompanionNotice");
            verify(!notice.visible);
            var said = flow.announcements;
            item("csvChooseFile_" + decimalIndex).clicked();
            testContext.answerDocumentUnavailable(requestId(), "noPicker");
            var message = "No compatible file picker on this device. Install a file manager app, then try again.";
            verify(item("csvCompanionSheet").visible);
            verify(flow.active);
            compare(completion.count, 0);
            compare(flow.companions[flow.requiredFiles[hexIndex]], hex, "the earlier choice is kept");
            verify(!flow.companions[flow.requiredFiles[decimalIndex]]);
            compare(flow.choosing, "");
            compare(flow.companionNotice, message);
            verify(notice.visible);
            compare(notice.text, message);
            compare(notice.Accessible.role, Accessible.AlertMessage);
            compare(flow.announcements, said + 1);
            compare(flow.announced, message);
            item("csvChooseFile_" + decimalIndex).clicked();
            compare(flow.companionNotice, "", "the next try clears the line");
            testContext.answerDocumentUnavailable(requestId(), "noPicker");
            compare(flow.announcements, said + 2, "a repeat is read out again");
            item("csvLibraryChoice_" + hexIndex + "_0").clicked();
            compare(flow.companionNotice, "", "a library choice clears the line");
            item("csvChooseFile_" + decimalIndex).clicked();
            testContext.answerDocumentUnavailable(requestId(), "failed");
            compare(flow.companionNotice, "The file picker could not open. Try again.");
            flow.cancel();
            compare(flow.companionNotice, "", "the end of the import clears the line");
        }

        // B6, desktop path: a dialog that answers without a new choice never
        // hands a companion slot the file chosen for another.
        function test_a_later_pick_never_carries_an_earlier_reference() {
            testContext.setDocumentPickerSupported(false);
            var desktop = createTemporaryObject(flowComponent, flow.parent);
            verify(desktop);
            var spy = createTemporaryObject(completionSpy, flow.parent, {target: desktop});
            var hex = addKeys("hex.csv", "keysHex");
            var path = testContext.writeFixtureCsv("flow-desktop.csv", companionRolesData()[0].csv);
            desktop.begin(path, "Roles.csv", "chan");
            compare(desktop.requiredFiles.length, 2);
            var first = desktop.requiredFiles[0];
            var second = desktop.requiredFiles[1];
            var dialog = findChild(findChild(desktop, "csvCompanionPicker"), "documentPickerDialog");
            verify(dialog, "the desktop flow uses Qt's FileDialog");
            desktop.chooseFile(first);
            tryCompare(dialog, "visible", true);
            // Qt's accept() takes the platform dialog's own selection, which a
            // test cannot set: say accepted() with the file, then close.
            dialog.selectedFile = "file://" + hex;
            dialog.accepted();
            dialog.close();
            compare(desktop.companions[first], "file://" + hex);
            compare(desktop.companionNames[first], "hex.csv");
            desktop.chooseFile(second);
            tryCompare(dialog, "visible", true);
            dialog.accept();
            verify(desktop.companions[second] !== "file://" + hex, "the other slot's file came back");
            verify(!desktop.companions[second]);
            compare(desktop.companions[first], "file://" + hex, "the earlier choice stays");
            compare(desktop.companionNotice, "No file came back from the picker. Try again.");
            verify(desktop.active);
            compare(spy.count, 0);
            desktop.cancel();
            compare(spy.count, 1);
        }
    }
    Component {
        id: completionSpy
        SignalSpy {
            signalName: "finished"
        }
    }
}
