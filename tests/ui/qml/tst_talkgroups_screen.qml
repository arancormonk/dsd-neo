// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

Item {
    id: root

    width: 420
    height: 900

    Loader {
        id: screenLoader

        anchors.fill: parent
        source: uiDir + "/TalkgroupsScreen.qml"
        onLoaded: item.systemName = "County Public Safety"
    }

    TestCase {
        id: tc

        name: "TalkgroupsScreen"
        when: windowShown

        property var grid: null
        property var chips: null
        property var search: null

        function initTestCase() {
            verify(screenLoader.item !== null, "TalkgroupsScreen.qml failed to load");
            tc.grid = findChild(screenLoader.item, "talkgroupGrid");
            tc.chips = findChild(screenLoader.item, "talkgroupChips");
            tc.search = findChild(screenLoader.item, "talkgroupSearch");
            verify(tc.grid !== null && tc.chips !== null && tc.search !== null);
        }

        function init() {
            callHistory.clearAll();
            talkgroups.historySession = 0;
            talkgroupView.filterTag = "";
            tc.search.text = "";
            talkgroupView.filterText = "";
            verify(testContext.clearTalkgroups());
            testContext.setGroupFileConfigured(false);
            testContext.setHostRunning(true);
            testContext.resetCommands();
            testContext.setMetric("uiMessage", "");
            screenLoader.item.bulkMessage = "";
            verify(testContext.pushTalkgroup(1001, "A", "Fire Dispatch", "FIRE"));
            verify(testContext.pushTalkgroup(1002, "B", "Fire Tactical", "FIRE"));
            verify(testContext.pushTalkgroup(2001, "A", "EMS Dispatch", "EMS"));
            tryCompare(tc.grid, "count", 3);
            tc.grid.positionViewAtBeginning();
            tc.waitForRendering(tc.grid);
            tryVerify(function () {
                return tc.grid.itemAtIndex(2) !== null;
            });
        }

        function cleanup() {
            Ui.Theme.resetFontScale();
            tc.search.text = "";
            talkgroupView.filterText = "";
            talkgroupView.filterTag = "";
            callHistory.clearAll();
            verify(testContext.clearTalkgroups());
            testContext.setGroupFileConfigured(false);
            testContext.setHostRunning(false);
            testContext.resetCommands();
            testContext.setMetric("uiMessage", "");
            screenLoader.item.bulkMessage = "";
        }

        function test_long_names_fit_cards_data() {
            return [
                {
                    tag: "normal",
                    scale: 1
                },
                {
                    tag: "large",
                    scale: 1.6
                }
            ];
        }
        function test_long_names_fit_cards(data) {
            Ui.Theme.fontScale = data.scale;
            verify(testContext.pushTalkgroup(3001, "A", "Regional Dispatch Operations", "DISPATCH"));
            tc.search.text = "Regional";
            tryCompare(tc.grid, "count", 1);
            tryVerify(function () {
                return tc.grid.itemAtIndex(0) !== null;
            });
            var card = tc.grid.itemAtIndex(0);
            var content = findChild(card, "talkgroupCardContent");
            tryVerify(function () {
                return content.implicitHeight <= content.height + 1;
            }, 5000, "two-line name must leave the Listening status inside the card");
        }

        function selectFire() {
            var fire = null;
            tryVerify(function () {
                for (var i = 0; i < tc.chips.count; ++i) {
                    var chip = tc.chips.itemAtIndex(i);
                    if (chip !== null && chip.pill.text === "FIRE") {
                        fire = chip;
                        return true;
                    }
                }
                return false;
            }, 5000, "the FIRE category chip is missing");
            mouseClick(fire, fire.width / 2, fire.height / 2);
            tryCompare(talkgroupView, "filterTag", "FIRE");
        }

        function test_long_press_opens_captured_policy_without_toggling() {
            var card = tc.grid.itemAtIndex(0);
            var sheet = findChild(screenLoader.item, "talkgroupEditSheet");
            verify(card !== null && sheet !== null);
            mousePress(card, card.width / 2, card.height / 2);
            wait(1000);
            mouseRelease(card, card.width / 2, card.height / 2);
            tryCompare(sheet, "visible", true);
            compare(sheet.idStart, 1001);
            compare(sheet.policyContext, talkgroups.policyContext);
            compare(sheet.policyGeneration, talkgroups.policyGeneration);
            compare(testContext.talkgroupListenCalls(), 0);
            sheet.visible = false;
        }

        function test_rows_render_and_blocked_card_requests_listening() {
            var dispatch = tc.grid.itemAtIndex(0);
            var tactical = tc.grid.itemAtIndex(1);
            var ems = tc.grid.itemAtIndex(2);
            verify(dispatch !== null && tactical !== null && ems !== null);
            compare(dispatch.idText, "1001");
            compare(dispatch.name, "Fire Dispatch");
            compare(dispatch.tags, "FIRE");
            verify(dispatch.listening && dispatch.listed);
            compare(tactical.idText, "1002");
            compare(tactical.name, "Fire Tactical");
            verify(!tactical.listening);
            compare(ems.idText, "2001");
            compare(ems.name, "EMS Dispatch");
            compare(ems.tags, "EMS");
            verify(ems.listening);

            var toggle = findChild(tactical, "talkgroupListeningSwitch");
            verify(toggle !== null);
            mouseClick(toggle, toggle.width / 2, toggle.height / 2);
            compare(testContext.talkgroupListenCalls(), 1);
            compare(testContext.lastTalkgroupListenIdStart(), 1002);
            compare(testContext.lastTalkgroupListenIdEnd(), 1002);
            compare(testContext.lastTalkgroupListenOn(), true);
            // The decoder has not published an updated snapshot: do not pretend
            // the queued request has already changed the effective policy.
            compare(tactical.listening, false);
        }

        function test_bulk_selection_intersects_search_and_category() {
            selectFire();
            tryCompare(tc.grid, "count", 2);
            tryVerify(function () {
                var first = tc.grid.itemAtIndex(0);
                var second = tc.grid.itemAtIndex(1);
                return first !== null && second !== null && first.idText === "1001" && second.idText === "1002" && first.tags === "FIRE" && second.tags === "FIRE";
            });

            tc.search.text = "Tactical";
            tryCompare(tc.grid, "count", 1);
            tryVerify(function () {
                var card = tc.grid.itemAtIndex(0);
                return card !== null && card.idText === "1002";
            });
            var listen = findChild(screenLoader.item, "listenAllButton");
            verify(listen !== null && listen.enabled);
            mouseClick(listen, listen.width / 2, listen.height / 2);
            compare(testContext.allTalkgroupsListenCalls(), 0);
            var confirm = findChild(screenLoader.item, "confirmTalkgroupBulk");
            verify(confirm !== null && confirm.visible);
            mouseClick(confirm, confirm.width / 2, confirm.height / 2);
            compare(testContext.allTalkgroupsListenCalls(), 1);
            compare(testContext.lastAllTalkgroupsListenOn(), true);
            compare(commands.selectionRows().length, 1);
            compare(commands.selectionRows()[0].first, 1002);
        }

        // A digit-pattern row is a card like a range's, with its canonical text as the ID, and its Listening toggle
        // names the row (table index and pattern) with the captured list version.
        function test_pattern_card_renders_and_toggle_submits_token() {
            verify(testContext.pushPatternTalkgroup("12*", "A", "Twelve hundreds", "FIRE"));
            tryCompare(tc.grid, "count", 4);
            tc.search.text = "12*";
            tryCompare(tc.grid, "count", 1);
            var card = null;
            tryVerify(function () {
                card = tc.grid.itemAtIndex(0);
                return card !== null && card.idText === "12*";
            }, 5000, "the pattern card is missing");
            tc.waitForRendering(tc.grid);
            compare(card.name, "Twelve hundreds");
            compare(findChild(card, "talkgroupBadge").text, "12*");
            verify(card.listening && card.listed);

            var toggle = findChild(card, "talkgroupListeningSwitch");
            verify(toggle !== null);
            mouseClick(toggle, toggle.width / 2, toggle.height / 2);
            compare(testContext.talkgroupListenCalls(), 1);
            compare(testContext.lastTalkgroupListenOn(), false);
            var token = testContext.lastTalkgroupListenToken();
            compare(token.tableIndex, 3);
            compare(token.pattern, "12*");
            compare(token.context, talkgroups.policyContext);
            compare(token.generation, talkgroups.policyGeneration);
            compare(testContext.lastTalkgroupListenIdStart(), 0);
            compare(testContext.lastTalkgroupListenIdEnd(), 0);

            // A typed number finds the pattern row that matches it.
            tc.search.text = "1234";
            tryCompare(tc.grid, "count", 1);
            tryVerify(function () {
                var only = tc.grid.itemAtIndex(0);
                return only !== null && only.idText === "12*";
            });
        }

        // B3: the visible lines under the screen that read exactly @p message.
        function shownCopies(message) {
            var found = 0;
            var walk = function (node) {
                if (!node.visible)
                    return;
                if (node.text !== undefined && node.text === message)
                    found++;
                for (var i = 0; i < node.children.length; ++i)
                    walk(node.children[i]);
            };
            walk(screenLoader.item);
            return found;
        }
        function applyBulk() {
            var listen = findChild(screenLoader.item, "listenAllButton");
            verify(listen !== null && listen.enabled);
            mouseClick(listen, listen.width / 2, listen.height / 2);
            var confirm = findChild(screenLoader.item, "confirmTalkgroupBulk");
            verify(confirm !== null && confirm.visible);
            mouseClick(confirm, confirm.width / 2, confirm.height / 2);
            tryCompare(findChild(screenLoader.item, "talkgroupBulkConfirm"), "visible", false);
        }

        // B3: the engine's answer is said once, on the command line, not on a second line above it as well.
        function test_engine_answer_shows_once() {
            var answer = "Applied: 3 talkgroups listened to";
            testContext.setMetric("uiMessage", answer);
            var toast = findChild(screenLoader.item, "talkgroupCommandToast");
            tryCompare(toast, "visible", true);
            compare(toast.text, answer);
            compare(shownCopies(answer), 1, "the engine's answer is shown once");
        }

        // B3: one line, the newest message on it. A refusal after an older engine answer shows at once; an engine
        // answer after an accepted request replaces "Update requested"; an expired answer leaves the line empty
        // rather than bringing back the older message it replaced.
        function test_command_line_shows_the_newest_message() {
            var toast = findChild(screenLoader.item, "talkgroupCommandToast");
            var refused = "The update could not be queued. Try again.";
            var requested = "Update requested";

            testContext.setMetric("uiMessage", "Applied: 2 talkgroups listened to");
            tryCompare(toast, "text", "Applied: 2 talkgroups listened to");
            testContext.setTalkgroupSelectionAccepted(false);
            applyBulk();
            compare(testContext.allTalkgroupsListenCalls(), 0, "a refused request sends nothing");
            compare(toast.text, refused, "a refusal after an older answer shows at once");
            verify(toast.visible);
            compare(shownCopies(refused), 1);
            compare(shownCopies("Applied: 2 talkgroups listened to"), 0, "the older answer is gone");
            // The older answer expiring leaves the newer refusal in place.
            testContext.setMetric("uiMessage", "");
            compare(toast.text, refused);
            verify(toast.visible);

            testContext.setTalkgroupSelectionAccepted(true);
            applyBulk();
            compare(testContext.allTalkgroupsListenCalls(), 1);
            compare(toast.text, requested);
            compare(shownCopies(requested), 1);
            var rejected = "Talkgroup list changed: stale selection rejected";
            testContext.setMetric("uiMessage", rejected);
            tryCompare(toast, "text", rejected, 5000, "the engine's later answer replaces the pending request");
            compare(shownCopies(rejected), 1);
            compare(shownCopies(requested), 0, "the request it answered is not shown beside it");

            // The answer expires: nothing older comes back in its place.
            testContext.setMetric("uiMessage", "");
            tryCompare(toast, "visible", false);
            compare(toast.text, "");
            compare(shownCopies(requested), 0, "an expired answer does not bring back the request");
            compare(shownCopies(refused), 0, "nor the refusal before it");
        }

        function test_session_note_tracks_group_file_configuration() {
            var note = findChild(screenLoader.item, "sessionOnlyNote");
            verify(note !== null, "the session-only persistence note is missing");
            tryCompare(note, "visible", true);
            testContext.setGroupFileConfigured(true);
            tryCompare(note, "visible", false);
            testContext.setGroupFileConfigured(false);
            tryCompare(note, "visible", true);
        }
    }
}
