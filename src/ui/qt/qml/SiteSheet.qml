// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

ModalSheet {
    id: sheet
    objectName: "siteSheet"
    panelObjectName: "siteSheetPanel"
    spacing: 14

    function hex(value, width) {
        return Number(value).toString(16).toUpperCase().padStart(width, "0");
    }
    // Close when a session boundary clears its identity.
    readonly property bool hasSite: metrics.siteLine.length > 0
    onHasSiteChanged: { if (!hasSite) visible = false; }

    Text {
        width: parent.width
        text: qsTr("Site identity") + " · " + metrics.siteProtocol
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        font.family: Theme.sans
        font.pixelSize: 20 * Theme.fontScale
        color: Theme.textPrimary
    }
    Text {
        objectName: "siteRetained"
        width: parent.width
        visible: !metrics.siteConfirmed
        text: qsTr("Retained identity · no current sync")
        wrapMode: Text.Wrap
        font.family: Theme.sans
        font.pixelSize: 13 * Theme.fontScale
        color: Theme.textSubdued
    }
    component FieldGrid: Grid {
        property var fields: []
        width: parent.width
        columns: 1
        spacing: 10
        Repeater {
            model: fields
            delegate: Grid {
                required property var modelData
                width: parent.width
                columns: 2
                spacing: 10
                visible: modelData.valid
                Text {
                    width: (parent.width - parent.spacing) / 2
                    text: modelData.label
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    font.family: Theme.mono
                    font.pixelSize: 13 * Theme.fontScale
                    color: Theme.textSubdued
                }
                Text {
                    objectName: modelData.name
                    width: (parent.width - parent.spacing) / 2
                    text: modelData.value
                    textFormat: Text.PlainText
                    wrapMode: Text.WrapAnywhere
                    font.family: Theme.mono
                    font.pixelSize: 13 * Theme.fontScale
                    color: Theme.textPrimary
                }
            }
        }
    }

    FieldGrid {
        visible: metrics.siteProtocol === "P25"
        fields: [
            { label: qsTr("NAC"), name: "siteNac", valid: metrics.p25NacValid, value: hex(metrics.p25Nac, 3) },
            { label: qsTr("WACN"), name: "siteWacn", valid: metrics.p25WacnValid, value: hex(metrics.p25Wacn, 5) },
            { label: qsTr("SYS"), name: "siteSys", valid: metrics.p25SysIdValid, value: hex(metrics.p25SysId, 3) },
            { label: qsTr("RFSS"), name: "siteRfss", valid: metrics.p25Rfss > 0, value: metrics.p25Rfss },
            { label: qsTr("SITE"), name: "siteId", valid: metrics.p25Site > 0, value: metrics.p25Site },
            { label: qsTr("LRA"), name: "siteLra", valid: metrics.p25LraValid, value: hex(metrics.p25Lra, 2) },
            { label: qsTr("Phase 2 parameters"), name: "sitePhase2", valid: true, value: metrics.p25Phase2ParamsReady ? qsTr("Ready") : qsTr("Incomplete") }
        ]
    }
    FieldGrid {
        visible: metrics.siteProtocol === "DMR"
        fields: [
            { label: qsTr("Color code"), name: "siteDmrColor", valid: metrics.dmrColorCode >= 0, value: metrics.dmrColorCode },
            { label: qsTr("Site"), name: "siteDmrText", valid: metrics.dmrSiteText.length > 0, value: metrics.dmrSiteText },
            { label: qsTr("Rest LSN"), name: "siteDmrRest", valid: metrics.dmrRestLsn > 0, value: metrics.dmrRestLsn }
        ]
    }
    FieldGrid {
        visible: metrics.siteProtocol === "NXDN" || metrics.siteProtocol === "IDAS"
        fields: [
            { label: metrics.siteProtocol === "IDAS" ? qsTr("Area") : qsTr("RAN"), name: "siteRan", valid: metrics.nxdnRan >= 0, value: metrics.nxdnRan },
            { label: qsTr("Category"), name: "siteCategory", valid: metrics.nxdnSiteCode > 0 && metrics.nxdnLocationCategory.length > 0, value: metrics.nxdnLocationCategory },
            { label: qsTr("System"), name: "siteNxdnSys", valid: metrics.nxdnSiteCode > 0, value: metrics.nxdnSysCode },
            { label: qsTr("Site"), name: "siteNxdnSite", valid: metrics.nxdnSiteCode > 0, value: metrics.nxdnSiteCode }
        ]
    }
    FieldGrid {
        visible: metrics.siteProtocol === "EDACS"
        fields: [
            { label: qsTr("Site"), name: "siteEdacs", valid: metrics.edacsSiteText.length > 0, value: metrics.edacsSiteText }
        ]
    }
    FieldGrid {
        visible: true
        fields: [
            { label: qsTr("CC"), name: "siteCcFreq", valid: metrics.ccFreqHz > 0, value: (metrics.ccFreqHz / 1000000).toFixed(6) + " MHz" },
            { label: qsTr("VC"), name: "siteVcFreq", valid: metrics.vcFreqHz > 0, value: (metrics.vcFreqHz / 1000000).toFixed(6) + " MHz" }
        ]
    }
    OutlineButton {
        width: parent.width
        text: qsTr("Close")
        onClicked: sheet.visible = false
    }
}
