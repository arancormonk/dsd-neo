// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

Item {
    id: flow
    visible: false
    property string requestId: ""
    property string session: ""
    property string message: ""
    readonly property bool pending: requestId.length > 0
    signal completed(bool success)
    function begin(id, context) {
        if (!id.length) {
            message = qsTr("The request was not queued. Wait for any pending change, then try again.");
            return false;
        }
        requestId = id;
        session = context.session;
        message = qsTr("Pending — waiting for the decoder");
        consume();
        return true;
    }
    function consume() {
        var result = uiController.decryptionResult;
        if (!pending || !result || result.requestId !== requestId || result.session !== session)
            return;
        requestId = "";
        var status = result.status;
        message = status === 1 ? (result.scope === 1 ? qsTr("Active target profile updated.") : qsTr("Session defaults updated. Explicit scan profiles keep their own keys.")) : status === -2 ? qsTr("The session or target changed. Reopen the editor before applying.") : status === -4 ? qsTr("The key collection or mapping could not be loaded. Check the profile's files.") : status === -5 ? qsTr("The target is changing. Wait for tuning to finish, then try again.") : status === -6 ? qsTr("The session stopped before the change was applied.") : status === -3 ? qsTr("This change requires a compatible scan target or a session restart. Vendor keystreams cannot be replaced live.") : qsTr("The decoder rejected the change. The previous configuration remains active.");
        completed(status === 1);
    }
    Connections {
        target: uiController
        ignoreUnknownSignals: true
        function onDecryptionResultChanged() {
            flow.consume();
        }
    }
}
