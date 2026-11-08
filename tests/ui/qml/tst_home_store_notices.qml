// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// Issue #828: Home's store notice card, over the production `storeNotices` list. Notices arrive as the app's do: json_store
// posts them from a worker thread (testContext.postStoreNotice()), the Signal search model hands over its store's files
// (testContext.setSignalSearchStoreFiles()). The list is process-wide and outlives every engine, so each case dismisses
// what it posted, and a later post of the same notice stays dismissed: each case posts notices no earlier one did.
Item {
    id: root
    width: 360
    height: 800

    Loader {
        id: loader
        anchors.fill: parent
        sourceComponent: homeComponent
    }
    Component {
        id: homeComponent
        Ui.HomeScreen {
            localDeviceRelevant: false
        }
    }
    // A sheet over Home, as any of Main's would be.
    Ui.ModalSheet {
        id: cover
        anchors.fill: parent
    }

    TestCase {
        name: "HomeStoreNotices"
        when: windowShown

        function card() {
            var found = findChild(loader.item, "storeNoticeCard");
            verify(found !== null, "the card exists");
            return found;
        }
        function texts(name) {
            var out = [];
            var c = card();
            for (var i = 0; i < 64; ++i) {
                var item = findChild(c, name);
                if (!item)
                    break;
                out.push(item.text);
                item.objectName = name + "-read";
            }
            for (var j = 0; j < 64; ++j) {
                var renamed = findChild(c, name + "-read");
                if (!renamed)
                    break;
                renamed.objectName = name;
            }
            return out;
        }
        function waitForNotices(count) {
            tryCompare(storeNotices.notices, "length", count);
            tryCompare(card(), "visible", count > 0);
        }
        function init() {
            loader.active = true;
            cover.visible = false;
            Ui.Theme.resetFontScale();
            testContext.setSignalSearchStoreFiles([]);
        }
        function cleanup() {
            cover.visible = false;
            var files = storeNotices.notices.map(function (notice) {
                return notice.file;
            });
            for (var file of files)
                storeNotices.dismiss(file);
            testContext.setSignalSearchStoreFiles([]);
            Ui.Theme.resetFontScale();
            compare(JSON.stringify(storeNotices.notices), "[]", "each case leaves nothing shown");
        }

        function test_a_hidden_without_notices() {
            verify(!card().visible);
        }

        // Each store is named as the backup names it; call history's settings file follows the application's name.
        function test_b_labels_per_store() {
            var files = ["saved_systems.json", "scan_lists.json", "imported_files.json", "decryption_profiles.json",
                         "signal-search-ranges.json", "call_history.json", "dsd-neo-app.ini", Qt.application.name + ".conf"];
            for (var file of files)
                testContext.postStoreNotice(file, "");
            waitForNotices(files.length);
            compare(texts("storeNoticeTitle"), ["Saved systems couldn't be read completely", "Scan lists couldn't be read completely",
                                               "Imported files couldn't be read completely", "Decryption profiles couldn't be read completely",
                                               "Signal search ranges couldn't be read completely", "Call history couldn't be read completely",
                                               "Settings couldn't be read completely", "Call history settings couldn't be read completely"]);
            for (var detail of texts("storeNoticeDetail"))
                compare(detail, "Some entries may be missing. The file won't be overwritten.");
        }

        // The seen store is the app's own bookkeeping: logged, never on the card.
        function test_c_seen_store_is_not_shown() {
            testContext.postStoreNotice("call_history_seen.json", "");
            testContext.postStoreNotice("scan_lists.json", "scan_lists.json.unreadable-1");
            waitForNotices(1);
            compare(storeNotices.notices[0].file, "scan_lists.json");
        }

        // Once moved, the card names the kept file, and a dismissed notice that changes shows again.
        function test_d_kept_name_after_the_move() {
            testContext.postStoreNotice("saved_systems.json", "saved_systems.json.unreadable-1");
            waitForNotices(1);
            compare(texts("storeNoticeTitle"), ["Saved systems couldn't be read completely"]);
            compare(texts("storeNoticeDetail"), ["Some entries may be missing. The file was kept as saved_systems.json.unreadable-1."]);
            compare(card().Accessible.name, "Saved systems couldn't be read completely. Some entries may be missing. The file was kept as saved_systems.json.unreadable-1.");
            // Unreadable again after a clean save: the copy kept before is still named.
            testContext.postStoreNotice("saved_systems.json", "");
            tryVerify(function () {
                return texts("storeNoticeDetail")[0] === "Some entries may be missing. The file won't be overwritten. An earlier copy was kept as saved_systems.json.unreadable-1.";
            }, 2000, "the earlier copy is still named");
        }

        // The Signal search store's files, as its model reports them; version 1's journal is never moved.
        function test_e_signal_search_files() {
            testContext.setSignalSearchStoreFiles([
                {file: "signal-search-history.v2", state: "kept", kept: "signal-search-history.v2.unreadable-1"},
                {file: "signal-search-history.v1.avoids.json", state: "unreadable", kept: ""},
                {file: "signal-search-history.v1", state: "leftInPlace", kept: ""},
                {file: "signal-search-ranges.json", state: "", kept: ""}]);
            waitForNotices(3);
            compare(texts("storeNoticeTitle"), ["Signal search history couldn't be read completely", "Signal search avoids couldn't be read completely",
                                               "Signal search history from an older version couldn't be read"]);
            compare(texts("storeNoticeDetail"), ["Some entries may be missing. The file was kept as signal-search-history.v2.unreadable-1.",
                                                "Some entries may be missing. The file won't be overwritten.", "It was left in place."]);
        }

        // Dismissed in one engine's Home, still dismissed in the next: the list is not the screen's.
        function test_f_dismiss_survives_a_reload() {
            testContext.postStoreNotice("decryption_profiles.json", "decryption_profiles.json.unreadable-1");
            waitForNotices(1);
            var dismiss = findChild(card(), "storeNoticeDismiss");
            verify(dismiss.height >= Ui.Theme.minimumTouchSize);
            waitForRendering(loader.item);
            var at = dismiss.mapToItem(root, dismiss.width / 2, dismiss.height / 2);
            verify(at.y > 0 && at.y < root.height, "the button is on screen");
            mouseClick(root, at.x, at.y);
            waitForNotices(0);
            loader.active = false;
            loader.active = true;
            verify(loader.item !== null);
            waitForRendering(loader.item);
            verify(!card().visible, "a rebuilt Home shows nothing dismissed");
            // The same notice again stays dismissed; the file kept aside once more shows again.
            testContext.postStoreNotice("decryption_profiles.json", "decryption_profiles.json.unreadable-1");
            wait(20);
            compare(storeNotices.notices.length, 0);
            testContext.postStoreNotice("decryption_profiles.json", "decryption_profiles.json.unreadable-2");
            waitForNotices(1);
        }

        // One alert, named by all it says and heard once per notice; out of the tree while a sheet covers Home.
        function test_g_alert_gated_and_announced_once() {
            var c = card();
            var before = c.announcements;
            testContext.postStoreNotice("imported_files.json", "imported_files.json.unreadable-1");
            waitForNotices(1);
            compare(c.Accessible.role, Accessible.AlertMessage);
            verify(!c.Accessible.ignored);
            tryCompare(c, "announcements", before + 1);
            testContext.postStoreNotice("signal-search-ranges.json", "signal-search-ranges.json.unreadable-1");
            waitForNotices(2);
            tryCompare(c, "announcements", before + 2);
            compare(c.Accessible.name, "Imported files couldn't be read completely. Some entries may be missing. The file was kept as imported_files.json.unreadable-1. "
                    + "Signal search ranges couldn't be read completely. Some entries may be missing. The file was kept as signal-search-ranges.json.unreadable-1.");
            storeNotices.dismiss("signal-search-ranges.json");
            waitForNotices(1);
            wait(20);
            compare(c.announcements, before + 2, "a dismissal repeats nothing");
            cover.visible = true;
            tryCompare(c, "navigationAllowed", false);
            verify(c.Accessible.ignored, "covered, the card leaves the tree");
            verify(findChild(c, "storeNoticeDismiss").Accessible.ignored, "and so does its button");
            cover.visible = false;
            tryCompare(c, "navigationAllowed", true);
            verify(!c.Accessible.ignored);
        }

        // 360 px with the largest text: every line wraps inside the card, and the card holds them all.
        function test_h_wraps_at_large_text() {
            Ui.Theme.fontScale = 1.6;
            testContext.postStoreNotice("signal-search-ranges.json", "signal-search-ranges.json.unreadable-12");
            waitForNotices(1);
            waitForRendering(loader.item);
            var c = card();
            verify(c.width <= root.width);
            for (var name of ["storeNoticeTitle", "storeNoticeDetail"]) {
                var text = findChild(c, name);
                verify(text.lineCount > 1, name + " wraps");
                verify(text.contentWidth <= text.width + 0.5, name + " fits its width");
                var bottom = text.mapToItem(c, 0, text.height).y;
                verify(bottom <= c.height, name + " sits inside the card");
            }
        }
    }
}
