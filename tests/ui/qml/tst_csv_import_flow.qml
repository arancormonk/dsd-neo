// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui
import "../../../src/ui/qt/qml/Util.js" as Util

Item {
    id: root
    width: 420
    height: 900
    // The control the flow is opened from: its focus comes back to it.
    Ui.OutlineButton {
        id: opener
        objectName: "flowOpener"
        width: 200
        text: "Import"
    }
    Ui.CsvImportFlow {
        id: flow
        anchors.fill: parent
    }
    SignalSpy {
        id: completion
        target: flow
        signalName: "finished"
    }
    // #832: a Cancel that lands after the commit ran, before its answer is in.
    // The commit changes the library at once and answers later, queued.
    Connections {
        id: lateCancel
        target: importedFiles
        enabled: false
        property int asked: 0
        function onEntriesChanged() {
            if (!flow.copying)
                return;
            lateCancel.asked++;
            flow.requestCancel();
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
            completion.clear();
            source = testContext.writeFixtureCsv("flow-primary.csv",
                "channel,frequency,mode,keys_dec_csv\n1,851012500,p25,unavailable/wanted.csv\n");
        }
        function cleanup() {
            lateCancel.enabled = false;
            lateCancel.asked = 0;
            testContext.holdImports(false);
            flow.cancel();
            tryVerify(function () { return !flow.active; }, 5000, "the flow ends");
            while (importedFiles.count)
                importedFiles.remove(0);
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
            tryVerify(function () { return flow.requiredFiles.length > 0; }, 5000, "the companion choices show");
            verify(!flow.copying);
            compare(completion.count, 0);
        }
        function standaloneMap() {
            return testContext.writeFixtureCsv("standalone-map.csv", "channel,frequency\n1,851012500\n");
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
            var path = standaloneMap();
            flow.pick("chan");
            var picker = item("csvPrimaryPicker");
            picker.selectedFile = "file://" + path;
            picker.accepted();
            picker.close();
            tryCompare(completion, "count", 1);
            verify(completion.signalArguments[0][0].ok);
            flow.pick("chan");
            picker.reject();
            compare(completion.count, 2);
            compare(completion.signalArguments[1][0].error, "cancelled");
        }
        function test_failed_import_finishes_once() {
            flow.begin("/missing/primary.csv", "Map.csv", "chan");
            tryCompare(completion, "count", 1);
            verify(!completion.signalArguments[0][0].ok);
            compare(completion.signalArguments[0][0].error, "read");
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
            tryCompare(completion, "count", 1);
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
        // A provider's reference names the file URL-encoded: the line under
        // the choice reads its decoded name, as the busy sheet does.
        function test_selected_names_an_encoded_reference() {
            openSheet();
            flow.selectCompanion(flow.requiredFiles[0], "content://com.android.providers.downloads.documents/document/raw%3A%2Fstorage%2Femulated%2F0%2FDownload%2Fcounty-keys.csv");
            compare(item("csvCompanionSelected_0").text, "Selected: county-keys.csv");
            flow.selectCompanion(flow.requiredFiles[0], "file:///data/keys/plain%zz.csv");
            compare(item("csvCompanionSelected_0").text, "Selected: plain%zz.csv", "a name that does not decode shows as it came");
        }
        // A library file's stored path is not a URI: its real name shows as
        // it is, never decoded.
        function test_selected_names_a_library_path_as_stored() {
            openSheet();
            flow.selectCompanion(flow.requiredFiles[0], "/data/imports/keys%20old.csv");
            compare(item("csvCompanionSelected_0").text, "Selected: keys%20old.csv");
            flow.selectCompanion(flow.requiredFiles[0], "/data/imports/bundles/r1/a%2Fb.csv");
            compare(item("csvCompanionSelected_0").text, "Selected: a%2Fb.csv", "an encoded slash in a real name is not a folder");
            flow.selectCompanion(flow.requiredFiles[0], "file:///data/keys/keys%20old.csv");
            compare(item("csvCompanionSelected_0").text, "Selected: keys old.csv", "a file: URI still decodes");
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
            // The stored copy takes the document's own name.
            var path = testContext.writeFixtureCsv(name, "id,value\n1,12345\n");
            var result = testContext.importNow(path, type);
            verify(result.ok, "the key fixture did not import: " + result.error);
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
            item("csvCompanionPicker").reject();
            compare(flow.companions[flow.requiredFiles[0]], stored, "a rejected picker keeps the choice");
            verify(choice.Accessible.checked);
            verify(!/[●○]/.test(choice.Accessible.name));
            choice.clicked();
            verify(choice.Accessible.checked);
            compare(flow.companions[flow.requiredFiles[0]], stored);
            item("csvCompleteBundle").clicked();
            tryCompare(completion, "count", 1);
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
            tryVerify(function () { return flow.requiredFiles.length === 2; }, 5000, "both companions are asked for");
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
            tryVerify(function () { return flow.requiredFiles.length === 2; }, 5000, "both companions are asked for");
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
            tryVerify(function () { return !flow.copying; }, 5000, "the retry answers");
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
            tryCompare(completion, "count", 1);
            verify(completion.signalArguments[0][0].ok);
            compare(importedFiles.count, 3);
            flow.cancel();
            compare(completion.count, 1);
        }

        // #832: the copy runs on the import worker. The sheet shows from the
        // start, says what is being copied, and the flow finishes once, with
        // the import's own answer.
        function test_busy_sheet_shows_while_copying() {
            testContext.holdImports(true);
            flow.begin(standaloneMap(), "Map.csv", "chan");
            var sheet = item("csvCompanionSheet");
            verify(flow.copying, "the import is in flight");
            verify(sheet.visible, "the busy sheet shows at once");
            compare(item("csvCopyingText").text, "Copying Map.csv…");
            verify(item("csvCopyingText").visible);
            verify(!item("csvCompleteBundle").visible, "no companion choices while the first copy runs");
            compare(sheet.Accessible.name, "Copying Map.csv");
            compare(item("csvCancel").text, "Cancel");
            verify(item("csvCancel").enabled);
            wait(100);
            compare(completion.count, 0, "nothing finishes while the copy is held");
            testContext.holdImports(false);
            tryCompare(completion, "count", 1);
            verify(completion.signalArguments[0][0].ok);
            verify(!sheet.visible);
            verify(!flow.copying);
            compare(importedFiles.count, 1);
            wait(50);
            compare(completion.count, 1, "one finished per flow");
        }
        // A picked name arrives URL-encoded: the sheet reads it decoded.
        function test_busy_sheet_names_an_encoded_pick() {
            testContext.holdImports(true);
            flow.begin(standaloneMap(), "primary%3ADownload%2FCounty%20map.csv", "chan");
            compare(item("csvCopyingText").text, "Copying County map.csv…");
        }
        // Cancel, Back and Escape ask the import to stop. The flow finishes at
        // once with "cancelled", even when the copy ignores its cancel (a
        // provider that never lets go): nothing waits on the copy, the copy
        // adds nothing when it returns, and the next import runs after it.
        function test_busy_cancel_data() {
            return [{tag: "direct"}, {tag: "button"}, {tag: "back"}, {tag: "escape"}];
        }
        function test_busy_cancel(data) {
            // The cancelled copy stays held: a copy that ignores its cancel.
            testContext.holdImports(true, true);
            flow.begin(standaloneMap(), "Map.csv", "chan");
            var sheet = item("csvCompanionSheet");
            verify(sheet.visible);
            // A request cancelled before its copy starts answers at once; this
            // case cancels one that is copying.
            tryVerify(function () { return testContext.heldImports() === 1; }, 5000, "the copy is running");
            if (data.tag === "direct") {
                // Until the queued answer is in, Cancel says so and a second
                // dismiss changes nothing.
                flow.requestCancel();
                verify(flow.cancelling, "the cancel was asked for");
                compare(item("csvCancel").text, "Cancelling…");
                verify(!item("csvCancel").enabled);
                sheet.requestDismiss();
                compare(completion.count, 0, "the answer is queued");
            } else if (data.tag === "button")
                mouseClick(item("csvCancel"));
            else {
                sheet.forceActiveFocus();
                keyClick(data.tag === "back" ? Qt.Key_Back : Qt.Key_Escape);
            }
            tryCompare(completion, "count", 1, 1000, "the flow finishes while the copy is still held");
            compare(completion.signalArguments[0][0].error, "cancelled");
            compare(completion.signalArguments[0][0].type, "chan");
            verify(!sheet.visible);
            verify(!flow.active && !flow.copying && !flow.cancelling);
            compare(testContext.heldImports(), 1, "the copy is still stuck");
            // The next import waits behind the stuck copy, then runs.
            verify(flow.begin(standaloneMap(), "Next.csv", "chan", {tag: "next"}), "a new import starts");
            wait(100);
            compare(completion.count, 1, "the next import waits for the stuck copy");
            compare(testContext.heldImports(), 1, "its copy has not started");
            testContext.holdImports(false);
            tryCompare(completion, "count", 2);
            verify(completion.signalArguments[1][0].ok, "the next import completes");
            compare(completion.signalArguments[1][1].tag, "next");
            compare(importedFiles.count, 1, "the cancelled copy added nothing when it returned");
            wait(50);
            compare(completion.count, 2, "one answer each");
        }
        // Cancelling wins only before the commit: once the file is in the
        // library, the flow reports that, not a cancel.
        function test_cancel_after_the_commit_reports_the_commit() {
            lateCancel.enabled = true;
            flow.begin(standaloneMap(), "Map.csv", "chan");
            tryCompare(completion, "count", 1);
            compare(lateCancel.asked, 1, "the cancel landed between the commit and its answer");
            verify(completion.signalArguments[0][0].ok, "the commit's outcome is the answer");
            compare(importedFiles.count, 1);
            verify(!findChild(flow, "csvCompanionSheet").visible);
        }
        // The companions sheet's retry: the import button says it is working
        // and stays disabled until the answer is in.
        function test_companion_retry_shows_importing() {
            addKeys("wanted.csv", "keysDec");
            openSheet();
            verify(item("csvLibraryChoice_0_0").enabled && item("csvChooseFile_0").enabled);
            testContext.holdImports(true);
            item("csvCompleteBundle").clicked();
            verify(flow.copying);
            compare(item("csvCompleteBundle").text, "Importing…");
            verify(!item("csvCompleteBundle").enabled);
            // A changed choice would not reach the copy running now.
            verify(!item("csvLibraryChoice_0_0").enabled, "the library choice waits for the copy");
            verify(!item("csvChooseFile_0").enabled, "Choose a file waits for the copy");
            var chosen = flow.companions[flow.requiredFiles[0]];
            item("csvLibraryChoice_0_0").activate();
            item("csvChooseFile_0").activate();
            compare(flow.choosing, "", "no picker opens");
            compare(flow.companions[flow.requiredFiles[0]], chosen, "the choice stays");
            verify(item("csvCompleteBundle").visible, "the companion choices stay on show");
            verify(!item("csvCopyingText").visible);
            item("csvCompleteBundle").clicked();
            testContext.holdImports(false);
            tryCompare(completion, "count", 1);
            verify(completion.signalArguments[0][0].ok);
            compare(importedFiles.count, 2, "one bundle beside the key file");
        }
        // Cancelling a retry keeps the discard question for choices made.
        function test_companion_retry_cancel_asks_then_cancels() {
            var stored = addKeys("other.csv", "keysDec");
            openSheet();
            item("csvLibraryChoice_0_0").clicked();
            testContext.holdImports(true, true);
            item("csvCompleteBundle").clicked();
            var sheet = item("csvCompanionSheet");
            tryVerify(function () { return testContext.heldImports() >= 1; }, 5000, "the retry is copying");
            mouseClick(item("csvCancel"));
            var asked = question(sheet);
            verify(asked.visible);
            visualChild(asked, "discardDialogDiscard").activate();
            compare(item("csvCancel").text, "Cancelling…");
            compare(item("csvCompleteBundle").text, "Importing…");
            compare(completion.count, 0);
            tryCompare(completion, "count", 1, 1000, "the cancel is answered while the copy is still held");
            compare(completion.signalArguments[0][0].error, "cancelled");
            testContext.holdImports(false);
            tryVerify(function () { return testContext.heldImports() === 0; }, 5000, "the held copy returns");
            wait(50);
            compare(completion.count, 1);
            compare(importedFiles.count, 1, "the cancelled retry added nothing");
        }
        // A new start while a copy runs is refused: the running one keeps the
        // flow and finishes it once.
        function test_start_refused_while_copying() {
            testContext.holdImports(true);
            flow.begin(standaloneMap(), "Map.csv", "chan", {tag: "first"});
            verify(!flow.begin("/missing/other.csv", "Other.csv", "group", {tag: "second"}));
            verify(!flow.start("group"));
            compare(flow.reference.indexOf("standalone-map.csv") >= 0, true);
            compare(flow.type, "chan");
            testContext.holdImports(false);
            tryCompare(completion, "count", 1);
            verify(completion.signalArguments[0][0].ok);
            compare(completion.signalArguments[0][1].tag, "first", "the host's context comes back with the answer");
            wait(50);
            compare(completion.count, 1);
        }
        // The busy sheet is the existing modal sheet: it takes the focus and the
        // screen reader's turn at once, keeps Tab inside, and gives the focus
        // back to the control that opened the flow.
        function test_busy_sheet_is_modal() {
            opener.forceActiveFocus();
            verify(opener.activeFocus);
            testContext.holdImports(true);
            flow.begin(standaloneMap(), "Map.csv", "chan");
            var sheet = item("csvCompanionSheet");
            tryVerify(function () { return sheet.activeFocus; }, 2000, "the sheet takes the focus");
            verify(sheet.isTopModal);
            verify(!Ui.Navigation.allows(opener), "the screen behind is out of reach");
            verify(opener.Accessible.ignored, "and out of the accessibility tree");
            verify(!item("csvCancel").Accessible.ignored, "Cancel is reachable");
            for (var i = 0; i < 4; ++i) {
                keyClick(Qt.Key_Tab);
                verify(Ui.Navigation.contains(sheet, sheet.Window.window.activeFocusItem), "Tab stays inside the sheet");
            }
            testContext.holdImports(false);
            tryCompare(completion, "count", 1);
            tryVerify(function () { return opener.activeFocus; }, 2000, "the focus goes back to the opener");
        }
    }

    // #832: one message per failure, the same on every screen.
    TestCase {
        name: "ImportMessages"

        function test_message_data() {
            var mib = 1024 * 1024;
            return [
                {tag: "ok", result: {ok: true, error: "", name: "Map.csv"}, text: ""},
                {tag: "empty", result: {ok: true, error: "empty", name: "Map.csv"}, text: ""},
                {tag: "cancelled", result: {ok: false, error: "cancelled", name: "Map.csv"}, text: ""},
                {tag: "too large file", result: {ok: false, error: "too_large", name: "Map.csv", limitBytes: 32 * mib}, text: "Map.csv is too large to import. A CSV file can be up to 32\u00a0MiB."},
                {tag: "too large bundle", result: {ok: false, error: "too_large", name: "Map.csv", limitBytes: 64 * mib}, text: "These files are too large to import together. A CSV and its companion files can be up to 64\u00a0MiB in all."},
                {tag: "read", result: {ok: false, error: "read", name: "Map.csv"}, text: "Could not read Map.csv. Check that it is still available, then try again."},
                {tag: "read unnamed", result: {ok: false, error: "read", name: ""}, text: "Could not read this file. Check that it is still available, then try again."},
                {tag: "local", result: {ok: false, error: "local", name: "Map.csv", noSpace: false}, text: "Could not save a copy of Map.csv on this device."},
                {tag: "local no space", result: {ok: false, error: "local", name: "Map.csv", noSpace: true}, text: "Could not save a copy of Map.csv on this device. Free up some storage, then try again."},
                {tag: "format detail", result: {ok: false, error: "format", name: "Map.csv", type: "chan", detail: "Line 2: no frequency."}, text: "Line 2: no frequency."},
                {tag: "format", result: {ok: false, error: "format", name: "Map.csv", type: "chan", detail: ""}, text: "Map.csv is not a valid channel map."},
                {tag: "format keys", result: {ok: false, error: "format", name: "Keys.csv", type: "keysHex"}, text: "Keys.csv is not a valid hex key file."},
                {tag: "stale", result: {ok: false, error: "stale", name: "Map.csv"}, text: "The imported file changed while the new copy was being made. Nothing was replaced."},
                {tag: "busy targets", result: {ok: false, error: "busy", name: "T.csv", type: "trunkTargets"}, text: "Stop the decoder before replacing target files."},
                {tag: "busy group", result: {ok: false, error: "busy", name: "G.csv", type: "group"}, text: "Stop the decoder before replacing a talkgroup list."},
                {tag: "save", result: {ok: false, error: "save", name: "Map.csv"}, text: "Could not save the list of imported files. Try again."},
                {tag: "save detail", result: {ok: false, error: "save", name: "Map.csv", detail: "The file could not be replaced, and the library could not be put back as it was."}, text: "The file could not be replaced, and the library could not be put back as it was."},
                {tag: "path", result: {ok: false, error: "path", name: "Map.csv"}, text: "Cannot replace this imported file."},
                {tag: "unknown", result: {ok: false, error: "future", name: "Map.csv"}, text: "Could not import Map.csv."},
                // The file's own size, not the size plus the store's reserve.
                {tag: "replay too large", context: "replay", result: {ok: false, error: "too_large", name: "cap.wav", neededBytes: 3.4e9 + 536870912, freeBytes: 1.2e9, reserveBytes: 536870912}, text: "There is not enough free space to copy cap.wav (3.4\u00a0GB). This device has 1.2\u00a0GB free, and DSD-neo keeps about 0.5\u00a0GB spare."},
                // Smaller than the free space, still too large: the spare part says why.
                {tag: "replay inside the reserve", context: "replay", result: {ok: false, error: "too_large", name: "cap.wav", neededBytes: 1.0e9 + 536870912, freeBytes: 1.2e9, reserveBytes: 536870912}, text: "There is not enough free space to copy cap.wav (1\u00a0GB). This device has 1.2\u00a0GB free, and DSD-neo keeps about 0.5\u00a0GB spare."},
                {tag: "replay no space", context: "replay", result: {ok: false, error: "too_large", name: "cap.wav", neededBytes: -1, freeBytes: 2.5e8, reserveBytes: 536870912}, text: "There is not enough free space to copy cap.wav. This device has 250\u00a0MB free, and DSD-neo keeps about 0.5\u00a0GB spare."},
                // A result with no reserve says nothing about one, rather than "0.0 GB".
                {tag: "replay without a reserve", context: "replay", result: {ok: false, error: "too_large", name: "cap.wav", neededBytes: 3.4e9, freeBytes: 1.2e9}, text: "There is not enough free space to copy cap.wav (3.4\u00a0GB). This device has 1.2\u00a0GB free."},
                {tag: "replay no space without a reserve", context: "replay", result: {ok: false, error: "too_large", name: "cap.wav", neededBytes: -1, freeBytes: 2.5e8}, text: "There is not enough free space to copy cap.wav. This device has 250\u00a0MB free."},
                {tag: "replay local", context: "replay", result: {ok: false, error: "local", name: "cap.wav", noSpace: true}, text: "Could not save a copy of cap.wav on this device. Free up some storage, then try again."},
                {tag: "replay read", context: "replay", result: {ok: false, error: "read", name: "cap.wav"}, text: "Could not read cap.wav. Check that it is still available, then try again."},
                {tag: "replay cancelled", context: "replay", result: {ok: false, error: "cancelled", name: "cap.wav"}, text: ""}
            ];
        }
        function test_message(data) {
            compare(Util.importMessage(data.result, data.context || "csv"), data.text);
        }
        // Each failure reads differently: none falls back to one catch-all line.
        function test_messages_are_distinct() {
            var seen = {};
            for (var row of test_message_data()) {
                var text = Util.importMessage(row.result, row.context || "csv");
                if (!text.length || row.tag.indexOf("replay") === 0 && row.tag !== "replay too large" && row.tag !== "replay no space" && row.tag !== "replay inside the reserve")
                    continue;
                verify(seen[text] === undefined, row.tag + " reads as " + seen[text]);
                seen[text] = row.tag;
            }
        }
    }
}
