// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

ModalSheet {
    id: sheet
    objectName: "keySheet"
    panelObjectName: "keySheetPanel"
    accessibleName: qsTr("Decryption keys")
    property string notice: ""
    property string protocol: "mixed"
    property string targetName: ""
    property string selectedProfileUid: ""
    property int profileScope: 0
    property bool advancedOpen: false
    property var context: ({})
    readonly property bool sessionRunning: decoderHost.running
    readonly property var materialProfile: {
        if (typeof decryptionProfiles === "undefined")
            return ({});
        var count = decryptionProfiles.count;
        return decryptionProfiles.forRuntimeReference(metrics.keyProfileRef || "");
    }
    function open() {
        editor.reset();
        notice = "";
        selectedProfileUid = materialProfile.uid || "";
        profileScope = (metrics.scanTargetId || "").length ? 1 : 0;
        context = commands.decryptionContext(metrics.scanTargetId || "", metrics.keyEpoch || "0");
        var force = metrics.configuredForce || 0;
        editor.forceMode = force === 33 ? 2 : force <= 1 ? force : 0;
        editor.forceChanged = false;
        editor.currentForceSummary = force === 0 ? qsTr("normal identifiers") : force === 1 ? qsTr("privacy forcing") : qsTr("algorithm %1 fallback").arg(force.toString(16).toUpperCase());
        advancedOpen = false;
        visible = sessionRunning;
    }
    function apply() {
        if (!visible || !sessionRunning || !editor.valid || applyFlow.pending)
            return;
        if (!editor.keyType.length && !editor.forceChanged) {
            visible = false;
            return;
        }
        notice = "";
        var id = commands.applyDecryptionDraft(editor.keyType, editor.keyValue, editor.forceChanged, editor.forceMode === 2 ? 33 : editor.forceMode, context);
        applyFlow.begin(id, context);
        notice = applyFlow.message;
    }
    onSessionRunningChanged: {
        if (!sessionRunning)
            visible = false;
    }
    onVisibleChanged: {
        if (!visible) {
            editor.reset();
            notice = "";
            Qt.inputMethod.hide();
        }
    }
    DecryptionApplyFlow {
        id: applyFlow
        objectName: "decryptionApplyFlow"
        onCompleted: function (success) {
            sheet.notice = message;
            if (success && sheet.visible)
                sheet.visible = false;
        }
    }
    Repeater {
        model: metrics.decryptionSlots || []
        Column {
            required property var modelData
            width: parent.width
            spacing: 6
            Text {
                width: parent.width
                text: qsTr("Slot %1 · %2%3").arg(modelData.slot).arg(modelData.status).arg(modelData.lastObserved ? qsTr(" · last observed") : "") + (modelData.algorithm.length ? qsTr("\nAlgorithm in use: %1").arg(modelData.algorithm) : "") + (modelData.keyId.length ? qsTr("\nCall key ID: %1").arg(modelData.keyId) : "") + "\n" + modelData.source + (modelData.effectiveId.length ? " · " + modelData.effectiveId : "") + "\n" + modelData.availability + (modelData.fallback.length ? "\n" + modelData.fallback : "") + (modelData.privateCall && modelData.dmr ? qsTr("\nPrivate call: talkgroup overrides do not apply.") : "") + (modelData.blockReason.length ? qsTr("\nListening policy: %1").arg(modelData.blockReason) : "")
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: Theme.textPrimary
                font.pixelSize: Theme.fontSize(14)
            }
            OutlineButton {
                width: parent.width
                visible: modelData.keyId.length > 0 && typeof decryptionProfiles !== "undefined"
                text: qsTr("Manage matching key")
                enabled: !applyFlow.pending
                onClicked: {
                    var profile = decryptionProfiles.forRuntimeReference(modelData.profileRef);
                    matchingEditor.openMatching(profile.uid || "", modelData.protocol, parseInt(modelData.keyId, 16), modelData.materialKind);
                }
            }
            OutlineButton {
                width: parent.width
                visible: modelData.dmr && modelData.group && sheet.selectedProfileUid.length > 0
                text: qsTr("Talkgroup key selection")
                enabled: !applyFlow.pending
                onClicked: matchingEditor.openTalkgroup(sheet.selectedProfileUid, "dmr", Number(modelData.targetId))
            }
        }
    }
    DecryptionProfileEditor {
        id: matchingEditor
        parent: sheet
        onSaved: function (uid) {
            sheet.selectedProfileUid = uid;
        }
    }
    Text {
        width: parent.width
        visible: sheet.materialProfile.outdated === true
        text: qsTr("The saved profile has changed since it was loaded. Apply the saved profile to use its current revision.")
        color: Theme.alert
        wrapMode: Text.Wrap
        font.pixelSize: Theme.fontSize(13)
    }
    DecryptionProfileSelector {
        width: parent.width
        overlayParent: sheet
        enabled: !applyFlow.pending
        profileUid: sheet.selectedProfileUid
        protocol: sheet.protocol
        onSelected: function (uid) {
            sheet.selectedProfileUid = uid;
        }
    }
    PlexComboBox {
        width: parent.width
        visible: (metrics.scanTargetId || "").length > 0
        enabled: !applyFlow.pending
        Accessible.name: qsTr("Profile application scope")
        model: [qsTr("Session defaults"), sheet.targetName.length ? qsTr("Active target: %1").arg(sheet.targetName) : qsTr("Active scan target")]
        currentIndex: sheet.profileScope
        onActivated: sheet.profileScope = currentIndex
    }
    GradientButton {
        width: parent.width
        visible: sheet.selectedProfileUid.length > 0
        enabled: sheet.sessionRunning && !applyFlow.pending
        text: sheet.profileScope === 1 ? qsTr("Apply profile to this target") : qsTr("Apply profile to session defaults")
        onClicked: {
            if (typeof decryptionProfiles !== "undefined" && decryptionProfiles.get(sheet.selectedProfileUid).mode === "vendor") {
                sheet.notice = qsTr("Vendor keystream profiles require Stop and a new standalone session.");
                return;
            }
            var id = commands.applyDecryptionProfile(sheet.selectedProfileUid, sheet.profileScope, sheet.context);
            applyFlow.begin(id, sheet.context);
            sheet.notice = applyFlow.message;
        }
    }
    OutlineButton {
        width: parent.width
        text: qsTr("Advanced: session direct override")
        enabled: !applyFlow.pending
        onClicked: sheet.advancedOpen = !sheet.advancedOpen
    }
    EncryptionEditor {
        id: editor
        objectName: "sessionEncryptionEditor"
        protocol: sheet.protocol
        width: parent.width
        liveMode: true
        visible: sheet.advancedOpen || keyType.length > 0 || forceChanged
        enabled: !applyFlow.pending
    }
    Text {
        width: parent.width
        text: qsTr("Direct changes affect session defaults. An explicit scan profile stays effective until rotation restores those defaults. Keys and listening policy are separate; excluded talkgroups remain blocked.")
        wrapMode: Text.Wrap
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(13)
        color: Theme.textSecondary
    }
    Text {
        width: parent.width
        visible: text.length > 0
        text: sheet.notice
        wrapMode: Text.Wrap
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(13)
        color: Theme.textSecondary
    }
    GradientButton {
        objectName: "applySessionKeyButton"
        width: parent.width
        text: qsTr("Apply changed defaults")
        enabled: sheet.sessionRunning && editor.valid && !applyFlow.pending
        onClicked: sheet.apply()
    }
    OutlineButton {
        width: parent.width
        text: applyFlow.pending ? qsTr("Close") : qsTr("Cancel")
        onClicked: sheet.visible = false
    }
}
