// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// Issue #831: a file another app opened with DSD-neo, or shared to it, through
// Main's own bindings. The host offers it (testContext.emitIncomingDocument(),
// as Android's poll does); Main takes it once nothing on screen has state to
// lose; the import worker tells a backup from anything else (the production
// probe over a local file, held by testContext.holdImports()); and the sheet
// starts Restore, or asks what the file contains and imports it.
Item {
    width: 420
    height: 900
    Loader {
        id: appLoader
        anchors.fill: parent
    }
    // A flow with no BackupFlow to take its words from.
    Component {
        id: bareFlow
        Ui.IncomingDocumentFlow {
            anchors.fill: parent
        }
    }

    TestCase {
        name: "IncomingDocument"
        when: windowShown
        property var app: null
        property var flow: null
        property var sheet: null
        property bool savedOnboarding: false
        property bool savedAutoStart: false

        function init() {
            // The intake's bindings and Main's routing run in every case: any warning is a defect.
            failOnWarning(/./);
            testContext.useLifecycleHost(true);
            testContext.resetBackup();
            testContext.resetHostPlayback();
            testContext.setLifecyclePhase(0);
            savedOnboarding = prefs.onboardingDone;
            savedAutoStart = prefs.autoStartOnAttach;
            prefs.onboardingDone = true;
            prefs.autoStartOnAttach = false;
            loadMain();
        }

        function loadMain() {
            appLoader.source = uiDir + "/Main.qml";
            app = appLoader.item;
            verify(app !== null);
            flow = findChild(app, "incomingDocumentFlow");
            sheet = findChild(app, "incomingDocumentSheet");
            verify(flow !== null);
            verify(sheet !== null);
            app.requestActivate();
            tryCompare(app, "active", true);
        }

        function cleanup() {
            testContext.holdImports(false);
            testContext.holdRecordingTransfers(false);
            if (flow && flow.busy && flow.phase !== "handingOff")
                flow.drop();
            tryVerify(function () {
                return !flow || !flow.busy;
            }, 5000, "the intake is idle before Main goes");
            tryCompare(importedFiles, "importing", false);
            Ui.Navigation.clearInput(app);
            for (var modal of Ui.Navigation.modals.slice())
                modal.visible = false;
            appLoader.source = "";
            app = null;
            flow = null;
            sheet = null;
            // Idle first: a Target CSV row will not go while a session runs.
            testContext.setLifecyclePhase(0);
            while (importedFiles.count)
                importedFiles.remove(0);
            while (savedSystems.count)
                savedSystems.remove(0);
            while (scanLists.count)
                scanLists.remove(0);
            testContext.resetBackup();
            testContext.resetHostPlayback();
            testContext.setDelayedStop(false);
            testContext.setRadioReference("busy", false);
            testContext.setLocalDeviceRecord("usb", false, "");
            testContext.setAttachedDeviceSource("");
            prefs.onboardingDone = savedOnboarding;
            prefs.autoStartOnAttach = savedAutoStart;
            prefs.lastStartedKind = "";
            prefs.lastStartedUid = "";
            testContext.useLifecycleHost(false);
        }

        // ---- fixtures ----

        function zeros(n) {
            return "\u0000".repeat(n);
        }
        // A sealed backup's 64-byte header (backup_seal.c) and a 16-byte tag:
        // with an empty payload, 80 bytes is the whole of a well-formed one.
        function backupText(version, log2n, length) {
            return "DSDNEOBK" + "\u0000" + String.fromCharCode(version) + "\u0001" + String.fromCharCode(log2n) + "\u0000\u0000\u0000\u0008" + "\u0000\u0000\u0000\u0001" + zeros(16) + zeros(12) + length + zeros(8) + "t".repeat(16);
        }
        function write(name, text) {
            var path = testContext.writeFixtureCsv(name, text);
            verify(path.length > 0, name);
            return path;
        }
        function backupFile(name) {
            return write(name || "phone.dsdbak", backupText(1, 14, zeros(8)));
        }
        function groupsFile(name) {
            return write(name || "groups.csv", "DEC,Mode,Name,Tag\n100,A,One,Law\n200,A,Two,Fire\n");
        }
        function channelsFile(name) {
            return write(name || "channels.csv", "channel,frequency\n1,851012500\n2,852012500\n");
        }
        function targetsFile(name) {
            return write(name || "targets.csv", "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes\none,p25-conventional,851500000,,,,\ntwo,p25-conventional,852500000,,,,\n");
        }

        function part(name) {
            var found = findChild(sheet, name);
            verify(found !== null, name);
            return found;
        }
        // The buttons the sheet offers now, by accessible name.
        function buttons(item) {
            var names = [];
            for (var child of item.children || []) {
                if (!child.visible)
                    continue;
                if (child.Accessible.role === Accessible.Button)
                    names.push(child.Accessible.name);
                names = names.concat(buttons(child));
            }
            return names;
        }
        // Offer @p path and wait for the sheet to say what it is.
        function offer(path) {
            testContext.emitIncomingDocument(path);
            tryCompare(flow, "phase", "presenting", 5000);
            verify(sheet.visible);
        }
        function chooseKind(kind) {
            var pill = findChild(sheet, "importKind_" + kind);
            verify(pill !== null, kind);
            pill.clicked();
        }
        function backupCalls(name) {
            return testContext.backupCalls().filter(function (call) {
                return call.name === name;
            });
        }
        // Nothing taken for a while: the document still waits in the host's slot.
        function verifyWaits(why) {
            wait(60);
            verify(!flow.busy, why);
            verify(!sheet.visible, why);
            verify(decoderHost.incomingDocumentPending, why);
        }

        // ---- the sheet's states (spec §4.3) ----

        function test_checking() {
            testContext.holdImports(true);
            testContext.emitIncomingDocument(backupFile());
            tryCompare(sheet, "visible", true);
            compare(flow.phase, "probing");
            verify(flow.busy);
            verify(!decoderHost.incomingDocumentPending, "taken from the host");
            compare(part("incomingDocumentName").text, "This file");
            compare(part("incomingDocumentBody").text, "Checking the file…");
            verify(!part("incomingDocumentProblem").visible);
            compare(buttons(sheet), ["Cancel"]);
            testContext.holdImports(false);
            tryCompare(flow, "phase", "presenting");
            compare(part("incomingDocumentName").text, "phone.dsdbak");
        }

        function test_states_data() {
            return [
                {tag: "backup", text: backupText(1, 14, zeros(8)), name: "phone.dsdbak", body: "A DSD-neo backup. Next you enter its passphrase and see what it holds. Nothing changes until you confirm.", problem: "", buttons: ["Restore…", "Cancel"]},
                {tag: "newer backup", text: backupText(2, 14, zeros(8)), name: "newer.dsdbak", body: "", problem: "This backup was made by a newer DSD-neo. Update first.", buttons: ["Close"]},
                {tag: "damaged backup", text: backupText(1, 5, zeros(8)), name: "damaged.dsdbak", body: "", problem: "The backup is damaged.", buttons: ["Close"]},
                {tag: "too large backup", text: backupText(1, 14, "\u0000\u0000\u0000\u0000\u0004\u0000\u0000\u0001"), name: "large.dsdbak", body: "", problem: "This file is too large to be a DSD-neo backup.", buttons: ["Close"]},
                {tag: "other", text: "DEC,Mode,Name,Tag\n100,A,One,Law\n", name: "groups.csv", body: "", problem: "", buttons: ["Channel map", "Talkgroups", "Keys", "P25 band plan", "Radio IDs", "DMR key mappings", "Vertex keystreams", "Target CSV", "Import", "Cancel"]}
            ];
        }
        function test_states(data) {
            offer(write(data.name, data.text));
            compare(part("incomingDocumentName").text, data.name);
            compare(part("incomingDocumentBody").text, data.body);
            compare(part("incomingDocumentProblem").text, data.problem);
            compare(part("incomingDocumentProblem").visible, data.problem.length > 0);
            compare(buttons(sheet), data.buttons);
            compare(flow.announcements, data.problem.length ? 1 : 0, "a problem is read out once");
            if (data.problem.length)
                compare(flow.announced, data.problem);
        }

        function test_unreadable() {
            offer(testContext.storePath("fixtures/gone-831.csv"));
            compare(part("incomingDocumentName").text, "This file", "no name came with it");
            compare(part("incomingDocumentProblem").text, "Couldn't read that file. Open it again from the other app.");
            compare(buttons(sheet), ["Close"]);
            compare(flow.announcements, 1);
        }

        function test_other_asks_before_import() {
            offer(groupsFile());
            compare(findChild(sheet, "importKindPicker").parent.visible, true);
            var importButton = part("incomingDocumentImport");
            verify(!importButton.enabled, "nothing chosen yet");
            chooseKind("trunkTargets");
            verify(part("incomingDocumentTargetsHint").visible);
            compare(part("incomingDocumentTargetsHint").text, "Starts a new scan list with these targets.");
            chooseKind("group");
            verify(!part("incomingDocumentTargetsHint").visible);
            verify(importButton.enabled);
        }

        function test_backup_job_running() {
            offer(backupFile());
            var restore = part("incomingDocumentRestore");
            verify(restore.enabled);
            testContext.setBackup("busy", true);
            compare(part("incomingDocumentProblem").text, "Another backup task is still running. Try again in a moment.");
            verify(!restore.enabled);
            compare(buttons(sheet), ["Restore…", "Cancel"]);
            compare(flow.announcements, 1);
            testContext.setBackup("busy", false);
            verify(restore.enabled, "Restore once the job ends");
            verify(!part("incomingDocumentProblem").visible);
        }

        function test_backup_restore_pending_data() {
            return [
                {tag: "cancellable", cancellable: true, text: "A restore is already waiting. Cancel it in Settings → Backup, or close and reopen DSD-neo to finish it."},
                {tag: "part-way", cancellable: false, text: "This restore is part-way through and can't be cancelled. Close and reopen DSD-neo to finish it."}
            ];
        }
        function test_backup_restore_pending(data) {
            testContext.setBackup("restorePending", true);
            testContext.setBackup("restoreCancellable", data.cancellable);
            offer(backupFile());
            compare(part("incomingDocumentProblem").text, data.text);
            compare(buttons(sheet), ["Close"]);
            compare(part("incomingDocumentProblem").Accessible.role, Accessible.AlertMessage);
            compare(flow.announcements, 1);
            part("incomingDocumentCancel").clicked();
            verify(!flow.busy);
            verify(!sheet.visible);
            compare(backupCalls("setFlowActive").length, 0, "no restore started");
        }

        // D3: while a session runs, the sheet offers its stop; Restore waits
        // for it, and the sheet never stops it by itself.
        function test_backup_while_listening_data() {
            return [
                {tag: "listening", replay: false, stop: "Stop listening"},
                {tag: "replay", replay: true, stop: "Stop replay"}
            ];
        }
        function test_backup_while_listening(data) {
            testContext.setLifecyclePhase(2);
            if (data.replay)
                app.sessionSystem = {name: "Replay", sourceType: "file", filePath: "/tmp/a.wav"};
            testContext.setDelayedStop(true);
            var stops = decoderHost.stopCalls();
            offer(backupFile());
            compare(decoderHost.stopCalls(), stops, "nothing stops by itself");
            compare(part("incomingDocumentProblem").text, "Stop listening before restoring.");
            compare(buttons(sheet), [data.stop, "Restore…", "Cancel"]);
            var stop = part("incomingDocumentStop");
            verify(stop.enabled);
            verify(!part("incomingDocumentRestoreLater").enabled);
            compare(flow.announcements, 1);
            stop.clicked();
            compare(decoderHost.stopCalls(), stops + 1);
            compare(decoderHost.sessionState, 3);
            compare(stop.text, "Stopping…");
            verify(!stop.enabled);
            testContext.setLifecyclePhase(0);
            verify(!part("incomingDocumentProblem").visible);
            compare(buttons(sheet), ["Restore…", "Cancel"]);
            var restore = part("incomingDocumentRestore");
            verify(restore.enabled, "Restore once the session has stopped");
            restore.clicked();
            var backupFlow = findChild(app, "backupFlow");
            tryCompare(findChild(backupFlow, "backupUnlockSheet"), "visible", true);
            verify(!flow.busy);
            compare(app.currentTab, 2);
        }

        // ---- routing ----

        function test_restore_opens_the_unlock_on_settings() {
            app.currentTab = 0;
            var path = backupFile();
            offer(path);
            part("incomingDocumentRestore").clicked();
            verify(!sheet.visible);
            var backupFlow = findChild(app, "backupFlow");
            compare(backupFlow.step, "unlock");
            compare(backupFlow.reference, path, "the reference as the host gave it");
            tryCompare(findChild(backupFlow, "backupUnlockSheet"), "visible", true);
            compare(app.currentTab, 2, "Settings, whose Backup card shows how it ends");
            verify(!flow.busy);
            compare(backupCalls("setFlowActive").length, 1);
        }

        function test_keys_choose_their_format_data() {
            return [
                {tag: "decimal", hex: false, type: "keysDec"},
                {tag: "hex", hex: true, type: "keysHex"}
            ];
        }
        function test_keys_choose_their_format(data) {
            offer(write("keys.csv", "id,key\n1,1234\n"));
            chooseKind("keys");
            var format = part("importKeyFormat");
            verify(format.visible);
            format.selected(data.hex ? 1 : 0);
            testContext.holdImports(true);
            part("incomingDocumentImport").clicked();
            var imports = findChild(app, "importsScreen");
            var csvImport = findChild(imports, "importsCsvImport");
            tryCompare(csvImport, "copying", true);
            compare(csvImport.type, data.type);
            verify(!flow.busy, "handed off");
            testContext.holdImports(false);
            tryCompare(csvImport, "active", false);
        }

        function test_channel_map_imports_data() {
            return [
                {tag: "idle", session: false},
                {tag: "during a session", session: true}
            ];
        }
        function test_channel_map_imports(data) {
            if (data.session) {
                testContext.setLifecyclePhase(2);
                tryCompare(app, "monitorMode", true);
            }
            offer(channelsFile());
            chooseKind("chan");
            part("incomingDocumentImport").clicked();
            compare(flow.phase, "handingOff", "busy until the Imports screen has it");
            verify(flow.busy);
            var imports = findChild(app, "importsScreen");
            verify(app.importsOpen);
            compare(app.sessionDestination, data.session ? "settings" : "");
            verify(imports.visible);
            tryCompare(flow, "busy", false);
            tryCompare(imports, "notice", "Imported · 2 channels", 5000);
            compare(importedFiles.count, 1);
            compare(importedFiles.get(0).type, "chan");
            compare(importedFiles.get(0).name, "channels.csv");
            compare(Ui.Navigation.layers[Ui.Navigation.layers.length - 1].surface, imports, "Imports is the top layer");
        }

        function test_target_csv_starts_a_new_scan_list() {
            offer(targetsFile());
            chooseKind("trunkTargets");
            part("incomingDocumentImport").clicked();
            verify(!flow.busy);
            verify(app.scanListOpen);
            var editor = findChild(app, "scanListScreen");
            compare(editor.editRow, -1, "a new list");
            tryCompare(findChild(editor, "targetCsvImport"), "active", false, 5000);
            verify(editor.csvMode);
            compare(editor.targetRows.length, 2);
            compare(editor.targetRows[0].id, "one");
            verify(editor.fingerprint() !== editor.initialDraft, "closing asks before the targets are lost");
            app.scanListOpen = false;
        }

        // Layers with nothing to lose may stay under the sheet; routing closes
        // them so nothing covers where the file goes.
        function test_idle_layers_close_on_routing_data() {
            return [
                {tag: "restore", backup: true},
                {tag: "import", backup: false}
            ];
        }
        function test_idle_layers_close_on_routing(data) {
            app.importsOpen = true;
            app.diagnosticsOpen = true;
            app.licensesOpen = true;
            app.callRecordingsOpen = true;
            offer(data.backup ? backupFile() : groupsFile());
            if (data.backup) {
                part("incomingDocumentRestore").clicked();
            } else {
                chooseKind("group");
                part("incomingDocumentImport").clicked();
            }
            verify(!app.diagnosticsOpen);
            verify(!app.licensesOpen);
            verify(!app.callRecordingsOpen);
            compare(app.importsOpen, !data.backup);
            tryCompare(flow, "busy", false);
            if (!data.backup)
                tryCompare(findChild(app, "importsScreen"), "notice", "Imported · 2 talkgroups", 5000);
        }

        function test_session_layers_close_on_routing() {
            testContext.setLifecyclePhase(2);
            tryCompare(app, "monitorMode", true);
            app.talkgroupsOpen = true;
            app.spectrumOpen = true;
            app.sessionDestination = "history";
            offer(groupsFile());
            chooseKind("group");
            part("incomingDocumentImport").clicked();
            verify(!app.talkgroupsOpen);
            verify(!app.spectrumOpen);
            compare(app.sessionDestination, "settings");
            verify(findChild(app, "importsScreen").visible);
            tryCompare(findChild(app, "importsScreen"), "notice", "Imported · 2 talkgroups", 5000);
        }

        // Target CSV during a session: the editor opens over the monitor, and
        // a mouse click on its Save, over "Stop listening", saves the list and
        // keeps listening. The monitor's taps are passive, so only standing it
        // down keeps the release that closed the editor from reaching Stop. A
        // scan warning on the monitor waits under the editor, not over Save.
        function test_target_csv_over_a_session_keeps_listening() {
            testContext.setLifecyclePhase(2);
            tryCompare(app, "monitorMode", true);
            var monitor = findChild(app, "monitorScreen");
            tryCompare(monitor, "enabled", true);
            app.scanWarnings = "Source aliases were replaced";
            var warnings = findChild(app, "scanWarningsPanel");
            verify(warnings !== null && warnings.visible);
            var lists = scanLists.count;
            offer(targetsFile());
            chooseKind("trunkTargets");
            part("incomingDocumentImport").clicked();
            verify(app.scanListOpen);
            var editor = findChild(app, "scanListScreen");
            tryCompare(findChild(editor, "targetCsvImport"), "active", false, 5000);
            // Read here, checked once Save has closed the editor: the file it
            // imported is still held while the editor is open.
            var stoodDown = !monitor.enabled;
            verify(!warnings.visible, "the scan warning waits under the editor");
            editor.draft.name = "Shared targets";
            var stop = findChild(monitor, "stopListeningButton");
            var save = findChild(editor, "scanSave");
            var scroll = findChild(editor, "scanListScroll");
            verify(stop !== null && stop.visible);
            verify(save !== null && scroll !== null);
            // Scroll Save onto Stop listening's centre.
            var target = stop.mapToItem(null, stop.width / 2, stop.height / 2);
            var at = save.mapToItem(null, save.width / 2, save.height / 2);
            scroll.contentY += at.y - target.y;
            waitForRendering(editor);
            var onSave = save.mapFromItem(null, target.x, target.y);
            verify(save.contains(onSave), "Save lies over Stop listening");
            var stops = decoderHost.stopCalls();
            mousePress(save, onSave.x, onSave.y);
            mouseRelease(save, onSave.x, onSave.y);
            tryCompare(app, "scanListOpen", false);
            compare(scanLists.count, lists + 1, "the list was saved");
            wait(60);
            compare(decoderHost.stopCalls(), stops, "the click that saved did not stop the session");
            compare(decoderHost.sessionState, 2);
            verify(stoodDown, "the monitor took no taps under the editor");
            tryCompare(monitor, "enabled", true);
            verify(warnings.visible, "the scan warning is back on the monitor");
        }

        // A short window during a session: the sheet scrolls, and its Cancel
        // can sit over "Stop listening". A mouse click there drops the file
        // and keeps listening: Stop's press began under the sheet, so the
        // release that closed the sheet does not stop the session.
        function test_cancel_over_stop_keeps_listening() {
            app.height = 400;
            testContext.setLifecyclePhase(2);
            tryCompare(app, "monitorMode", true);
            var monitor = findChild(app, "monitorScreen");
            tryCompare(monitor, "enabled", true);
            var stop = findChild(monitor, "stopListeningButton");
            verify(stop !== null && stop.visible);
            offer(targetsFile());
            var cancel = part("incomingDocumentCancel");
            var scroll = findChild(sheet, "modalSheetScroll");
            verify(scroll !== null);
            // Scroll Cancel onto Stop listening's centre, as far as the sheet goes.
            var target = stop.mapToItem(null, stop.width / 2, stop.height / 2);
            var at = cancel.mapToItem(null, cancel.width / 2, cancel.height / 2);
            scroll.contentY = Math.max(0, Math.min(scroll.contentHeight - scroll.height, scroll.contentY + at.y - target.y));
            waitForRendering(sheet);
            var onCancel = cancel.mapFromItem(null, target.x, target.y);
            verify(cancel.contains(onCancel), "Cancel lies over Stop listening");
            var stops = decoderHost.stopCalls();
            mousePress(cancel, onCancel.x, onCancel.y);
            mouseRelease(cancel, onCancel.x, onCancel.y);
            tryCompare(sheet, "visible", false);
            verify(!flow.busy);
            verify(!decoderHost.incomingDocumentPending, "Cancel dropped the file");
            wait(60);
            compare(decoderHost.stopCalls(), stops, "the click that cancelled did not stop the session");
            compare(decoderHost.sessionState, 2);
            verify(monitor.enabled);
        }

        // The intake lets the Spectrum stay beneath its sheet. A click on the
        // sheet's Cancel over the waterfall drops the file without retuning:
        // the waterfall's press began under the sheet. A drag on the sheet's
        // scrim over it does not pan (a pan retunes on release).
        function test_cancel_over_the_spectrum_does_not_retune() {
            testContext.setLifecyclePhase(2);
            tryCompare(app, "monitorMode", true);
            app.exploring = true;
            findChild(app, "monitorScreen").openSpectrum();
            var spectrum = null;
            tryVerify(function () {
                spectrum = findChild(app, "spectrumScreen");
                return spectrum !== null && spectrum.visible && !spectrum.viewOnly;
            }, 5000, "the Spectrum opened for exploring");
            var area = findChild(spectrum, "spectrumTapArea");
            verify(area !== null);
            waitForRendering(spectrum);
            offer(groupsFile());
            testContext.resetCommands();
            var cancel = part("incomingDocumentCancel");
            var onArea = cancel.mapToItem(area, cancel.width / 2, cancel.height / 2);
            verify(area.contains(onArea), "Cancel lies over the waterfall");
            mousePress(cancel, cancel.width / 2, cancel.height / 2);
            mouseRelease(cancel, cancel.width / 2, cancel.height / 2);
            tryCompare(sheet, "visible", false);
            wait(60);
            compare(testContext.manualTuneCalls(), 0, "the click that cancelled did not retune");
            verify(app.spectrumOpen);

            offer(groupsFile());
            testContext.resetCommands();
            // The scrim above the panel, over the waterfall.
            var start = area.mapToItem(sheet, area.width / 4, 12);
            verify(!sheet.hitsPanel(start.x, start.y), "the drag starts on the scrim");
            mousePress(sheet, start.x, start.y);
            for (var i = 1; i <= 8; ++i)
                mouseMove(sheet, start.x + i * 20, start.y);
            mouseRelease(sheet, start.x + 160, start.y);
            wait(60);
            compare(testContext.manualTuneCalls(), 0, "a drag on the sheet did not pan the Spectrum");
            verify(sheet.visible, "a drag is not a tap outside the panel");
            // The same by touch, as Android sends it.
            var touch = touchEvent(sheet);
            touch.press(0, sheet, start.x, start.y).commit();
            for (var j = 1; j <= 8; ++j)
                touch.move(0, sheet, start.x + j * 20, start.y).commit();
            touch.release(0, sheet, start.x + 160, start.y).commit();
            wait(60);
            compare(testContext.manualTuneCalls(), 0, "a touch drag on the sheet did not pan the Spectrum");
            verify(sheet.visible);
            flow.drop();
        }

        // Imports at rest stays beneath the sheet too: Cancel over its
        // "Import file" drops the file without opening the import sheet.
        function test_cancel_over_imports_does_not_import() {
            app.height = 400;
            app.importsOpen = true;
            var imports = findChild(app, "importsScreen");
            tryCompare(imports, "enabled", true);
            var importButton = findChild(imports, "importFileButton");
            verify(importButton !== null && importButton.visible);
            offer(groupsFile());
            var cancel = part("incomingDocumentCancel");
            var scroll = findChild(sheet, "modalSheetScroll");
            var target = importButton.mapToItem(null, importButton.width / 2, importButton.height / 2);
            var at = cancel.mapToItem(null, cancel.width / 2, cancel.height / 2);
            scroll.contentY = Math.max(0, Math.min(scroll.contentHeight - scroll.height, scroll.contentY + at.y - target.y));
            waitForRendering(sheet);
            var onCancel = cancel.mapFromItem(null, target.x, target.y);
            verify(cancel.contains(onCancel), "Cancel lies over Import file");
            mousePress(cancel, onCancel.x, onCancel.y);
            mouseRelease(cancel, onCancel.x, onCancel.y);
            tryCompare(sheet, "visible", false);
            wait(60);
            verify(!decoderHost.incomingDocumentPending, "Cancel dropped the file");
            compare(Ui.Navigation.modals.length, 0, "Import file did not open its sheet");
            verify(app.importsOpen);
        }

        // ---- waiting until the screen is free (spec §4.4) ----

        function test_owners_make_it_wait_data() {
            return [
                {tag: "a sheet", set: function (app) { findChild(app, "failureDetails").visible = true; }, clear: function (app) { findChild(app, "failureDetails").visible = false; }},
                {tag: "wizard", set: function (app) { app.wizardOpen = true; }, clear: function (app) { app.wizardOpen = false; }},
                {tag: "onboarding", set: function () { prefs.onboardingDone = false; }, clear: function () { prefs.onboardingDone = true; }},
                {tag: "scan-list editor", set: function (app) { app.scanListOpen = true; }, clear: function (app) { app.scanListOpen = false; }},
                {tag: "Explore setup", set: function (app) { app.exploreSetupOpen = true; }, clear: function (app) { app.exploreSetupOpen = false; }},
                {tag: "Signal search", set: function (app) { app.signalSearchOpen = true; }, clear: function (app) { app.signalSearchOpen = false; }},
                {tag: "RadioReference", set: function (app) { app.radioReferenceOpen = true; }, clear: function (app) { app.radioReferenceOpen = false; }},
                {tag: "RadioReference account", set: function (app) { app.radioReferenceAccountOpen = true; }, clear: function (app) { app.radioReferenceAccountOpen = false; }},
                {tag: "backup flow", set: function () { testContext.setBackup("flowActive", true); }, clear: function () { testContext.setBackup("flowActive", false); }},
                {tag: "backup job", set: function () { testContext.setBackup("busy", true); }, clear: function () { testContext.setBackup("busy", false); }},
                {tag: "RadioReference refresh", set: function () { testContext.setRadioReference("busy", true); }, clear: function () { testContext.setRadioReference("busy", false); }},
                {tag: "picker in flight", set: function () { testContext.setDocumentRequestPending(true); }, clear: function () { testContext.setDocumentRequestPending(false); }},
                {tag: "USB access", set: function (app) { app.awaitingUsbAccess = true; }, clear: function (app) { app.awaitingUsbAccess = false; }},
                {tag: "USB prompt", set: function () { testContext.setLocalDeviceRecord("usb", true, "usb"); }, clear: function () { testContext.setLocalDeviceRecord("usb", false, ""); }},
                {tag: "restart waiting", set: function (app) { app.pendingRestart = {uid: "gone"}; }, clear: function (app) { app.cancelPendingRestart(); }},
                {tag: "session state unknown", set: function () { testContext.setSessionStateKnown(false); }, clear: function () { testContext.setSessionStateKnown(true); }},
                {tag: "key mapping apply", set: function (app) { findChild(findChild(app, "importsScreen"), "importsMapApply").requestId = "incoming"; }, clear: function (app) { findChild(findChild(app, "importsScreen"), "importsMapApply").requestId = ""; }},
                {tag: "decryption apply", set: function (app) { findChild(findChild(app, "keySheet"), "decryptionApplyFlow").requestId = "incoming"; }, clear: function (app) { findChild(findChild(app, "keySheet"), "decryptionApplyFlow").requestId = ""; }}
            ];
        }
        function test_owners_make_it_wait(data) {
            var path = groupsFile();
            data.set(app);
            verify(!app.intakeFree, data.tag);
            testContext.emitIncomingDocument(path);
            verifyWaits(data.tag);
            data.clear(app);
            tryCompare(flow, "phase", "presenting", 5000);
            verify(sheet.visible);
        }

        // A copy, a transfer or a picker outlives the sheet that started it.
        function test_work_after_its_sheet_closed_makes_it_wait_data() {
            return [
                {tag: "library import"},
                {tag: "Imports picker"},
                {tag: "replay copy"},
                {tag: "recording save"},
                {tag: "recording export"}
            ];
        }
        function test_work_after_its_sheet_closed_makes_it_wait(data) {
            var imports = findChild(app, "importsScreen");
            var path = groupsFile("waiting.csv");
            if (data.tag === "library import") {
                testContext.holdImports(true);
                importedFiles.beginImport(groupsFile("first.csv"), "first.csv", "group", {}, "");
                verify(importedFiles.importing);
            } else if (data.tag === "Imports picker") {
                app.importsOpen = true;
                tryCompare(imports, "visible", true);
                imports.pendingType = "group";
                imports.importPickedFile();
                verify(imports.importActive);
                compare(Ui.Navigation.modals.length, 0, "no sheet shows while the picker is up");
                // The host's own reading stays false here (a case sets it), so the import alone holds the file.
                verify(!decoderHost.documentRequestPending);
                verify(!importedFiles.importing);
            } else if (data.tag === "replay copy") {
                replayFiles.beginReplayImport("content://com.Example.Files/document/a.wav", "a.wav");
                verify(replayFiles.copying);
            } else if (data.tag === "recording save") {
                testContext.holdRecordingTransfers(true);
                verify(decoderHost.saveRecording({selection: 1, paths: [path], names: []}, "file://" + path + ".copy"));
                verify(decoderHost.recordingTransferActive);
            } else {
                testContext.holdRecordingTransfers(true);
                verify(decoderHost.exportRecordings({selection: 2, paths: [path], names: ["a.csv"], bytes: 10}, "file://" + path + ".zip"));
                verify(decoderHost.recordingTransferActive);
            }
            testContext.emitIncomingDocument(path);
            verifyWaits(data.tag);
            if (data.tag === "library import") {
                testContext.holdImports(false);
            } else if (data.tag === "Imports picker") {
                testContext.answerDocumentCancelled(testContext.lastDocumentRequest().id);
            } else if (data.tag === "replay copy") {
                replayFiles.cancelReplayImport();
            } else {
                testContext.holdRecordingTransfers(false);
            }
            tryCompare(flow, "phase", "presenting", 5000);
            compare(part("incomingDocumentName").text, "waiting.csv");
        }

        // A restart is in flight from its request until its start lands:
        // the request is cleared before the start, and that start ignores the
        // auto-start gate.
        function test_restart_in_flight_makes_it_wait() {
            savedSystems.add({name: "Bench", sourceType: "udp", port: 7355, freqMhz: "851.5"});
            var uid = savedSystems.get(savedSystems.count - 1).uid;
            app.pendingRestart = {uid: uid};
            testContext.emitIncomingDocument(groupsFile());
            verifyWaits("requested");
            app.finishPendingRestart();
            compare(app.pendingRestart, null);
            compare(decoderHost.sessionState, 1, "starting");
            verify(app.restartInFlight, "until the start lands");
            verifyWaits("starting");
            testContext.setLifecyclePhase(2);
            verify(!app.restartInFlight);
            tryCompare(flow, "phase", "presenting", 5000);
        }

        // A restart whose start fails, or never begins, stops holding the
        // file as soon as that is known.
        function test_restart_that_does_not_run_lets_go_data() {
            return [
                {tag: "start failed", accept: true, uid: true},
                {tag: "start refused by the host", accept: false, uid: true},
                {tag: "system removed", accept: true, uid: false}
            ];
        }
        function test_restart_that_does_not_run_lets_go(data) {
            savedSystems.add({name: "Bench", sourceType: "udp", port: 7355, freqMhz: "851.5"});
            var uid = data.uid ? savedSystems.get(savedSystems.count - 1).uid : "gone";
            testContext.setScanTestAcceptStart(data.accept);
            try {
                app.pendingRestart = {uid: uid};
                testContext.emitIncomingDocument(groupsFile());
                verifyWaits("requested");
                app.finishPendingRestart();
                if (data.tag === "start failed") {
                    compare(decoderHost.sessionState, 1);
                    verifyWaits("starting");
                    testContext.setLifecyclePhase(4);
                } else {
                    compare(decoderHost.sessionState, 0, "nothing started");
                }
                verify(!app.restartInFlight);
                tryCompare(flow, "phase", "presenting", 5000);
            } finally {
                testContext.setScanTestAcceptStart(true);
            }
        }

        // A document that comes while one is being handled waits for it to
        // be handed off and its import to end, then opens.
        function test_second_document_waits_for_the_first() {
            offer(groupsFile("first.csv"));
            testContext.emitIncomingDocument(groupsFile("second.csv"));
            verify(decoderHost.incomingDocumentPending);
            chooseKind("group");
            testContext.holdImports(true);
            part("incomingDocumentImport").clicked();
            var imports = findChild(app, "importsScreen");
            tryCompare(imports, "importActive", true);
            verify(!flow.busy);
            wait(60);
            verify(decoderHost.incomingDocumentPending, "the second waits while the first copies");
            verify(!sheet.visible);
            testContext.holdImports(false);
            tryCompare(imports, "notice", "Imported · 2 talkgroups", 5000);
            tryCompare(flow, "phase", "presenting", 5000);
            compare(part("incomingDocumentName").text, "second.csv");
            compare(importedFiles.count, 1, "only the first was imported");
        }

        // Auto-start on an attachment holds while the intake is busy, through
        // the hand-off; a session that starts under the hand-off sends it back.
        function test_usb_and_session_during_handoff() {
            scanLists.add({name: "Night scan", sourceType: "usb", entries: [{kind: "freq", name: "Simplex", protocol: "p25", freqMhz: "851.5", enabled: true}]});
            prefs.lastStartedKind = "scan";
            prefs.lastStartedUid = scanLists.get(scanLists.count - 1).uid;
            prefs.autoStartOnAttach = true;
            testContext.setAttachedDeviceSource("usb");
            testContext.setLocalDeviceRecord("usb", false, "usb");
            testContext.setDongleStatus(true, 0, "Ready: RTL-SDR");
            try {
                testContext.holdImports(true);
                testContext.emitIncomingDocument(groupsFile());
                tryCompare(flow, "phase", "probing");
                verify(uiController.autoStartBlocked);
                testContext.emitLocalDeviceAttached("usb");
                compare(decoderHost.sessionState, 0, "no start while the file is checked");
                testContext.holdImports(false);
                tryCompare(flow, "phase", "presenting");
                chooseKind("group");
                part("incomingDocumentImport").clicked();
                compare(flow.phase, "handingOff");
                verify(uiController.autoStartBlocked, "held through the hand-off");
                // The session starts before the Imports screen takes the file:
                // the screen stands down for the monitor, and refuses it.
                testContext.setLifecyclePhase(2);
                tryCompare(flow, "phase", "presenting");
                verify(sheet.visible, "the file is still here");
                compare(part("incomingDocumentProblem").text, "Couldn't open that now. Try again.");
                compare(flow.announced, "Couldn't open that now. Try again.");
                compare(importedFiles.count, 0);
                part("incomingDocumentImport").clicked();
                tryCompare(flow, "busy", false);
                compare(app.sessionDestination, "settings");
                tryCompare(findChild(app, "importsScreen"), "notice", "Imported · 2 talkgroups", 5000);
            } finally {
                while (scanLists.count)
                    scanLists.remove(0);
                testContext.setDongleStatus(false, 0, "");
            }
        }

        // A destination that refuses keeps the file in the sheet to try again.
        function test_refused_restore_keeps_the_file() {
            var path = backupFile();
            offer(path);
            var backupFlow = findChild(app, "backupFlow");
            backupFlow.step = "unlock"; // another restore step, which refuses a new one
            part("incomingDocumentRestore").clicked();
            verify(sheet.visible);
            compare(flow.phase, "presenting");
            compare(flow.reference, path);
            compare(part("incomingDocumentProblem").text, "Couldn't open that now. Try again.");
            compare(flow.announcements, 1);
            backupFlow.step = "";
            part("incomingDocumentRestore").clicked();
            compare(backupFlow.step, "unlock");
            compare(backupFlow.reference, path);
            verify(!flow.busy);
        }

        // ---- leaving ----

        function test_dismissal_drops_the_file_data() {
            return [
                {tag: "Cancel while checking", how: "cancel", held: true},
                {tag: "Back while checking", how: "back", held: true},
                {tag: "outside tap while checking", how: "outside", held: true},
                {tag: "Cancel at the question", how: "cancel", held: false},
                {tag: "Back at the question", how: "back", held: false},
                {tag: "outside tap at the question", how: "outside", held: false}
            ];
        }
        function test_dismissal_drops_the_file(data) {
            testContext.holdImports(data.held);
            testContext.emitIncomingDocument(groupsFile());
            tryCompare(flow, "phase", data.held ? "probing" : "presenting", 5000);
            if (data.held)
                tryVerify(function () {
                    return testContext.heldImports() === 1;
                }, 5000, "the probe reads");
            if (data.how === "cancel") {
                part("incomingDocumentCancel").clicked();
            } else if (data.how === "back") {
                verify(Ui.Navigation.back(app));
            } else {
                waitForRendering(sheet);
                mouseClick(sheet, 4, 4);
            }
            verify(!sheet.visible);
            verify(!flow.busy);
            compare(flow.reference, "");
            verify(!decoderHost.incomingDocumentPending);
            if (data.held)
                tryVerify(function () {
                    return testContext.heldImports() === 0;
                }, 5000, "the probe was told to stop");
            testContext.holdImports(false);
            wait(60);
            verify(!sheet.visible, "a late answer opens nothing");
            compare(importedFiles.count, 0);
        }

        // The name is the other app's: shown as written, never as markup, and
        // handed on as it came.
        function test_name_is_plain_text() {
            testContext.holdImports(true);
            testContext.emitIncomingDocument(groupsFile());
            tryCompare(flow, "phase", "probing");
            flow.inspected(flow.probeId, {kind: "other", name: "<b>x</b>.csv"});
            compare(flow.phase, "presenting");
            var name = part("incomingDocumentName");
            compare(name.text, "<b>x</b>.csv");
            compare(name.textFormat, Text.PlainText);
            compare(name.Accessible.role, Accessible.Heading);
            chooseKind("group");
            part("incomingDocumentImport").clicked();
            var csvImport = findChild(findChild(app, "importsScreen"), "importsCsvImport");
            tryCompare(csvImport, "copying", true);
            compare(csvImport.fileName, "<b>x</b>.csv");
            testContext.holdImports(false);
            tryCompare(csvImport, "active", false, 5000);
        }

        // Without its BackupFlow a backup's problem has no words, but nothing throws.
        function test_backup_problem_without_its_backup_flow() {
            var bare = createTemporaryObject(bareFlow, appLoader.parent);
            verify(bare !== null);
            compare(bare.restoreFlow, null);
            bare.phase = "probing";
            bare.probeId = 4242;
            bare.inspected(4242, {kind: "damagedBackup", name: "broken.dsdbak"});
            compare(bare.phase, "presenting");
            compare(bare.problemText, "");
            bare.reset();
        }

        function test_accessible_names() {
            offer(write("damaged.dsdbak", backupText(1, 5, zeros(8))));
            // The name heading and "From another app" follow it, so the sheet's name says neither again.
            compare(sheet.accessibleName, "Incoming file");
            compare(sheet.Accessible.name, "Incoming file");
            var problem = part("incomingDocumentProblem");
            compare(problem.Accessible.role, Accessible.AlertMessage);
            compare(problem.Accessible.name, "The backup is damaged.");
            compare(flow.announcements, 1);
            compare(flow.announced, "The backup is damaged.");
        }
    }
}
