// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

ModalSheet {
    id: sheet
    objectName: "keySheet"
    panelObjectName: "keySheetPanel"
    accessibleName: qsTr("Decryption keys")
    // A form sheet (issue #718): an outside tap keeps it; Cancel, Back and Escape ask first.
    closesOnOutsideTap: false
    property string notice: ""
    property string protocol: "mixed"
    property string targetName: ""
    property string selectedProfileUid: ""
    property int profileScope: 0
    property bool advancedOpen: false
    property var context: ({})
    // The standalone saved system the session runs, by UID; empty for a scan
    // list, Explore or a wizard test. "Use for" writes only that system.
    property string systemUid: ""
    // Its name as the session started, for when it has since been removed.
    property string systemName: ""
    // Bumped where this sheet writes the system, so the binding below reads it again.
    property int systemRevision: 0
    readonly property var savedSystem: {
        var revision = systemRevision;
        if (!systemUid.length || typeof savedSystems === "undefined")
            return ({});
        var count = savedSystems.count;
        return savedSystems.getByUid(systemUid);
    }
    readonly property string savedSystemName: savedSystem.name || systemName
    // A direct draft handed to the profile editor: its save clears the draft.
    property bool savingDraft: false
    /** The saved system now names a decryption profile for its future starts. */
    signal systemProfileSaved(string uid)
    readonly property bool sessionRunning: decoderHost.running
    readonly property var materialProfile: {
        if (typeof decryptionProfiles === "undefined")
            return ({});
        var count = decryptionProfiles.count;
        return decryptionProfiles.forRuntimeReference(metrics.keyProfileRef || "");
    }
    // Resolve the active runtime reference, which may belong to a scan target
    // rather than the session default. Never infer effective keys from the editor.
    readonly property string effectiveState: {
        if (!sessionRunning || !metrics.optionsKnown)
            return qsTr("Not yet known");
        if ((metrics.keyProfileRef || "").length > 0)
            return materialProfile.label || qsTr("Active profile");
        if (metrics.directKeys)
            return qsTr("Direct override");
        if (metrics.automaticKeys)
            return qsTr("Automatic keys");
        return qsTr("No keys");
    }
    // An empty direct draft showing the session's configured identifier policy.
    function resetDraft() {
        editor.reset();
        var force = metrics.configuredForce || 0;
        editor.forceMode = force === 33 ? 2 : force <= 1 ? force : 0;
        editor.forceChanged = false;
        editor.currentForceSummary = force === 0 ? qsTr("normal identifiers") : force === 1 ? qsTr("privacy forcing") : qsTr("algorithm %1 fallback").arg(force.toString(16).toUpperCase());
    }
    function open() {
        resetDraft();
        notice = "";
        selectedProfileUid = materialProfile.uid || "";
        profileScope = (metrics.scanTargetId || "").length ? 1 : 0;
        context = commands.decryptionContext(metrics.scanTargetId || "", metrics.keyEpoch || "0");
        // Only a real target has a name: the label source falls back to
        // "Target <ordinal>" without one.
        openedTargetName = (metrics.scanTargetId || "").length ? targetName : "";
        targetLeft = false;
        advancedOpen = false;
        savingDraft = false;
        systemRevision++;
        openedChoice = choiceValue();
        visible = sessionRunning;
    }
    // The target as the sheet opened, which is the one a target-scope Apply
    // addresses (context), by its name then.
    property string openedTargetName: ""
    // The scan has left that target while the sheet was open: a target-scope
    // Apply is then refused as stale. Latched until the sheet opens again, so
    // a rotating scan shows the note once instead of on every hop.
    property bool targetLeft: false
    readonly property string liveTargetId: metrics.scanTargetId || ""
    onLiveTargetIdChanged: {
        if (visible && liveTargetId !== (context.target || ""))
            targetLeft = true;
    }
    // The profile and scope as the sheet opened: a different choice is an edit.
    property string openedChoice: ""
    function choiceValue() {
        return JSON.stringify([selectedProfileUid, profileScope]);
    }
    /** The direct-key draft: typed here, and lost when the sheet closes. */
    function draftEdited() {
        return editor.keyType.length > 0 || editor.keyValue.length > 0 || editor.forceChanged;
    }
    /**
     * Another profile or scope than the sheet opened with, not applied yet.
     * The profile itself is saved (picked, or saved by "Manage matching key");
     * what closing gives up is applying it, so the question says that.
     */
    function choiceUnapplied() {
        // "No profile" leaves nothing to apply.
        return selectedProfileUid.length > 0 && choiceValue() !== openedChoice;
    }
    function requestClose() {
        var close = function () {
            sheet.visible = false;
        };
        if (!visible || applyFlow.pending)
            visible = false;
        else if (draftEdited())
            discard.ask(close);
        else if (choiceUnapplied())
            discard.askWith(close, profileScope === 1 ? qsTr("Close without applying the profile to this target?") : qsTr("Close without applying the profile to session defaults?"), qsTr("Close without applying"));
        else
            visible = false;
    }
    dismissHandler: requestClose
    DiscardDialog {
        id: discard
        parent: sheet
    }
    /**
     * The identifier policy the draft runs with, as a profile force value: a
     * changed policy as apply() submits it, else the session's configured one.
     */
    function draftForce() {
        if (editor.forceChanged)
            return editor.forceMode === 2 ? 33 : editor.forceMode;
        return metrics.configuredForce || 0;
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
            replaceProfile.visible = false;
            editor.reset();
            notice = "";
            Qt.inputMethod.hide();
        }
    }
    /**
     * "Use for <system> on future starts": the system is looked up again by UID
     * here, so a row that moved, changed or went away since the session started
     * is never written by position. Replacing another profile asks first.
     */
    function useForSystem() {
        var system = savedSystems.getByUid(systemUid);
        if (!system.uid) {
            notice = qsTr("“%1” is no longer a saved system. Nothing was changed.").arg(savedSystemName);
            systemRevision++;
            return;
        }
        var current = system.decryptionProfileUid || "";
        var currentLabel = current.length ? (decryptionProfiles.get(current).label || "") : "";
        if (current === selectedProfileUid) {
            systemRevision++;
            return;
        }
        if (currentLabel.length) {
            var chosen = decryptionProfiles.get(selectedProfileUid).label || qsTr("this profile");
            replaceProfile.title = qsTr("Replace the profile for “%1”?").arg(system.name);
            replaceProfile.message = qsTr("Future starts will use “%1” instead of “%2”. This session keeps its keys.").arg(chosen).arg(currentLabel);
            replaceProfile.confirmText = qsTr("Use “%1”").arg(chosen);
            replaceProfile.profileUid = selectedProfileUid;
            replaceProfile.open();
            return;
        }
        writeSystemProfile(selectedProfileUid);
    }
    function writeSystemProfile(profileUid) {
        var row = savedSystems.rowForUid(systemUid);
        if (row < 0) {
            notice = qsTr("“%1” is no longer a saved system. Nothing was changed.").arg(savedSystemName);
        } else if (savedSystems.update(row, {
                decryptionProfileUid: profileUid
            })) {
            var label = decryptionProfiles.get(profileUid).label || qsTr("this profile");
            var name = savedSystems.get(row).name;
            notice = (materialProfile.uid || "") === profileUid ? qsTr("“%1” will start with “%2”.").arg(name).arg(label) : qsTr("“%1” will start with “%2”. Apply it to use it in this session too.").arg(name).arg(label);
            systemProfileSaved(systemUid);
        } else {
            notice = qsTr("“%1” could not be saved. Its profile is unchanged.").arg(savedSystemName);
        }
        systemRevision++;
    }
    ConfirmDialog {
        id: replaceProfile
        objectName: "replaceSystemProfile"
        parent: sheet
        property string profileUid: ""
        onConfirmed: sheet.writeSystemProfile(profileUid)
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
    // Closed, the slots stay unbuilt (issue #782): they change with every call
    // and scan hop, and each row created under the hidden sheet raises an
    // accessibility event. Opening fills them from the readings as they are.
    Repeater {
        objectName: "keySheetSlots"
        model: sheet.visible ? (metrics.decryptionSlots || []) : []
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
                    sheet.savingDraft = false;
                    var profile = decryptionProfiles.forRuntimeReference(modelData.profileRef);
                    matchingEditor.openMatching(profile.uid || "", modelData.protocol, parseInt(modelData.keyId, 16), modelData.materialKind);
                }
            }
            OutlineButton {
                width: parent.width
                visible: modelData.dmr && modelData.group && sheet.selectedProfileUid.length > 0
                text: qsTr("Talkgroup key selection")
                enabled: !applyFlow.pending
                onClicked: {
                    sheet.savingDraft = false;
                    matchingEditor.openTalkgroup(sheet.selectedProfileUid, "dmr", Number(modelData.targetId));
                }
            }
        }
    }
    DecryptionProfileEditor {
        id: matchingEditor
        objectName: "keySheetProfileEditor"
        parent: sheet
        onSaved: function (uid) {
            sheet.selectedProfileUid = uid;
            if (sheet.savingDraft) {
                // The key and its identifier policy now live in the profile: the session draft is done.
                sheet.savingDraft = false;
                sheet.resetDraft();
                sheet.notice = qsTr("Profile saved and selected. Apply it to use it in this session.");
            }
        }
    }
    Text {
        width: parent.width
        visible: sheet.materialProfile.outdated === true
        text: qsTr("The saved profile has changed since it was loaded. Apply the saved profile to use its current revision.")
        Accessible.role: Accessible.StaticText
        Accessible.name: text
        Accessible.ignored: !visible
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
    // A fixed pair (issue #782): a model carrying the target's name was rebuilt
    // on every scan hop, which reset the choice to session defaults and, while
    // the sheet was closed, sent accessibility events for each rebuilt option.
    PlexComboBox {
        id: scopeChoice
        objectName: "keySheetScope"
        width: parent.width
        visible: (metrics.scanTargetId || "").length > 0
        enabled: !applyFlow.pending
        Accessible.name: qsTr("Profile application scope")
        model: [qsTr("Session defaults"), qsTr("Active scan target")]
        currentIndex: sheet.profileScope
        onActivated: sheet.profileScope = currentIndex
    }
    // Where the target scope applies: the target the sheet opened on, which
    // Apply addresses, not the scan's current one. One line, elided.
    // A plain Text is not in the accessibility tree; these lines name
    // themselves so a screen reader reaches them, and leave it while hidden.
    Text {
        objectName: "keySheetTargetName"
        width: parent.width
        visible: scopeChoice.visible && sheet.profileScope === 1 && sheet.openedTargetName.length > 0
        text: qsTr("Applies to %1").arg(sheet.openedTargetName)
        Accessible.role: Accessible.StaticText
        Accessible.name: text
        Accessible.ignored: !visible
        textFormat: Text.PlainText
        elide: Text.ElideRight
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(13)
        color: Theme.textSecondary
    }
    // Read where it is reached, and with Apply below it, never announced on
    // its own: it appears mid-scan, and Apply is where it matters.
    Text {
        id: targetLeftNote
        objectName: "keySheetTargetLeft"
        width: parent.width
        // It promises no target to come: the scan may have stopped. The second
        // sentence is the decoder's own refusal advice.
        visible: sheet.profileScope === 1 && sheet.targetLeft
        text: sheet.openedTargetName.length ? qsTr("The scan has left %1. Reopen the editor before applying.").arg(sheet.openedTargetName) : qsTr("The scan target changed. Reopen the editor before applying.")
        Accessible.role: Accessible.StaticText
        Accessible.name: text
        Accessible.ignored: !visible
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(13)
        color: Theme.alert
    }
    GradientButton {
        width: parent.width
        visible: sheet.selectedProfileUid.length > 0
        enabled: sheet.sessionRunning && !applyFlow.pending
        text: sheet.profileScope === 1 ? qsTr("Apply profile to this target") : qsTr("Apply profile to session defaults")
        Accessible.description: targetLeftNote.visible ? targetLeftNote.text : ""
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
        objectName: "useProfileForSystem"
        width: parent.width
        visible: sheet.systemUid.length > 0 && sheet.selectedProfileUid.length > 0
        readonly property bool alreadyUsed: sheet.savedSystem.uid !== undefined && (sheet.savedSystem.decryptionProfileUid || "") === sheet.selectedProfileUid
        // Future starts only; Apply above is the live change.
        enabled: !alreadyUsed
        text: alreadyUsed ? qsTr("Used for %1 on future starts").arg(sheet.savedSystemName) : qsTr("Use for %1 on future starts").arg(sheet.savedSystemName)
        onClicked: sheet.useForSystem()
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
        objectName: "directOverrideNote"
        width: parent.width
        text: qsTr("A direct override is for this session only and is cleared when you stop. It changes session defaults; an explicit scan profile stays effective until rotation restores them. Keys and listening policy are separate; excluded talkgroups remain blocked.")
        Accessible.role: Accessible.StaticText
        Accessible.name: text
        Accessible.ignored: !visible
        wrapMode: Text.Wrap
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(13)
        color: Theme.textSecondary
    }
    OutlineButton {
        objectName: "saveDirectAsProfile"
        width: parent.width
        visible: editor.visible && editor.allowedTypes.length > 1 && typeof decryptionProfiles !== "undefined"
        text: qsTr("Save as profile…")
        inputValid: editor.keyType.length > 0 && editor.keyValue.length > 0 && editor.valid
        enabled: !applyFlow.pending && (inputValid || Navigation.composing)
        onClicked: {
            sheet.savingDraft = true;
            matchingEditor.openDirect(sheet.protocol, editor.keyType, editor.keyValue, sheet.systemUid.length ? sheet.savedSystemName : "", sheet.draftForce());
        }
    }
    Text {
        width: parent.width
        visible: text.length > 0
        text: sheet.notice
        Accessible.role: Accessible.StaticText
        Accessible.name: text
        Accessible.ignored: !visible
        wrapMode: Text.Wrap
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(13)
        color: Theme.textSecondary
    }
    GradientButton {
        objectName: "applySessionKeyButton"
        width: parent.width
        text: qsTr("Apply changed defaults")
        inputValid: editor.valid
        enabled: sheet.sessionRunning && !applyFlow.pending && (inputValid || Navigation.composing)
        onClicked: sheet.apply()
    }
    OutlineButton {
        objectName: "keySheetClose"
        width: parent.width
        text: applyFlow.pending ? qsTr("Close") : qsTr("Cancel")
        onClicked: sheet.requestClose()
    }
}
