// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

ModalSheet {
    id: sheet
    objectName: "siteSheet"
    panelObjectName: "siteSheetPanel"
    spacing: 14

    signal networkRequested

    function hex(value, width) {
        return Number(value).toString(16).toUpperCase().padStart(width, "0");
    }
    // Close when a session boundary clears its identity.
    readonly property bool hasSite: metrics.siteLine.length > 0
    onHasSiteChanged: {
        if (!hasSite)
            visible = false;
    }

    // DMR RAS (Restricted Access to System): its control and data fail the
    // standard CRC, which Relaxed CRC accepts. The sheet turns Relaxed CRC on
    // through the command and preference Settings uses, and waits for the
    // session's snapshot to confirm it.
    readonly property bool rasShown: metrics.siteProtocol === "DMR" && metrics.dmrRasDetected === true
    readonly property bool relaxedCrcConfigured: metrics.relaxedCrcConfigured === true
    property bool relaxedCrcPending: false
    property string relaxedCrcError: ""
    onRelaxedCrcConfiguredChanged: {
        if (relaxedCrcConfigured) {
            relaxedCrcPending = false;
            relaxedCrcAckTimer.stop();
        }
    }
    onRasShownChanged: relaxedCrcError = ""
    onVisibleChanged: relaxedCrcError = ""

    function turnOnRelaxedCrc() {
        if (relaxedCrcPending)
            return;
        relaxedCrcError = "";
        if (!commands.setRelaxedCrc(true)) {
            relaxedCrcError = qsTr("Could not change the running decoder's setting. Try again.");
            return;
        }
        prefs.relaxedCrc = true;
        relaxedCrcPending = !relaxedCrcConfigured;
        if (relaxedCrcPending)
            relaxedCrcAckTimer.restart();
    }

    Timer {
        id: relaxedCrcAckTimer
        interval: 5000
        onTriggered: {
            sheet.relaxedCrcPending = false;
            sheet.relaxedCrcError = qsTr("The decoder has not confirmed the change. Try again.");
        }
    }

    Text {
        width: parent.width
        text: qsTr("Site identity") + " · " + metrics.siteProtocol
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(20)
        color: Theme.textPrimary
    }
    Text {
        objectName: "siteRetained"
        width: parent.width
        visible: !metrics.siteConfirmed
        text: qsTr("Retained identity · no current sync")
        wrapMode: Text.Wrap
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(13)
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
                    font.pixelSize: Theme.fontSize(13)
                    color: Theme.textSubdued
                }
                Text {
                    objectName: modelData.name
                    width: (parent.width - parent.spacing) / 2
                    text: modelData.value
                    textFormat: Text.PlainText
                    wrapMode: Text.WrapAnywhere
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(13)
                    color: Theme.textPrimary
                }
            }
        }
    }

    FieldGrid {
        visible: metrics.siteProtocol === "P25"
        fields: [
            {
                label: qsTr("NAC"),
                name: "siteNac",
                valid: metrics.p25NacValid,
                value: hex(metrics.p25Nac, 3)
            },
            {
                label: qsTr("WACN"),
                name: "siteWacn",
                valid: metrics.p25WacnValid,
                value: hex(metrics.p25Wacn, 5)
            },
            {
                label: qsTr("SYS"),
                name: "siteSys",
                valid: metrics.p25SysIdValid,
                value: hex(metrics.p25SysId, 3)
            },
            {
                label: qsTr("RFSS"),
                name: "siteRfss",
                valid: metrics.p25Rfss > 0,
                value: metrics.p25Rfss
            },
            {
                label: qsTr("SITE"),
                name: "siteId",
                valid: metrics.p25Site > 0,
                value: metrics.p25Site
            },
            {
                label: qsTr("LRA"),
                name: "siteLra",
                valid: metrics.p25LraValid,
                value: hex(metrics.p25Lra, 2)
            },
            {
                label: qsTr("Phase 2 parameters"),
                name: "sitePhase2",
                valid: true,
                value: metrics.p25Phase2ParamsReady ? qsTr("Ready") : qsTr("Incomplete")
            },
            {
                label: qsTr("Phase 2 seed"),
                name: "sitePhase2Seed",
                valid: metrics.p25Phase2Seed.length > 0,
                value: metrics.p25Phase2SeedSet ? metrics.p25Phase2Seed + " " + qsTr("(set)") : metrics.p25Phase2Seed
            }
        ]
    }
    FieldGrid {
        visible: metrics.siteProtocol === "DMR"
        fields: [
            {
                label: qsTr("Color code"),
                name: "siteDmrColor",
                valid: metrics.dmrColorCode >= 0,
                value: metrics.dmrColorCode
            },
            {
                label: qsTr("Site"),
                name: "siteDmrText",
                valid: metrics.dmrSiteText.length > 0,
                value: metrics.dmrSiteText
            },
            {
                label: qsTr("Rest LSN"),
                name: "siteDmrRest",
                valid: metrics.dmrRestLsn > 0,
                value: metrics.dmrRestLsn
            },
            {
                label: qsTr("Free LCN"),
                name: "siteDmrXptFree",
                valid: metrics.dmrXptFreeLcn >= 0,
                value: metrics.dmrXptFreeLcn
            },
            {
                label: qsTr("System code"),
                name: "siteDmrSyscode",
                valid: metrics.dmrT3Syscode >= 0,
                value: hex(metrics.dmrT3Syscode, 4)
            },
            {
                label: qsTr("Trunking"),
                name: "siteDmrBrand",
                valid: metrics.dmrBrand.length > 0,
                value: metrics.dmrBrand
            },
            {
                label: qsTr("Access"),
                name: "siteDmrRas",
                valid: metrics.dmrRasDetected === true,
                value: "RAS"
            }
        ]
    }
    FieldGrid {
        visible: metrics.siteProtocol === "NXDN" || metrics.siteProtocol === "IDAS"
        fields: [
            {
                label: metrics.siteProtocol === "IDAS" ? qsTr("Area") : qsTr("RAN"),
                name: "siteRan",
                valid: metrics.nxdnRan >= 0,
                value: metrics.nxdnRan
            },
            {
                label: qsTr("Category"),
                name: "siteCategory",
                valid: metrics.nxdnSiteCode > 0 && metrics.nxdnLocationCategory.length > 0,
                value: metrics.nxdnLocationCategory
            },
            {
                label: qsTr("System"),
                name: "siteNxdnSys",
                valid: metrics.siteProtocol === "IDAS" ? metrics.nxdnSysCode > 0 : metrics.nxdnSiteCode > 0,
                value: metrics.nxdnSysCode
            },
            {
                label: qsTr("Site"),
                name: "siteNxdnSite",
                valid: metrics.nxdnSiteCode > 0,
                value: metrics.nxdnSiteCode
            }
        ]
    }
    FieldGrid {
        visible: metrics.siteProtocol === "EDACS"
        fields: [
            {
                label: qsTr("Site"),
                name: "siteEdacs",
                valid: metrics.edacsSiteText.length > 0,
                value: metrics.edacsSiteText
            },
            {
                label: qsTr("System"),
                name: "siteEdacsSystem",
                valid: metrics.edacsSystem >= 0,
                value: metrics.edacsSystem
            },
            {
                label: qsTr("Area"),
                name: "siteEdacsArea",
                valid: metrics.edacsArea >= 0,
                value: metrics.edacsArea
            },
            {
                label: qsTr("CC LCN"),
                name: "siteEdacsCcLcn",
                valid: metrics.edacsCcLcn >= 0,
                value: metrics.edacsCcLcn
            }
        ]
    }
    FieldGrid {
        visible: metrics.siteProtocol === "dPMR" || metrics.siteProtocol === "M17"
        fields: [
            {
                label: metrics.siteProtocol === "M17" ? qsTr("CAN") : qsTr("Color code"),
                name: "siteAccess",
                valid: metrics.siteAccessCode >= 0,
                value: metrics.siteAccessCode
            }
        ]
    }
    FieldGrid {
        visible: metrics.siteProtocol === "TETRA"
        fields: [
            {
                label: qsTr("MCC"),
                name: "siteTetraMcc",
                valid: metrics.tetraSiteMcc >= 0,
                value: metrics.tetraSiteMcc
            },
            {
                label: qsTr("MNC"),
                name: "siteTetraMnc",
                valid: metrics.tetraSiteMnc >= 0,
                value: metrics.tetraSiteMnc
            },
            {
                label: qsTr("Color code"),
                name: "siteTetraColor",
                valid: metrics.siteAccessCode >= 0,
                value: metrics.siteAccessCode
            },
            {
                label: qsTr("LA"),
                name: "siteTetraLa",
                valid: metrics.tetraSiteLa >= 0,
                value: metrics.tetraSiteLa
            }
        ]
    }
    FieldGrid {
        visible: true
        fields: [
            {
                label: qsTr("CC"),
                name: "siteCcFreq",
                valid: metrics.ccFreqHz > 0,
                value: (metrics.ccFreqHz / 1000000).toFixed(6) + " MHz"
            },
            {
                label: qsTr("VC"),
                name: "siteVcFreq",
                valid: metrics.vcFreqHz > 0,
                value: (metrics.vcFreqHz / 1000000).toFixed(6) + " MHz"
            }
        ]
    }
    // What RAS means for decoding, in the terminal notice's words.
    Text {
        objectName: "siteDmrRasNote"
        width: parent.width
        visible: sheet.rasShown
        text: metrics.dmrRasNeedsRelaxed !== true ? qsTr("Relaxed CRC is on, so this RAS system's control and data decode.")
            : metrics.relaxedCrcRowOverride === true ? qsTr("DMR RAS (Restricted Access to System) detected: control and data fail the standard CRC; Relaxed CRC decodes them. This channel sets strict CRC itself; Relaxed CRC applies from the next channel without its own.")
            : qsTr("DMR RAS (Restricted Access to System) detected: control and data fail the standard CRC; Relaxed CRC decodes them.")
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(13)
        color: Theme.textSecondary
    }
    Text {
        objectName: "siteRelaxedCrcError"
        width: parent.width
        visible: sheet.rasShown && text.length > 0
        text: sheet.relaxedCrcError
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(13)
        color: Theme.textSecondary
    }
    OutlineButton {
        objectName: "siteNetworkButton"
        width: parent.width
        text: qsTr("Network")
        visible: metrics.siteProtocol === "P25"
        onClicked: sheet.networkRequested()
    }
    // Not offered while a scan row sets strict CRC itself: the row would overrule it.
    OutlineButton {
        objectName: "siteRelaxedCrcButton"
        width: parent.width
        text: qsTr("Turn on Relaxed CRC")
        visible: sheet.rasShown && metrics.dmrRasNeedsRelaxed === true && metrics.relaxedCrcRowOverride !== true
        enabled: metrics.optionsKnown === true && !sheet.relaxedCrcPending
        onClicked: sheet.turnOnRelaxedCrc()
        // Confirmed, it goes: keep a keyboard user's place in the sheet.
        onVisibleChanged: {
            if (!visible && activeFocus)
                siteCloseButton.forceActiveFocus(Qt.TabFocusReason);
        }
    }
    OutlineButton {
        id: siteCloseButton
        objectName: "siteCloseButton"
        width: parent.width
        text: qsTr("Close")
        onClicked: sheet.visible = false
    }
}
