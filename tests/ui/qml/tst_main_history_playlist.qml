// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// Issue #909, in Main: the one playlistStatus both History screens read, its events read once across the idle
// History tab and the session tools' History, the bar placed above the bottom navigation (or beside the side rail)
// and at the session tools' bottom, and B5: the session History's row sheet does not come back stale after a Stop
// took the session tools away under it. Names are made up.
Item {
    width: 420
    height: 900
    Loader {
        id: loader
        source: uiDir + "/Main.qml"
    }
    TestCase {
        name: "MainHistoryPlaylist"
        when: windowShown
        readonly property var app: loader.item
        property bool originalOnboarding: false

        function screen(name) {
            return findChild(app, name);
        }
        function bar(name) {
            return findChild(screen(name), "nowPlayingBar");
        }
        function rectIn(item, ancestor) {
            var p = item.mapToItem(ancestor, 0, 0);
            return {x: p.x, y: p.y, width: item.width, height: item.height};
        }
        function describe(r) {
            return "(" + r.x.toFixed(1) + ", " + r.y.toFixed(1) + " " + r.width.toFixed(1) + "x" + r.height.toFixed(1) + ")";
        }
        function logCalls(count) {
            var calls = [];
            for (var i = 0; i < count; i++) {
                var uid = callHistory.pushWithRecordings([{id: "r" + i, path: "/rec/main-" + i + ".wav", fileName: "main-" + i + ".wav", folderUid: "u1",
                    start: 1700000000 + i, durationMs: 12000, rate: 8000, bytes: 96000, failed: false, available: true,
                    identitySize: 192044, mtimeSec: 1700000100, mtimeNanos: 0}]);
                calls.push({uid: uid, name: callHistory.recordFor(uid).name});
            }
            return calls;
        }
        function playAllFrom(name, calls) {
            testContext.setHistoryPlaylistAnswer("accepted", calls.map(function (call, i) {
                return {path: "/rec/main-" + i + ".wav", size: 192044, mtimeSec: 1700000100, mtimeNanos: 0, call: i, partOrdinal: 1,
                    partCount: 1, title: call.name, line: "", key: String(call.uid)};
            }));
            var button = findChild(screen(name), "historyPlayAll");
            verify(button.visible && button.enabled, name + " offers Play all");
            button.activate();
            return decoderHost.playbackGeneration;
        }

        function init() {
            verify(app !== null);
            originalOnboarding = prefs.onboardingDone;
            prefs.onboardingDone = true;
            testContext.setPlaybackSupported(true);
            testContext.resetHostPlayback();
            // Idle: the History tab shows (the readings host starts with a session).
            testContext.setHostReading("sessionActive", false);
            app.currentTab = 0;
            app.sessionDestination = "";
            app.requestActivate();
            tryCompare(app, "active", true);
            callHistory.clearAll();
            historyView.filterText = "";
            for (var name of ["idleHistoryScreen", "sessionHistoryScreen"]) {
                bar(name).announcements = 0;
                bar(name).announced = "";
            }
        }
        function cleanup() {
            testContext.emitApplicationState(Qt.ApplicationActive);
            app.sessionDestination = "";
            app.currentTab = 0;
            app.width = 420;
            for (var name of ["idleHistoryScreen", "sessionHistoryScreen"])
                findChild(screen(name), "historyDetailSheet").visible = false;
            testContext.resetHostPlayback();
            testContext.setPlaybackSupported(false);
            testContext.setHostReading("sessionActive", true);
            callHistory.clearAll();
            prefs.onboardingDone = originalOnboarding;
        }

        // One app-level playlist status, which both History screens show.
        function test_one_status_for_both_screens() {
            var status = findChild(app, "playlistStatus");
            verify(status !== null);
            compare(screen("idleHistoryScreen").playlistStatus, status);
            compare(screen("sessionHistoryScreen").playlistStatus, status);
        }

        // A start nobody heard (the app went away first), a finish while the app is away, then the return through
        // the other History screen: exactly the finish is read, by that screen's bar, and no start.
        function test_one_announcement_across_the_two_screens() {
            var calls = logCalls(3);
            app.currentTab = 1;
            var generation = playAllFrom("idleHistoryScreen", calls);
            verify(generation > 0);
            verify(bar("idleHistoryScreen").visible);
            testContext.emitApplicationState(Qt.ApplicationSuspended);
            testContext.publishPlaybackReading({item: 0, playbackState: 2, playbackToken: 4, playbackDurationMs: 12000});
            testContext.publishPlaybackReading({item: 2, playbackState: 0, playbackEndedReason: "finished"});
            app.sessionDestination = "history";
            verify(!screen("idleHistoryScreen").visible && screen("sessionHistoryScreen").visible);
            wait(20);
            compare(bar("idleHistoryScreen").announcements + bar("sessionHistoryScreen").announcements, 0, "nothing while away");
            testContext.emitApplicationState(Qt.ApplicationActive);
            tryCompare(bar("sessionHistoryScreen"), "announcements", 1);
            compare(bar("sessionHistoryScreen").announced, "Finished · 3 calls");
            app.sessionDestination = "";
            app.currentTab = 1;
            wait(20);
            compare(bar("idleHistoryScreen").announcements, 0, "the idle tab reads nothing");
            compare(bar("sessionHistoryScreen").announcements, 1);
        }

        // The idle History screen ends above the bottom navigation, or beside the side rail on a wide window; the
        // session tools' History at the tools' bottom. Its bar is the last thing in it, and the list ends above it.
        function test_bar_placement_data() {
            return [{tag: "bottom navigation", width: 420, session: false}, {tag: "side rail", width: 900, session: false},
                    {tag: "session", width: 420, session: true}];
        }
        function test_bar_placement(data) {
            var calls = logCalls(20);
            app.width = data.width;
            app.currentTab = 1;
            if (data.session)
                app.sessionDestination = "history";
            var name = data.session ? "sessionHistoryScreen" : "idleHistoryScreen";
            var history = screen(name);
            tryVerify(function () { return history.visible; });
            playAllFrom(name, calls);
            var barItem = bar(name);
            verify(barItem.visible);
            var nav = findChild(app, "primaryNavigation");
            var root = app.contentItem;
            verify(waitForPolish(history));
            waitForRendering(history);
            var barRect = rectIn(barItem, root), navRect = rectIn(nav, root), screenRect = rectIn(history, root);
            verify(Math.abs(barRect.y + barRect.height - (screenRect.y + screenRect.height)) < 0.5, "the bar ends the screen: " + describe(barRect) + " " + describe(screenRect));
            if (data.session) {
                var tools = findChild(app, "sessionTools");
                var toolsRect = rectIn(tools, root);
                verify(Math.abs(barRect.y + barRect.height - (toolsRect.y + toolsRect.height)) < 0.5, "at the session tools' bottom");
            } else if (data.width >= 840) {
                verify(app.expanded);
                verify(barRect.x >= navRect.x + navRect.width - 0.5, "beside the side rail: " + describe(barRect) + " " + describe(navRect));
            } else {
                verify(!app.expanded);
                verify(barRect.y + barRect.height <= navRect.y + 0.5, "above the bottom navigation: " + describe(barRect) + " " + describe(navRect));
            }
            var list = findChild(history, "callLogList");
            list.positionViewAtEnd();
            waitForRendering(list);
            var last = rectIn(list.itemAtIndex(list.count - 1), root);
            verify(last.y + last.height <= barRect.y + 0.5, "the last row above the bar: " + describe(last) + " " + describe(barRect));
        }

        // B5: the session tools' History with a row's sheet open; the session stops under it (the tools close and
        // the idle History tab shows); a new session's History opens again: no sheet from before comes back. The
        // map's "Show on map" still returns to the row's sheet (tst_main_map).
        function test_session_history_sheet_does_not_come_back_stale() {
            testContext.useLifecycleHost(true);
            try {
                testContext.setLifecycleFailure("");
                testContext.setLifecyclePhase(2);
                verify(app.monitorMode);
                callHistory.push("TODAY");
                var uid = callHistory.newestUid();
                app.sessionDestination = "history";
                var session = screen("sessionHistoryScreen");
                var sheet = findChild(session, "historyDetailSheet");
                sheet.open(callHistory.recordFor(uid));
                verify(sheet.visible);
                testContext.setLifecyclePhase(0);
                tryCompare(app, "sessionDestination", "");
                compare(app.currentTab, 1, "the Stop leaves History on its tab");
                verify(!session.visible);
                callHistory.push("TODAY");
                testContext.setLifecyclePhase(2);
                verify(app.monitorMode);
                app.sessionDestination = "history";
                verify(session.visible);
                wait(20);
                verify(!sheet.visible, "no sheet of a call from before over the new session's History");
                compare(Ui.Navigation.modals.indexOf(sheet), -1);
            } finally {
                testContext.setLifecyclePhase(0);
                testContext.useLifecycleHost(false);
            }
        }
    }
}
