// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

// Match one export at a time; a request replaced or ended can never associate a later result.
// Each step after the export reports its own outcome: adding the file to Imports, then attaching it to the saved
// system the request named. A failed step ends the request where the list got to (the exported file, and its Imports
// row if that step worked) and says so; no step is offered again, and the user saves again if they want.
Item {
    id: flow
    property var bridge: commands
    property var library: importedFiles
    property var systems: savedSystems
    // Every completion the controller's last poll read, oldest first, and the newest sequence it could not read.
    property var results: uiController.talkgroupExportResults
    property var pending: null
    property string lastSequence: "0"
    property int timeoutMs: 15000
    property bool retryAvailable: false
    property string message: ""
    signal saved(string uid, string path)

    Timer {
        id: completionTimeout
        interval: Math.max(1, Math.min(flow.timeoutMs, 15000))
        onTriggered: {
            if (flow.pending !== null) {
                flow.retryAvailable = true;
                flow.message = qsTr("No export completion received. Try again.");
            }
        }
    }
    // The request survives the session ending (Idle or Failed): its export may complete between the controller's
    // result poll and the host's refresh, and the result read later still belongs to it. The timeout only permits a
    // retry, which replaces it.
    onPendingChanged: {
        if (pending === null)
            completionTimeout.stop();
    }
    Component.onCompleted: lastSequence = flow.newestIn(results, "0")

    function newer(a, b) {
        a = String(a);
        b = String(b);
        return a.length > b.length || (a.length === b.length && a > b);
    }
    // The newest sequence @p batch names, or @p fallback.
    function newestIn(batch, fallback) {
        var newest = String(fallback);
        var list = batch && batch.results ? batch.results : [];
        for (var i = 0; i < list.length; ++i) {
            if (newer(list[i].sequence || "0", newest))
                newest = String(list[i].sequence);
        }
        var missed = batch && batch.missedThrough ? String(batch.missedThrough) : "0";
        return newer(missed, newest) ? missed : newest;
    }
    // The fields of @p system whose change ends a saved site's provenance: what attachment compares later.
    function tuningOf(system) {
        var tuning = {};
        if (!system)
            return tuning;
        var fields = systems.tuningProvenanceFields();
        for (var i = 0; i < fields.length; ++i)
            tuning[fields[i]] = system[fields[i]];
        return tuning;
    }
    // The request ends with no Imports row for its file: its name goes back.
    function end(request) {
        library.releaseTalkgroupListPath(request.path);
    }
    // @p uid is the standalone saved system the session runs (empty for Explore, a scan list or a hold: Imports only)
    // and @p tuning its tuning when the request was made.
    function save(uid, context, generation, tuning) {
        if (pending !== null && !retryAvailable)
            return false;
        // A retry replaces the request given up on: its late result matches nothing.
        if (pending !== null) {
            end(pending);
            pending = null;
        }
        retryAvailable = false;
        message = "";
        var path = library.newTalkgroupListPath();
        if (path.length === 0) {
            message = qsTr("Could not create talkgroup list path");
            return false;
        }
        pending = {
            uid: uid,
            tuning: tuning || ({}),
            context: String(context),
            generation: generation,
            path: path,
            sequence: lastSequence
        };
        if (!bridge.saveTalkgroupList(context, generation, path)) {
            end(pending);
            pending = null;
            retryAvailable = true;
            message = qsTr("Talkgroup list export was not queued. Try again.");
            return false;
        }
        message = qsTr("Saving talkgroup list…");
        completionTimeout.restart();
        return true;
    }
    function matches(request, r) {
        return newer(r.sequence || "0", request.sequence) && String(r.policyContext) === String(request.context) && (Number(r.policyGeneration) >>> 0) === (Number(request.generation) >>> 0) && r.path === request.path;
    }

    onResultsChanged: {
        var list = results && results.results ? results.results : [];
        for (var i = 0; i < list.length; ++i) {
            if (pending !== null && matches(pending, list[i])) {
                var request = pending;
                pending = null;
                finish(request, list[i].success === true);
            }
        }
        // More completions finished between two polls than app-control keeps; the request's may have been among them.
        var missed = results && results.missedThrough ? String(results.missedThrough) : "0";
        if (pending !== null && newer(missed, pending.sequence)) {
            end(pending);
            pending = null;
            retryAvailable = false;
            message = qsTr("The export result was not received: too many exports finished at once.");
        }
        lastSequence = newestIn(results, lastSequence);
    }
    function finish(request, ok) {
        retryAvailable = false;
        if (!ok) {
            end(request);
            retryAvailable = true;
            message = qsTr("Talkgroup list export failed or was canceled. Try again.");
            return;
        }
        if (!library.registerTalkgroupList(request.path)) {
            end(request);
            message = qsTr("Talkgroup list exported, but adding it to Imports failed; system association was not changed");
            return;
        }
        attach(request);
    }
    function attach(request) {
        if (!request.uid) {
            message = qsTr("Talkgroup list saved in Imports; system association was not changed");
            return;
        }
        // Resolved by the captured UUID now, never a row index: the row can have become another system.
        if (!systems.groupListAttachable(request.uid, request.tuning)) {
            message = qsTr("Talkgroup list saved in Imports; system association was not changed: the system was removed, retuned or already has a talkgroup list");
            return;
        }
        if (!systems.attachGroupList(request.uid, request.path, request.tuning)) {
            message = qsTr("Talkgroup list saved in Imports, but the system could not be saved");
            return;
        }
        saved(request.uid, request.path);
        message = qsTr("Talkgroup list saved");
    }
}
