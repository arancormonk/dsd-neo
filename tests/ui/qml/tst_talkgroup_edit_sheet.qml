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
        function setTalkgroupPolicy(a, b, c, g, n, l, p, e) {
            edits++;
            last = [a, b, c, g, n, l, p, e];
            return true;
        }
        function addTalkgroup(a, b, c, g, n, l, p, e) {
            return setTalkgroupPolicy(a, b, c, g, n, l, p, e);
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
            verify(loader.item !== null, "edit sheet must load");
            loader.item.bridge = recorder;
            loader.item.openRow(42, 49, "Dispatch", true, true, 25, false, "18446744073709551614", 9);
            recorder.edits = 0;
            recorder.removes = 0;
            waitForRendering(loader.item);
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
            compare(recorder.last[6], 100);
            compare(recorder.last[7], true);
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
                preempt: true
            });
            verify(card !== null);
            compare(findChild(card, "talkgroupBadge").text, "42  P50 ⚡");
            card.preempt = false;
            compare(findChild(card, "talkgroupBadge").text, "42  P50");
            card.destroy();
        }

        function test_heard_add_and_backend_toast() {
            loader.item.openRow(55, 55, "", false, false, 0, false, "7", 2);
            compare(findChild(loader.item, "saveTalkgroup").text, "Add to list");
            verify(!findChild(loader.item, "removeTalkgroup").visible);
            loader.item.backendMessage = "Talkgroup edit refused: stale policy context";
            compare(findChild(loader.item, "talkgroupEditToast").text, loader.item.backendMessage);
            mouseClick(findChild(loader.item, "saveTalkgroup"));
            compare(recorder.edits, 1);
            verify(loader.item.visible); // Retain the sheet to show decoder refusal.
        }
    }
}
