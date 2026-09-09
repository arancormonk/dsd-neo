// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

// Match one export at a time; abandoned requests can never associate a later result.
Item {
    id: flow
    property var bridge: commands
    property var library: importedFiles
    property var systems: savedSystems
    property var result: uiController.talkgroupExportResult
    property var pending: null
    property int sessionState: decoderHost.sessionState
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
    function clearPending(text, retry) {
        completionTimeout.stop();
        pending = null;
        retryAvailable = retry;
        message = text;
    }
    // Keep the request tuple through Idle/Failed: export may complete between
    // the controller's result poll and host refresh. The retained result on the
    // next tick still belongs to this request. Timeout permits an explicit retry.
    onPendingChanged: {
        if (pending === null)
            completionTimeout.stop();
    }

    function newer(a, b) {
        a = String(a);
        b = String(b);
        return a.length > b.length || (a.length === b.length && a > b);
    }
    function save(uid, context, generation) {
        if (pending !== null && !retryAvailable)
            return false;
        // A retry gets a new destination and captures the current policy version.
        clearPending("", false);
        var path = library.newTalkgroupListPath();
        if (path.length === 0) {
            message = qsTr("Could not create talkgroup list path");
            return false;
        }
        pending = {
            uid: uid,
            context: context,
            generation: generation,
            path: path,
            sequence: result.sequence || "0"
        };
        if (!bridge.saveTalkgroupList(context, generation, path)) {
            clearPending(qsTr("Talkgroup list export was not queued. Try again."), true);
            return false;
        }
        message = qsTr("Saving talkgroup list…");
        completionTimeout.restart();
        return true;
    }
    onResultChanged: {
        if (pending === null || !result || !newer(result.sequence || "0", pending.sequence) || result.policyContext !== pending.context || result.policyGeneration !== pending.generation || result.path !== pending.path)
            return;
        var request = pending;
        clearPending("", false);
        if (!result.success) {
            message = qsTr("Talkgroup list export failed or was canceled. Try again.");
            retryAvailable = true;
            return;
        }
        if (!library.registerTalkgroupList(request.path)) {
            message = qsTr("Talkgroup list exported, but library registration failed");
            return;
        }
        // Resolve the captured UUID now: a row index can have become another system.
        var row = systems.rowForUid(request.uid);
        if (row >= 0 && !systems.get(row).groupCsvPath) {
            systems.update(row, {
                groupCsvPath: request.path
            });
            saved(request.uid, request.path);
            message = qsTr("Talkgroup list saved");
        } else {
            message = qsTr("Talkgroup list saved in Imports; system association was not changed");
        }
    }
}
