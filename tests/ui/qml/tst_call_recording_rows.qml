// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest

// Issue #708: a call with a recording carries a small play glyph in the shared CallRow, on History and on Home's
// recent calls, and says so to a screen reader; a row without one is unchanged. History also says when its own
// saves are failing (B6), which was only logged before.
Item {
    width: 420
    height: 900

    Loader {
        id: history
        anchors.fill: parent
        source: uiDir + "/HistoryScreen.qml"
    }
    // Home's recent calls show in the supporting pane of a wide window.
    Loader {
        id: home
        width: 1000
        height: 900
        active: false
        visible: active
        source: uiDir + "/HomeScreen.qml"
    }

    TestCase {
        name: "CallRecordingRows"
        when: windowShown

        function rec(id) {
            return {id: id, path: "/r/" + id + ".wav", fileName: id + ".wav", folderUid: "u1", start: 1700000000, durationMs: 4000, rate: 8000, bytes: 64000, failed: false, available: true};
        }
        // The CallRow delegates of the History list, top first.
        function rows() {
            var list = findChild(history.item, "callLogList");
            var found = [];
            for (var child of list.contentItem.children) {
                if (child.hasOwnProperty("hasRecording") && child.visible)
                    found.push(child);
            }
            found.sort(function (a, b) { return a.y - b.y; });
            return found;
        }
        function glyph(row) {
            return findChild(row, "rowRecordingGlyph");
        }

        function init() {
            callHistory.clearAll();
            callHistory.setSaveFailing(false);
        }
        function cleanup() {
            callHistory.clearAll();
            callHistory.setSaveFailing(false);
        }

        function test_glyph_marks_a_recorded_call() {
            callHistory.push("TODAY");
            var uid = callHistory.pushWithRecordings([rec("a")]);
            callHistory.pushEncrypted("TODAY");
            callHistory.setRowRecordings(callHistory.newestUid(), [rec("b")]);
            waitForRendering(history.item);
            var shown = rows();
            compare(shown.length, 3);
            // Newest first: the encrypted call with a recording, the recorded call, the plain one.
            verify(glyph(shown[0]).visible, "an encrypted call with a recording");
            verify(glyph(shown[1]).visible);
            verify(!glyph(shown[2]).visible, "a row without one is unchanged");
            compare(shown[1].Accessible.name, shown[1].accessibleName + ", has a recording");
            compare(shown[2].Accessible.name, shown[2].accessibleName);
            // Before the time, or before the ENC tag.
            var mark = glyph(shown[1]).mapToItem(shown[1], 0, 0);
            var time = findChild(shown[1], "callRowTime").mapToItem(shown[1], 0, 0);
            compare(mark.x + glyph(shown[1]).width + 8, time.x, "the glyph sits just before the time");
            verify(mark.y > 0 && mark.y + glyph(shown[1]).height < shown[1].height);
            var encMark = glyph(shown[0]).mapToItem(shown[0], 0, 0);
            var enc = findChild(shown[0], "rowEncTag");
            verify(enc.visible && !findChild(shown[0], "callRowTime").visible, "an encrypted row shows ENC instead of the time");
            compare(encMark.x + glyph(shown[0]).width + 8, enc.mapToItem(shown[0], 0, 0).x, "and the glyph sits just before it");

            // A recording deleted elsewhere takes the glyph with it.
            verify(callHistory.setRowRecordings(uid, []));
            verify(!glyph(rows()[1]).visible);
        }

        function test_glyph_beside_emergency() {
            callHistory.pushEmergency();
            callHistory.setRowRecordings(callHistory.newestUid(), [rec("c")]);
            waitForRendering(history.item);
            var row = rows()[0];
            var tag = findChild(row, "rowEmergencyTag");
            verify(tag.visible && glyph(row).visible);
            verify(tag.x + tag.width <= glyph(row).x, "EMERGENCY, then the glyph, then the time");
        }

        function test_home_recent_calls() {
            callHistory.push("TODAY");
            var uid = callHistory.pushWithRecordings([rec("h")]);
            home.active = true;
            waitForRendering(home.item);
            var list = findChild(home.item, "homeRecentList");
            verify(list !== null && list.visible);
            var found = [];
            for (var child of list.contentItem.children) {
                if (child.hasOwnProperty("hasRecording") && child.visible)
                    found.push(child);
            }
            found.sort(function (a, b) { return a.y - b.y; });
            compare(found.length, 2);
            verify(glyph(found[0]).visible, "the recorded call carries the glyph");
            verify(!glyph(found[1]).visible);
            compare(found[0].Accessible.name, found[0].accessibleName + ", has a recording");
            // Its detail sheet follows the row, recordings and all.
            found[0].activate();
            var sheet = findChild(home.item, "homeRecentDetailSheet");
            verify(sheet.visible);
            compare(sheet.rowRecordings.length, 1);
            verify(findChild(sheet, "historyRecording").visible);
            sheet.visible = false;
            home.active = false;
        }

        // Issue #782: hidden under the monitor, Home's recent calls hold no rows,
        // so calls landing there build none; shown again they list the calls as
        // they are then, newest first, and the empty line stays away.
        function test_home_recent_calls_hidden() {
            callHistory.clearAll();
            callHistory.push("TODAY");
            home.active = true;
            waitForRendering(home.item);
            var list = findChild(home.item, "homeRecentList");
            compare(list.count, 1);
            home.item.visible = false;
            var newest = "";
            for (var i = 0; i < 3; ++i) {
                newest = callHistory.push("TODAY");
                waitForRendering(home);
            }
            compare(list.count, 0, "a hidden list holds no rows");
            home.item.visible = true;
            waitForRendering(home.item);
            compare(list.count, 4);
            tryVerify(function () { return list.itemAtIndex(0) !== null && list.itemAtIndex(0).name === newest; });
            var empty = findChild(home.item, "homeRecentEmpty");
            verify(empty !== null && !empty.visible, "a list with calls never says it is empty");
            home.active = false;
        }

        // A reader who had scrolled down Home's recent calls is back on the same
        // row at the same height when the screen shows again.
        function test_home_recent_calls_keep_the_place() {
            callHistory.clearAll();
            callHistory.pushMany(40, "TODAY");
            home.active = true;
            waitForRendering(home.item);
            var list = findChild(home.item, "homeRecentList");
            list.contentY = list.originY + 400;
            waitForRendering(list);
            var at = list.indexAt(list.width / 2, list.contentY + 1);
            verify(at > 0);
            var name = list.itemAtIndex(at).name;
            var offset = list.itemAtIndex(at).y - list.contentY;
            home.item.visible = false;
            callHistory.push("TODAY");
            waitForRendering(home);
            home.item.visible = true;
            waitForRendering(list);
            var now = list.indexAt(list.width / 2, list.contentY + 1);
            compare(list.itemAtIndex(now).name, name);
            verify(Math.abs(list.itemAtIndex(now).y - list.contentY - offset) <= 2);
            home.active = false;
        }

        function test_save_failing_notice() {
            var notice = findChild(history.item, "historySaveFailing");
            verify(!notice.visible);
            callHistory.setSaveFailing(true);
            verify(notice.visible);
            compare(notice.text, "History isn't being saved right now. It keeps trying, but calls since the last save are lost if DSD-neo closes. Free up space if the phone is full.");
            compare(notice.Accessible.role, Accessible.AlertMessage);
            callHistory.setSaveFailing(false);
            verify(!notice.visible, "a save that lands ends it");
        }
    }
}
