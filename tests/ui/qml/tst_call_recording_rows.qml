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
            verify(!findChild(shown[0], "callRowTime").visible, "an encrypted row shows ENC instead of the time");
            verify(encMark.x + glyph(shown[0]).width + 8 <= shown[0].width - 18 - 20, "and the glyph sits before it");

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
