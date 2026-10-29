// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest

// Issue #782: the Talkgroups screen while nobody looks at it.
//
// A trunk scan brings each target's own talkgroup table, so every hop between targets with different tables resets
// the list and changes its categories. The screen stays loaded under the monitor while hidden; its cards and
// category chips must not be rebuilt for those tables, since each delegate created under a hidden parent raises an
// accessibility event that costs Android's main thread eight blocking calls into the Qt thread. Shown again, the
// screen lists the table as it is then, under the filters set before it was hidden.
Item {
    width: 420
    height: 900
    Loader {
        id: screenLoader
        anchors.fill: parent
        source: uiDir + "/TalkgroupsScreen.qml"
    }
    // A delegate is parented into its list's contentItem as it is created, so these count every card and chip
    // built, including one built and then dropped again.
    SignalSpy {
        id: cardBuildSpy
        signalName: "childrenChanged"
    }
    SignalSpy {
        id: chipBuildSpy
        signalName: "childrenChanged"
    }
    TestCase {
        name: "A11yTalkgroups"
        when: windowShown
        readonly property var screen: screenLoader.item
        readonly property int switches: 10
        // The prepared tables (testContext.applyTalkgroupTable): target 0 is table A, target 1 table B.
        readonly property var tableB: [
            {id: 3001, name: "Police Dispatch", tags: "POLICE", listening: true},
            {id: 3002, name: "Police Tactical", tags: "POLICE", listening: false},
            {id: 4001, name: "Public Works", tags: "WORKS", listening: true},
            {id: 4002, name: "Water Department", tags: "WORKS", listening: false}
        ]

        function item(name) {
            var found = findChild(screen, name);
            verify(found !== null, name + " is missing");
            return found;
        }
        function showHide(scope) {
            return testContext.accessibilityCountWithin(scope, "ObjectShow")
                + testContext.accessibilityCountWithin(scope, "ObjectHide");
        }
        function churn() {
            var counts = testContext.accessibilityCount();
            return JSON.stringify(Object.keys(counts.chains).filter(function (chain) {
                return /^(ObjectShow|ObjectHide) /.test(chain);
            }).map(function (chain) {
                return counts.chains[chain] + "x " + chain;
            }));
        }
        function chipTexts(chips) {
            var texts = [];
            for (var i = 0; i < chips.count; ++i) {
                var chip = chips.itemAtIndex(i);
                texts.push(chip !== null ? chip.text : null);
            }
            return texts;
        }

        function initTestCase() {
            verify(testContext.accessibilityActive(), "run with DSD_QML_TEST_ACCESSIBILITY=1, or nothing is counted");
            verify(screen !== null, "TalkgroupsScreen.qml failed to load");
        }
        function init() {
            testContext.setHostRunning(true);
            testContext.resetCommands();
            talkgroupView.filterText = "";
            talkgroupView.filterTag = "";
            screen.visible = true;
            verify(testContext.applyTalkgroupTable(1));
            tryCompare(item("talkgroupGrid"), "count", tableB.length);
        }
        function cleanup() {
            testContext.stopAccessibilityCount();
            cardBuildSpy.target = null;
            chipBuildSpy.target = null;
            item("talkgroupEditSheet").visible = false;
            item("talkgroupSearch").text = "";
            talkgroupView.filterText = "";
            talkgroupView.filterTag = "";
            screen.visible = true;
            verify(testContext.applyTalkgroupTable(-1));
            testContext.resetCommands();
            testContext.setHostRunning(false);
        }

        // Hidden, tables A and B take turns as a scan's hops bring them: no card and no chip is built, and nothing
        // under the screen shows or hides. Shown again, it lists table B under the search and category set before.
        function test_hidden_screen_leaves_its_lists_unbuilt() {
            var cards = item("talkgroupGrid");
            var chips = item("talkgroupChips");
            var search = item("talkgroupSearch");
            // A category picked and a search typed while the screen was open.
            talkgroupView.filterTag = "WORKS";
            search.text = "Water";
            compare(talkgroupView.filterText, "Water");
            tryCompare(cards, "count", 1);
            screen.visible = false;
            cardBuildSpy.target = cards.contentItem;
            chipBuildSpy.target = chips.contentItem;
            cardBuildSpy.clear();
            chipBuildSpy.clear();
            testContext.startAccessibilityCount();
            for (var i = 0; i < switches; ++i) {
                verify(testContext.applyTalkgroupTable(i % 2));
                compare(talkgroups.count, i % 2 === 0 ? 3 : tableB.length, "table " + (i % 2 === 0 ? "A" : "B") + " is shown");
            }
            compare(cardBuildSpy.count, 0, "cards were built under the hidden screen");
            compare(chipBuildSpy.count, 0, "chips were built under the hidden screen");
            verify(cards.itemAtIndex(0) === null, "a card exists under the hidden screen");
            verify(chips.itemAtIndex(0) === null, "a chip exists under the hidden screen");
            compare(showHide(screen), 0, churn());

            // Shown again: table B's rows, under the search and category set before.
            screen.visible = true;
            compare(search.text, "Water");
            compare(talkgroupView.filterText, "Water");
            compare(talkgroupView.filterTag, "WORKS");
            tryCompare(cards, "count", 1);
            tryVerify(function () { return cards.itemAtIndex(0) !== null; });
            var card = cards.itemAtIndex(0);
            var row = tableB[3];
            compare(card.idText, String(row.id));
            compare(card.name, row.name);
            compare(card.tags, row.tags);
            compare(card.listening, row.listening);
            verify(cardBuildSpy.count > 0, "opening builds the cards, and the spy sees it");
            tryCompare(chips, "count", 3);
            tryVerify(function () { return chips.itemAtIndex(2) !== null; });
            verify(chipBuildSpy.count > 0, "opening builds the chips, and the spy sees it");
            compare(chipTexts(chips), ["All", "POLICE", "WORKS"]);
            verify(chips.itemAtIndex(2).active, "the picked category is still the active chip");
            // Without the search, the category alone: both WORKS rows.
            search.text = "";
            tryCompare(cards, "count", 2);
            tryVerify(function () { return cards.itemAtIndex(1) !== null; });
            compare(cards.itemAtIndex(0).idText, String(tableB[2].id));
            compare(cards.itemAtIndex(1).idText, String(tableB[3].id));
            // Shown, the lists follow the tables again.
            verify(testContext.applyTalkgroupTable(0));
            talkgroupView.filterTag = "";
            tryCompare(cards, "count", 3);
            tryCompare(chips, "count", 3);
            compare(chipTexts(chips), ["All", "EMS", "FIRE"]);
        }

        // An edit opened on table B carries B's policy version; a hop to table A under it leaves it at that, and
        // the decoder's version check refuses it as stale, as it always has.
        function test_edit_keeps_the_version_it_opened_with() {
            var cards = item("talkgroupGrid");
            tryVerify(function () { return cards.itemAtIndex(0) !== null; });
            var context = talkgroups.policyContext;
            var generation = talkgroups.policyGeneration;
            verify(testContext.talkgroupEditVersionValid(context, generation), "B's own version is current on B");
            cards.itemAtIndex(0).editRequested();
            var sheet = item("talkgroupEditSheet");
            verify(sheet.visible);
            compare(sheet.idStart, tableB[0].id);
            compare(sheet.policyContext, context);
            verify(testContext.applyTalkgroupTable(0));
            verify(talkgroups.policyContext !== context, "table A is another policy context");
            verify(sheet.visible, "the edit stays open across the table change");
            item("talkgroupName").text = "Police Dispatch North";
            item("saveTalkgroup").activate();
            var edit = commands.lastTalkgroupEdit();
            compare(edit.length, 5, JSON.stringify(edit));
            compare(Number(edit[0]), tableB[0].id);
            compare(edit[2], context, "the edit carries the context it opened with");
            compare(Number(edit[3]), generation, "and the generation it opened with");
            verify(!testContext.talkgroupEditVersionValid(edit[2], edit[3]), "the decoder refuses it as stale");
        }
    }
}
