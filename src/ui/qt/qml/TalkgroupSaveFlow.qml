// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

// Match one export at a time; abandoned requests can never associate a later result.
// Each step after the export reports its own outcome: adding the file to Imports, then attaching it to the saved
// system the request named. A failed step keeps the exported file and offers that step again, never a new export:
// the decoder already writes its edits to that file, and only while that file is still there (and, to attach it, still
// in Imports). A request given up on (no completion in time, or its completion missed) is kept until its result comes,
// and a late success is then only added to Imports.
Item {
    id: flow
    property var bridge: commands
    property var library: importedFiles
    property var systems: savedSystems
    // Every completion the controller's last poll read, oldest first, and the newest sequence it could not read.
    property var results: uiController.talkgroupExportResults
    property var pending: null
    // A successful export whose later step failed: {step: "register" or "attach", uid, tuning, path}.
    property var stalled: null
    // Requests given up on: {path, context, generation, sequence}, each until its completion is read.
    property var abandoned: []
    // Late successes Imports could not take yet: retried at each later poll and request.
    property var owedRegistrations: []
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
                flow.abandon(flow.pending);
                flow.pending = null;
                flow.retryAvailable = true;
                flow.message = qsTr("No export completion received. Try again.");
            }
        }
    }
    // The request survives the session ending (Idle or Failed): its export may complete between the controller's
    // result poll and the host's refresh, and the result read later still belongs to it.
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
    // @p uid is the standalone saved system the session runs (empty for Explore, a scan list or a hold: Imports only)
    // and @p tuning its tuning when the request was made. With a step to retry, any call retries that step alone.
    function save(uid, context, generation, tuning) {
        retryOwedRegistrations();
        if (stalled !== null)
            return retryStep();
        if (pending !== null)
            return false;
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
            pending = null;
            // Never queued, so never written: the name goes back.
            library.releaseTalkgroupListPath(path);
            retryAvailable = true;
            message = qsTr("Talkgroup list export was not queued. Try again.");
            return false;
        }
        message = qsTr("Saving talkgroup list…");
        completionTimeout.restart();
        return true;
    }
    function abandon(request) {
        abandoned = abandoned.concat([{
                path: request.path,
                context: request.context,
                generation: request.generation,
                sequence: request.sequence
            }]);
    }
    function matches(request, r) {
        return newer(r.sequence || "0", request.sequence) && String(r.policyContext) === String(request.context) && (Number(r.policyGeneration) >>> 0) === (Number(request.generation) >>> 0) && r.path === request.path;
    }
    function idle() {
        return pending === null && stalled === null;
    }

    onResultsChanged: {
        retryOwedRegistrations();
        var list = results && results.results ? results.results : [];
        for (var i = 0; i < list.length; ++i)
            accept(list[i]);
        var missed = results && results.missedThrough ? String(results.missedThrough) : "0";
        if (newer(missed, "0"))
            recoverMissed(missed);
        lastSequence = newestIn(results, lastSequence);
    }
    function accept(r) {
        if (pending !== null && matches(pending, r)) {
            var request = pending;
            pending = null;
            finish(request, r.success === true);
            return;
        }
        for (var i = 0; i < abandoned.length; ++i) {
            if (matches(abandoned[i], r)) {
                var old = abandoned[i];
                abandoned = abandoned.slice(0, i).concat(abandoned.slice(i + 1));
                settleAbandoned(old.path, r.success === true);
                return;
            }
        }
    }
    function finish(request, ok) {
        if (!ok) {
            library.releaseTalkgroupListPath(request.path);
            retryAvailable = true;
            message = qsTr("Talkgroup list export failed or was canceled. Try again.");
            return;
        }
        register(request);
    }
    // A late success: written by the decoder, so kept in Imports, but no system is changed for it.
    function settleAbandoned(path, ok) {
        if (!ok) {
            library.releaseTalkgroupListPath(path);
            return;
        }
        if (!library.registerTalkgroupList(path)) {
            owedRegistrations = owedRegistrations.concat([path]);
            return;
        }
        if (idle())
            message = qsTr("An earlier talkgroup list export finished late. It was saved in Imports; system association was not changed");
    }
    function retryOwedRegistrations() {
        var owed = [];
        for (var i = 0; i < owedRegistrations.length; ++i) {
            if (!library.registerTalkgroupList(owedRegistrations[i]))
                owed.push(owedRegistrations[i]);
        }
        if (owed.length !== owedRegistrations.length)
            owedRegistrations = owed;
    }
    // A missed success: kept in Imports (never attached), or owed there when Imports cannot record it now.
    function keepMissed(path) {
        if (library.registerTalkgroupList(path))
            return true;
        owedRegistrations = owedRegistrations.concat([path]);
        return false;
    }
    // More completions finished between two polls than app-control keeps; a request older than the newest one lost
    // may have been among them. A file at its path means its export succeeded (the decoder writes it only then). A
    // request with no file stays waiting: its completion may still come, and if a failure was lost, it keeps its name.
    function recoverMissed(missed) {
        if (pending !== null && newer(missed, pending.sequence)) {
            var request = pending;
            pending = null;
            if (!library.talkgroupListWritten(request.path)) {
                abandon(request);
                retryAvailable = true;
                message = qsTr("The export result was not received: too many exports finished at once. Try again.");
            } else if (keepMissed(request.path)) {
                message = qsTr("The export result was not received, but the list was saved in Imports; system association was not changed");
            } else {
                message = qsTr("The export result was not received. The list was exported, but adding it to Imports failed; it is tried again later");
            }
        }
        var kept = [];
        for (var i = 0; i < abandoned.length; ++i) {
            var old = abandoned[i];
            if (newer(missed, old.sequence) && library.talkgroupListWritten(old.path))
                keepMissed(old.path);
            else
                kept.push(old);
        }
        if (kept.length !== abandoned.length)
            abandoned = kept;
    }
    function stallAt(step, request, text) {
        stalled = {
            step: step,
            uid: request.uid,
            tuning: request.tuning,
            path: request.path
        };
        retryAvailable = true;
        message = text;
    }
    // Whether the exported file @p request (a stalled step) finishes with is still there: written, and to attach it,
    // still in Imports. Removed since (in Imports, possibly in a later session), it leaves nothing to finish.
    function stepFileKept(request) {
        return library.talkgroupListWritten(request.path) && (request.step === "register" || library.rowForPath(request.path) >= 0);
    }
    function retryStep() {
        var request = stalled;
        stalled = null;
        retryAvailable = false;
        // No system is changed for a file that is gone, and its name stays reserved: the decoder may still write it.
        if (!stepFileKept(request)) {
            message = "";
            return false;
        }
        if (request.step === "register")
            register(request);
        else
            attach(request);
        return true;
    }
    function register(request) {
        if (!library.registerTalkgroupList(request.path)) {
            stallAt("register", request, qsTr("Talkgroup list exported, but adding it to Imports failed. Try again."));
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
            stallAt("attach", request, qsTr("Talkgroup list saved in Imports, but the system could not be saved. Try again."));
            return;
        }
        saved(request.uid, request.path);
        message = qsTr("Talkgroup list saved");
    }
}
