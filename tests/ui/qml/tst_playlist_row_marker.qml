// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// Issue #909: the call a History playlist is on is marked in the Monitor's recent calls and in Listen's Recent
// activity as in History: two magenta bars in place of the recording glyph (dimmed while paused), the row named
// playing or paused, and gone once the playlist ends; a sheet's own play marks nothing. Names are made up.
Item {
    id: root
    width: 960
    height: 900

    Loader {
        id: monitorLoader
        width: 420
        height: 900
        source: uiDir + "/MonitorScreen.qml"
    }
    Ui.HomeScreen {
        id: home
        x: 0
        width: 960
        height: 900
        visible: false
        localDeviceRelevant: false
    }

    TestCase {
        name: "PlaylistRowMarker"
        when: windowShown

        function listOf(which) {
            return which === "monitor" ? findChild(monitorLoader.item, "recentCallsList") : findChild(home, "homeRecentList");
        }
        // The row of @p uid, by its name (the Monitor's rows keep no uid of their own; names are unique here).
        function rowOf(list, uid) {
            var name = callHistory.recordFor(uid).name;
            for (var i = 0; i < list.count; i++) {
                var item = list.itemAtIndex(i);
                if (item && item.name === name)
                    return item;
            }
            return null;
        }
        function init() {
            testContext.setPlaybackSupported(true);
            testContext.resetHostPlayback();
            monitorView.historySession = 0;
            callHistory.clearAll();
        }
        function cleanup() {
            testContext.resetHostPlayback();
            testContext.setPlaybackSupported(false);
            monitorLoader.item.visible = true;
            home.visible = false;
            callHistory.clearAll();
        }

        function test_marker_data() {
            return [{tag: "monitor", which: "monitor"}, {tag: "home", which: "home"}];
        }
        function test_marker(data) {
            monitorLoader.item.visible = data.which === "monitor";
            home.visible = data.which === "home";
            var uids = [];
            for (var i = 0; i < 3; i++)
                uids.push(callHistory.pushWithRecordings([{id: "m" + i, path: "/rec/m-" + i + ".wav", fileName: "m-" + i + ".wav", folderUid: "u1",
                    start: 1700000000 + i, durationMs: 12000, rate: 8000, bytes: 96000, failed: false, available: true,
                    identitySize: 192044, mtimeSec: 1700000100, mtimeNanos: 0}]));
            var list = listOf(data.which);
            verify(list !== null && list.visible, data.which + " list shown");
            tryVerify(function () { return rowOf(list, uids[1]) !== null; }, 2000, "the row is listed");
            var row = rowOf(list, uids[1]);
            var glyph = findChild(row, "rowRecordingGlyph");
            compare(row.playback, 0);
            verify(!glyph.bars);

            // A sheet's own single play marks nothing.
            decoderHost.playRecordingOwned("/rec/m-1.wav");
            compare(row.playback, 0, "a sheet's play is not a playlist");

            decoderHost.playRecordings([{path: "/rec/m-0.wav", size: 192044, mtimeSec: 1700000100, mtimeNanos: 0, call: 0, partOrdinal: 1, partCount: 1, title: "A", key: String(uids[0])},
                                        {path: "/rec/m-1.wav", size: 192044, mtimeSec: 1700000100, mtimeNanos: 0, call: 1, partOrdinal: 1, partCount: 1, title: "B", key: String(uids[1])}], 2, "history");
            testContext.publishPlaybackReading({item: 1, playbackState: 2, playbackToken: 3});
            compare(row.playback, 1);
            verify(glyph.visible && glyph.bars);
            compare(String(glyph.color), String(Ui.Theme.magenta));
            compare(row.node.Accessible.name, row.accessibleName + ", playing");
            compare(rowOf(list, uids[0]).playback, 0, "only the call it is on");
            compare(rowOf(list, uids[2]).playback, 0);

            // Issue #909 (R49): a start asked for and not yet made reads as playing.
            testContext.publishPlaybackReading({playbackState: 5});
            compare(row.playback, 1, "starting: marked playing");
            verify(glyph.bars);
            testContext.publishPlaybackReading({playbackState: 2});
            testContext.publishPlaybackReading({playbackState: 3, playbackPausedReason: "user"});
            compare(row.playback, 2);
            compare(String(glyph.color), String(Ui.Theme.textSubdued), "dimmed while paused");
            compare(row.node.Accessible.name, row.accessibleName + ", paused");
            // Opening paused (Next while paused): the player says why it pauses, so the row reads paused.
            testContext.publishPlaybackReading({playbackState: 1, playbackToken: 4});
            compare(row.playback, 2, "opening, paused");
            testContext.publishPlaybackReading({playbackState: 1, playbackPausedReason: ""});
            compare(row.playback, 1, "opening, wanted");
            // Halted for another app's audio while the part opens: still wanted (no paused reason), but the bar and
            // the sheet say "Paused for another app's audio", and so does the row.
            testContext.publishPlaybackReading({playbackState: 1, playbackInterrupted: true});
            compare(row.playback, 2, "opening, interrupted: paused");
            compare(row.node.Accessible.name, row.accessibleName + ", paused");
            testContext.publishPlaybackReading({playbackState: 3});
            compare(row.playback, 2, "interrupted once prepared: paused");
            testContext.publishPlaybackReading({playbackState: 2, playbackInterrupted: false});
            compare(row.playback, 1, "carried on: playing");

            testContext.publishPlaybackReading({playbackState: 0, playbackEndedReason: "finished"});
            compare(row.playback, 0, "gone once it ends");
            verify(!glyph.bars);
            compare(row.node.Accessible.name, row.accessibleName + ", has a recording");
        }
    }
}
