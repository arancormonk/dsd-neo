// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

// Process one export at a time. A stopped session can still have a retained result.
Item {
    id: flow
    property var bridge: commands
    property var library: importedFiles
    property var systems: savedSystems
    property var result: uiController.talkgroupExportResult
    property var pending: null
    property string message: ""
    signal saved(string uid, string path)

    function newer(a, b) {
        a = String(a);
        b = String(b);
        return a.length > b.length || (a.length === b.length && a > b);
    }
    function save(uid, context, generation) {
        if (pending !== null)
            return false;
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
            pending = null;
            message = qsTr("Talkgroup list export was not queued");
            return false;
        }
        message = qsTr("Saving talkgroup list…");
        return true;
    }
    onResultChanged: {
        if (pending === null || !result || !newer(result.sequence || "0", pending.sequence) || result.policyContext !== pending.context || result.policyGeneration !== pending.generation || result.path !== pending.path)
            return;
        var request = pending;
        pending = null;
        if (!result.success) {
            message = qsTr("Talkgroup list export refused or failed");
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
