// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

ModalSheet {
    id: sheet
    objectName: "networkSheet"
    panelObjectName: "networkSheetPanel"
    property var network: p25Network
    onVisibleChanged: network.active = visible
    Component.onDestruction: network.active = false
    function open() {
        visible = true;
    }

    function hex(value, width) {
        return Number(value).toString(16).toUpperCase().padStart(width, "0");
    }
    function rowText(section, row) {
        if (section === 0) {
            return (row.freqHz / 1000000).toFixed(6) + " MHz" + (row.isCurrentCc ? " [CC]" : "") + (row.isCandidate ? " [C]" : "") + " · SYS:" + hex(row.sysid, 3) + " RFSS:" + row.rfss + " Site:" + row.site + (row.wacnValid ? " WACN:" + hex(row.wacn, 5) : "") + (row.lraValid ? " LRA:" + hex(row.lra, 2) : "") + " CFVA:" + row.cfvaText;
        }
        if (section === 1)
            return "SG:" + row.sgid + (row.isPatch ? " · Patch" : " · Simulselect") + (row.groups.length ? " · TG:" + row.groups.join(", ") : "") + (row.radios.length ? " · RID:" + row.radios.join(", ") : "");
        return "RID:" + row.rid + (section === 2 ? " · TG:" + row.tg : "");
    }
    // What tells one entry from another, for the rows' pace below.
    function entryKey(section, row) {
        if (section === 0)
            return String(row.freqHz);
        if (section === 1)
            return String(row.sgid);
        return row.rid + (section === 2 ? ":" + row.tg : "");
    }

    MicroLabel {
        text: qsTr("P25 Network")
    }
    Repeater {
        model: [qsTr("Neighbours"), qsTr("Patches"), qsTr("Affiliations"), qsTr("Radios")]
        Column {
            id: section
            required property int index
            required property string modelData
            readonly property var rows: [sheet.network.neighbours, sheet.network.patches, sheet.network.affiliations, sheet.network.radios][index]
            // Issue #782: each section lists its entries most recent first, so
            // an entry heard again moves to the top, and every row above where
            // it was then shows another entry: about once a second on a control
            // channel that cycles its neighbours. Each row is named at the
            // readings' pace (SpeechPacer), and at once when an entry arrives
            // or leaves; its text on screen stays live.
            readonly property string entries: rows.map(function (row) {
                return sheet.entryKey(section.index, row);
            }).sort().join(" ")
            readonly property var spokenRows: rowSpeech.spoken.length > 0 ? rowSpeech.spoken.split("\n") : []
            SpeechPacer {
                id: rowSpeech
                shown: Navigation.allows(section) && section.visible
                step: section.entries
                read: function () {
                    return section.rows.map(function (row) {
                        return sheet.rowText(section.index, row);
                    }).join("\n");
                }
            }
            width: parent.width
            spacing: 8
            MicroLabel {
                text: section.modelData
            }
            ReadableText {
                visible: section.rows.length === 0
                text: qsTr("(none announced)")
                font.family: Theme.sans
                color: Theme.textSubdued
            }
            // Issue #782: one row per entry, kept across refreshes (StableRows): a
            // refresh hands a new list, and a list model would rebuild every row
            // a screen reader is told about. Each row reads its entry in place,
            // and is named for it at the section's pace; a row the pace has not
            // reached yet, one just added, is named by its text.
            Repeater {
                model: StableRows {
                    size: section.rows.length
                }
                ReadableText {
                    required property int index
                    readonly property var row: section.rows[index]
                    width: parent.width
                    text: row ? sheet.rowText(section.index, row) : ""
                    Accessible.name: Accessible.ignored ? "" : index >= 0 && index < section.spokenRows.length ? section.spokenRows[index] : text
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(12)
                    color: Theme.textPrimary
                }
            }
        }
    }
    OutlineButton {
        objectName: "networkDone"
        width: parent.width
        text: qsTr("Done")
        onClicked: sheet.visible = false
    }
}
