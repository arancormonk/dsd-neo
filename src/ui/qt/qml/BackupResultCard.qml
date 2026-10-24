// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import "Util.js" as Util

// Issue #580: what the restore at this start did (applied, not applied, still
// waiting, or stuck), at the top of Home until dismissed. The actions are the
// card's own: Dismiss acknowledges the outcome, and a waiting restore can also
// be discarded. The message comes from the restore's result file, so every
// text here is plain.
UiPanel {
    id: card
    objectName: "backupResultCard"

    property var outcome: ({kind: ""})
    readonly property string kind: String(outcome.kind || "")
    readonly property bool problem: kind !== "applied"
    property string actionError: ""
    // The last text read out, so a screen reader hears each outcome once.
    property string announced: ""
    property int announcements: 0

    readonly property string reason: sentence(outcome.message)
    readonly property string title: kind === "applied" ? qsTr("Backup restored") : kind === "failed" ? qsTr("Restore not applied. Nothing was changed.") : !kind.length ? "" : reason.length ? reason : kind === "deferred" ? qsTr("The restore couldn't run yet.") : qsTr("The restore couldn't finish.")
    readonly property string detail: {
        if (kind === "applied") {
            var lines = [qsTr("This phone now has the backup's saved systems (%1), scan lists (%2), imported files (%3), decryption profiles (%4), search ranges and settings.").arg(count(outcome.systems)).arg(count(outcome.scanLists)).arg(count(outcome.libraryFiles)).arg(count(outcome.profiles))];
            var replays = count(outcome.recordingsToChoose);
            if (replays === 1)
                lines.push(qsTr("1 replay system needs its recording chosen again."));
            else if (replays > 1)
                lines.push(qsTr("%1 replay systems need their recording chosen again.").arg(replays));
            if ((outcome.notIncluded || []).length)
                lines.push(Util.notIncludedText(outcome.notIncluded));
            return lines.join("\n");
        }
        if (kind === "failed")
            return failedReason(outcome.message);
        // One whose apply has started can only be finished; the storage reason
        // already says to reopen DSD-neo.
        if (kind === "deferred")
            return partWay ? Util.restorePartWayText() : String(outcome.message || "").trim() === qsTranslate("BackupApply", "Not enough free storage to restore. Free up storage and reopen DSD-neo") ? "" : qsTr("Reopen DSD-neo to try again.");
        return "";
    }
    // A deferred restore is offered for discarding only while it can be.
    readonly property bool partWay: backup.restorePending && !backup.restoreCancellable
    readonly property bool discardable: kind === "deferred" && backup.restoreCancellable
    readonly property string speech: [title, detail].filter(function (text) {
        return text.length > 0;
    }).join(" ")

    visible: kind.length > 0
    height: body.implicitHeight + 2 * Theme.cardPadding
    border.color: problem ? Theme.alertBorder : Theme.panelBorder
    Accessible.role: Accessible.AlertMessage
    Accessible.name: speech
    readonly property bool navigationAllowed: Navigation.allows(card)
    Accessible.ignored: !navigationAllowed || !visible

    function count(value) {
        var n = Number(value);
        return n > 0 ? Math.floor(n) : 0;
    }
    // The restore's reasons are clauses without a full stop.
    function sentence(text) {
        var clause = String(text || "").trim();
        return clause.length && !/[.!?]$/.test(clause) ? clause + "." : clause;
    }
    // The title already says nothing was changed; the reason keeps only its cause.
    function failedReason(text) {
        var clause = String(text || "").trim();
        var tail = qsTranslate("BackupApply", "Nothing was changed");
        if (clause.endsWith(tail))
            clause = clause.slice(0, clause.length - tail.length).trim();
        return sentence(clause.replace(/[.\s]+$/, ""));
    }
    function announceOutcome() {
        if (!visible || !speech.length || announced === speech)
            return;
        announced = speech;
        announcements++;
        if (typeof Accessible.announce === "function")
            Accessible.announce(speech);
    }
    function discard() {
        actionError = "";
        var result = backup.discardDeferredRestore();
        if (result.discarded)
            return;
        actionError = result.error === "RestoreInProgress" ? Util.restorePartWayText() : qsTr("Couldn't discard the restore. DSD-neo tries it again the next time it opens.");
    }
    function dismiss() {
        actionError = "";
        backup.acknowledgeStartupOutcome();
    }
    // Later, once every binding has the new outcome.
    onOutcomeChanged: actionError = ""
    onSpeechChanged: Qt.callLater(announceOutcome)
    onVisibleChanged: Qt.callLater(announceOutcome)
    Component.onCompleted: Qt.callLater(announceOutcome)

    Column {
        id: body
        x: Theme.cardPadding
        y: Theme.cardPadding
        width: parent.width - 2 * Theme.cardPadding
        spacing: 10

        Text {
            objectName: "backupResultTitle"
            width: parent.width
            text: card.title
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(16)
            font.weight: Font.DemiBold
            color: Theme.textPrimary
        }
        Text {
            objectName: "backupResultDetail"
            width: parent.width
            visible: text.length > 0
            text: card.detail
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(13)
            color: Theme.textSecondary
        }
        Text {
            objectName: "backupResultError"
            width: parent.width
            visible: text.length > 0
            text: card.actionError
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(13)
            color: Theme.alert
            Accessible.role: Accessible.AlertMessage
            Accessible.name: text
        }
        Flow {
            width: parent.width
            spacing: 8
            OutlineButton {
                objectName: "backupResultDiscard"
                visible: card.discardable
                width: (parent.width - 8) / 2
                text: qsTr("Discard restore")
                onClicked: card.discard()
            }
            OutlineButton {
                objectName: "backupResultDismiss"
                width: card.discardable ? (parent.width - 8) / 2 : parent.width
                text: qsTr("Dismiss")
                onClicked: card.dismiss()
            }
        }
    }
}
