// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest

Item {
    QtObject {
        id: library
        property int registrations: 0
        property int paths: 0
        property bool failRegister: false
        // Whether a file is at every export path (the decoder wrote it).
        property bool written: true
        // Whether a registered list is still listed in Imports (false: the user removed it since).
        property bool listed: true
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
        function talkgroupListWritten(path) {
            return written;
        }
        function rowForPath(path) {
            return listed && registered.indexOf(path) >= 0 ? registered.indexOf(path) : -1;
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
    Loader {
        id: loader
        source: uiDir + "/TalkgroupSaveFlow.qml"
    }
    TestCase {
        when: windowShown
        name: "TalkgroupSaveFlow"
        function init() {
            verify(loader.item !== null, "save flow must load");
            loader.item.library = library;
            loader.item.systems = systems;
            loader.item.bridge = bridge;
            loader.item.pending = null;
            loader.item.stalled = null;
            loader.item.abandoned = [];
            loader.item.owedRegistrations = [];
            loader.item.lastSequence = "5";
            loader.item.timeoutMs = 15000;
            loader.item.retryAvailable = false;
            loader.item.message = "";
            library.registrations = 0;
            library.paths = 0;
            library.failRegister = false;
            library.written = true;
            library.listed = true;
            library.registered = [];
            library.released = [];
            systems.updates = 0;
            systems.attempts = 0;
            systems.failSave = false;
            systems.hasList = false;
            bridge.accept = true;
            bridge.calls = 0;
        }
        function result(sequence, context, generation, path, success) {
            return {
                sequence: sequence,
                policyContext: context,
                policyGeneration: generation,
                path: path,
                success: success
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
            verify(loader.item.save("original", "9007199254740993", 8));
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
        }
        function test_failures_do_not_associate() {
            bridge.accept = false;
            verify(!loader.item.save("original", "7", 1));
            compare(loader.item.pending, null);
            // A request the queue refused will never be written: its name goes back.
            compare(library.released, ["/owned/list.csv"]);
            verify(loader.item.retryAvailable);
            bridge.accept = true;
            verify(loader.item.save("original", "7", 1));
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
                verify(loader.item.save("original", "7", 1));
                verify(loader.item.pending !== null);
                deliver([result(String(6 + round), "7", 1, "/owned/list.csv", true)]);
                compare(systems.updates, 1);
                compare(library.registrations, 1);
                compare(loader.item.pending, null);
                systems.updates = 0;
                library.registrations = 0;
            }
        }
        function test_missing_completion_offers_retry() {
            verify(loader.item.hasOwnProperty("timeoutMs"));
            loader.item.timeoutMs = 50;
            verify(loader.item.save("original", "7", 1));
            verify(!loader.item.save("original", "7", 1));
            tryCompare(loader.item, "retryAvailable", true);
            verify(loader.item.message.indexOf("Try again") >= 0);
            compare(library.registrations, 0);
            compare(systems.updates, 0);
            verify(loader.item.save("original", "7", 1));
            compare(loader.item.retryAvailable, false);
            // A late success for the abandoned destination does not complete this retry, and is not attached: the
            // decoder wrote that file, so it is kept in Imports.
            deliver([result("6", "7", 1, "/owned/list.csv", true)]);
            verify(loader.item.pending !== null);
            compare(systems.updates, 0);
            compare(library.registrations, 1);
            compare(library.registered, ["/owned/list.csv"]);
            deliver([result("7", "7", 1, loader.item.pending.path, false)]);
            compare(loader.item.pending, null);
            verify(loader.item.message.indexOf("Try again") >= 0);
            verify(loader.item.save("original", "7", 1));
            deliver([result("8", "7", 1, loader.item.pending.path, true)]);
            compare(systems.updates, 1);
            // The abandoned export and the retry that completed.
            compare(library.registrations, 2);
            compare(loader.item.abandoned.length, 0);
        }
        function test_abandoned_failure_releases_its_name() {
            loader.item.timeoutMs = 50;
            verify(loader.item.save("original", "7", 1));
            tryCompare(loader.item, "retryAvailable", true);
            compare(loader.item.abandoned.length, 1);
            deliver([result("6", "7", 1, "/owned/list.csv", false)]);
            compare(loader.item.abandoned.length, 0);
            compare(library.released, ["/owned/list.csv"]);
            compare(library.registrations, 0);
        }
        // Two completions read in one poll: the abandoned request's and the retry's. Each is handled.
        function test_two_completions_between_polls() {
            loader.item.timeoutMs = 50;
            verify(loader.item.save("original", "7", 1));
            tryCompare(loader.item, "retryAvailable", true);
            verify(loader.item.save("original", "7", 1));
            var retry = loader.item.pending.path;
            deliver([result("6", "7", 1, "/owned/list.csv", true), result("7", "7", 1, retry, true)]);
            compare(library.registered, ["/owned/list.csv", retry]);
            compare(systems.updates, 1);
            compare(systems.lastPath, retry);
            compare(loader.item.pending, null);
            compare(loader.item.message, "Talkgroup list saved");
        }
        // More completions than app-control keeps came between two polls, the request's among them.
        function test_missed_completion_is_reported() {
            verify(loader.item.save("original", "7", 1));
            // A gap below the request's own start says nothing about it.
            deliver([], "4");
            verify(loader.item.pending !== null);
            // The file is not there (yet): the request is reported and kept for a result still to come.
            library.written = false;
            deliver([result("80", "7", 1, "/other.csv", true)], "79");
            compare(loader.item.pending, null);
            verify(loader.item.message.indexOf("not received") >= 0);
            verify(loader.item.retryAvailable);
            compare(loader.item.abandoned.length, 1);
            compare(systems.attempts, 0);
            compare(library.registrations, 0);
            library.written = true;
            deliver([result("81", "7", 1, "/owned/list.csv", true)]);
            compare(library.registered, ["/owned/list.csv"]);
            compare(systems.updates, 0);
            compare(loader.item.abandoned.length, 0);
        }
        function test_missed_completion_with_its_file_is_kept_in_imports() {
            verify(loader.item.save("original", "7", 1));
            deliver([], "70");
            compare(loader.item.pending, null);
            compare(library.registered, ["/owned/list.csv"]);
            compare(loader.item.abandoned.length, 0);
            compare(systems.attempts, 0);
            verify(loader.item.message.indexOf("not received") >= 0);
            verify(loader.item.message.indexOf("Imports") >= 0);
        }
        // The file is there but Imports cannot record it now: it is owed, not waiting on a completion that was lost,
        // and the next poll records it. An abandoned request in the gap is treated the same way.
        function test_missed_completion_with_unrecorded_file_is_owed() {
            loader.item.timeoutMs = 50;
            verify(loader.item.save("original", "7", 1));
            tryCompare(loader.item, "retryAvailable", true);
            verify(loader.item.save("original", "7", 1));
            var retry = loader.item.pending.path;
            library.failRegister = true;
            deliver([], "70");
            compare(loader.item.pending, null);
            compare(loader.item.abandoned.length, 0);
            compare(loader.item.owedRegistrations, [retry, "/owned/list.csv"]);
            verify(loader.item.message.indexOf("adding it to Imports failed") >= 0);
            compare(systems.attempts, 0);
            library.failRegister = false;
            deliver([]);
            compare(library.registered, [retry, "/owned/list.csv"]);
            compare(loader.item.owedRegistrations, []);
            compare(systems.attempts, 0);
        }
        // B8: a failed save of the system keeps the export and offers that step again, not another export.
        function test_failed_system_save_retries_attachment_only() {
            systems.failSave = true;
            verify(loader.item.save("original", "7", 1, {
                freqMhz: "851.0125",
                groupCsvPath: ""
            }));
            deliver([result("6", "7", 1, "/owned/list.csv", true)]);
            compare(library.registrations, 1);
            compare(systems.updates, 0);
            verify(loader.item.stalled !== null);
            verify(loader.item.retryAvailable);
            verify(loader.item.message.indexOf("system could not be saved") >= 0);
            verify(loader.item.message !== "Talkgroup list saved");
            systems.failSave = false;
            verify(loader.item.save("ignored", "8", 2));
            compare(bridge.calls, 1);
            compare(library.paths, 1);
            compare(library.registrations, 1);
            compare(systems.updates, 1);
            compare(systems.lastPath, "/owned/list.csv");
            compare(loader.item.stalled, null);
            compare(loader.item.retryAvailable, false);
            compare(loader.item.message, "Talkgroup list saved");
        }
        // The attachment is retried in a later session, after the user removed the exported list in Imports (or the
        // file went): the system is left as it was, and no step or export is started for it.
        function test_attachment_retry_after_the_list_went_data() {
            return [{tag: "removed from Imports", written: false, listed: false},
                    {tag: "file gone, row kept", written: false, listed: true},
                    {tag: "row gone, file kept", written: true, listed: false}];
        }
        function test_attachment_retry_after_the_list_went(data) {
            systems.failSave = true;
            verify(loader.item.save("original", "7", 1, {
                freqMhz: "851.0125",
                groupCsvPath: ""
            }));
            deliver([result("6", "7", 1, "/owned/list.csv", true)]);
            verify(loader.item.stalled !== null);
            compare(systems.attempts, 1);
            systems.failSave = false;
            library.written = data.written;
            library.listed = data.listed;
            verify(!loader.item.save("original", "8", 2));
            compare(systems.attempts, 1, "a list no longer in Imports was attached");
            compare(systems.updates, 0);
            compare(bridge.calls, 1, "a new export was started");
            compare(loader.item.stalled, null);
            compare(loader.item.retryAvailable, false);
            compare(loader.item.message, "");
        }
        // The same for a registration retry: a file that went leaves nothing to add to Imports.
        function test_registration_retry_after_the_file_went() {
            library.failRegister = true;
            verify(loader.item.save("original", "7", 1));
            deliver([result("6", "7", 1, "/owned/list.csv", true)]);
            verify(loader.item.stalled !== null);
            library.failRegister = false;
            library.written = false;
            verify(!loader.item.save("original", "7", 1));
            compare(library.registrations, 0);
            compare(systems.attempts, 0);
            compare(bridge.calls, 1);
            compare(loader.item.stalled, null);
            compare(loader.item.retryAvailable, false);
        }
        function test_failed_registration_retries_registration_only() {
            library.failRegister = true;
            verify(loader.item.save("original", "7", 1));
            deliver([result("6", "7", 1, "/owned/list.csv", true)]);
            compare(systems.attempts, 0);
            verify(loader.item.stalled !== null);
            verify(loader.item.message.indexOf("Imports failed") >= 0);
            compare(library.released, []);
            library.failRegister = false;
            verify(loader.item.save("original", "7", 1));
            compare(bridge.calls, 1);
            compare(library.registered, ["/owned/list.csv"]);
            compare(systems.updates, 1);
            compare(loader.item.message, "Talkgroup list saved");
        }
        function test_refused_attachment_names_it() {
            verify(loader.item.save("original", "7", 1, {
                freqMhz: "changed",
                groupCsvPath: ""
            }));
            deliver([result("6", "7", 1, "/owned/list.csv", true)]);
            compare(library.registrations, 1);
            compare(systems.updates, 0);
            compare(loader.item.stalled, null);
            compare(loader.item.retryAvailable, false);
            verify(loader.item.message.indexOf("system association was not changed") >= 0);
            systems.hasList = true;
            verify(loader.item.save("original", "7", 2));
            deliver([result("7", "7", 2, loader.item.pending.path, true)]);
            compare(systems.updates, 0);
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
            verify(loader.item.save("deleted", "7", 1));
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
