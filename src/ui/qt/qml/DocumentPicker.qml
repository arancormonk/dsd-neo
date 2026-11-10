// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtQuick.Dialogs

// Issue #829: the one file picker every screen uses. Where the host has its own
// picker (decoderHost.documentPickerSupported, Android) a request goes to the
// host and is answered later by id; elsewhere Qt's FileDialog asks. Either way a
// request ends in exactly one of accepted(), rejected() or unavailable(), except
// after close(), which ends it silently.
//
// Issue #830: an open picker that names what it opens (browseKind) can be
// served by DSD-neo's own file browser instead (FileBrowser, one request
// app-wide): first, where the host has the browser and the user chose it in
// Settings, and whenever Android answers that it has no picker. The browser's
// "Choose with another app" goes back to the host's picker. The browser's
// answers count as this picker's own: still exactly one per open().
//
// The root is a QtObject, not an Item: Item's own `visible` is FINAL, and here
// `visible` means "a request is waiting". The FileDialog is made by an
// Instantiator rather than a Loader so that its QObject parents lead through
// this object to the Item that declares the picker, which is how a dialog finds
// the window it opens in (a Loader is an Item without a window of its own).
QtObject {
    id: picker
    // "open" reads an existing document, "save" asks where to write a new one.
    property string mode: "open"
    property string title: ""
    // Desktop only: the FileDialog's filters and the suffix it appends.
    property var nameFilters: []
    property string defaultSuffix: ""
    // Host save only: the new document's type.
    property string mimeType: ""
    property string suggestedName: ""
    // Open only: what DSD-neo's file browser lists for this picker, "csv",
    // "backup" or "replay"; empty never browses.
    property string browseKind: ""
    readonly property bool visible: d.liveRequestId !== 0 || d.dialogPending || d.browseToken !== 0 || d.browseSuperseded
    // The last accepted save went to the Download folder, as no save picker was
    // there to ask. Never true on the desktop path.
    readonly property bool savedToDownloads: d.downloads

    signal accepted(string reference, string displayName)
    signal rejected()
    signal unavailable(string reason, string message)

    function open() {
        if (d.canBrowse() && prefs.chooseFilesInApp === true)
            d.openBrowser(true);
        else
            d.openPlatform();
    }
    // Ends a waiting request without a signal, for a flow that is ending anyway.
    function close() {
        d.abandonRequest();
        d.dismissBrowse();
        d.closeDialog();
    }
    // Ends a waiting request as if the user had backed out, so the owner's own
    // cancel path runs. Nothing happens when no request is waiting.
    function cancel() {
        if (!visible)
            return;
        close();
        rejected();
    }
    function messageFor(reason) {
        switch (reason) {
        case "noPicker":
            return qsTr("No compatible file picker on this device. Install a file manager app, then try again.");
        case "noSavePicker":
            return qsTr("This device has no save picker, and DSD-neo could not save to the Download folder.");
        case "unreadable":
            return qsTr("That app gave DSD-neo a file it can't open. Pick the file from another app.");
        case "noResult":
            return qsTr("No file came back from the picker. Try again.");
        default:
            return qsTr("The file picker could not open. Try again.");
        }
    }

    // Not part of the API: the request state, the host's answers and the
    // desktop dialog, which owners must leave alone for each open() to end in
    // exactly one answer. Everything above is the API.
    readonly property QtObject internals: QtObject {
        id: d
        // The host request this picker waits for, 0 when none.
        property real liveRequestId: 0
        property bool dialogPending: false
        property bool downloads: false
        // Requests this picker gave up (reopened, closed or cancelled) whose
        // answer could still be on its way: a Download-folder pick for one of
        // them is thrown away. Only this picker's own, so a pick another picker
        // is waiting for is never touched; bounded like the host's own records.
        property var abandoned: []
        // This picker's live FileBrowser request, 0 when none.
        property int browseToken: 0
        // The request was superseded before FileBrowser.open() returned (the
        // owner it superseded opened again while being told): rejected() is
        // still to come, after open() returns.
        property bool browseSuperseded: false
        // Bumped whenever this picker gives a request up (close, cancel, a
        // reopen), so openBrowser() can tell that the owner it superseded did
        // that to this picker while being told.
        property int endings: 0

        function canBrowse() {
            return decoderHost.fileBrowserSupported === true && picker.browseKind.length > 0 && picker.mode === "open";
        }
        // @p allowOtherApp offers the host's picker from the browser; false
        // once the host has said it has none.
        function openBrowser(allowOtherApp) {
            abandonRequest();
            dismissBrowse();
            var before = endings;
            var token = FileBrowser.open(picker.browseKind, picker.title, allowOtherApp);
            if (endings !== before) {
                // Closed, cancelled or reopened while FileBrowser.open() ran:
                // this request is already given up.
                FileBrowser.dismiss(token);
                return;
            }
            if (FileBrowser.request && FileBrowser.request.token === token) {
                browseToken = token;
            } else {
                browseSuperseded = true;
                Qt.callLater(reportBrowseSuperseded);
            }
        }
        function reportBrowseSuperseded() {
            if (!browseSuperseded)
                return;
            browseSuperseded = false;
            picker.rejected();
        }
        function dismissBrowse() {
            endings += 1;
            browseSuperseded = false;
            if (!browseToken)
                return;
            var token = browseToken;
            browseToken = 0;
            FileBrowser.dismiss(token);
        }
        // True when @p token is this picker's browse request, which it then
        // stops being, before the owner hears the answer.
        function settleBrowse(token) {
            if (!browseToken || token !== browseToken)
                return false;
            browseToken = 0;
            return true;
        }
        function openPlatform() {
            if (decoderHost.documentPickerSupported)
                openHost();
            else
                openDialog();
        }

        function openHost() {
            abandonRequest();
            dismissBrowse();
            liveRequestId = picker.mode === "save" ? decoderHost.requestSaveDocument(picker.suggestedName, picker.mimeType) : decoderHost.requestOpenDocument();
        }
        function abandonRequest() {
            endings += 1;
            if (!liveRequestId)
                return;
            decoderHost.cancelDocumentRequest(liveRequestId);
            var ids = abandoned.concat([liveRequestId]);
            abandoned = ids.slice(Math.max(0, ids.length - 16));
            liveRequestId = 0;
        }
        // True when @p id is the live request, which it then stops being; any
        // answer for an abandoned one is forgotten with it.
        function settle(id) {
            var index = abandoned.indexOf(id);
            if (index >= 0) {
                var ids = abandoned.slice();
                ids.splice(index, 1);
                abandoned = ids;
            }
            if (!liveRequestId || id !== liveRequestId)
                return false;
            liveRequestId = 0;
            return true;
        }

        function openDialog() {
            var dialog = desktop.object;
            dismissBrowse();
            closeDialog();
            if (dialog) {
                // A dialog keeps its selection between opens: clear it, so a
                // dialog that answers without a new choice never hands back the
                // old file. `selectedFile = ""` is refused in the open modes and
                // selectedFiles is read-only; currentFiles writes the same
                // selection and takes [].
                if (picker.mode === "save")
                    dialog.selectedFile = picker.suggestedName;
                else
                    dialog.currentFiles = [];
                dialog.open();
            }
            dialogPending = true;
            if (!dialog || !dialog.visible)
                Qt.callLater(reportDialogFailure);
        }
        function closeDialog() {
            dialogPending = false;
            if (desktop.object)
                desktop.object.close();
        }
        // A dialog that could not open never answers; say so, after open()
        // returns as a host answer would.
        function reportDialogFailure() {
            if (!dialogPending || (desktop.object && desktop.object.visible))
                return;
            dialogPending = false;
            picker.unavailable("failed", picker.messageFor("failed"));
        }
        function dialogAccepted(dialog) {
            if (!dialogPending)
                return;
            dialogPending = false;
            var reference = dialog.selectedFile.toString();
            if (!reference.length) {
                picker.unavailable("noResult", picker.messageFor("noResult"));
                return;
            }
            var segment = reference.substring(reference.lastIndexOf('/') + 1);
            var name = segment;
            try {
                name = decodeURIComponent(segment);
            } catch (error) {
                // A malformed escape: show the segment as it came.
            }
            downloads = false;
            picker.accepted(reference, name);
        }
        function dialogRejected() {
            if (!dialogPending)
                return;
            dialogPending = false;
            picker.rejected();
        }

        readonly property Connections hostAnswers: Connections {
            target: decoderHost
            ignoreUnknownSignals: true
            function onDocumentPicked(requestId, reference, displayName, inDownloads) {
                var wasAbandoned = d.abandoned.indexOf(requestId) >= 0;
                if (!d.settle(requestId)) {
                    if (wasAbandoned && inDownloads)
                        decoderHost.discardPendingDownload(reference);
                    return;
                }
                d.downloads = picker.mode === "save" && inDownloads;
                picker.accepted(reference, displayName);
            }
            function onDocumentPickCancelled(requestId) {
                if (d.settle(requestId))
                    picker.rejected();
            }
            function onDocumentPickUnavailable(requestId, reason) {
                if (!d.settle(requestId))
                    return;
                // No picker on this device: DSD-neo's browser answers instead,
                // without the way back to a picker that is not there.
                if (reason === "noPicker" && d.canBrowse())
                    d.openBrowser(false);
                else
                    picker.unavailable(reason, picker.messageFor(reason));
            }
        }

        readonly property Connections browserAnswers: Connections {
            target: FileBrowser
            function onPicked(token, reference, name) {
                if (!d.settleBrowse(token))
                    return;
                d.downloads = false;
                picker.accepted(reference, name);
            }
            function onRejected(token) {
                if (d.settleBrowse(token))
                    picker.rejected();
            }
            // "Choose with another app": the host's picker, whatever the
            // Settings toggle says, which is what the user just asked for.
            function onOtherAppRequested(token) {
                if (d.settleBrowse(token))
                    d.openPlatform();
            }
        }

        readonly property Instantiator desktop: Instantiator {
            id: desktop
            active: !decoderHost.documentPickerSupported
            delegate: FileDialog {
                id: dialog
                objectName: "documentPickerDialog"
                title: picker.title
                fileMode: picker.mode === "save" ? FileDialog.SaveFile : FileDialog.OpenFile
                nameFilters: picker.nameFilters
                defaultSuffix: picker.defaultSuffix
                onAccepted: d.dialogAccepted(dialog)
                onRejected: d.dialogRejected()
            }
        }
    }

    Component.onDestruction: {
        if (d.liveRequestId)
            decoderHost.cancelDocumentRequest(d.liveRequestId);
        if (d.browseToken)
            FileBrowser.dismiss(d.browseToken);
    }
}
