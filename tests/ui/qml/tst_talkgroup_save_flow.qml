// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest

Item {
    QtObject {
        id: library
        property int registrations: 0
        property int paths: 0
        property bool failRegister: false
        property var registered: []
        property var released: []
        function newTalkgroupListPath() {
            paths++;
            return paths === 1 ? "/owned/list.csv" : "/owned/list-" + paths + ".csv";
        }
        function registerTalkgroupList(path) {
            if (failRegister)
                return false;
            registrations++;
            registered = registered.concat([path]);
            return true;
        }
        function releaseTalkgroupListPath(path) {
            released = released.concat([path]);
        }
    }
    QtObject {
        id: systems
        property int updates: 0
        property int attempts: 0
        property string lastPath: ""
        property bool failSave: false
        property bool hasList: false
        function tuningProvenanceFields() {
            return ["freqMhz", "groupCsvPath"];
        }
        // Only "original" exists; a tuning whose frequency changed since the request was captured is refused.
        function groupListAttachable(uid, tuning) {
            return uid === "original" && !hasList && !(tuning && tuning.freqMhz === "changed");
        }
        function attachGroupList(uid, path, tuning) {
            attempts++;
            if (!groupListAttachable(uid, tuning) || failSave)
                return false;
            updates++;
            lastPath = path;
            return true;
        }
    }
    QtObject {
        id: bridge
        property bool accept: true
        property int calls: 0
        function saveTalkgroupList(context, generation, path) {
            calls++;
            return accept;
        }
    }
    // The newest export result app-control holds when Save is pressed, as UiController reads it.
    QtObject {
        id: exportsNow
        property string sequence: "0"
        function talkgroupExportSequenceNow() {
            return sequence;
        }
    }
    Loader {
        id: loader
        source: uiDir + "/TalkgroupSaveFlow.qml"
    }
    TestCase {
        when: windowShown
        name: "TalkgroupSaveFlow"
        readonly property var running: ({
                freqMhz: "851.0125",
                groupCsvPath: ""
            })
        function init() {
            verify(loader.item !== null, "save flow must load");
            loader.item.library = library;
            loader.item.systems = systems;
            loader.item.bridge = bridge;
            loader.item.controller = exportsNow;
            exportsNow.sequence = "5";
            loader.item.pending = null;
            loader.item.lastSequence = "5";
            loader.item.timeoutMs = 15000;
            loader.item.retryAvailable = false;
            loader.item.unregisteredPath = "";
            loader.item.message = "";
            library.registrations = 0;
            library.paths = 0;
            library.failRegister = false;
            library.registered = [];
            library.released = [];
            systems.updates = 0;
            systems.attempts = 0;
            systems.failSave = false;
            systems.hasList = false;
            bridge.accept = true;
            bridge.calls = 0;
        }
        // A completion as the controller reads it; @p tuningMoved (false when left out) is the decoder's record that the
        // receiver's tuning had changed since the run began when the export ran.
        function result(sequence, context, generation, path, success, tuningMoved) {
            return {
                sequence: sequence,
                policyContext: context,
                policyGeneration: generation,
                path: path,
                success: success,
                tuningMoved: tuningMoved === true
            };
        }
        // One poll's batch: its completions, oldest first, and the newest sequence it could not read.
        function deliver(list, missedThrough) {
            loader.item.results = {
                results: list,
                missedThrough: missedThrough || "0"
            };
        }
        function test_waits_for_new_matching_result() {
            verify(loader.item.save("original", "9007199254740993", 8, running));
            compare(library.registrations, 0);
            compare(systems.updates, 0);
            verify(!loader.item.save("another", "7", 1));
            deliver([result("5", "9007199254740993", 8, "/owned/list.csv", true)]);
            compare(systems.updates, 0);
            deliver([result("6", "7", 8, "/owned/list.csv", true)]);
            compare(systems.updates, 0);
            deliver([result("7", "9007199254740993", 9, "/owned/list.csv", true)]);
            compare(systems.updates, 0);
            deliver([result("8", "9007199254740993", 8, "/other.csv", true)]);
            compare(systems.updates, 0);
            deliver([result("9", "9007199254740993", 8, "/owned/list.csv", true)]);
            compare(library.registrations, 1);
            compare(systems.updates, 1);
            compare(systems.lastPath, "/owned/list.csv");
            compare(loader.item.message, "Talkgroup list saved");
            deliver([result("10", "9007199254740993", 8, "/owned/list.csv", true)]);
            compare(systems.updates, 1);
            // The row holds the name from now on: nothing is given back.
            compare(library.released, []);
        }
        function test_failures_do_not_associate() {
            bridge.accept = false;
            verify(!loader.item.save("original", "7", 1, running));
            compare(loader.item.pending, null);
            // A request the queue refused will never be written: its name goes back.
            compare(library.released, ["/owned/list.csv"]);
            verify(loader.item.retryAvailable);
            bridge.accept = true;
            verify(loader.item.save("original", "7", 1, running));
            var path = loader.item.pending.path;
            deliver([result("6", "7", 1, path, false)]);
            compare(library.registrations, 0);
            compare(systems.updates, 0);
            compare(loader.item.pending, null);
            compare(library.released, ["/owned/list.csv", path]);
            verify(loader.item.message.indexOf("export failed") >= 0);
            verify(loader.item.retryAvailable);
        }
        // B5: the flow keeps no session state, so a session that ends while the export is out cannot drop it; the
        // completion read after the Stop still belongs to this request.
        function test_completion_between_polls_survives_terminal_edge() {
            verify(!loader.item.hasOwnProperty("sessionState"));
            for (var round = 0; round < 2; ++round) {
                library.paths = 0;
                verify(loader.item.save("original", "7", 1, running));
                verify(loader.item.pending !== null);
                deliver([result(String(6 + round), "7", 1, "/owned/list.csv", true)]);
                compare(systems.updates, 1);
                compare(library.registrations, 1);
                compare(loader.item.pending, null);
                systems.updates = 0;
                library.registrations = 0;
            }
        }
        // No completion in time: Try again replaces the request, whose name goes back and whose late result matches
        // nothing. Until then the request still takes its own result.
        function test_missing_completion_offers_retry() {
            verify(loader.item.hasOwnProperty("timeoutMs"));
            loader.item.timeoutMs = 50;
            verify(loader.item.save("original", "7", 1, running));
            verify(!loader.item.save("original", "7", 1, running));
            tryCompare(loader.item, "retryAvailable", true);
            verify(loader.item.message.indexOf("Try again") >= 0);
            compare(library.registrations, 0);
            compare(systems.updates, 0);
            verify(loader.item.save("original", "7", 1, running));
            compare(loader.item.retryAvailable, false);
            compare(library.released, ["/owned/list.csv"]);
            deliver([result("6", "7", 1, "/owned/list.csv", true)]);
            verify(loader.item.pending !== null);
            compare(systems.updates, 0);
            compare(library.registrations, 0);
            deliver([result("7", "7", 1, loader.item.pending.path, true)]);
            compare(loader.item.pending, null);
            compare(systems.updates, 1);
            compare(library.registered, ["/owned/list-2.csv"]);
        }
        function test_late_completion_after_the_timeout_still_finishes() {
            loader.item.timeoutMs = 50;
            verify(loader.item.save("original", "7", 1, running));
            tryCompare(loader.item, "retryAvailable", true);
            deliver([result("6", "7", 1, "/owned/list.csv", true)]);
            compare(loader.item.pending, null);
            compare(loader.item.retryAvailable, false);
            compare(systems.updates, 1);
            compare(loader.item.message, "Talkgroup list saved");
        }
        // Two completions read in one poll: another reader's and this request's.
        function test_two_completions_between_polls() {
            verify(loader.item.save("original", "7", 1, running));
            deliver([result("6", "3", 1, "/other.csv", true), result("7", "7", 1, "/owned/list.csv", true)]);
            compare(library.registered, ["/owned/list.csv"]);
            compare(systems.updates, 1);
            compare(loader.item.pending, null);
            compare(loader.item.message, "Talkgroup list saved");
        }
        // More completions than app-control keeps came between two polls, the request's maybe among them: it ends,
        // reported as not received, its name given back. A result for it read later matches nothing.
        function test_missed_completion_is_reported() {
            verify(loader.item.save("original", "7", 1, running));
            // A gap below the request's own start says nothing about it.
            deliver([], "4");
            verify(loader.item.pending !== null);
            deliver([result("80", "3", 1, "/other.csv", true)], "79");
            compare(loader.item.pending, null);
            compare(loader.item.message, "The export result was not received: too many exports finished at once.");
            compare(loader.item.retryAvailable, false);
            compare(library.released, ["/owned/list.csv"]);
            deliver([result("81", "7", 1, "/owned/list.csv", true)]);
            compare(library.registrations, 0);
            compare(systems.attempts, 0);
        }
        // Issue #903: more completions than app-control keeps finished before Save was pressed, none polled yet. The
        // request's baseline is the newest result held when it was queued, so a gap wholly before it cannot end it.
        function test_gap_before_the_request_keeps_it() {
            exportsNow.sequence = "90";
            verify(loader.item.save("original", "7", 1, running));
            deliver([result("90", "3", 1, "/other.csv", true)], "89");
            verify(loader.item.pending !== null, "a gap before the request ended it");
            deliver([result("91", "7", 1, "/owned/list.csv", true)]);
            compare(loader.item.pending, null);
            compare(library.registered, ["/owned/list.csv"]);
            compare(loader.item.message, "Talkgroup list saved");
        }
        // B8: a failed save of the system leaves the list in Imports and says so; the next Save is a new export.
        function test_failed_system_save_is_reported() {
            systems.failSave = true;
            verify(loader.item.save("original", "7", 1, running));
            deliver([result("6", "7", 1, "/owned/list.csv", true)]);
            compare(library.registrations, 1);
            compare(systems.updates, 0);
            compare(loader.item.pending, null);
            compare(loader.item.retryAvailable, false);
            compare(loader.item.message, "Talkgroup list saved in Imports, but the system could not be saved");
            compare(library.released, []);
            systems.failSave = false;
            verify(loader.item.save("original", "8", 2, running));
            compare(bridge.calls, 2, "Save did not export anew");
            compare(loader.item.pending.path, "/owned/list-2.csv");
        }
        // A failed registration leaves the exported file where it is and attaches nothing; the name goes back.
        function test_failed_registration_is_reported() {
            library.failRegister = true;
            verify(loader.item.save("original", "7", 1, running));
            deliver([result("6", "7", 1, "/owned/list.csv", true)]);
            compare(systems.attempts, 0);
            compare(loader.item.pending, null);
            compare(loader.item.retryAvailable, false);
            compare(loader.item.message, "Talkgroup list exported, but adding it to Imports failed; system association was not changed");
            compare(library.released, ["/owned/list.csv"]);
            // The exported file Imports does not have: a new Save may be made while the decoder's list is that file.
            compare(loader.item.unregisteredPath, "/owned/list.csv");
            library.failRegister = false;
            verify(loader.item.save("original", "7", 1, running));
            compare(loader.item.unregisteredPath, "");
        }
        function test_refused_attachment_names_it() {
            var changed = {
                freqMhz: "changed",
                groupCsvPath: ""
            };
            verify(loader.item.save("original", "7", 1, changed));
            deliver([result("6", "7", 1, "/owned/list.csv", true)]);
            compare(library.registrations, 1);
            compare(systems.updates, 0);
            compare(loader.item.retryAvailable, false);
            verify(loader.item.message.indexOf("system association was not changed") >= 0);
            systems.hasList = true;
            verify(loader.item.save("original", "7", 2, running));
            deliver([result("7", "7", 2, loader.item.pending.path, true)]);
            compare(systems.updates, 0);
            verify(loader.item.message.indexOf("system association was not changed") >= 0);
        }
        // The decoder ran the export after the receiver's tuning changed live (its result says so), or its result says
        // nothing of the tuning: the list is not the start tuning's, and stays in Imports.
        function test_export_after_a_tuning_change_is_refused_data() {
            return [{tag: "tuning moved", moved: true}, {tag: "no tuning record", moved: undefined}];
        }
        function test_export_after_a_tuning_change_is_refused(data) {
            verify(loader.item.save("original", "7", 1, running));
            var completion = result("6", "7", 1, "/owned/list.csv", true, true);
            if (data.moved === undefined)
                delete completion.tuningMoved;
            deliver([completion]);
            compare(library.registrations, 1);
            compare(systems.attempts, 0);
            verify(loader.item.message.indexOf("system association was not changed") >= 0);
        }
        // S2: an Explore session (or a scan list, or a hold) has no saved system: the list stays in Imports.
        function test_explore_export_stays_in_imports() {
            verify(loader.item.save("", "7", 1));
            deliver([result("6", "7", 1, "/owned/list.csv", true)]);
            compare(library.registrations, 1);
            compare(systems.attempts, 0);
            compare(loader.item.message, "Talkgroup list saved in Imports; system association was not changed");
        }
        function test_deleted_system_does_not_modify_replacement_row() {
            verify(loader.item.save("deleted", "7", 1, running));
            deliver([result("6", "7", 1, "/owned/list.csv", true)]);
            compare(library.registrations, 1);
            compare(systems.updates, 0);
        }
        function test_tuning_of_reads_the_provenance_fields() {
            var tuning = loader.item.tuningOf({
                freqMhz: "851.0125",
                groupCsvPath: "",
                name: "Ignored"
            });
            compare(Object.keys(tuning).sort(), ["freqMhz", "groupCsvPath"]);
            compare(tuning.freqMhz, "851.0125");
        }
    }
}
