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
    readonly property bool visible: d.liveRequestId !== 0 || d.dialogPending
    // The last accepted save went to the Download folder, as no save picker was
    // there to ask. Never true on the desktop path.
    readonly property bool savedToDownloads: d.downloads

    signal accepted(string reference, string displayName)
    signal rejected()
    signal unavailable(string reason, string message)

    function open() {
        if (decoderHost.documentPickerSupported)
            d.openHost();
        else
            d.openDialog();
    }
    // Ends a waiting request without a signal, for a flow that is ending anyway.
    function close() {
        d.abandonRequest();
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

        function openHost() {
            abandonRequest();
            liveRequestId = picker.mode === "save" ? decoderHost.requestSaveDocument(picker.suggestedName, picker.mimeType) : decoderHost.requestOpenDocument();
        }
        function abandonRequest() {
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
                if (d.settle(requestId))
                    picker.unavailable(reason, picker.messageFor(reason));
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
    }
}
