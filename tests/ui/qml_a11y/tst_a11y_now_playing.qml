// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// Issue #909: what a screen reader reaches of History's now-playing bar: one group named Now playing, its controls
// named for what they do (Show in list described by the call, where it is and what it is doing), Play all beside Map;
// none of it while a sheet covers the screen or the screen is hidden. Names are made up.
Item {
    width: 360
    height: 640

    Ui.PlaylistStatus {
        id: status
    }
    Loader {
        id: loader
        anchors.fill: parent
        source: uiDir + "/HistoryScreen.qml"
        onLoaded: {
            item.playlistStatus = status;
            item.systems = clockStub;
        }
    }
    QtObject {
        id: clockStub
        function realtimeNowMs() { return new Date(2026, 10, 9, 12, 0, 0).getTime(); }
    }

    TestCase {
        name: "A11yNowPlaying"
        when: windowShown
        readonly property var screen: loader.item

        property int appSpeechPeriod: 0
        function initTestCase() {
            verify(testContext.accessibilityActive(), "run with DSD_QML_TEST_ACCESSIBILITY=1, or nothing is in the tree");
            appSpeechPeriod = Ui.Navigation.speechPeriodMs;
            Ui.Navigation.speechPeriodMs = 50;
            Ui.Navigation.speechStepFloorMs = 1;
        }
        function cleanupTestCase() {
            Ui.Navigation.speechPeriodMs = appSpeechPeriod;
            Ui.Navigation.speechStepFloorMs = 1000;
            testContext.resetHostPlayback();
            testContext.setPlaybackSupported(false);
        }
        function node(name) {
            var item = findChild(screen, name);
            verify(item !== null, name);
            return testContext.accessibleNode(item);
        }
        // The bar's nodes a screen reader can land on now: in the tree, with a name or a stop.
        function reachableInBar() {
            return testContext.accessibleNodesUnder(findChild(screen, "nowPlayingBar")).filter(function (n) {
                return n.inTree && (n.name.length > 0 || n.focusable);
            }).map(function (n) {
                return (n.objectName || n.file) + " \"" + n.name + "\"";
            });
        }
        function init() {
            testContext.setPlaybackSupported(true);
            testContext.resetHostPlayback();
            callHistory.clearAll();
            status.forget();
            screen.visible = true;
            var calls = [];
            for (var i = 0; i < 2; i++) {
                var uid = callHistory.pushWithRecordings([{id: "a" + i, path: "/rec/a-" + i + ".wav", fileName: "a-" + i + ".wav", folderUid: "u1",
                    start: 1700000000 + i, durationMs: 31000, rate: 8000, bytes: 96000, failed: false, available: true,
                    identitySize: 192044, mtimeSec: 1700000100, mtimeNanos: 0}]);
                calls.push({path: "/rec/a-" + i + ".wav", size: 192044, mtimeSec: 1700000100, mtimeNanos: 0, call: i, partOrdinal: 1,
                    partCount: 1, title: "Dispatch " + i, line: "", key: String(uid)});
            }
            testContext.setHistoryPlaylistAnswer("accepted", calls);
            findChild(screen, "historyPlayAll").activate();
            testContext.publishPlaybackReading({item: 0, playbackState: 2, playbackToken: 2, playbackDurationMs: 31000, playbackPositionMs: 12000, playbackCanSeek: true});
        }
        function cleanup() {
            findChild(screen, "historyDetailSheet").visible = false;
            screen.visible = true;
            testContext.resetHostPlayback();
            status.forget();
        }

        function test_names_and_reach() {
            var play = node("historyPlayAll");
            verify(play.inTree);
            compare(play.name, "Play all");
            compare(play.description, "Plays the recorded calls shown, oldest first");
            var group = node("nowPlayingBar");
            verify(group.inTree);
            compare(group.role, Accessible.Grouping);
            compare(group.name, "Now playing");
            var expected = {nowPlayingShowInList: "Show in list", nowPlayingPrevious: "Previous call", nowPlayingPlay: "Pause",
                            nowPlayingNext: "Next call", nowPlayingClose: "Stop playing"};
            for (var name in expected) {
                var n = node(name);
                verify(n.inTree && n.focusable, name + " is a stop");
                compare(n.role, Accessible.Button, name);
                compare(n.name, expected[name], name);
            }
            tryVerify(function () { return node("nowPlayingShowInList").description === "Dispatch 0, Call 1 of 2, 0:12 of 0:31"; }, 2000,
                      "described by the call, where it is: " + node("nowPlayingShowInList").description);
            testContext.publishPlaybackReading({playbackState: 3, playbackPausedReason: "user"});
            compare(node("nowPlayingPlay").name, "Resume");
            tryVerify(function () { return node("nowPlayingShowInList").description === "Dispatch 0, Call 1 of 2, Paused"; }, 2000,
                      "and what it is doing: " + node("nowPlayingShowInList").description);
            testContext.publishPlaybackReading({playbackState: 0, playbackEndedReason: "finished"});
            compare(node("nowPlayingClose").name, "Close");
            tryVerify(function () { return node("nowPlayingShowInList").description === "Dispatch 0, Finished · 2 calls"; }, 2000,
                      "ended: how: " + node("nowPlayingShowInList").description);
        }

        // Covered by a sheet or hidden, nothing of the bar is reachable; back again, it is.
        function test_gated() {
            verify(reachableInBar().length > 0);
            var sheet = findChild(screen, "historyDetailSheet");
            sheet.open(callHistory.recordFor(callHistory.newestUid()));
            var leaked = reachableInBar();
            compare(leaked.length, 0, "under the sheet: " + JSON.stringify(leaked));
            sheet.requestDismiss();
            verify(reachableInBar().length > 0);
            screen.visible = false;
            leaked = reachableInBar();
            compare(leaked.length, 0, "hidden: " + JSON.stringify(leaked));
        }
    }
}
