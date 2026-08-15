// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtQuick.Dialogs

// The imported-files library: every channel map, talkgroup list, and key file
// that has been copied into app storage, with import/update/remove flows. The
// wizard's pickers reference the same library, so a file imported here serves
// any saved system.
Item {
    id: screen

    signal closed()
    // Asks Main.qml to push the RadioReference screen over this one.
    signal openRadioReference()

    // FileDialog routing: a row index means "update that row in place";
    // -1 means a new import of pendingType.
    property int pendingRow: -1
    property string pendingType: "chan"
    property bool pendingKeyHex: false
    // Row the action sheet is open on.
    property int actionRow: -1
    // Transient outcome line under the header; magenta when it reports a problem.
    property string notice: ""
    property bool noticeIsProblem: false

    function nounFor(type, count) {
        if (type === "chan")
            return count === 1 ? qsTr("channel") : qsTr("channels")
        if (type === "group")
            return count === 1 ? qsTr("talkgroup") : qsTr("talkgroups")
        return count === 1 ? qsTr("key") : qsTr("keys")
    }

    function summaryFor(type, accepted, skipped, importedAt) {
        if (accepted === 0)
            return qsTr("No usable rows — check the file format")
        var parts = [accepted + " " + nounFor(type, accepted)]
        if (skipped > 0)
            parts.push(skipped === 1 ? qsTr("1 row skipped") : qsTr("%1 rows skipped").arg(skipped))
        parts.push(Qt.formatDate(new Date(importedAt * 1000), "MMM d"))
        return parts.join(" · ")
    }

    function resultNotice(verb, result) {
        if (!result.ok) {
            screen.notice = qsTr("Could not read that file")
            screen.noticeIsProblem = true
            return
        }
        if (result.error === "empty") {
            screen.notice = qsTr("%1 — no usable rows. Check the file format, then update it.").arg(verb)
            screen.noticeIsProblem = true
            return
        }
        // The model already knew the kind when it built this result; reverse-
        // looking it up by path would scan the library twice and quietly name a
        // channel map "keys" if the lookup missed.
        screen.notice = verb + " · " + result.accepted + " " + screen.nounFor(result.type, result.accepted)
        screen.noticeIsProblem = false
    }

    // No CSV name filter: on Android it becomes a SAF MIME filter, and the
    // Files app indexes .csv as text/comma-separated-values — not the text/csv
    // Qt asks for — which greys out exactly the files the user came to pick.
    //
    // The kind was already chosen in the sheet, and the dry run counts rows
    // against that kind, so a file of the wrong kind lands here as "no usable
    // rows". That check is by content, not by name: a channel map and a decimal
    // key list are both `number,number`, and the header line is free text, so
    // what separates them is the channel importer refusing a second column that
    // cannot be a radio frequency. Two lists of the same kind are still
    // indistinguishable — nothing stops one site's map being picked for another.
    FileDialog {
        id: fileDialog

        onAccepted: {
            var reference = selectedFile.toString()
            var hint = reference.substring(reference.lastIndexOf('/') + 1)
            if (screen.pendingRow >= 0) {
                screen.resultNotice(qsTr("Updated"), importedFiles.updateFile(screen.pendingRow, reference, hint))
            } else {
                var type = screen.pendingType === "keys"
                           ? (screen.pendingKeyHex ? "keysHex" : "keysDec") : screen.pendingType
                screen.resultNotice(qsTr("Imported"), importedFiles.importFile(reference, hint, type))
            }
            screen.pendingRow = -1
        }
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.bg
    }

    // Header: back chevron, title, count.
    Item {
        id: header

        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: Theme.screenPadding
        height: 46

        Text {
            id: back
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            text: "‹"
            font.pixelSize: 28
            color: Theme.textSecondary

            TapHandler {
                onTapped: screen.closed()
            }
        }

        Text {
            anchors.left: back.right
            anchors.leftMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Imported files")
            font.family: Theme.sans
            font.pixelSize: 22
            font.weight: Font.Bold
            font.letterSpacing: -0.22
            color: Theme.textPrimary
        }
    }

    MicroLabel {
        id: countLabel
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.topMargin: 6
        anchors.leftMargin: Theme.screenPadding
        text: importedFiles.count === 1 ? qsTr("1 file") : qsTr("%1 files").arg(importedFiles.count)
    }

    Text {
        id: noticeLine

        anchors.top: countLabel.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.topMargin: 8
        anchors.leftMargin: Theme.screenPadding
        anchors.rightMargin: Theme.screenPadding
        visible: screen.notice.length > 0
        text: screen.notice
        font.family: Theme.sans
        font.pixelSize: 13
        color: screen.noticeIsProblem ? Theme.magenta : Theme.textSubdued
        wrapMode: Text.Wrap
    }

    ListView {
        id: fileList

        // Named so UI_QT_QML_CALL_LISTS can reach it with findChild().
        objectName: "importedFilesList"

        anchors.top: noticeLine.visible ? noticeLine.bottom : countLabel.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: radioReferenceButton.visible ? radioReferenceButton.top : importButton.top
        anchors.topMargin: 14
        anchors.bottomMargin: 14
        // The screen padding is the view's, not the delegate's: a vertical
        // ListView writes each item's x itself on every layout pass, so an x
        // declared in the delegate is overwritten with 0 and the cards end up
        // flush against the left edge with the whole inset piled on the right.
        anchors.leftMargin: Theme.screenPadding
        anchors.rightMargin: Theme.screenPadding
        clip: true
        model: importedFiles
        spacing: Theme.gap

        delegate: UiPanel {
            id: fileRow

            required property int index
            required property string name
            required property string path
            required property string type
            required property int accepted
            required property int skipped
            required property var importedAt

            width: ListView.view.width
            height: rowColumn.height + 2 * Theme.cardPadding

            Column {
                id: rowColumn

                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: Theme.cardPadding
                spacing: 5

                Item {
                    width: parent.width
                    height: Math.max(fileName.implicitHeight, badge.implicitHeight)

                    Text {
                        id: fileName
                        anchors.left: parent.left
                        anchors.right: badge.left
                        anchors.rightMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        text: fileRow.name
                        font.family: Theme.sans
                        font.pixelSize: 15
                        font.weight: Font.DemiBold
                        color: Theme.textPrimary
                        elide: Text.ElideRight
                    }

                    CsvTypeBadge {
                        id: badge
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        type: fileRow.type
                    }
                }

                Text {
                    width: parent.width
                    text: screen.summaryFor(fileRow.type, fileRow.accepted, fileRow.skipped, Number(fileRow.importedAt))
                    font.family: Theme.sans
                    font.pixelSize: 12
                    color: fileRow.accepted === 0 ? Theme.magenta : Theme.textSubdued
                    elide: Text.ElideRight
                }
            }

            TapHandler {
                onTapped: {
                    screen.actionRow = fileRow.index
                    actionSheet.visible = true
                }
            }
        }

        // Empty state: an invitation to act, not mood.
        Text {
            // Named so UI_QT_QML_CALL_LISTS can reach it with findChild().
            objectName: "importsEmptyMessage"

            anchors.centerIn: parent
            // The view is already inset by the screen padding, so this is the
            // second step in from the edge, not the first.
            width: parent.width - 2 * Theme.screenPadding
            visible: importedFiles.count === 0
            text: qsTr("No imported files yet. Import a channel map, talkgroup list, or key file to use it in your systems.")
            font.family: Theme.sans
            font.pixelSize: 14
            color: Theme.textSubdued
            wrapMode: Text.Wrap
            horizontalAlignment: Text.AlignHCenter
        }
    }

    // The header here holds only the back chevron and the title, so the second
    // way in stacks above the primary action rather than sitting up there.
    OutlineButton {
        id: radioReferenceButton

        // Named so UI_QT_QML_CALL_LISTS can reach it with findChild().
        objectName: "importFromRadioReferenceButton"

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: importButton.top
        anchors.margins: Theme.screenPadding
        anchors.bottomMargin: Theme.gap
        visible: radioReference.available
        text: qsTr("Import from RadioReference")
        onClicked: {
            screen.notice = ""
            screen.openRadioReference()
        }
    }

    GradientButton {
        id: importButton

        // Named so UI_QT_QML_CALL_LISTS can reach it with findChild().
        objectName: "importFileButton"

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: Theme.screenPadding
        anchors.bottomMargin: 22
        text: qsTr("Import file")
        onClicked: {
            screen.notice = ""
            typeSheet.visible = true
        }
    }

    // What kind of file is being imported; the parser and the flag differ per kind.
    ModalSheet {
        id: typeSheet

        MicroLabel {
            text: qsTr("Import file")
        }

        Text {
            width: parent.width
            text: qsTr("What does this file contain?")
            font.family: Theme.sans
            font.pixelSize: 15
            font.weight: Font.DemiBold
            color: Theme.textPrimary
            wrapMode: Text.Wrap
        }

        SegmentedControl {
            width: parent.width
            model: [qsTr("Channel map"), qsTr("Talkgroups"), qsTr("Keys")]
            currentIndex: screen.pendingType === "chan" ? 0 : screen.pendingType === "group" ? 1 : 2
            onSelected: function (index) {
                screen.pendingType = index === 0 ? "chan" : index === 1 ? "group" : "keys"
            }
        }

        SegmentedControl {
            width: parent.width
            visible: screen.pendingType === "keys"
            model: [qsTr("Decimal keys"), qsTr("Hex keys")]
            currentIndex: screen.pendingKeyHex ? 1 : 0
            onSelected: function (index) { screen.pendingKeyHex = index === 1 }
        }

        GradientButton {
            width: parent.width
            text: qsTr("Choose file")
            onClicked: {
                typeSheet.visible = false
                screen.pendingRow = -1
                fileDialog.open()
            }
        }
    }

    // Actions on one library file.
    ModalSheet {
        id: actionSheet

        Text {
            width: parent.width
            text: screen.actionRow >= 0 ? importedFiles.get(screen.actionRow).name : ""
            font.family: Theme.sans
            font.pixelSize: 15
            font.weight: Font.DemiBold
            color: Theme.textPrimary
            elide: Text.ElideRight
        }

        OutlineButton {
            width: parent.width
            text: qsTr("Update from file")
            onClicked: {
                actionSheet.visible = false
                screen.notice = ""
                screen.pendingRow = screen.actionRow
                fileDialog.open()
            }
        }

        OutlineButton {
            width: parent.width
            visible: decoderHost.running
            text: qsTr("Apply to running session")
            onClicked: {
                actionSheet.visible = false
                var entry = importedFiles.get(screen.actionRow)
                var sent = entry.type === "chan" ? commands.importChannelMap(entry.path)
                           : entry.type === "group" ? commands.importGroupList(entry.path)
                           : commands.importKeys(entry.path, entry.type === "keysHex")
                screen.notice = sent ? qsTr("Sent to decoder") : qsTr("The decoder is not accepting commands")
                screen.noticeIsProblem = !sent
            }
        }

        OutlineButton {
            width: parent.width
            text: qsTr("Remove")
            onClicked: {
                actionSheet.visible = false
                removeSheet.visible = true
            }
        }
    }

    // Removing deletes the stored copy; systems that reference it lose the file,
    // so say which ones before it happens.
    ModalSheet {
        id: removeSheet

        readonly property var usedBy: visible && screen.actionRow >= 0
                                      ? savedSystems.systemsReferencingPath(importedFiles.get(screen.actionRow).path)
                                      : []

        Text {
            width: parent.width
            text: screen.actionRow >= 0
                  ? qsTr("Remove %1?").arg(importedFiles.get(screen.actionRow).name) : ""
            font.family: Theme.sans
            font.pixelSize: 15
            font.weight: Font.DemiBold
            color: Theme.textPrimary
            wrapMode: Text.Wrap
        }

        Text {
            width: parent.width
            visible: removeSheet.usedBy.length > 0
            text: removeSheet.usedBy.length === 1
                  ? qsTr("Used by %1 — removing clears it from that system.").arg(removeSheet.usedBy[0])
                  : qsTr("Used by %1 — removing clears it from those systems.").arg(removeSheet.usedBy.join(", "))
            font.family: Theme.sans
            font.pixelSize: 13
            color: Theme.textSubdued
            wrapMode: Text.Wrap
        }

        OutlineButton {
            width: parent.width
            text: qsTr("Remove file")
            onClicked: {
                removeSheet.visible = false
                var path = importedFiles.get(screen.actionRow).path
                savedSystems.clearCsvPath(path)
                importedFiles.remove(screen.actionRow)
                screen.actionRow = -1
                screen.notice = qsTr("Removed")
                screen.noticeIsProblem = false
            }
        }
    }
}
