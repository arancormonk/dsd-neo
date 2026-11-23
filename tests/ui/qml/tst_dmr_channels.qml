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
                sheet.slowRequestMs = 2000;
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
            var monitorChannels = findChild(monitor, "channelsSheet");
            monitorChannels.owedSaves = {};
            monitorChannels.owedSaveCount = 0;
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
            // A stale refusal closes the editor instead (test_stale_closes_the_editor).
            var cases = [
                {result: "full", text: "You already have 256 channels of your own. Remove one first."},
                {result: "trunk-scan", text: "Channel editing is available when one system is running."},
                {result: "dropped", text: "Listening stopped before the change was made."},
                {result: "search", text: "Channels can't be changed while Signal search is running."},
                {result: "other-system", text: "This channel belongs to another system now, so it can't be changed here."},
                {result: "no-dmr", text: "This session isn't decoding DMR now, so channels can't be changed."},
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

        // Review gate b-r5 U7: a stale refusal closes the editor that sent the edit, and the sheet says so, read out
        // once. Opened again, the editor shows the channel as it is now, and an edit is sent against that.
        function test_stale_closes_the_editor() {
            openSheet(view({}));
            var eighteen = row("channelsChannelRow", 18);
            eighteen.forceActiveFocus();
            tap(eighteen);
            tryCompare(edit, "visible", true);
            type(findChild(edit, "channelEditFrequency"), "451.0250");
            tap(findChild(edit, "channelEditSave"));
            compare(lastRequest().generation, 3);
            // Meanwhile channel 18 became one of yours, on a new network generation and session.
            var moved = view({generation: 4, session: 6, revision: 12});
            moved.channels[0] = {lpcn: 18, origin: 2, hz: 451050000, baseHz: 451012500};
            testContext.setDmrChannelsView(moved);
            var before = sheet.announcements;
            commands.answerDmrChannel({result: "stale"});
            tryCompare(edit, "visible", false);
            var text = "This channel changed while you were editing it. Open it again to see the current value.";
            compare(item("channelsStatus").text, text);
            verify(item("channelsStatus").visible);
            compare(sheet.announced, text);
            compare(sheet.announcements, before + 1, "read out once");
            wait(50);
            compare(sheet.announcements, before + 1, "and only once");
            compare(sheet.followLpcn, 0, "a refusal moves nothing");
            tap(row("channelsChannelRow", 18));
            tryCompare(edit, "visible", true);
            compare(findChild(edit, "channelEditFrequency").text, "451.0500", "opened again: the channel as it is now");
            verify(findChild(edit, "channelEditRemove").visible, "and the actions of its origin");
            compare(findChild(edit, "channelEditMessage").text, "");
            type(findChild(edit, "channelEditFrequency"), "451.0250");
            tap(findChild(edit, "channelEditSave"));
            var retry = lastRequest();
            compare(retry.lpcn, 18);
            compare(retry.hz, 451025000);
            compare(retry.expectedHz, 451050000);
            compare(retry.expectedOrigin, 2);
            compare(retry.generation, 4);
            compare(retry.session, "6");
        }

        // Review gate b-r5 U6: Add channel expects the channel its number names as it was when the number was typed. A
        // frequency the decoder learns for it while the frequency is typed is not sent as the expected one (the decoder
        // then refuses the edit as stale); a number typed again reads the channel again.
        function test_add_expects_the_channel_as_typed() {
            openSheet(view({}));
            tap(item("channelsAddButton"));
            tryCompare(edit, "visible", true);
            type(findChild(edit, "channelEditNumber"), "40");
            var learned = view({revision: 12});
            learned.extra = [];
            learned.channels.push({lpcn: 40, origin: 3, hz: 451487500});
            testContext.setDmrChannelsView(learned);
            wait(20);
            type(findChild(edit, "channelEditFrequency"), "451.5000");
            tap(findChild(edit, "channelEditSave"));
            var sent = lastRequest();
            compare(sent.lpcn, 40);
            compare(sent.expectedHz, 0, "the channel as it was when 40 was typed: no frequency");
            compare(sent.expectedOrigin, 0);
            commands.answerDmrChannel({result: "stale"});
            tryCompare(edit, "visible", false);

            tap(item("channelsAddButton"));
            tryCompare(edit, "visible", true);
            type(findChild(edit, "channelEditNumber"), "4");
            type(findChild(edit, "channelEditNumber"), "40");
            type(findChild(edit, "channelEditFrequency"), "451.5000");
            tap(findChild(edit, "channelEditSave"));
            sent = lastRequest();
            compare(sent.expectedHz, 451487500, "typed again: the channel as it is now");
            compare(sent.expectedOrigin, 3);
        }

        // Review gate b-r6: Add channel starts with no frequency, whatever channel the editor showed before. A new
        // number alone then sends nothing: the previous channel's frequency is never saved for it.
        function test_add_starts_with_no_frequency() {
            openSheet(view({}));
            tap(row("channelsChannelRow", 18));
            tryCompare(edit, "visible", true);
            compare(findChild(edit, "channelEditFrequency").text, "451.0125");
            edit.requestClose();
            tryCompare(edit, "visible", false);
            tap(item("channelsAddButton"));
            tryCompare(edit, "visible", true);
            compare(findChild(edit, "channelEditNumber").text, "");
            compare(findChild(edit, "channelEditFrequency").text, "", "no frequency from channel 18");
            compare(edit.opened.lpcn, 0);
            type(findChild(edit, "channelEditNumber"), "30");
            verify(!findChild(edit, "channelEditSave").inputValid, "a number alone is no edit");
            tap(findChild(edit, "channelEditSave"));
            compare(commands.dmrChannelRequests().length, 0, "nothing sent");
        }

        // Review gate b-r4 S5: while the editor's own edit is due its fields take no typing, so nothing typed after
        // Apply is lost when the answer closes the editor; Close needs no question then. An editor opened on another
        // channel while an earlier edit is due takes typing, and Close asks before that goes.
        function test_fields_read_only_while_the_edit_is_due() {
            openSheet(view({}));
            tap(row("channelsChannelRow", 18));
            tryCompare(edit, "visible", true);
            var freq = findChild(edit, "channelEditFrequency");
            type(freq, "451.0250");
            tap(findChild(edit, "channelEditSave"));
            var first = lastRequest().request;
            verify(edit.pending && edit.sentPending);
            verify(freq.readOnly, "the frequency sent takes no typing");
            freq.forceActiveFocus();
            keyClick(Qt.Key_5);
            compare(freq.text, "451.0250", "nothing typed after Apply");
            verify(!findChild(edit, "channelEditUseCc").enabled);
            findChild(edit, "channelEditClose").activate();
            tryCompare(edit, "visible", false);
            verify(!findChild(edit, "discardDialogDiscard").visible, "Close does not ask: the edit is on its way");
            // Another channel's editor while that edit is due.
            tap(row("channelsChannelRow", 22));
            tryCompare(edit, "visible", true);
            verify(edit.pending && !edit.sentPending);
            var other = findChild(edit, "channelEditFrequency");
            verify(!other.readOnly, "not this editor's edit: typing is kept");
            type(other, "451.3125");
            verify(!findChild(edit, "channelEditSave").enabled, "but nothing more is sent");
            findChild(edit, "channelEditClose").activate();
            var discard = findChild(edit, "discardDialogDiscard");
            tryVerify(function () { return discard.visible; }, 2000, "Close asks before what was typed goes");
            findChild(edit, "discardDialogKeep").activate();
            tryVerify(function () { return !discard.visible; });
            verify(edit.visible);
            commands.answerDmrChannel({result: "ok", request: first});
            verify(edit.visible, "the earlier edit's answer leaves this editor open");
            compare(other.text, "451.3125");
            verify(!other.readOnly && findChild(edit, "channelEditSave").enabled);
            // Its own edit refused: the fields take typing again.
            tap(findChild(edit, "channelEditSave"));
            verify(other.readOnly);
            commands.answerDmrChannel({result: "full"});
            tryVerify(function () { return !other.readOnly; });
            other.forceActiveFocus();
            other.cursorPosition = other.text.length;
            keyClick(Qt.Key_0);
            compare(other.text, "451.31250");
        }

        // Review gate b-r4 S10: a live mode change left DMR off. The open sheet lists no rows and the open editor
        // sends nothing, each saying why, until DMR is decoded again.
        function test_not_decoding_dmr_is_read_only() {
            openSheet(view({}));
            tap(row("channelsChannelRow", 18));
            tryCompare(edit, "visible", true);
            var freq = findChild(edit, "channelEditFrequency");
            type(freq, "451.0250");
            var text = "This session isn't decoding DMR now, so channels can't be changed.";
            testContext.setDmrChannelsView(view({dmrDecoding: false}));
            var locked = findChild(edit, "channelEditLocked");
            tryCompare(locked, "visible", true);
            compare(locked.text, text);
            compare(edit.announced, text);
            verify(freq.readOnly, "the fields are read-only too");
            verify(!findChild(edit, "channelEditSave").enabled);
            verify(!findChild(edit, "channelEditHide").enabled);
            findChild(edit, "channelEditSave").activate();
            compare(commands.dmrChannelRequests().length, 0, "the editor sends nothing");
            findChild(edit, "channelEditClose").activate();
            var discard = findChild(edit, "discardDialogDiscard");
            tryVerify(function () { return discard.visible; });
            discard.activate();
            tryCompare(edit, "visible", false);
            var note = item("channelsReadOnlyNote");
            verify(note.visible);
            compare(note.text, text);
            tryVerify(function () { return rows("channelsMissingRow").length === 0; }, 2000, "no Tier III rows");
            compare(rows("channelsChannelRow").length, 0);
            verify(!item("channelsMissingLabel").visible);
            verify(!item("channelsMissingEmpty").visible);
            verify(!item("channelsAddButton").enabled);
            compare(sheet.submit(18, dmrChannels.opHide, 0, dmrChannels.channel(18), dmrChannels.generation, dmrChannels.session), 0);
            compare(commands.dmrChannelRequests().length, 0);
            testContext.setDmrChannelsView(view({}));
            tryVerify(function () { return row("channelsChannelRow", 18) !== null; });
            verify(!note.visible);
            verify(item("channelsAddButton").enabled);
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

        // The shared map row under one of the user's entries (what Restore shows) is not what DMR uses, and another
        // protocol's writer can empty or fill it: the sheet shows the frequency DMR uses, and an edit expects it (the
        // decoder's STALE check compares against it).
        function test_rows_show_the_frequency_dmr_uses() {
            var spec = view({});
            spec.channels[1] = {lpcn: 20, origin: 2, hz: 451137500, baseHz: 0};
            spec.channels[2] = {lpcn: 21, origin: 2, hidden: true, baseHz: 452000000};
            openSheet(spec);
            compare(row("channelsChannelRow", 20).words, "Channel 20 · 451.137500 MHz · yours");
            compare(row("channelsChannelRow", 21).words, "Channel 21 · hidden");
            tap(row("channelsChannelRow", 20));
            tryCompare(edit, "visible", true);
            compare(findChild(edit, "channelEditFrequency").text, "451.1375", "opens on the frequency DMR uses");
            type(findChild(edit, "channelEditFrequency"), "451.1500");
            tap(findChild(edit, "channelEditSave"));
            compare(lastRequest().expectedHz, 451137500);
            commands.answerDmrChannel({result: "ok"});
            tryCompare(edit, "visible", false);
            tap(row("channelsChannelRow", 21));
            tryCompare(edit, "visible", true);
            tap(findChild(edit, "channelEditRestore"));
            compare(lastRequest().op, dmrChannels.opRestore);
            compare(lastRequest().expectedHz, 0, "a hidden channel expects none, whatever the shared row holds");
        }

        // The ledger can hold every channel number: "No frequency" builds a page of them, and Show more adds the
        // next. The rows held are bounded by the pages shown, the count stays whole, and the sheet opens on the first
        // page again.
        function test_missing_are_paged() {
            var all = [];
            for (var lpcn = 1; lpcn <= 4094; ++lpcn) {
                if ([9, 12, 7].indexOf(lpcn) < 0)
                    all.push(lpcn);
            }
            openSheet(view({extra: all, channels: []}));
            compare(dmrChannels.missingCount, 4094, "the count is every channel");
            compare(rows("channelsMissingRow").length, 50);
            compare(allRows("channelsMissingRow").length, 50, "no row built past the page");
            compare(words("channelsMissingRow")[0], "Channel 9 (unchecked) · slot 1 · data · TG 77 · last heard just now",
                    "the most recently heard first");
            var more = item("channelsMissingShowMore");
            verify(more.visible);
            compare(more.text, "Show more channels without a frequency");
            tap(more);
            tryVerify(function () { return rows("channelsMissingRow").length === 100; });
            compare(allRows("channelsMissingRow").length, 100);
            // The ledger shrinks: the rows held stay within the pages shown.
            testContext.setDmrChannelsView(view({extra: [40], channels: []}));
            tryVerify(function () { return rows("channelsMissingRow").length === 4; });
            verify(allRows("channelsMissingRow").length <= 100);
            verify(!more.visible);
            sheet.visible = false;
            openSheet(view({extra: all, channels: []}));
            compare(rows("channelsMissingRow").length, 50, "opened on the first page again");
            compare(allRows("channelsMissingRow").length, 50, "and holding no rows past it");
        }

        // The last page of channels with no frequency takes its Show more with it: the focus goes on to the first
        // channel it added.
        function test_last_missing_page_moves_the_focus_on() {
            var extra = [];
            for (var lpcn = 100; lpcn < 158; ++lpcn)
                extra.push(lpcn);
            openSheet(view({extra: extra}));
            compare(dmrChannels.missingCount, 61);
            var more = item("channelsMissingShowMore");
            more.forceActiveFocus();
            more.activate();
            tryVerify(function () { return rows("channelsMissingRow").length === 61; });
            verify(!more.visible);
            // The 51st: 9, 12 and 7 heard most recently, then 100 on by number.
            tryVerify(function () { return row("channelsMissingRow", 147).activeFocus; }, 2000, "on the first one added");
        }

        // A channel given a frequency from "No frequency" can land past the Channels page shown: the page that holds
        // it is shown, and the focus follows it there.
        function test_follow_reveals_the_page_of_the_channel() {
            var spec = view({channels: manyChannels(80)});
            spec.missing = [{lpcn: 200, slot: 0, group: true, caller: 5001, recipient: 1234, agoMs: 1000}];
            spec.extra = [];
            openSheet(spec);
            var missing = row("channelsMissingRow", 200);
            missing.forceActiveFocus();
            tap(missing);
            tryCompare(edit, "visible", true);
            type(findChild(edit, "channelEditFrequency"), "452.5");
            tap(findChild(edit, "channelEditSave"));
            commands.answerDmrChannel({result: "ok", revision: 10});
            tryCompare(edit, "visible", false);
            var moved = view({revision: 10, channels: manyChannels(80).concat([{lpcn: 200, origin: 2, hz: 452500000}])});
            moved.missing = [];
            moved.extra = [];
            testContext.setDmrChannelsView(moved);
            tryVerify(function () { return row("channelsChannelRow", 200) !== null; }, 2000, "its page is shown");
            tryVerify(function () { return row("channelsChannelRow", 200).activeFocus; }, 2000, "the focus follows it");
            compare(sheet.followLpcn, 0);
            compare(rows("channelsChannelRow").length, 81);
        }

        // R9: a channel listed further on than the next page is not revealed: that would build every row before it at
        // once on a large map. The focus waits on the Channels heading, and only the pages shown are built.
        function test_follow_far_channel_stays_on_the_heading() {
            var spec = view({channels: manyChannels(300)});
            spec.missing = [{lpcn: 400, slot: 0, group: true, caller: 5001, recipient: 1234, agoMs: 1000}];
            spec.extra = [];
            openSheet(spec);
            var missing = row("channelsMissingRow", 400);
            missing.forceActiveFocus();
            tap(missing);
            tryCompare(edit, "visible", true);
            type(findChild(edit, "channelEditFrequency"), "452.5");
            tap(findChild(edit, "channelEditSave"));
            commands.answerDmrChannel({result: "ok", revision: 10});
            tryCompare(edit, "visible", false);
            var moved = view({revision: 10, channels: manyChannels(300).concat([{lpcn: 400, origin: 2, hz: 452500000}])});
            moved.missing = [];
            moved.extra = [];
            testContext.setDmrChannelsView(moved);
            var heading = item("channelsChannelsLabel");
            tryVerify(function () { return heading.activeFocus; }, 2000, "the focus waits on the Channels heading");
            compare(sheet.followLpcn, 0);
            compare(sheet.channelLimit, 50);
            wait(50);
            compare(allRows("channelsChannelRow").length, 50, "no page past the one shown is built");
            verify(heading.activeFocus);
        }

        // An earlier edit's answer is still handled while an editor is open on another channel, but the focus stays
        // in that editor, whether the record came with the answer or after the editor opened.
        function test_earlier_answer_keeps_the_open_editor_focus() {
            openSheet(view({}));
            tap(row("channelsChannelRow", 18));
            tryCompare(edit, "visible", true);
            type(findChild(edit, "channelEditFrequency"), "451.0250");
            tap(findChild(edit, "channelEditSave"));
            edit.visible = false;
            tap(row("channelsChannelRow", 22));
            tryCompare(edit, "visible", true);
            var freq = findChild(edit, "channelEditFrequency");
            freq.forceActiveFocus();
            tryVerify(function () { return freq.activeFocus; });
            commands.answerDmrChannel({result: "ok", revision: 10});
            testContext.setDmrChannelsView(view({revision: 10}));
            wait(50);
            compare(sheet.request, 0, "the answer is handled");
            verify(findChild(edit, "channelEditSave").enabled, "and the open editor can send");
            verify(edit.visible, "the newer editor stays");
            verify(freq.activeFocus, "and keeps the focus");
            edit.visible = false;

            // Answered with no editor open, the record still to come; an editor opened meanwhile keeps the focus.
            tap(row("channelsChannelRow", 22));
            tryCompare(edit, "visible", true);
            tap(findChild(edit, "channelEditHide"));
            commands.answerDmrChannel({result: "ok", revision: 11});
            tryCompare(edit, "visible", false);
            compare(sheet.followLpcn, 22, "waiting for the record");
            tap(row("channelsChannelRow", 23));
            tryCompare(edit, "visible", true);
            freq.forceActiveFocus();
            tryVerify(function () { return freq.activeFocus; });
            testContext.setDmrChannelsView(view({revision: 11}));
            wait(50);
            verify(freq.activeFocus, "the record coming later moves nothing either");
            compare(sheet.followLpcn, 0);
        }

        // Review gate b-r5 U8: the focus follows an answer only when the editor that sent the edit is still open when it
        // comes. A late answer, after that editor closed (the sheet open, or closed and opened again), is handled and
        // saved, but the focus stays where the user has it now.
        function test_late_answer_leaves_the_focus() {
            openSheet(view({}));
            tap(row("channelsMissingRow", 40));
            tryCompare(edit, "visible", true);
            type(findChild(edit, "channelEditFrequency"), "451.5");
            tap(findChild(edit, "channelEditSave"));
            edit.visible = false;
            // The editor hands the focus back as it closes: the user then moves on to another row.
            wait(50);
            var twentyTwo = row("channelsChannelRow", 22);
            twentyTwo.forceActiveFocus();
            tryVerify(function () { return twentyTwo.activeFocus; });
            wait(50);
            verify(twentyTwo.activeFocus);
            commands.answerDmrChannel({result: "ok", revision: 10});
            compare(sheet.request, 0, "the answer is handled");
            compare(sheet.followLpcn, 0, "and not followed");
            // Channel 40 moves to the end of Channels: the rows before it stay as they are.
            var moved = view({revision: 10});
            moved.extra = [];
            moved.channels.push({lpcn: 40, origin: 2, hz: 451500000});
            testContext.setDmrChannelsView(moved);
            tryVerify(function () { return row("channelsChannelRow", 40) !== null; });
            wait(50);
            verify(row("channelsChannelRow", 22).activeFocus, "the focus stays on the row the user is on");

            // Sent, both closed, the sheet opened again and a row focused: the answer moves nothing either.
            tap(row("channelsChannelRow", 18));
            tryCompare(edit, "visible", true);
            type(findChild(edit, "channelEditFrequency"), "451.0250");
            tap(findChild(edit, "channelEditSave"));
            edit.visible = false;
            sheet.visible = false;
            sheet.open();
            tryCompare(sheet, "visible", true);
            testContext.setDmrChannelsView(moved);
            wait(50);
            var twentyThree = row("channelsChannelRow", 23);
            twentyThree.forceActiveFocus();
            tryVerify(function () { return twentyThree.activeFocus; });
            wait(50);
            verify(twentyThree.activeFocus);
            commands.answerDmrChannel({result: "ok", revision: 11});
            compare(sheet.followLpcn, 0);
            testContext.setDmrChannelsView(view({revision: 11}));
            wait(50);
            verify(row("channelsChannelRow", 23).activeFocus, "a new visit keeps its own focus");
            compare(item("channelsStatus").text, "", "kept on the saved system");
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
            // R8: while it is owed nothing more is changed (fail closed), so the saved list never takes a later edit
            // before it.
            var saveFirst = item("channelsSaveFirst");
            verify(saveFirst.visible);
            compare(saveFirst.text, "Save the earlier change first: Try saving again.");
            verify(!item("channelsAddButton").enabled);
            compare(row("channelsChannelRow", 22).Accessible.role, Accessible.StaticText);
            tap(row("channelsChannelRow", 22));
            wait(50);
            verify(!edit.visible, "a row opens no editor");
            compare(commands.dmrChannelRequests().length, 1);
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
            // Saved: edits are made again.
            verify(!saveFirst.visible);
            tap(row("channelsChannelRow", 22));
            tryCompare(edit, "visible", true);
            verify(findChild(edit, "channelEditHide").enabled);
        }

        // R8: an editor opened while an earlier edit was due goes read-only when that edit's save fails, and says why.
        function test_save_owed_locks_the_open_editor() {
            openSheet(view({}));
            tap(row("channelsChannelRow", 18));
            tryCompare(edit, "visible", true);
            type(findChild(edit, "channelEditFrequency"), "451.0250");
            tap(findChild(edit, "channelEditSave"));
            edit.visible = false;
            tap(row("channelsChannelRow", 22));
            tryCompare(edit, "visible", true);
            var locked = findChild(edit, "channelEditLocked");
            verify(!locked.visible);
            commands.answerDmrChannel({result: "ok", saved: false, saveError: "not-saved"});
            tryCompare(locked, "visible", true);
            compare(locked.text, "Save the earlier change first: Try saving again.");
            compare(edit.announced, locked.text);
            verify(!findChild(edit, "channelEditHide").enabled);
            verify(!findChild(edit, "channelEditSave").enabled);
            compare(sheet.submit(22, dmrChannels.opHide, 0, dmrChannels.channel(22), dmrChannels.generation, dmrChannels.session), 0,
                    "and the sheet sends nothing");
            compare(commands.dmrChannelRequests().length, 1);
            // Another system's save owed does not hold this one's edits.
            dmrChannels.sessionSystemUid = "99999999-2222-3333-4444-555555555555";
            verify(!locked.visible);
            verify(findChild(edit, "channelEditHide").enabled);
        }

        // R7: the carrier names another system (Connect Plus, P25 under Auto): its map is that system's. The open sheet
        // lists no Tier III rows and the open editor sends nothing, each saying why, until the receiver is back.
        function test_other_system_is_read_only() {
            openSheet(view({}));
            tap(row("channelsChannelRow", 18));
            tryCompare(edit, "visible", true);
            type(findChild(edit, "channelEditFrequency"), "451.0250");
            var text = "dsd-neo is on another system now, so Tier III channels can't be changed here.";
            testContext.setDmrChannelsView(view({otherSystem: true}));
            var locked = findChild(edit, "channelEditLocked");
            tryCompare(locked, "visible", true);
            compare(locked.text, text);
            compare(edit.announced, text);
            verify(!findChild(edit, "channelEditSave").enabled);
            verify(!findChild(edit, "channelEditHide").enabled);
            verify(!findChild(edit, "channelEditUseCc").enabled);
            findChild(edit, "channelEditSave").activate();
            compare(commands.dmrChannelRequests().length, 0, "the editor sends nothing");
            edit.visible = false;
            var note = item("channelsReadOnlyNote");
            verify(note.visible);
            compare(note.text, text);
            tryVerify(function () { return rows("channelsMissingRow").length === 0; }, 2000, "no Tier III rows");
            compare(rows("channelsChannelRow").length, 0);
            verify(!item("channelsMissingLabel").visible);
            verify(!item("channelsMissingEmpty").visible);
            verify(!item("channelsAddButton").enabled);
            compare(dmrChannels.channel(18).valid, false);
            compare(sheet.submit(18, dmrChannels.opHide, 0, dmrChannels.channel(18), dmrChannels.generation, dmrChannels.session), 0);
            compare(commands.dmrChannelRequests().length, 0);
            // Back on the Tier III system: the rows and the edits come back.
            testContext.setDmrChannelsView(view({}));
            tryVerify(function () { return row("channelsChannelRow", 18) !== null; });
            verify(!note.visible);
            verify(item("channelsAddButton").enabled);
            tap(row("channelsChannelRow", 18));
            tryCompare(edit, "visible", true);
            verify(!findChild(edit, "channelEditLocked").visible);
        }

        // R7: the focus on a row that went with the other system's carrier moves to the words that say why.
        function test_other_system_moves_the_focus_to_the_note() {
            openSheet(view({}));
            var eighteen = row("channelsChannelRow", 18);
            eighteen.forceActiveFocus();
            testContext.setDmrChannelsView(view({otherSystem: true}));
            var note = item("channelsReadOnlyNote");
            tryVerify(function () { return note.activeFocus; }, 2000, "the focus moves to the note");
            compare(sheet.announced, note.text);
        }

        // An edit the decoder holds (one made during a DMR voice call is applied when the call ends) says so after a
        // moment, in the editor that sent it or, closed, in the sheet; the answer takes the line away.
        function test_slow_edit_says_when_it_is_made() {
            sheet.slowRequestMs = 50;
            openSheet(view({}));
            tap(row("channelsChannelRow", 22));
            tryCompare(edit, "visible", true);
            var waiting = findChild(edit, "channelEditWaiting");
            tap(findChild(edit, "channelEditHide"));
            verify(!waiting.visible, "not at once");
            tryCompare(waiting, "visible", true);
            compare(waiting.text, "Saved when the current call ends.");
            compare(waiting.Accessible.role, Accessible.StaticText);
            compare(edit.announced, waiting.text);
            findChild(edit, "channelEditClose").activate();
            tryCompare(edit, "visible", false);
            var sheetWaiting = item("channelsWaiting");
            verify(sheetWaiting.visible, "the sheet says it once the editor is closed");
            commands.answerDmrChannel({result: "ok"});
            tryCompare(sheetWaiting, "visible", false);
            verify(!sheet.requestSlow);
            // Closed before the moment passed: the sheet says it, and reads it out.
            tap(row("channelsChannelRow", 23));
            tryCompare(edit, "visible", true);
            tap(findChild(edit, "channelEditHide"));
            findChild(edit, "channelEditClose").activate();
            tryCompare(edit, "visible", false);
            tryCompare(sheetWaiting, "visible", true);
            compare(sheet.announced, "Saved when the current call ends.");
            commands.answerDmrChannel({result: "ok"});
            tryCompare(sheetWaiting, "visible", false);
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

        // The Monitor keeps a way to the sheet while the live list holds channels of the user's, before the site
        // names itself (no Site row) and with every grant given a frequency (no NO FREQ): by key, tap and screen
        // reader. It gives way to NO FREQ, which opens the same sheet. The Site row opens its sheet from the keyboard
        // too.
        function test_monitor_channels_entry() {
            monitor.visible = true;
            var entry = monitorItem("monitorChannelsRow");
            var site = monitorItem("siteRow");
            verify(!entry.visible, "no channels of the user's");
            verify(!site.visible, "no site yet");
            testContext.setMetric("dmrUserChannels", true);
            tryCompare(entry, "visible", true);
            verify(entry.activeFocusOnTab, "reached with Tab");
            compare(entry.Accessible.role, Accessible.Button);
            compare(entry.Accessible.name, "Tier III channels");
            compare(monitorItem("monitorChannelsText").text, "Tier III channels ›");
            entry.forceActiveFocus();
            keyClick(Qt.Key_Return);
            tryCompare(monitorItem("channelsSheet"), "visible", true);
            monitorItem("channelsSheet").visible = false;
            entry.Accessible.pressAction();
            tryCompare(monitorItem("channelsSheet"), "visible", true);
            monitorItem("channelsSheet").visible = false;
            waitForRendering(entry);
            tap(entry);
            tryCompare(monitorItem("channelsSheet"), "visible", true);
            monitorItem("channelsSheet").visible = false;
            // One entry at a time: NO FREQ opens the same sheet.
            testContext.setMetric("noFreqCount", 2);
            tryCompare(monitorItem("noFreqRow"), "visible", true);
            tryCompare(entry, "visible", false);
            testContext.setMetric("noFreqCount", 0);
            tryCompare(entry, "visible", true);
            testContext.setMetric("dmrUserChannels", false);
            tryCompare(entry, "visible", false);

            testContext.setMetric("siteLine", "DMR · CC 1");
            tryCompare(site, "visible", true);
            verify(site.activeFocusOnTab, "the Site row is reached with Tab");
            site.forceActiveFocus();
            keyClick(Qt.Key_Space);
            tryCompare(monitorItem("siteSheet"), "visible", true);
        }

        // Review gate b-r4 S8: a save owed for the session's system keeps the way to the sheet (its Try saving again)
        // whatever the live lists hold: the last channel of the user's removed, nothing without a frequency, no site.
        function test_monitor_route_while_a_save_is_owed() {
            monitor.visible = true;
            var entry = monitorItem("monitorChannelsRow");
            var channels = monitorItem("channelsSheet");
            verify(!entry.visible, "no channels of the user's, nothing owed");
            channels.owe({systemUid: dmrChannels.sessionSystemUid, lpcn: 20, op: dmrChannels.opRestore, hz: 0,
                          saved: false, saveError: "not-saved"});
            tryCompare(entry, "visible", true);
            entry.Accessible.pressAction();
            tryCompare(channels, "visible", true);
            verify(findChild(channels, "channelsRetrySave").visible, "the sheet it opens can save it");
            channels.visible = false;
            // NO FREQ opens the same sheet: one entry at a time.
            testContext.setMetric("noFreqCount", 1);
            tryCompare(entry, "visible", false);
            testContext.setMetric("noFreqCount", 0);
            tryCompare(entry, "visible", true);
            // Owed for another saved system, not this session's.
            dmrChannels.sessionSystemUid = "99999999-2222-3333-4444-555555555555";
            tryCompare(entry, "visible", false);
            dmrChannels.sessionSystemUid = "11111111-2222-3333-4444-555555555555";
            tryCompare(entry, "visible", true);
            channels.owe({systemUid: dmrChannels.sessionSystemUid, lpcn: 20, saved: true, saveError: ""});
            tryCompare(entry, "visible", false, 2000, "saved: the route goes with the live lists");
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
