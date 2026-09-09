// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

ModalSheet {
    id: sheet
    objectName: "networkSheet"
    panelObjectName: "networkSheetPanel"
    property var network: p25Network
    onVisibleChanged: network.active = visible
    Component.onDestruction: network.active = false
    function open() { visible = true; }

    function hex(value, width) { return Number(value).toString(16).toUpperCase().padStart(width, "0"); }
    function rowText(section, row) {
        if (section === 0) {
            return (row.freqHz / 1000000).toFixed(6) + " MHz"
                + (row.isCurrentCc ? " [CC]" : "") + (row.isCandidate ? " [C]" : "")
                + " · SYS:" + hex(row.sysid, 3) + " RFSS:" + row.rfss + " Site:" + row.site
                + (row.wacnValid ? " WACN:" + hex(row.wacn, 5) : "")
                + (row.lraValid ? " LRA:" + hex(row.lra, 2) : "") + " CFVA:" + row.cfvaText;
        }
        if (section === 1)
            return "SG:" + row.sgid + (row.isPatch ? " · Patch" : " · Simulselect")
                + (row.groups.length ? " · TG:" + row.groups.join(", ") : "")
                + (row.radios.length ? " · RID:" + row.radios.join(", ") : "");
        return "RID:" + row.rid + (section === 2 ? " · TG:" + row.tg : "");
    }

    MicroLabel { text: qsTr("P25 Network") }
    Repeater {
        model: [qsTr("Neighbours"), qsTr("Patches"), qsTr("Affiliations"), qsTr("Radios")]
        Column {
            id: section
            required property int index
            required property string modelData
            readonly property var rows: [sheet.network.neighbours, sheet.network.patches,
                                         sheet.network.affiliations, sheet.network.radios][index]
            width: parent.width
            spacing: 8
            MicroLabel { text: section.modelData }
            Text {
                visible: section.rows.length === 0
                text: qsTr("(none announced)")
                font.family: Theme.sans
                color: Theme.textSubdued
            }
            Repeater {
                model: section.rows
                Text {
                    required property var modelData
                    width: parent.width
                    text: sheet.rowText(section.index, modelData)
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    font.family: Theme.mono
                    font.pixelSize: 12 * Theme.fontScale
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
