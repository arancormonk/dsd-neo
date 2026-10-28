// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest

Item {
    width: 420
    height: 900
    QtObject {
        id: recorder
        property int edits: 0
        property int removes: 0
        property var last: []
        function setTalkgroupPolicy(a, b, c, g, changes) {
            edits++;
            last = [a, b, c, g, changes];
            return true;
        }
        function addTalkgroup(a, b, c, g, n, l, p, e, r) {
            return setTalkgroupPolicy(a, b, c, g, {
                name: n,
                listening: l,
                priority: p,
                preempt: e,
                record: r
            });
        }
        function removeTalkgroup(a, b, c, g) {
            removes++;
            last = [a, b, c, g];
            return true;
        }
    }
    Loader {
        id: loader
        anchors.fill: parent
        source: uiDir + "/TalkgroupEditSheet.qml"
    }
    TestCase {
        name: "TalkgroupEditSheet"
        when: windowShown
        function init() {
            testContext.resetCommands();
            verify(loader.item !== null, "edit sheet must load");
            loader.item.bridge = recorder;
            loader.item.openRow(42, 49, "Dispatch", true, true, 25, false, "18446744073709551614", 9);
            recorder.edits = 0;
            recorder.removes = 0;
            waitForRendering(loader.item);
        }
        function cleanup() {
            loader.item.visible = false;
            testContext.resetCommands();
        }
        // Issue #718: the last word of a name still composing on the keyboard
        // is part of the name Apply saves, whether tapped or activated.
        function test_apply_keeps_the_composing_word_data() {
            return [{tag: "tap", tap: true}, {tag: "keyboard or accessibility", tap: false}];
        }
        function test_apply_keeps_the_composing_word(data) {
            var sheet = loader.item;
            var name = findChild(sheet, "talkgroupName");
            name.input.forceActiveFocus();
            name.input.cursorPosition = name.text.length;
            verify(testContext.composeText(" North"));
            compare(name.text, "Dispatch", "the word is still the keyboard's");
            var apply = findChild(sheet, "saveTalkgroup");
            if (data.tap)
                mouseClick(apply, apply.width / 2, apply.height / 2);
            else
                apply.activate();
            compare(recorder.edits, 1);
            compare(recorder.last[4].name, "Dispatch North");
            verify(!sheet.visible);
        }
        // A form sheet: a tap beside it keeps it, and the visible Close asks
        // before an edit goes (including one that is only composing text).
        function test_cancel_is_visible_and_asks() {
            var sheet = loader.item;
            var cancel = findChild(sheet, "talkgroupEditClose");
            verify(cancel !== null && cancel.visible, "the editor offers a visible Close");
            verify(cancel.height >= 48);
            mouseClick(sheet, 4, sheet.height / 2);
            wait(50);
            verify(sheet.visible, "a tap beside the editor keeps it");
            var name = findChild(sheet, "talkgroupName");
            name.input.forceActiveFocus();
            name.input.cursorPosition = name.text.length;
            verify(testContext.composeText("X"));
            cancel.activate();
            verify(sheet.visible, "the composing edit is asked about");
            compare(name.text, "DispatchX");
            var question = null;
            for (var child of sheet.children) {
                if (child.accessibleName === "Discard changes?" && child.visible)
                    question = child;
            }
            verify(question !== null, "Cancel asks before the edit goes");
            findChild(question, "discardDialogDiscard").activate();
            verify(!sheet.visible);
            compare(recorder.edits, 0);
            // Untouched, Cancel just closes.
            sheet.openRow(42, 49, "Dispatch", true, true, 25, false, "18446744073709551614", 9);
            cancel.activate();
            verify(!sheet.visible);
        }
        function test_seven_digit_heading_data() {
            return [
                {tag: "exact", end: 1234567, text: "Talkgroup 1234567"},
                {tag: "range", end: 1234570, text: "Talkgroup 1234567–1234570"}
            ];
        }
        function test_seven_digit_heading(data) {
            // JSON numbers match the double-valued IDs supplied by the models.
            var ids = JSON.parse("[1234567," + data.end + "]");
            loader.item.openRow(ids[0], ids[1], "", true, true, 0, false, "7", 1);
            compare(findChild(loader.item, "talkgroupEditHeading").text, data.text);
        }
        function test_presets_steppers_and_preempt() {
            var sheet = loader.item;
            mouseClick(findChild(sheet, "priorityPreset50"));
            compare(sheet.priorityValue, 50);
            mouseClick(findChild(sheet, "priorityPlus"));
            compare(sheet.priorityValue, 55);
            mouseClick(findChild(sheet, "priorityMinus"));
            compare(sheet.priorityValue, 50);
            mouseClick(findChild(sheet, "priorityPreset0"));
            verify(!findChild(sheet, "preemptToggle").enabled);
            verify(findChild(sheet, "preemptToggle").opacity < 1.0, "a toggle that cannot change looks disabled");
            mouseClick(findChild(sheet, "priorityMinus"));
            compare(sheet.priorityValue, 0);
            mouseClick(findChild(sheet, "priorityPreset100"));
            mouseClick(findChild(sheet, "priorityPlus"));
            compare(sheet.priorityValue, 100);
            mouseClick(findChild(sheet, "preemptToggle"));
            mouseClick(findChild(sheet, "saveTalkgroup"));
            compare(recorder.edits, 1);
            compare(recorder.last[2], "18446744073709551614");
            compare(recorder.last[3], 9);
            compare(recorder.last[4].priority, 100);
            compare(recorder.last[4].preempt, true);
            compare(Object.keys(recorder.last[4]).sort(), ["preempt", "priority"]);
        }
        function test_preempt_hint_follows_listening() {
            var sheet = loader.item;
            var hint = findChild(sheet, "preemptHint");
            verify(hint.text.indexOf("lower-priority") >= 0);
            mouseClick(findChild(sheet, "listeningToggle"));
            verify(hint.text.indexOf("Not tuned") === 0);
        }
        function test_only_changed_fields_and_no_op() {
            mouseClick(findChild(loader.item, "saveTalkgroup"));
            compare(recorder.edits, 0);
            findChild(loader.item, "talkgroupName").text = "Renamed";
            mouseClick(findChild(loader.item, "saveTalkgroup"));
            compare(recorder.edits, 1);
            compare(Object.keys(recorder.last[4]), ["name"]);
            verify(!loader.item.visible);
            // Consecutive actions on the submitted form cannot reuse its generation.
            loader.item.saveRow();
            compare(recorder.edits, 1);
            var remove = findChild(loader.item, "removeTalkgroup");
            remove.clicked();
            remove.clicked();
            compare(recorder.removes, 0);
        }
        function test_remove_requires_two_taps_and_reopen_resets() {
            var button = findChild(loader.item, "removeTalkgroup");
            mouseClick(button);
            compare(recorder.removes, 0);
            mouseClick(button);
            compare(recorder.removes, 1);
            loader.item.openRow(42, 42, "", true, true, 0, false, "7", 1);
            mouseClick(button);
            compare(recorder.removes, 1);
        }
        function test_card_priority_badge() {
            var component = Qt.createComponent(uiDir + "/TalkgroupCard.qml");
            compare(component.status, Component.Ready);
            var card = component.createObject(loader, {
                width: 300,
                height: 130,
                idText: "42",
                name: "Fire",
                tags: "",
                listening: true,
                listed: true,
                priority: 50,
                preempt: true,
                temporaryAvoidCount: 0
            });
            verify(card !== null);
            compare(findChild(card, "talkgroupBadge").text, "42 · Priority 50 · Preempt");
            card.preempt = false;
            compare(findChild(card, "talkgroupBadge").text, "42 · Priority 50");
            card.destroy();
        }

        // Issue #708, D11: Record follows Listening. Off and disabled while not tuned, on again when tuned, and
        // editable before the save; sent only where it differs from what the decoder will hold.
        function test_record_follows_listening() {
            var sheet = loader.item;
            sheet.openRow(42, 42, "Dispatch", true, true, 0, false, "7", 3, false);
            var listen = findChild(sheet, "listeningToggle");
            var record = findChild(sheet, "recordToggle");
            var hint = findChild(sheet, "recordHint");
            verify(!record.checked, "the row's own Record");
            verify(record.enabled);
            compare(hint.text, "Saves this talkgroup's calls when call recording is on.");
            verify(record.mapToItem(sheet, 0, 0).y > listen.mapToItem(sheet, 0, 0).y, "under Listening");
            compare(record.opacity, 1.0);
            mouseClick(listen);
            verify(!record.checked && !record.enabled, "not tuned records nothing");
            verify(record.opacity < 1.0, "and looks it");
            compare(hint.text, "Off while not tuned. Tuning it again turns Record back on.");
            mouseClick(listen);
            verify(record.checked && record.enabled, "tuned again records again");
            mouseClick(findChild(sheet, "saveTalkgroup"));
            compare(recorder.edits, 1);
            compare(recorder.last[4], {record: true});
        }
        function test_record_edits_data() {
            return [
                {tag: "record-off", listening: true, record: true, clicks: ["recordToggle"], changes: {record: false}},
                {tag: "record-on", listening: true, record: false, clicks: ["recordToggle"], changes: {record: true}},
                {tag: "tune-without-record", listening: false, record: false, clicks: ["listeningToggle", "recordToggle"], changes: {listening: true, record: false}},
                {tag: "tune-and-record", listening: false, record: false, clicks: ["listeningToggle"], changes: {listening: true}},
                {tag: "untune", listening: true, record: false, clicks: ["listeningToggle"], changes: {listening: false}}
            ];
        }
        function test_record_edits(data) {
            var sheet = loader.item;
            sheet.openRow(42, 42, "Dispatch", data.listening, true, 0, false, "7", 3, data.record);
            for (var name of data.clicks)
                mouseClick(findChild(sheet, name));
            mouseClick(findChild(sheet, "saveTalkgroup"));
            compare(recorder.edits, 1);
            compare(recorder.last[4], data.changes);
        }
        function test_record_change_is_an_edit() {
            var sheet = loader.item;
            sheet.openRow(42, 42, "Dispatch", true, true, 0, false, "7", 3, true);
            mouseClick(findChild(sheet, "recordToggle"));
            findChild(sheet, "talkgroupEditClose").activate();
            verify(sheet.visible, "Close asks before a Record change goes");
            var question = null;
            for (var child of sheet.children) {
                if (child.accessibleName === "Discard changes?" && child.visible)
                    question = child;
            }
            verify(question !== null);
            findChild(question, "discardDialogDiscard").activate();
            compare(recorder.edits, 0);
        }
        function test_heard_add_takes_record() {
            var sheet = loader.item;
            sheet.openRow(55, 55, "", false, false, 0, false, "7", 2);
            verify(!findChild(sheet, "recordToggle").checked);
            mouseClick(findChild(sheet, "listeningToggle"));
            verify(findChild(sheet, "recordToggle").checked, "a heard talkgroup tuned records");
            mouseClick(findChild(sheet, "recordToggle"));
            mouseClick(findChild(sheet, "saveTalkgroup"));
            compare(recorder.last[4].listening, true);
            compare(recorder.last[4].record, false);
        }

        function test_heard_add_and_backend_toast() {
            loader.item.openRow(55, 55, "", false, false, 0, false, "7", 2);
            compare(findChild(loader.item, "saveTalkgroup").text, "Add to list");
            verify(!findChild(loader.item, "removeTalkgroup").visible);
            loader.item.backendMessage = "Talkgroup edit refused: stale policy context";
            compare(findChild(loader.item, "talkgroupEditToast").text, loader.item.backendMessage);
            mouseClick(findChild(loader.item, "saveTalkgroup"));
            compare(recorder.edits, 1);
            verify(!loader.item.visible); // Reopen through the model for a fresh version.
        }
    }
}
