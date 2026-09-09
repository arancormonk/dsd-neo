// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest

Item {
    QtObject {
        id: library
        property int registrations: 0
        property int paths: 0
        function newTalkgroupListPath() {
            paths++;
            return paths === 1 ? "/owned/list.csv" : "/owned/list-" + paths + ".csv";
        }
        function registerTalkgroupList(path) {
            registrations++;
            return true;
        }
    }
    QtObject {
        id: systems
        property int updates: 0
        property string lastPath: ""
        function rowForUid(uid) {
            return uid === "original" ? 3 : -1;
        }
        function get(row) {
            return {
                groupCsvPath: ""
            };
        }
        function update(row, fields) {
            if (row === 3) {
                updates++;
                lastPath = fields.groupCsvPath;
            }
        }
    }
    QtObject {
        id: bridge
        property bool accept: true
        function saveTalkgroupList(context, generation, path) {
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
            loader.item.result = {
                sequence: "5"
            };
            library.registrations = 0;
            library.paths = 0;
            loader.item.sessionState = 2;
            loader.item.timeoutMs = 15000;
            loader.item.retryAvailable = false;
            systems.updates = 0;
            bridge.accept = true;
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
        function test_waits_for_new_matching_result() {
            verify(loader.item.save("original", "9007199254740993", 8));
            compare(library.registrations, 0);
            compare(systems.updates, 0);
            verify(!loader.item.save("another", "7", 1));
            loader.item.result = result("5", "9007199254740993", 8, "/owned/list.csv", true);
            compare(systems.updates, 0);
            loader.item.result = result("6", "7", 8, "/owned/list.csv", true);
            compare(systems.updates, 0);
            loader.item.result = result("7", "9007199254740993", 9, "/owned/list.csv", true);
            compare(systems.updates, 0);
            loader.item.result = result("8", "9007199254740993", 8, "/other.csv", true);
            compare(systems.updates, 0);
            loader.item.result = result("9", "9007199254740993", 8, "/owned/list.csv", true);
            compare(library.registrations, 1);
            compare(systems.updates, 1);
            compare(systems.lastPath, "/owned/list.csv");
            loader.item.result = result("10", "9007199254740993", 8, "/owned/list.csv", true);
            compare(systems.updates, 1);
        }
        function test_failures_do_not_associate() {
            bridge.accept = false;
            verify(!loader.item.save("original", "7", 1));
            compare(loader.item.pending, null);
            bridge.accept = true;
            verify(loader.item.save("original", "7", 1));
            loader.item.result = result("6", "7", 1, loader.item.pending.path, false);
            compare(library.registrations, 0);
            compare(systems.updates, 0);
            compare(loader.item.pending, null);
        }
        function test_session_edges_recover_missing_completion() {
            verify(loader.item.hasOwnProperty("sessionState"));
            for (var phase of [0, 4]) {
                library.paths = 0;
                loader.item.sessionState = 2;
                verify(loader.item.save("original", "7", 1));
                loader.item.sessionState = phase;
                compare(loader.item.pending, null);
                verify(loader.item.message.length > 0);
                loader.item.result = result("6", "7", 1, "/owned/list.csv", true);
                compare(systems.updates, 0);
                compare(library.registrations, 0);
                loader.item.result = {
                    sequence: "5"
                };
            }
        }
        function test_missing_completion_offers_retry() {
            verify(loader.item.hasOwnProperty("timeoutMs"));
            loader.item.timeoutMs = 50;
            loader.item.sessionState = 2;
            verify(loader.item.save("original", "7", 1));
            verify(!loader.item.save("original", "7", 1));
            tryCompare(loader.item, "retryAvailable", true);
            verify(loader.item.message.indexOf("Try again") >= 0);
            compare(library.registrations, 0);
            compare(systems.updates, 0);
            verify(loader.item.save("original", "7", 1));
            compare(loader.item.retryAvailable, false);
            // A late success for the abandoned destination must not complete this retry.
            loader.item.result = result("6", "7", 1, "/owned/list.csv", true);
            verify(loader.item.pending !== null);
            compare(systems.updates, 0);
            loader.item.result = result("7", "7", 1, loader.item.pending.path, false);
            compare(loader.item.pending, null);
            verify(loader.item.message.indexOf("Try again") >= 0);
            verify(loader.item.save("original", "7", 1));
            loader.item.result = result("8", "7", 1, loader.item.pending.path, true);
            compare(systems.updates, 1);
            compare(library.registrations, 1);
        }
        function test_deleted_system_does_not_modify_replacement_row() {
            verify(loader.item.save("deleted", "7", 1));
            loader.item.result = result("6", "7", 1, "/owned/list.csv", true);
            compare(library.registrations, 1);
            compare(systems.updates, 0);
        }
    }
}
