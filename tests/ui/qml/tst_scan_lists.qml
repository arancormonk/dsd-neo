// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// Issue #697 on Android: Home's "Scan with other lists" sheet (rows per other
// ready list, off; a list that cannot join disabled with the first differing
// setting; drafts left out; the start button's count; the order handed to the
// start), the session's Scan lists sheet over the real toggle model (a switch
// waits for its answer, a refusal goes back and says why until the lists
// move, paused rows say how to get back), and a scan-list card's entry count
// in the singular and plural.
Item {
    id: root
    width: 411
    height: 860

    Ui.ScanTogetherSheet {
        id: together
    }
    Ui.ScanListsSheet {
        id: session
    }
    Ui.ScanListCard {
        id: card
        width: root.width
        listName: "Security"
    }

    TestCase {
        name: "ScanLists"
        when: windowShown

        function add(name, extra) {
            var list = {
                name: name,
                sourceType: "rtltcp",
                host: "127.0.0.1",
                port: 1234,
                entries: [{kind: "freq", name: name + " channel", protocol: "p25", freqMhz: "851.5", enabled: true}]
            };
            for (var key in extra || {})
                list[key] = extra[key];
            verify(scanLists.add(list));
            return scanLists.get(scanLists.count - 1);
        }
        function row(sheet, name) {
            var found = findChild(sheet, name);
            verify(found !== null, name + " exists");
            return found;
        }
        function cleanup() {
            together.visible = false;
            session.visible = false;
            testContext.resetScanLists();
            testContext.setHostRunning(false);
            while (scanLists.count)
                scanLists.remove(0);
        }

        // ---- Entry count ----

        function test_card_count_data() {
            return [{tag: "none", n: 0, text: "0 entries"}, {tag: "one", n: 1, text: "1 entry"},
                    {tag: "two", n: 2, text: "2 entries"}];
        }
        function test_card_count(data) {
            card.isDraft = false;
            card.entryCount = data.n;
            var shown = null;
            for (var i = 0; i < card.children.length && !shown; ++i) {
                var column = card.children[i];
                for (var j = 0; column.children && j < column.children.length; ++j) {
                    if (column.children[j].text === data.text)
                        shown = column.children[j];
                }
            }
            verify(shown !== null, "the card reads " + data.text);
        }

        // ---- Scan with other lists ----

        function test_join_sheet_rows_and_start() {
            var security = add("Security");
            add("Draft", {isDraft: true});
            var louder = add("Louder", {gainDb: 40});
            var hospital = add("Hospital");
            var parks = add("Parks");
            var started = [];
            var connection = function (primaryUid, uids) {
                started.push({primary: primaryUid, companions: uids});
            };
            together.startRequested.connect(connection);
            try {
                together.openFor(scanLists.rowForUid(security.uid));
                tryCompare(together, "visible", true);
                compare(row(together, "scanTogetherTitle").text, "Scan with other lists");
                // Every other ready list in Home order; the draft and the card's own are not offered.
                compare(together.others.map(function (o) { return o.name; }).join(), "Louder,Hospital,Parks");
                verify(findChild(together, "scanTogetherRow_" + security.uid) === null);
                var louderRow = row(together, "scanTogetherRow_" + louder.uid);
                var hospitalRow = row(together, "scanTogetherRow_" + hospital.uid);
                var parksRow = row(together, "scanTogetherRow_" + parks.uid);
                // A list on another gain cannot join: shown, disabled, naming why.
                verify(!louderRow.enabled);
                compare(louderRow.subtitle, "Different gain");
                verify(!louderRow.checked);
                // The rest start off, sized by their entries.
                verify(hospitalRow.enabled && !hospitalRow.checked);
                compare(hospitalRow.subtitle, "1 entry");
                var start = row(together, "scanTogetherStart");
                // Nothing chosen: nothing to start here (Play starts the one list).
                compare(start.text, "Switch on a list first");
                verify(!start.enabled);
                louderRow.activate();
                compare(together.chosen.length, 0);
                parksRow.activate();
                hospitalRow.activate();
                compare(start.text, "Start 3 lists");
                verify(start.enabled);
                verify(hospitalRow.checked && parksRow.checked);
                // Off again, then on: still Home order, whatever the order of the taps.
                parksRow.activate();
                compare(start.text, "Start 2 lists");
                parksRow.activate();
                start.clicked();
                tryCompare(together, "visible", false);
                compare(started.length, 1);
                compare(started[0].primary, security.uid);
                compare(started[0].companions.join(), [hospital.uid, parks.uid].join());
                // Opened again, nothing is on.
                together.openFor(scanLists.rowForUid(security.uid));
                compare(together.chosen.length, 0);
                compare(row(together, "scanTogetherStart").text, "Switch on a list first");
                verify(!row(together, "scanTogetherStart").enabled);
                // A chosen list that becomes a draft leaves the choice and the rows.
                row(together, "scanTogetherRow_" + hospital.uid).activate();
                compare(together.chosen.join(), hospital.uid);
                verify(scanLists.update(scanLists.rowForUid(hospital.uid), {isDraft: true}));
                tryCompare(together, "chosen", []);
                verify(findChild(together, "scanTogetherRow_" + hospital.uid) === null);
            } finally {
                together.startRequested.disconnect(connection);
            }
        }

        function test_join_sheet_with_nothing_to_add() {
            var only = add("Only");
            together.openFor(scanLists.rowForUid(only.uid));
            compare(together.others.length, 0);
            compare(row(together, "scanTogetherHint").text, "No other scan list is ready. Save another list first.");
            verify(!row(together, "scanTogetherRows").visible);
        }

        // ---- Scan lists, in session ----

        function threeLists(seq, extra) {
            var view = {
                session: 7,
                seq: seq,
                activeList: 2,
                lists: [{name: "security", on: true, targets: 12}, {name: "hospital", on: true, targets: 1, onAir: true},
                        {name: "parks", on: true, targets: 3}]
            };
            for (var key in extra || {})
                view[key] = extra[key];
            return view;
        }
        function openSession() {
            testContext.setHostRunning(true);
            testContext.setScanLists(threeLists(10));
            session.listName = function (number, fallback) {
                return ["Security", "Hospital", "Parks"][number - 1] || fallback;
            };
            session.open();
            tryCompare(session, "visible", true);
        }

        function test_session_sheet_rows() {
            openSession();
            compare(row(session, "scanListsTitle").text, "Scan lists");
            compare(row(session, "scanListsStatus").text, "3 of 3 on · 16 targets");
            var hospital = row(session, "scanListsRow_2");
            // The session's names, not the files'.
            compare(hospital.title, "Hospital");
            compare(hospital.subtitle, "1 target · On air");
            compare(row(session, "scanListsRow_1").subtitle, "12 targets");
            verify(hospital.checked && hospital.enabled);
        }

        function test_session_sheet_pending_then_switched() {
            openSession();
            var hospital = row(session, "scanListsRow_2");
            hospital.activate();
            var sent = testContext.scanListRequests();
            compare(sent.length, 1);
            compare(sent[0].list, 2);
            compare(sent[0].enabled, 0);
            compare(sent[0].session, 7);
            // Waiting: the switch where it was put, the row saying so, no second tap taken.
            tryCompare(hospital, "checked", false);
            compare(hospital.subtitle, "1 target · On air\nSwitching off…");
            verify(!hospital.enabled);
            hospital.activate();
            compare(testContext.scanListRequests().length, 1);
            // Answered ahead of the snapshot that shows it: it stays off.
            testContext.answerScanList(sent[0].request, 0, 11);
            tryCompare(hospital, "subtitle", "1 target · On air");
            verify(!hospital.checked && hospital.enabled);
            compare(hospital.note, "");
            compare(row(session, "scanListsStatus").text, "2 of 3 on · 15 targets");
            testContext.setScanLists(threeLists(11, {activeList: 3, lists: [{name: "security", targets: 12},
                {name: "hospital", on: false, targets: 1}, {name: "parks", targets: 3, onAir: true}]}));
            tryCompare(hospital, "subtitle", "1 target");
            verify(!hospital.checked);
        }

        // Issue #697: lists that share targets. security's file holds 12 (s, which hospital holds too, m, which parks
        // holds too, and 10 of its own), hospital's 1 (s, on air), parks' 3 (m and 2 of its own). Each row keeps its
        // file's count, both lists holding s say On air, and the total counts each target once, a switch included.
        function test_session_sheet_counts_a_shared_target_once() {
            testContext.setHostRunning(true);
            testContext.setScanLists(threeLists(10, {activeList: 1, memberships: {"1": 10, "3": 1, "5": 1, "4": 2},
                lists: [{name: "security", targets: 12, onAir: true}, {name: "hospital", targets: 1, onAir: true},
                        {name: "parks", targets: 3}]}));
            session.open();
            tryCompare(session, "visible", true);
            compare(row(session, "scanListsStatus").text, "3 of 3 on · 14 targets");
            compare(row(session, "scanListsRow_1").subtitle, "12 targets · On air");
            compare(row(session, "scanListsRow_2").subtitle, "1 target · On air");
            compare(row(session, "scanListsRow_3").subtitle, "3 targets");
            // hospital off: s stays through security.
            row(session, "scanListsRow_2").activate();
            tryCompare(row(session, "scanListsRow_2"), "checked", false);
            compare(row(session, "scanListsStatus").text, "2 of 3 on · 14 targets");
            // parks off too: m stays through security, parks' own two go.
            row(session, "scanListsRow_3").activate();
            tryCompare(row(session, "scanListsRow_3"), "checked", false);
            compare(row(session, "scanListsStatus").text, "1 of 3 on · 12 targets");
        }

        function test_session_sheet_refusal_reverts() {
            openSession();
            var security = row(session, "scanListsRow_1");
            security.activate();
            var sent = testContext.scanListRequests();
            tryCompare(security, "checked", false);
            // Refused: back on, the reason under the list until the lists move.
            testContext.answerScanList(sent[0].request, -1, 10);
            tryCompare(security, "checked", true);
            compare(security.subtitle, "12 targets");
            compare(security.note, "Nothing else to scan. Switch on another list first.");
            // Said aloud once, with the list it is about.
            compare(session.announced, "Security: Nothing else to scan. Switch on another list first.");
            var said = session.announcements;
            testContext.setScanLists(threeLists(10));
            compare(session.announcements, said);
            compare(security.note, "Nothing else to scan. Switch on another list first.");
            testContext.setScanLists(threeLists(11));
            tryCompare(security, "note", "");
            // A full queue: nothing sent, said under the list.
            testContext.setScanListQueueFull(true);
            row(session, "scanListsRow_3").activate();
            compare(testContext.scanListRequests().length, 1);
            compare(row(session, "scanListsRow_3").note, "Not sent. Try again.");
            verify(row(session, "scanListsRow_3").checked);
        }

        function test_session_sheet_paused() {
            openSession();
            testContext.setScanLists(threeLists(10, {paused: true}));
            tryCompare(row(session, "scanListsStatus"), "text", "Resume the scan to change lists.");
            for (var n = 1; n <= 3; ++n) {
                var list = row(session, "scanListsRow_" + n);
                verify(!list.enabled, "list " + n + " can be switched while paused");
                list.activate();
            }
            compare(testContext.scanListRequests().length, 0);
            testContext.setScanLists(threeLists(10));
            tryCompare(row(session, "scanListsRow_1"), "enabled", true);
        }

        function test_session_sheet_closes_with_the_session() {
            openSession();
            row(session, "scanListsRow_2").activate();
            // A new session drops the waiting switch; one list leaves nothing to switch.
            testContext.setScanLists({session: 8, seq: 1, lists: [{name: "security", targets: 12}]});
            tryCompare(session, "visible", false);
            openSession();
            testContext.setHostRunning(false);
            tryCompare(session, "visible", false);
        }
    }
}
