// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// Discussion #900 F3: the DMR Tier III Channels sheet and its channel editor, on the real DmrChannelsModel
// (testContext.setDmrChannelsView() publishes a synthetic Tier III record to it) and the recorded command bridge;
// and the Monitor's ways in: the Site sheet's Channels button and the NO FREQ reading.
Item {
    id: root
    width: 412
    height: 915

    Loader {
        id: sheetLoader
        anchors.fill: parent
        source: uiDir + "/ChannelsSheet.qml"
    }
    // The readings under test live in the Monitor's "Signal & site" disclosure (#718), open here.
    Ui.MonitorScreen {
        id: monitor
        anchors.fill: parent
        detailsExpanded: true
        visible: false
    }

    TestCase {
        name: "DmrChannels"
        when: windowShown
        readonly property var sheet: sheetLoader.item
        readonly property var edit: findChild(sheetLoader.item, "channelEditSheet")

        // A Tier III site, newest first: 9 a data grant only RAS accepted, 12 a group call on slot 1, 7 a private call on
        // slot 2, and 40 past the ledger's details; and one channel of each origin.
        function view(extra) {
            var spec = {
                generation: 3,
                revision: 9,
                session: 5,
                missing: [
                    {lpcn: 9, slot: 0, group: true, data: true, unverified: true, caller: 5004, recipient: 77, agoMs: 1000},
                    {lpcn: 12, slot: 0, group: true, caller: 5001, recipient: 1234, agoMs: 120000},
                    {lpcn: 7, slot: 1, group: false, caller: 5002, recipient: 5003, agoMs: 200000}
                ],
                extra: [40],
                names: [{tg: 1234, name: "Dispatch"}],
                channels: [
                    {lpcn: 18, origin: 1, hz: 451012500},
                    {lpcn: 20, origin: 2, hz: 451137500, baseHz: 451100000},
                    {lpcn: 21, origin: 2, hidden: true, baseHz: 451200000},
                    {lpcn: 22, origin: 3, hz: 451300000},
                    {lpcn: 23, origin: 4, hz: 451312500},
                    {lpcn: 24, origin: 5, hz: 451325000},
                    {lpcn: 25, origin: 6, hz: 451337500}
                ]
            };
            for (var key in extra)
                spec[key] = extra[key];
            return spec;
        }
        function rows(name) {
            var out = [];
            (function collect(item) {
                for (var k = 0; k < item.children.length; ++k) {
                    var child = item.children[k];
                    if (child.objectName === name && child.live)
                        out.push(child);
                    collect(child);
                }
            })(sheet);
            return out;
        }
        function words(name) {
            return rows(name).map(function (row) {
                return row.words;
            });
        }
        function row(name, lpcn) {
            var all = rows(name);
            for (var r of all) {
                if (r.channel === lpcn)
                    return r;
            }
            return null;
        }
        function item(name) {
            var found = findChild(sheet, name);
            verify(found !== null, name + " exists");
            return found;
        }
        function monitorItem(name) {
            var found = findChild(monitor, name);
            verify(found !== null, name + " exists");
            return found;
        }
        // A real tap, on the target scrolled into its sheet's view first: the lists run past a phone's screen.
        function tap(target) {
            for (var up = target.parent; up; up = up.parent) {
                if (typeof up.reveal === "function" && up.panelObjectName !== undefined) {
                    up.reveal(target);
                    break;
                }
            }
            // Laid out, not painted: a frame need not follow when nothing scrolled.
            waitForItemPolished(target);
            mouseClick(target, target.width / 2, target.height / 2);
        }
        function type(field, text) {
            field.text = text;
        }
        function lastRequest() {
            var all = commands.dmrChannelRequests();
            verify(all.length > 0, "a channel edit was sent");
            return all[all.length - 1];
        }
        function openSheet(spec) {
            testContext.setDmrChannelsView(spec);
            sheet.open();
            tryCompare(sheet, "visible", true);
            // Opening reads the record as it is (UiController does on activeChanged): published again here.
            testContext.setDmrChannelsView(spec);
            wait(20);
        }

        function init() {
            dmrChannels.sessionSystemUid = "11111111-2222-3333-4444-555555555555";
        }
        function cleanup() {
            if (edit)
                edit.visible = false;
            if (sheet) {
                sheet.visible = false;
                // A case that leaves its edit unanswered (the decoder always answers one).
                sheet.request = 0;
                sheet.setStatus("", false);
            }
            findChild(monitor, "siteSheet").visible = false;
            findChild(monitor, "channelsSheet").visible = false;
            monitor.visible = false;
            Ui.Navigation.modals = [];
            testContext.resetCommands();
            testContext.clearDmrChannels();
            testContext.setMetric("siteProtocol", "");
            testContext.setMetric("siteLine", "");
            testContext.setMetric("siteConfirmed", false);
            testContext.setMetric("dmrTier3", false);
            testContext.setMetric("dmrUserChannels", false);
            testContext.setMetric("noFreqCount", 0);
            if (sheet) {
                sheet.owedSaves = {};
                sheet.owedSaveCount = 0;
            }
        }
        // Every row a list built, held ones included.
        function allRows(name) {
            var out = [];
            (function collect(item) {
                for (var k = 0; k < item.children.length; ++k) {
                    var child = item.children[k];
                    if (child.objectName === name)
                        out.push(child);
                    collect(child);
                }
            })(sheet);
            return out;
        }
        // @p n channels heard from the site, 1..n.
        function manyChannels(n) {
            var out = [];
            for (var i = 1; i <= n; ++i)
                out.push({lpcn: i, origin: 3, hz: 450000000 + 12500 * i});
            return out;
        }

        function test_sections_and_rows() {
            openSheet(view({}));
            compare(dmrChannels.active, true, "the open sheet reads the record");
            compare(item("channelsMissingLabel").text, "No frequency");
            compare(item("channelsChannelsLabel").text, "Channels");
            verify(item("channelsMissingNote").visible);
            compare(item("channelsMissingNote").text, "dsd-neo cannot retune to calls on these channels until each has a frequency.");
            verify(!item("channelsMissingEmpty").visible);
            // Most recent first without a screen reader; the channel past the details is listed by its number.
            compare(words("channelsMissingRow"), [
                "Channel 9 (unchecked) · slot 1 · data · TG 77 · last heard just now",
                "Channel 12 · slot 1 · TG 1234 (Dispatch) · last heard 2 min ago",
                "Channel 7 · slot 2 · 5002 to 5003 · last heard 3 min ago",
                "Channel 40"
            ]);
            compare(words("channelsChannelRow"), [
                "Channel 18 · 451.012500 MHz · from file",
                "Channel 20 · 451.137500 MHz · yours",
                "Channel 21 · hidden",
                "Channel 22 · 451.300000 MHz · heard from site",
                "Channel 23 · 451.312500 MHz · unchecked",
                "Channel 24 · 451.325000 MHz · from a neighbour site",
                "Channel 25 · 451.337500 MHz · estimated"
            ]);
            // A private call is drawn with an arrow and read with "to".
            var seven = row("channelsMissingRow", 7);
            verify(seven.shownText.indexOf("5002 → 5003") >= 0, seven.shownText);
            compare(seven.Accessible.name, seven.words);
            compare(seven.Accessible.role, Accessible.Button);
            verify(!item("channelsReadOnlyNote").visible);
            sheet.visible = false;
            compare(dmrChannels.active, false, "closed, it reads nothing");
        }

        function test_empty_state() {
            openSheet(view({missing: [], extra: []}));
            verify(item("channelsMissingEmpty").visible);
            compare(item("channelsMissingEmpty").text, "No unresolved grants recorded.");
            verify(!item("channelsMissingNote").visible);
            compare(rows("channelsMissingRow").length, 0);
            compare(rows("channelsChannelRow").length, 7);
        }

        // A missing channel's row opens the editor on its number; the edit is sent against the row as the sheet read
        // it. Once the decoder answers and the record lists the channel, the row has moved to Channels and the
        // keyboard focus is on it there.
        function test_add_from_missing_row() {
            openSheet(view({}));
            var twelve = row("channelsMissingRow", 12);
            verify(twelve !== null);
            twelve.forceActiveFocus();
            tap(twelve);
            tryCompare(edit, "visible", true);
            compare(findChild(edit, "channelEditHeading").text, "Channel 12");
            verify(!findChild(edit, "channelEditNumber").visible, "the number is the row's");
            verify(!findChild(edit, "channelEditHelp").visible);
            compare(findChild(edit, "channelEditFrequency").text, "");
            var save = findChild(edit, "channelEditSave");
            compare(save.text, "Add channel");
            verify(!save.inputValid, "no frequency yet");
            verify(!findChild(edit, "channelEditHide").visible);
            verify(!findChild(edit, "channelEditRestore").visible);
            verify(!findChild(edit, "channelEditRemove").visible);
            type(findChild(edit, "channelEditFrequency"), "451.5");
            verify(save.inputValid);
            tap(save);
            var sent = lastRequest();
            compare(sent.lpcn, 12);
            compare(sent.op, dmrChannels.opSet);
            compare(sent.hz, 451500000);
            compare(sent.expectedHz, 0);
            compare(sent.expectedOrigin, 0);
            compare(sent.generation, 3);
            compare(sent.session, "5");
            compare(sent.systemUid, "11111111-2222-3333-4444-555555555555");
            // In flight: nothing more can be sent, and nothing alarming is said.
            verify(!save.enabled, "pending");
            compare(findChild(edit, "channelEditMessage").text, "");
            compare(edit.visible, true);
            // The decoder applies it; the record lists 12 with its new frequency.
            var moved = view({revision: 10});
            moved.missing = moved.missing.filter(function (m) { return m.lpcn !== 12; });
            moved.channels = [{lpcn: 12, origin: 2, hz: 451500000}].concat(moved.channels);
            commands.answerDmrChannel({result: "ok", revision: 10});
            tryCompare(edit, "visible", false);
            testContext.setDmrChannelsView(moved);
            tryVerify(function () { return row("channelsChannelRow", 12) !== null; });
            verify(row("channelsMissingRow", 12) === null);
            compare(row("channelsChannelRow", 12).words, "Channel 12 · 451.500000 MHz · yours");
            tryVerify(function () { return row("channelsChannelRow", 12).activeFocus; }, 2000, "the focus follows the row");
            compare(item("channelsStatus").text, "", "kept on the saved system: nothing more to say");
        }

        function test_refusals_are_said() {
            var cases = [
                {result: "stale", text: "This channel changed while you were editing it. Check it, then try again."},
                {result: "full", text: "You already have 256 channels of your own. Remove one first."},
                {result: "trunk-scan", text: "Channel editing is available when one system is running."},
                {result: "dropped", text: "Listening stopped before the change was made."},
                {result: "search", text: "Channels can't be changed while Signal search is running."},
                {result: "unknown", text: "dsd-neo did not make the change. Try again."}
            ];
            openSheet(view({}));
            for (var c of cases) {
                tap(row("channelsChannelRow", 18));
                tryCompare(edit, "visible", true);
                type(findChild(edit, "channelEditFrequency"), "451.0250");
                var save = findChild(edit, "channelEditSave");
                compare(save.text, "Apply");
                tap(save);
                var sent = lastRequest();
                compare(sent.expectedHz, 451012500);
                compare(sent.expectedOrigin, 1);
                commands.answerDmrChannel({result: c.result});
                tryCompare(findChild(edit, "channelEditMessage"), "text", c.text);
                verify(edit.visible, "a refused edit keeps the editor open");
                verify(save.enabled, "and it can be sent again");
                compare(findChild(edit, "channelEditMessage").Accessible.role, Accessible.AlertMessage);
                compare(edit.announced, c.text);
                // Close asks before the typed frequency goes.
                findChild(edit, "channelEditClose").activate();
                var discard = findChild(edit, "discardDialogDiscard");
                tryVerify(function () { return discard !== null && discard.visible; });
                discard.activate();
                tryCompare(edit, "visible", false);
            }
        }

        // An answer to an edit this sheet did not send changes nothing.
        function test_other_answers_are_ignored() {
            openSheet(view({}));
            tap(row("channelsChannelRow", 18));
            tryCompare(edit, "visible", true);
            type(findChild(edit, "channelEditFrequency"), "451.0250");
            tap(findChild(edit, "channelEditSave"));
            commands.answerDmrChannel({result: "stale", request: lastRequest().request + 100});
            wait(20);
            compare(findChild(edit, "channelEditMessage").text, "");
            verify(!findChild(edit, "channelEditSave").enabled, "still waiting for its own answer");
        }

        function test_save_error_is_said() {
            openSheet(view({}));
            tap(row("channelsChannelRow", 18));
            tryCompare(edit, "visible", true);
            type(findChild(edit, "channelEditFrequency"), "451.0250");
            tap(findChild(edit, "channelEditSave"));
            commands.answerDmrChannel({result: "ok", saved: false, saveError: "not-saved"});
            tryCompare(edit, "visible", false);
            var status = item("channelsStatus");
            compare(status.text, "Changed until you stop listening, but not saved: dsd-neo could not save this system.");
            verify(status.visible);
            compare(status.Accessible.role, Accessible.AlertMessage);
        }

        function test_not_kept_without_a_saved_system() {
            dmrChannels.sessionSystemUid = "";
            openSheet(view({}));
            tap(row("channelsMissingRow", 7));
            tryCompare(edit, "visible", true);
            var notKept = findChild(edit, "channelEditNotKept");
            verify(notKept.visible);
            compare(notKept.text, "Not kept: this session is not a saved system.");
            type(findChild(edit, "channelEditFrequency"), "451.0625");
            tap(findChild(edit, "channelEditSave"));
            compare(lastRequest().systemUid, "");
            commands.answerDmrChannel({result: "ok"});
            tryCompare(edit, "visible", false);
            compare(item("channelsStatus").text, "Not kept: this session is not a saved system.");
        }

        function test_use_control_channel_frequency() {
            openSheet(view({}));
            tap(row("channelsMissingRow", 12));
            tryCompare(edit, "visible", true);
            var useCc = findChild(edit, "channelEditUseCc");
            compare(useCc.text, "Use control channel frequency");
            verify(!useCc.enabled, "no checked control channel evidence on this carrier");
            edit.visible = false;
            openSheet(view({ccHz: 451987500}));
            tap(row("channelsMissingRow", 12));
            tryCompare(edit, "visible", true);
            tryVerify(function () { return useCc.enabled; });
            tap(useCc);
            compare(findChild(edit, "channelEditFrequency").text, "451.9875");
            tap(findChild(edit, "channelEditSave"));
            compare(lastRequest().hz, 451987500);
        }

        function test_number_and_frequency_checked_inline() {
            openSheet(view({}));
            tap(item("channelsAddButton"));
            tryCompare(edit, "visible", true);
            var number = findChild(edit, "channelEditNumber");
            var save = findChild(edit, "channelEditSave");
            compare(number.text, "");
            verify(number.visible);
            compare(findChild(edit, "channelEditHeading").text, "Add channel");
            verify(findChild(edit, "channelEditHelp").visible);
            compare(findChild(edit, "channelEditHelp").text, "Use the channel number dsd-neo shows. If your list counts each timeslot (DSD+, some RadioReference lists), take (number − 1) ÷ 2 and drop the remainder.");
            compare(number.error, "", "nothing typed, nothing wrong yet");
            verify(!save.inputValid);
            for (var bad of ["0", "4095"]) {
                type(number, bad);
                compare(number.error, "Enter a channel number from 1 to 4094.");
            }
            type(number, "4094");
            compare(number.error, "");
            compare(findChild(edit, "channelEditHeading").text, "Channel 4094", "named once the number is one");
            var freq = findChild(edit, "channelEditFrequency");
            var freqError = findChild(edit, "frequencyFieldInvalid");
            for (var badHz of ["0.05", "2147.483648", "3000"]) {
                type(freq, badHz);
                verify(freqError.visible, badHz);
                compare(freqError.text, "Enter a frequency from 0.1 to 2147.483647 MHz");
                verify(!save.inputValid, badHz);
            }
            type(freq, "0.1");
            verify(!freqError.visible);
            verify(save.inputValid);
            tap(save);
            var sent = lastRequest();
            compare(sent.lpcn, 4094);
            compare(sent.hz, 100000);
            compare(sent.expectedOrigin, 0);
        }

        // One action per row, by where its frequency came from: Hide a channel from the file or the air, Restore a
        // hidden one, Remove one of yours (back to the base frequency).
        function test_hide_restore_remove_by_origin() {
            var expect = [
                {lpcn: 18, shown: "channelEditHide", text: "Hide", op: dmrChannels.opHide, expectedHz: 451012500, origin: 1},
                {lpcn: 22, shown: "channelEditHide", text: "Hide", op: dmrChannels.opHide, expectedHz: 451300000, origin: 3},
                {lpcn: 21, shown: "channelEditRestore", text: "Restore", op: dmrChannels.opRestore, expectedHz: 0, origin: 2},
                {lpcn: 20, shown: "channelEditRemove", text: "Remove", op: dmrChannels.opRestore, expectedHz: 451137500, origin: 2}
            ];
            openSheet(view({}));
            for (var e of expect) {
                tap(row("channelsChannelRow", e.lpcn));
                tryCompare(edit, "visible", true);
                compare(findChild(edit, "channelEditHeading").text, "Channel " + e.lpcn);
                compare(findChild(edit, "channelEditRemove").text, "Remove", "disarmed when opened");
                verify(!findChild(edit, "channelEditNumber").visible, "an existing channel keeps its number");
                for (var name of ["channelEditHide", "channelEditRestore", "channelEditRemove"])
                    compare(findChild(edit, name).visible, name === e.shown, e.lpcn + " " + name);
                var action = findChild(edit, e.shown);
                compare(action.text, e.text);
                var before = commands.dmrChannelRequests().length;
                tap(action);
                if (e.shown === "channelEditRemove") {
                    // Asked twice, as a talkgroup's Remove is; Hide needs no second tap (Restore undoes it).
                    compare(action.text, "Tap again to remove");
                    compare(commands.dmrChannelRequests().length, before, "nothing sent on the first tap");
                    tap(action);
                    compare(action.text, "Remove");
                }
                compare(commands.dmrChannelRequests().length, before + 1);
                var sent = lastRequest();
                compare(sent.lpcn, e.lpcn);
                compare(sent.op, e.op);
                compare(sent.expectedHz, e.expectedHz);
                compare(sent.expectedOrigin, e.origin);
                commands.answerDmrChannel({result: "ok"});
                tryCompare(edit, "visible", false);
            }
        }

        // Under a trunk scan the channels are read-only: the rows open nothing and nothing can be added.
        function test_trunk_scan_is_read_only() {
            openSheet(view({trunkScan: true}));
            var note = item("channelsReadOnlyNote");
            verify(note.visible);
            compare(note.text, "Channel editing is available when one system is running.");
            verify(!item("channelsAddButton").enabled);
            var twelve = row("channelsMissingRow", 12);
            compare(twelve.Accessible.role, Accessible.StaticText);
            tap(twelve);
            tap(row("channelsChannelRow", 18));
            wait(50);
            verify(!edit.visible);
            compare(commands.dmrChannelRequests().length, 0);
        }

        // A stale refusal reads the channel again: what was typed stays, the actions follow the channel's new origin,
        // and the retry is sent against the record as it now is.
        function test_stale_then_retry() {
            openSheet(view({}));
            tap(row("channelsChannelRow", 18));
            tryCompare(edit, "visible", true);
            var freq = findChild(edit, "channelEditFrequency");
            type(freq, "451.0250");
            tap(findChild(edit, "channelEditSave"));
            compare(lastRequest().generation, 3);
            // Meanwhile channel 18 became one of yours, on a new network generation and session.
            var moved = view({generation: 4, session: 6, revision: 12});
            moved.channels[0] = {lpcn: 18, origin: 2, hz: 451050000, baseHz: 451012500};
            testContext.setDmrChannelsView(moved);
            commands.answerDmrChannel({result: "stale"});
            tryCompare(findChild(edit, "channelEditMessage"), "text", "This channel changed while you were editing it. Check it, then try again.");
            compare(freq.text, "451.0250", "what was typed stays");
            verify(!findChild(edit, "channelEditHide").visible);
            verify(findChild(edit, "channelEditRemove").visible, "the actions follow the new origin");
            tap(findChild(edit, "channelEditSave"));
            var retry = lastRequest();
            compare(retry.lpcn, 18);
            compare(retry.hz, 451025000);
            compare(retry.expectedHz, 451050000);
            compare(retry.expectedOrigin, 2);
            compare(retry.generation, 4);
            compare(retry.session, "6");
        }

        // After a stale refusal the editor follows the record, but an armed Remove asks again only when the channel it
        // would remove changed: an update to another channel leaves it armed.
        function test_stale_keeps_remove_armed_for_other_channels() {
            openSheet(view({}));
            tap(row("channelsChannelRow", 20));
            tryCompare(edit, "visible", true);
            type(findChild(edit, "channelEditFrequency"), "451.1500");
            tap(findChild(edit, "channelEditSave"));
            commands.answerDmrChannel({result: "stale"});
            tryCompare(findChild(edit, "channelEditMessage"), "text", "This channel changed while you were editing it. Check it, then try again.");
            var remove = findChild(edit, "channelEditRemove");
            tap(remove);
            compare(remove.text, "Tap again to remove");
            var other = view({revision: 10});
            other.channels[4] = {lpcn: 23, origin: 4, hz: 451362500};
            testContext.setDmrChannelsView(other);
            wait(20);
            compare(remove.text, "Tap again to remove", "another channel moved: still armed");
            var moved = view({revision: 11});
            moved.channels[1] = {lpcn: 20, origin: 2, hz: 451162500, baseHz: 451100000};
            testContext.setDmrChannelsView(moved);
            tryCompare(remove, "text", "Remove", 2000, "this channel moved: asks again");
            compare(commands.dmrChannelRequests().length, 1, "nothing more was sent");
        }

        // Remove of a channel of yours with no frequency to go back to: its row goes once the record has the edit,
        // and the focus does not follow it there; it lands on Add channel.
        function test_remove_focus_does_not_follow_a_vanishing_row() {
            var spec = view({});
            spec.channels.push({lpcn: 30, origin: 2, hz: 451400000});
            openSheet(spec);
            var thirty = row("channelsChannelRow", 30);
            thirty.forceActiveFocus();
            tap(thirty);
            tryCompare(edit, "visible", true);
            var remove = findChild(edit, "channelEditRemove");
            tap(remove);
            tap(remove);
            compare(lastRequest().op, dmrChannels.opRestore);
            commands.answerDmrChannel({result: "ok", revision: 10});
            tryCompare(edit, "visible", false);
            wait(20);
            compare(sheet.followLpcn, 30, "waiting for the record");
            verify(!item("channelsAddButton").activeFocus);
            var gone = view({revision: 10});
            testContext.setDmrChannelsView(gone);
            tryVerify(function () { return item("channelsAddButton").activeFocus; }, 2000, "the focus stays in the sheet");
            compare(sheet.followLpcn, 0);
            verify(row("channelsChannelRow", 30) === null);
        }

        // The answer is the sheet's to handle, whatever the editor did meanwhile: a late save error shows after the
        // editor was closed and reopened on another channel, and a "Not kept" that came while the sheet was closed is
        // still there when it opens.
        function test_late_answers_are_kept() {
            openSheet(view({}));
            tap(row("channelsChannelRow", 18));
            tryCompare(edit, "visible", true);
            type(findChild(edit, "channelEditFrequency"), "451.0250");
            tap(findChild(edit, "channelEditSave"));
            edit.visible = false;
            tap(row("channelsChannelRow", 22));
            tryCompare(edit, "visible", true);
            verify(!findChild(edit, "channelEditSave").enabled, "one edit at a time");
            commands.answerDmrChannel({result: "ok", saved: false, saveError: "no-system"});
            wait(20);
            verify(edit.visible, "the editor open on another channel stays");
            compare(item("channelsStatus").text, "Changed until you stop listening, but not saved: this system is no longer in your saved systems.");
            verify(findChild(edit, "channelEditSave").enabled);
            edit.visible = false;
            // Closed with the answer due.
            dmrChannels.sessionSystemUid = "";
            tap(row("channelsChannelRow", 22));
            tryCompare(edit, "visible", true);
            type(findChild(edit, "channelEditFrequency"), "451.3125");
            tap(findChild(edit, "channelEditSave"));
            compare(item("channelsStatus").text, "", "a new edit says its own outcome");
            edit.visible = false;
            sheet.visible = false;
            var before = sheet.announcements;
            commands.answerDmrChannel({result: "ok"});
            compare(sheet.followLpcn, 0, "nothing to follow in a closed sheet");
            compare(sheet.announcements, before, "nothing read out while the sheet is closed");
            sheet.open();
            tryCompare(sheet, "visible", true);
            compare(item("channelsStatus").text, "Not kept: this session is not a saved system.");
            verify(item("channelsStatus").visible);
            tryCompare(sheet, "announcements", before + 1);
            compare(sheet.announced, "Not kept: this session is not a saved system.", "read out once it opens");
            wait(20);
            compare(sheet.announcements, before + 1, "once");
            sheet.visible = false;
            sheet.open();
            compare(item("channelsStatus").text, "", "gone once it was on screen");
            wait(20);
            compare(sheet.announcements, before + 1, "nothing left to read out");
        }

        // Every save error, in its words.
        function test_save_error_words_data() {
            return [
                {tag: "not-dmr", text: "this saved system is not set to decode DMR."},
                {tag: "full", text: "this system already keeps 256 channels of yours."},
                {tag: "explicit-overrides", text: "the extra decoder arguments name another channels file, which dsd-neo uses instead."},
                {tag: "not-saved", text: "dsd-neo could not save this system."}
            ];
        }
        function test_save_error_words(data) {
            compare(sheet.saveErrorText(data.tag), "Changed until you stop listening, but not saved: " + data.text);
        }

        // Closing the sheet forgets a focus still waiting for the record.
        function test_closing_forgets_the_follow() {
            openSheet(view({}));
            tap(row("channelsMissingRow", 12));
            tryCompare(edit, "visible", true);
            type(findChild(edit, "channelEditFrequency"), "451.5");
            tap(findChild(edit, "channelEditSave"));
            commands.answerDmrChannel({result: "ok", revision: 10});
            tryCompare(edit, "visible", false);
            compare(sheet.followLpcn, 12);
            sheet.visible = false;
            compare(sheet.followLpcn, 0);
        }

        function test_site_sheet_channels_button() {
            monitor.visible = true;
            testContext.setMetric("siteProtocol", "DMR");
            testContext.setMetric("siteLine", "DMR · CC 1");
            testContext.setMetric("siteConfirmed", true);
            var siteSheet = monitorItem("siteSheet");
            siteSheet.visible = true;
            var button = monitorItem("siteChannelsButton");
            verify(!button.visible, "not Tier III");
            // Channels of the user's, entered from NO FREQ on a site only RAS named Tier III, stay reachable.
            testContext.setMetric("dmrUserChannels", true);
            tryCompare(button, "visible", true);
            testContext.setMetric("dmrUserChannels", false);
            tryCompare(button, "visible", false);
            testContext.setMetric("dmrTier3", true);
            tryCompare(button, "visible", true);
            compare(button.text, "Channels");
            tap(button);
            tryCompare(monitorItem("channelsSheet"), "visible", true);
            monitorItem("channelsSheet").visible = false;
            testContext.setMetric("siteProtocol", "P25");
            tryCompare(button, "visible", false);
        }

        // A site can list thousands of channels: the sheet builds a page of them, by number, and Show more channels
        // adds the next. Rows held past a page are bounded by it, and the sheet opens on the first page again.
        function test_channels_are_paged() {
            openSheet(view({channels: manyChannels(4094)}));
            var shown = words("channelsChannelRow");
            compare(shown.length, 50);
            compare(shown[0], "Channel 1 · 450.012500 MHz · heard from site");
            compare(shown[49], "Channel 50 · 450.625000 MHz · heard from site");
            compare(allRows("channelsChannelRow").length, 50, "no row built past the page");
            var more = item("channelsShowMore");
            verify(more.visible);
            compare(more.text, "Show more channels");
            tap(more);
            tryVerify(function () { return rows("channelsChannelRow").length === 100; });
            shown = words("channelsChannelRow");
            compare(shown[50], "Channel 51 · 450.637500 MHz · heard from site", "in channel order");
            compare(shown[99], "Channel 100 · 451.250000 MHz · heard from site");
            sheet.visible = false;
            openSheet(view({channels: manyChannels(4094)}));
            compare(rows("channelsChannelRow").length, 50, "opened on the first page again");
            compare(allRows("channelsChannelRow").length, 50, "and holding no rows past it");
        }

        // The last page takes Show more channels with it: the focus goes on to the first channel it added.
        function test_last_page_moves_the_focus_on() {
            openSheet(view({channels: manyChannels(120)}));
            var more = item("channelsShowMore");
            more.forceActiveFocus();
            more.activate();
            tryVerify(function () { return rows("channelsChannelRow").length === 100; });
            verify(more.visible && more.activeFocus, "more to come: the focus stays on the button");
            more.activate();
            tryVerify(function () { return rows("channelsChannelRow").length === 120; });
            verify(!more.visible);
            tryVerify(function () { return row("channelsChannelRow", 101).activeFocus; }, 2000, "on the first one added");
        }

        // The editor and its discard question go with the sheet, and with the session: shown again, the sheet opens
        // without them. An edit already sent is still answered.
        function test_editor_closes_with_the_sheet() {
            openSheet(view({}));
            tap(row("channelsChannelRow", 18));
            tryCompare(edit, "visible", true);
            type(findChild(edit, "channelEditFrequency"), "451.0250");
            findChild(edit, "channelEditClose").activate();
            var discard = findChild(edit, "discardDialogDiscard");
            tryVerify(function () { return discard !== null && discard.visible; });
            sheet.visible = false;
            sheet.open();
            tryCompare(sheet, "visible", true);
            wait(20);
            verify(!edit.visible, "the editor did not come back with the sheet");
            verify(!discard.visible, "nor its question");

            tap(row("channelsChannelRow", 18));
            tryCompare(edit, "visible", true);
            type(findChild(edit, "channelEditFrequency"), "451.0250");
            tap(findChild(edit, "channelEditSave"));
            sheet.visible = false;
            verify(sheet.request > 0, "the edit sent is still the sheet's");
            commands.answerDmrChannel({result: "ok", saved: false, saveError: "no-system"});
            compare(sheet.request, 0);
            sheet.open();
            tryCompare(sheet, "visible", true);
            verify(!edit.visible);
            compare(item("channelsStatus").text, "Changed until you stop listening, but not saved: this system is no longer in your saved systems.");
        }
        function test_editor_closes_with_the_session() {
            testContext.setHostRunning(true);
            openSheet(view({}));
            tap(row("channelsChannelRow", 18));
            tryCompare(edit, "visible", true);
            type(findChild(edit, "channelEditFrequency"), "451.0250");
            testContext.setHostRunning(false);
            tryCompare(edit, "visible", false);
            verify(sheet.visible, "the sheet itself is the Monitor's to close");
        }

        // A save that failed after the decoder made the change is kept: Try saving again saves it for the system the
        // edit was sent for. It stays while the sheet is closed, and goes once saved, the focus kept in the sheet.
        function test_try_saving_again() {
            openSheet(view({}));
            tap(row("channelsChannelRow", 20));
            tryCompare(edit, "visible", true);
            var remove = findChild(edit, "channelEditRemove");
            tap(remove);
            tap(remove);
            commands.answerDmrChannel({result: "ok", saved: false, saveError: "not-saved"});
            tryCompare(edit, "visible", false);
            var notSaved = "Changed until you stop listening, but not saved: dsd-neo could not save this system.";
            compare(item("channelsStatus").text, notSaved);
            var retry = item("channelsRetrySave");
            verify(retry.visible);
            compare(retry.text, "Try saving again");
            sheet.visible = false;
            sheet.open();
            tryCompare(sheet, "visible", true);
            verify(retry.visible, "kept while the sheet was closed");
            compare(item("channelsStatus").text, notSaved);
            // Another channel's edit, saved: the one owed still is.
            tap(row("channelsChannelRow", 22));
            tryCompare(edit, "visible", true);
            tap(findChild(edit, "channelEditHide"));
            commands.answerDmrChannel({result: "ok"});
            tryCompare(edit, "visible", false);
            compare(item("channelsStatus").text, notSaved);
            verify(retry.visible);
            commands.setDmrChannelSaveResult({saved: false, saveError: "not-saved"});
            tap(retry);
            var saves = commands.dmrChannelSaves();
            compare(saves.length, 1);
            compare(saves[0].systemUid, "11111111-2222-3333-4444-555555555555");
            compare(saves[0].lpcn, 20);
            compare(saves[0].op, dmrChannels.opRestore);
            compare(saves[0].hz, 0);
            verify(retry.visible, "still not saved");
            compare(item("channelsStatus").text, notSaved);
            commands.setDmrChannelSaveResult({saved: true, saveError: ""});
            retry.forceActiveFocus();
            retry.activate();
            compare(commands.dmrChannelSaves().length, 2);
            verify(!retry.visible);
            compare(item("channelsStatus").text, "Saved.");
            compare(sheet.announced, "Saved.");
            verify(item("channelsAddButton").activeFocus, "the focus stays in the sheet");
        }

        // The keyboard's action key in the channel number goes on to the frequency, through the field's next-field
        // path, which keeps the keyboard open.
        function test_number_field_goes_on_to_the_frequency() {
            openSheet(view({}));
            tap(item("channelsAddButton"));
            tryCompare(edit, "visible", true);
            var number = findChild(edit, "channelEditNumber");
            var freq = findChild(edit, "channelEditFrequency");
            verify(number.nextField !== null && number.nextField.input === freq, "the frequency is the next field");
            number.input.forceActiveFocus();
            keyClick(Qt.Key_1);
            keyClick(Qt.Key_2);
            keyClick(Qt.Key_Return);
            tryVerify(function () { return freq.activeFocus; });
            compare(number.text, "12");
        }

        function test_no_freq_row() {
            var period = Ui.Navigation.speechPeriodMs;
            Ui.Navigation.speechPeriodMs = 50;
            try {
                noFreqRow();
            } finally {
                Ui.Navigation.speechPeriodMs = period;
            }
        }
        function noFreqRow() {
            monitor.visible = true;
            var noFreq = monitorItem("noFreqRow");
            verify(!noFreq.visible, "hidden at 0");
            verify(monitor.detailsSummary.plain.indexOf("NO FREQ") < 0);
            testContext.setMetric("noFreqCount", 3);
            tryCompare(noFreq, "visible", true);
            compare(monitorItem("noFreqValue").text, "3");
            verify(Qt.colorEqual(monitorItem("noFreqValue").color, Ui.Theme.alert));
            compare(noFreq.Accessible.role, Accessible.Button);
            tryCompare(noFreq.Accessible, "name", "3 channels without a frequency");
            verify(monitor.detailsSummary.plain.indexOf("NO FREQ 3") >= 0, monitor.detailsSummary.plain);
            verify(monitor.detailsSummary.markup.indexOf("<font color=\"" + String(Ui.Theme.alert) + "\">NO FREQ 3</font>") >= 0,
                   "in the alert colour: " + monitor.detailsSummary.markup);
            // The Monitor was just shown and the row just appeared: drawn where the tap lands.
            waitForRendering(noFreq);
            tap(noFreq);
            tryCompare(monitorItem("channelsSheet"), "visible", true);
            monitorItem("channelsSheet").visible = false;
            testContext.setMetric("noFreqCount", 1);
            tryCompare(noFreq.Accessible, "name", "1 channel without a frequency");
            // The keyboard and a screen reader press it too.
            noFreq.Accessible.pressAction();
            tryCompare(monitorItem("channelsSheet"), "visible", true);
        }
    }
}
