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
            verify(screenLoader.item !== null, "TalkgroupsScreen.qml failed to load")
            tc.grid = findChild(screenLoader.item, "talkgroupGrid")
            tc.chips = findChild(screenLoader.item, "talkgroupChips")
            tc.search = findChild(screenLoader.item, "talkgroupSearch")
            verify(tc.grid !== null && tc.chips !== null && tc.search !== null)
        }

        function init() {
            callHistory.clearAll()
            talkgroups.sinceWhen = 0
            talkgroupView.filterTag = ""
            tc.search.text = ""
            talkgroupView.filterText = ""
            verify(testContext.clearTalkgroups())
            testContext.setGroupFileConfigured(false)
            testContext.setHostRunning(true)
            testContext.resetCommands()
            verify(testContext.pushTalkgroup(1001, "A", "Fire Dispatch", "FIRE"))
            verify(testContext.pushTalkgroup(1002, "B", "Fire Tactical", "FIRE"))
            verify(testContext.pushTalkgroup(2001, "A", "EMS Dispatch", "EMS"))
            tryCompare(tc.grid, "count", 3)
            tc.grid.positionViewAtBeginning()
            tc.waitForRendering(tc.grid)
            tryVerify(function () { return tc.grid.itemAtIndex(2) !== null })
        }

        function cleanup() {
            Ui.Theme.resetFontScale();
            tc.search.text = ""
            talkgroupView.filterText = ""
            talkgroupView.filterTag = ""
            callHistory.clearAll()
            verify(testContext.clearTalkgroups())
            testContext.setGroupFileConfigured(false)
            testContext.setHostRunning(false)
            testContext.resetCommands()
        }

        function test_long_names_fit_cards_data() {
            return [{tag: "normal", scale: 1}, {tag: "large", scale: 1.6}];
        }
        function test_long_names_fit_cards(data) {
            Ui.Theme.fontScale = data.scale;
            verify(testContext.pushTalkgroup(3001, "A", "Regional Dispatch Operations", "DISPATCH"));
            tc.search.text = "Regional";
            tryCompare(tc.grid, "count", 1);
            tryVerify(function() { return tc.grid.itemAtIndex(0) !== null; });
            var card = tc.grid.itemAtIndex(0);
            var content = findChild(card, "talkgroupCardContent");
            tryVerify(function() { return content.implicitHeight <= content.height + 1; }, 5000,
                      "two-line name must leave the Listening status inside the card");
        }

        function selectFire() {
            var fire = null
            tryVerify(function () {
                for (var i = 0; i < tc.chips.count; ++i) {
                    var chip = tc.chips.itemAtIndex(i)
                    if (chip !== null && chip.text === "FIRE") {
                        fire = chip
                        return true
                    }
                }
                return false
            }, 5000, "the FIRE category chip is missing")
            mouseClick(fire, fire.width / 2, fire.height / 2)
            tryCompare(talkgroupView, "filterTag", "FIRE")
        }

        function test_long_press_opens_captured_policy_without_toggling() {
            var card = tc.grid.itemAtIndex(0)
            var sheet = findChild(screenLoader.item, "talkgroupEditSheet")
            verify(card !== null && sheet !== null)
            mousePress(card, card.width / 2, card.height / 2)
            wait(1000)
            mouseRelease(card, card.width / 2, card.height / 2)
            tryCompare(sheet, "visible", true)
            compare(sheet.idStart, 1001)
            compare(sheet.policyContext, talkgroups.policyContext)
            compare(sheet.policyGeneration, talkgroups.policyGeneration)
            compare(testContext.talkgroupListenCalls(), 0)
            sheet.visible = false
        }

        function test_rows_render_and_blocked_card_requests_listening() {
            var dispatch = tc.grid.itemAtIndex(0)
            var tactical = tc.grid.itemAtIndex(1)
            var ems = tc.grid.itemAtIndex(2)
            verify(dispatch !== null && tactical !== null && ems !== null)
            compare(dispatch.idText, "1001")
            compare(dispatch.name, "Fire Dispatch")
            compare(dispatch.tags, "FIRE")
            verify(dispatch.listening && dispatch.listed)
            compare(tactical.idText, "1002")
            compare(tactical.name, "Fire Tactical")
            verify(!tactical.listening)
            compare(ems.idText, "2001")
            compare(ems.name, "EMS Dispatch")
            compare(ems.tags, "EMS")
            verify(ems.listening)

            mouseClick(tactical, tactical.width / 2, tactical.height / 2)
            compare(testContext.talkgroupListenCalls(), 1)
            compare(testContext.lastTalkgroupListenIdStart(), 1002)
            compare(testContext.lastTalkgroupListenIdEnd(), 1002)
            compare(testContext.lastTalkgroupListenOn(), true)
            // The decoder has not published an updated snapshot: do not pretend
            // the queued request has already changed the effective policy.
            compare(tactical.listening, false)
        }

        function test_category_filter_and_bulk_follow_tag_not_search() {
            selectFire()
            tryCompare(tc.grid, "count", 2)
            tryVerify(function () {
                var first = tc.grid.itemAtIndex(0)
                var second = tc.grid.itemAtIndex(1)
                return first !== null && second !== null
                        && first.idText === "1001" && second.idText === "1002"
                        && first.tags === "FIRE" && second.tags === "FIRE"
            })

            tc.search.text = "Tactical"
            tryCompare(tc.grid, "count", 1)
            tryVerify(function () {
                var card = tc.grid.itemAtIndex(0)
                return card !== null && card.idText === "1002"
            })
            var listen = findChild(screenLoader.item, "listenAllButton")
            verify(listen !== null && listen.enabled)
            mouseClick(listen, listen.width / 2, listen.height / 2)
            compare(testContext.allTalkgroupsListenCalls(), 1)
            compare(testContext.lastAllTalkgroupsListenOn(), true)
            compare(testContext.lastAllTalkgroupsTag(), "FIRE")
        }

        function test_session_note_tracks_group_file_configuration() {
            var note = findChild(screenLoader.item, "sessionOnlyNote")
            verify(note !== null, "the session-only persistence note is missing")
            tryCompare(note, "visible", true)
            testContext.setGroupFileConfigured(true)
            tryCompare(note, "visible", false)
            testContext.setGroupFileConfigured(false)
            tryCompare(note, "visible", true)
        }
    }
}
