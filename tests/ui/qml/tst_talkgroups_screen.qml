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
        // Issue #903: the saved system a remembered case views, on the shared saved systems (removed in cleanup()).
        property int ownerRow: -1

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
            testContext.setMetric("uiMessageSeq", 0);
            screenLoader.item.bulkMessage = "";
            screenLoader.item.bulkNewer = false;
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
            screenLoader.item.canSaveList = false;
            findChild(screenLoader.item, "talkgroupEditSheet").visible = false;
            findChild(screenLoader.item, "clearRememberedConfirm").visible = false;
            testContext.resetRememberedTalkgroups();
            testContext.setTalkgroupScanScope(false);
            if (tc.ownerRow >= 0) {
                savedSystems.remove(tc.ownerRow);
                tc.ownerRow = -1;
            }
            tc.search.text = "";
            talkgroupView.filterText = "";
            talkgroupView.filterTag = "";
            callHistory.clearAll();
            verify(testContext.clearTalkgroups());
            testContext.setGroupFileConfigured(false);
            testContext.setHostRunning(false);
            testContext.resetCommands();
            testContext.setMetric("uiMessage", "");
            testContext.setMetric("uiMessageSeq", 0);
            screenLoader.item.bulkMessage = "";
            screenLoader.item.bulkNewer = false;
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

        // Issue #903 (B4): on a scan row with its own list, edits stay in the session, but learned names may still be
        // written to that target's list; and the decoder refuses an export there, so Save is not offered.
        function test_scan_scope_footer_and_save_refusal() {
            var screen = screenLoader.item;
            var sheet = findChild(screen, "talkgroupEditSheet");
            var note = findChild(screen, "talkgroupScopeNote");
            verify(sheet !== null && note !== null);
            screen.canSaveList = true;
            verify(!screen.scanRowRefusesSave);
            verify(sheet.canSaveList);
            testContext.setTalkgroupScanScope(true);
            tryCompare(talkgroups, "persistenceScope", "scan");
            compare(note.text, "Edits last for this session; learned names may still be added to this target's list");
            verify(screen.scanRowRefusesSave);
            verify(!sheet.canSaveList);
            testContext.setTalkgroupScanScope(false);
            tryCompare(talkgroups, "persistenceScope", "session");
            verify(!screen.scanRowRefusesSave);
            verify(sheet.canSaveList);
            screen.canSaveList = false;
            verify(!sheet.canSaveList);
        }

        // The sheet's request reaches the screen's owner with the version the sheet captured.
        function test_save_request_forwards_the_sheet_context() {
            var screen = screenLoader.item;
            var sheet = findChild(screen, "talkgroupEditSheet");
            var received = [];
            var forward = function (context, generation) {
                received = [context, generation];
            };
            screen.saveListRequested.connect(forward);
            sheet.saveListRequested("123", 7);
            screen.saveListRequested.disconnect(forward);
            compare(received, ["123", 7]);
        }

        // Issue #903: a saved system named @p name that remembers what it hears, on air; its uid.
        function viewRememberingOwner(name) {
            verify(savedSystems.add({name: name, sourceType: "rtltcp", freqMhz: "851.5"}));
            tc.ownerRow = savedSystems.count - 1;
            var uid = savedSystems.get(tc.ownerRow).uid;
            verify(uid.length > 0);
            testContext.setTalkgroupRemembering(uid, true);
            testContext.setTalkgroupOwnerOnAir(uid);
            tryCompare(talkgroups, "viewedOwner", uid);
            verify(talkgroups.remembering);
            return uid;
        }
        // A tap on @p control once the screen has laid out (a control just shown moves at the next polish).
        function tap(control) {
            waitForRendering(screenLoader.item);
            mouseClick(control, control.width / 2, control.height / 2);
        }
        // The one card the search for @p id leaves.
        function cardFor(id) {
            tc.search.text = String(id);
            tryCompare(tc.grid, "count", 1);
            var card = null;
            tryVerify(function () {
                card = tc.grid.itemAtIndex(0);
                return card !== null && card.idText === String(id);
            }, 5000, "the card for " + id + " is missing");
            return card;
        }

        // Issue #903: a remembered card says when it was last heard, in its second line and in what TalkBack reads
        // with its switch (the talkgroup, then the lines in the order they are drawn); a card heard only in this
        // session keeps today's line.
        function test_remembered_card_says_last_heard() {
            var uid = viewRememberingOwner("Test North");
            testContext.rememberTalkgroup(uid, 1, 4242, 300);
            var card = cardFor(4242);
            verify(card.remembered && !card.listed);
            compare(findChild(card, "talkgroupCardTags").text, "Last heard 5 min ago");
            var toggle = findChild(card, "talkgroupListeningSwitch");
            compare(toggle.accessibleName, "Listen to talkgroup 4242, last heard 5 min ago");
            compare(toggle.Accessible.description, "", "the last heard is said once, in the name");
            testContext.rememberTalkgroup(uid, 1, 4242, 0);
            tryCompare(findChild(card, "talkgroupCardTags"), "text", "Last heard just now");
            compare(toggle.accessibleName, "Listen to talkgroup 4242, last heard just now");

            // A call this session heard on no saved system: not remembered, today's line (the owner's change is the
            // refresh that reads it).
            callHistory.pushWithIds(5151, 77);
            testContext.setTalkgroupOwnerOnAir("");
            tryCompare(talkgroups, "viewedOwner", "");
            card = cardFor(5151);
            verify(!card.remembered);
            compare(findChild(card, "talkgroupCardTags").text, "Heard in this session");
            compare(findChild(card, "talkgroupListeningSwitch").Accessible.description, "Heard in this session");
        }

        // Issue #903: the edit sheet offers Forget on a remembered card only, and forgets the number for the system the
        // sheet opened on, even when another is on air by the time it is tapped.
        function test_forget_this_talkgroup() {
            var uid = viewRememberingOwner("Test North");
            testContext.rememberTalkgroup(uid, 1, 4242, 300);
            var sheet = findChild(screenLoader.item, "talkgroupEditSheet");
            var forget = findChild(sheet, "forgetTalkgroup");
            verify(forget !== null, "the sheet has a Forget action");

            var listed = cardFor(1001);
            var edit = findChild(listed, "talkgroupEditButton");
            tap(edit);
            tryCompare(sheet, "visible", true);
            verify(!forget.visible, "a listed card is not remembered: nothing to forget");
            sheet.visible = false;

            var card = cardFor(4242);
            edit = findChild(card, "talkgroupEditButton");
            tap(edit);
            tryCompare(sheet, "visible", true);
            verify(forget.visible);
            compare(forget.text, "Forget this talkgroup");
            verify(forget.height >= 48);
            testContext.setTalkgroupOwnerOnAir("");
            tryCompare(talkgroups, "viewedOwner", "");
            tap(forget);
            compare(forget.text, "Tap again to forget");
            compare(testContext.rememberedTalkgroupCount(uid), 1);
            tap(forget);
            compare(testContext.rememberedTalkgroupCount(uid), 0);
            verify(!sheet.visible);
        }

        // Issue #903 (PR2 review r2, P2): a Forget History refuses (it cannot save its log) leaves the sheet open with the
        // card still remembered, and says why; once History saves, tapping again forgets it.
        function test_refused_forget_stays_in_the_sheet() {
            var uid = viewRememberingOwner("Test North");
            testContext.rememberTalkgroup(uid, 1, 4242, 300);
            var sheet = findChild(screenLoader.item, "talkgroupEditSheet");
            var forget = findChild(sheet, "forgetTalkgroup");
            var error = findChild(sheet, "forgetTalkgroupError");
            var card = cardFor(4242);
            tap(findChild(card, "talkgroupEditButton"));
            tryCompare(sheet, "visible", true);
            verify(!error.visible);
            testContext.setRememberedEditsRefused(true);
            tap(forget);
            tap(forget);
            compare(testContext.rememberedTalkgroupCount(uid), 1, "nothing was forgotten");
            verify(sheet.visible, "the sheet stays open");
            verify(error.visible);
            compare(error.text, "Call history could not be saved, so this talkgroup is still remembered.");
            testContext.setRememberedEditsRefused(false);
            tap(forget);
            tap(forget);
            compare(testContext.rememberedTalkgroupCount(uid), 0);
            verify(!sheet.visible);
        }

        // Issue #903 (PR2 review r2, P2): a Clear History refuses keeps its confirmation open, with the list still
        // remembered, and says why there, read out; tapping again once History saves clears it, and asking again starts
        // without the refusal.
        function test_refused_clear_stays_in_the_confirmation() {
            var screen = screenLoader.item;
            var clear = findChild(screen, "clearRememberedTalkgroups");
            var confirm = findChild(screen, "clearRememberedConfirm");
            var error = findChild(confirm, "clearRememberedError");
            var yes = findChild(confirm, "confirmClearRemembered");
            verify(error !== null);
            var uid = viewRememberingOwner("Test North");
            testContext.rememberTalkgroup(uid, 1, 4242, 300);
            tryCompare(clear, "visible", true);
            tap(clear);
            tryCompare(confirm, "visible", true);
            verify(!error.visible);
            testContext.setRememberedEditsRefused(true);
            var heard = confirm.announcements;
            tap(yes);
            compare(testContext.rememberedTalkgroupCount(uid), 1, "nothing was cleared");
            verify(confirm.visible, "the confirmation stays open");
            verify(error.visible);
            compare(error.text, "Call history could not be saved, so these talkgroups are still remembered.");
            compare(error.Accessible.role, Accessible.AlertMessage);
            compare(confirm.announcements, heard + 1, "TalkBack hears the refusal");
            compare(confirm.announced, error.text);
            tap(findChild(confirm, "cancelClearRemembered"));
            verify(!confirm.visible);
            tap(clear);
            tryCompare(confirm, "visible", true);
            verify(!error.visible, "asking again starts without the refusal");
            testContext.setRememberedEditsRefused(false);
            tap(yes);
            verify(!confirm.visible);
            compare(testContext.rememberedTalkgroupCount(uid), 0);
        }

        // Issue #903: Clear is offered while the saved system on air has something remembered (remembering or not), asks
        // first naming that system, and clears that system's list.
        function test_clear_remembered_talkgroups() {
            var screen = screenLoader.item;
            var clear = findChild(screen, "clearRememberedTalkgroups");
            var confirm = findChild(screen, "clearRememberedConfirm");
            verify(clear !== null && confirm !== null);
            verify(!screenReader.active, "this run has no screen reader");
            verify(!clear.visible, "no saved system on air: nothing to clear");
            var uid = viewRememberingOwner("Test North");
            verify(!talkgroups.hasRemembered);
            verify(!clear.visible, "a system that remembers nothing yet: nothing to clear");
            testContext.rememberTalkgroup(uid, 1, 4242, 300);
            testContext.rememberTalkgroup(uid, 1, 4343, 600);
            tryCompare(clear, "visible", true);
            compare(clear.text, "Clear remembered talkgroups…");
            verify(clear.height >= 48);
            testContext.setTalkgroupRemembering(uid, false);
            verify(!talkgroups.remembering);
            verify(clear.visible, "Clear stays while remembering is off");

            tap(clear);
            tryCompare(confirm, "visible", true);
            compare(findChild(confirm, "clearRememberedQuestion").text, "Forget the talkgroups remembered for Test North?");
            compare(findChild(confirm, "confirmClearRemembered").text, "Forget remembered talkgroups");
            var cancel = findChild(confirm, "cancelClearRemembered");
            tap(cancel);
            verify(!confirm.visible);
            compare(testContext.rememberedTalkgroupCount(uid), 2);

            tap(clear);
            tryCompare(confirm, "visible", true);
            // The system the question named is the one cleared.
            testContext.setTalkgroupOwnerOnAir("");
            var yes = findChild(confirm, "confirmClearRemembered");
            tap(yes);
            verify(!confirm.visible);
            compare(testContext.rememberedTalkgroupCount(uid), 0);
            tryVerify(function () {
                return !clear.visible;
            });
            // On air again with its list cleared: still nothing to clear.
            testContext.setTalkgroupOwnerOnAir(uid);
            tryCompare(talkgroups, "viewedOwner", uid);
            verify(!talkgroups.hasRemembered && !clear.visible);
        }

        // Issue #903: the footer offers Save talkgroup list while the list lasts for this session only, never on a scan
        // row the decoder refuses to export, and it sends the list on show.
        function test_footer_save_talkgroup_list() {
            var screen = screenLoader.item;
            var save = findChild(screen, "footerSaveTalkgroupList");
            verify(save !== null);
            verify(!save.visible, "not offered until the owner offers it");
            screen.canSaveList = true;
            tryCompare(save, "visible", true);
            compare(save.text, "Save talkgroup list");
            verify(save.height >= 48);
            var received = [];
            var forward = function (context, generation) {
                received = [context, generation];
            };
            screen.saveListRequested.connect(forward);
            tap(save);
            screen.saveListRequested.disconnect(forward);
            compare(received, [talkgroups.policyContext, talkgroups.policyGeneration]);
            testContext.setTalkgroupScanScope(true);
            tryCompare(save, "visible", false);
            testContext.setTalkgroupScanScope(false);
            tryCompare(save, "visible", true);
            testContext.setHostRunning(false);
            tryCompare(save, "visible", false);
            testContext.setHostRunning(true);
            screen.canSaveList = false;
            tryCompare(save, "visible", false);
        }

        // Issue #903: with a system remembering what it hears, the session note says so, alone (the scope note gives way
        // to it); otherwise both keep their text. A scan row keeps its own scope note.
        function test_session_note_says_remembered() {
            var note = findChild(screenLoader.item, "sessionOnlyNote");
            var scope = findChild(screenLoader.item, "talkgroupScopeNote");
            compare(note.text, "Changes last for this session. Give the system a talkgroup list to keep them.");
            verify(scope.visible);
            var uid = viewRememberingOwner("Test North");
            tryCompare(note, "text", "Talkgroups heard on this system are remembered. Save a talkgroup list to keep names and listen settings.");
            verify(note.visible);
            tryCompare(scope, "visible", false);
            testContext.setTalkgroupScanScope(true);
            tryCompare(scope, "visible", true);
            testContext.setTalkgroupScanScope(false);
            tryCompare(scope, "visible", false);
            testContext.setTalkgroupRemembering(uid, false);
            tryCompare(note, "text", "Changes last for this session. Give the system a talkgroup list to keep them.");
            tryCompare(scope, "visible", true);
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

        // B3 (r2 E2): the one command line kept the look of the line it replaced, the app's 13 px text in the primary
        // colour, so it grows with the text size: at text 2.0 a bulk status or an engine answer is drawn at 26 px.
        function test_command_line_follows_the_text_size() {
            var toast = findChild(screenLoader.item, "talkgroupCommandToast");
            Ui.Theme.fontScale = 2;
            testContext.setMetric("uiMessage", "Applied: 2 talkgroups listened to");
            tryCompare(toast, "visible", true);
            compare(toast.font.pixelSize, Ui.Theme.fontSize(13), "an engine answer at the theme's 13 px");
            compare(toast.font.pixelSize, 26);
            verify(Qt.colorEqual(toast.color, Ui.Theme.textPrimary), "in the primary text colour");
            testContext.setMetric("uiMessage", "");
            testContext.setTalkgroupSelectionAccepted(false);
            applyBulk();
            compare(toast.text, "The update could not be queued. Try again.");
            compare(toast.font.pixelSize, 26, "a refusal at the same size");
        }

        // B3: the engine can answer with exactly the text still shown from its last answer (the same selection applied
        // twice within the answer's life). Its count moves, so the answer replaces "Update requested", and when it
        // expires the request does not come back.
        function test_same_answer_text_again_replaces_the_request() {
            var toast = findChild(screenLoader.item, "talkgroupCommandToast");
            var answer = "2 selected talkgroups listened to";
            testContext.setMetric("uiMessage", answer);
            testContext.setMetric("uiMessageSeq", 1);
            tryCompare(toast, "text", answer);
            applyBulk();
            compare(toast.text, "Update requested");
            testContext.setMetric("uiMessageSeq", 2);
            tryCompare(toast, "text", answer, 5000, "the same text answered again replaces the pending request");
            compare(shownCopies(answer), 1);
            compare(shownCopies("Update requested"), 0);
            testContext.setMetric("uiMessage", "");
            tryCompare(toast, "visible", false);
            compare(toast.text, "");
            compare(shownCopies("Update requested"), 0, "an expired answer does not bring back the request");
        }

        // B3: an accepted request the engine never answers (decoding stopped) leaves the line after the same 4 s an
        // engine answer lives, and the older answer it replaced does not come back in its place.
        function test_unanswered_request_expires() {
            var toast = findChild(screenLoader.item, "talkgroupCommandToast");
            testContext.setMetric("uiMessage", "Applied: 2 talkgroups listened to");
            tryCompare(toast, "text", "Applied: 2 talkgroups listened to");
            applyBulk();
            compare(toast.text, "Update requested");
            wait(3000);
            compare(toast.text, "Update requested", "still shown inside its 4 s");
            verify(toast.visible);
            tryCompare(toast, "visible", false, 3000, "the unanswered request leaves after 4 s");
            compare(toast.text, "");
            compare(shownCopies("Applied: 2 talkgroups listened to"), 0, "the older answer does not come back");
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
