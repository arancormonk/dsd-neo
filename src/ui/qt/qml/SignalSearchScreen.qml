// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import "Util.js" as Util

Item {
    id: screen
    objectName: "signalSearchScreen"
    property var searchModel: null
    property bool sessionActive: false
    // Main binds this to its failure text (connection, USB access and
    // configuration failures). Never assigned here: an assignment would replace
    // that binding, and no later failure would reach this screen, which covers
    // Home's banner.
    property string failure: ""
    // This screen's own refusal of a Start or Resume (searchModel.arm()), shown
    // beside the host's failure until the next attempt or until Back closes it.
    property string localFailure: ""
    property string sourceType: "usb"
    property string host: ""
    property int port: 1234
    // The receiver the running session uses, copied from the decoder's options
    // snapshot (Main binds metrics.radioSource). Home seeds the source fields
    // only when it opens this screen; one reopened over a running search after
    // Activity recreation never passed through it. So while a search the decoder
    // owns runs, the fields follow its receiver: a paused range edit offers that
    // receiver's gain controls, and a stopped search restarts on it.
    property var sessionSource: ({})
    readonly property var status: searchModel ? searchModel.status : ({})
    readonly property var hit: status.hit || ({})
    readonly property int phase: status.phase || 0
    readonly property bool receiving: sessionActive && !!hit.id && hit.id !== "0" && !hit.ended && (phase === 2 || phase === 3 || phase === 4 || phase === 5)
    readonly property bool editable: !sessionActive || phase === 6
    readonly property bool ownedSession: sessionActive && !!status.owned
    signal closed
    signal backRequested
    signal start(string sourceType, string host, int port, string frequency)
    // A Start refused before it reached Main: any start asked for earlier and
    // still waiting (for USB access) is no longer what this screen wants.
    signal startRefused
    signal stop
    signal saveDetection(var hit)
    function followSessionSource() {
        var source = sessionSource || {};
        if (!ownedSession || ["usb", "airspy", "rtltcp"].indexOf(source.sourceType) < 0)
            return;
        sourceType = source.sourceType;
        if (source.sourceType === "rtltcp") {
            host = source.host || "";
            port = source.port || 1234;
        }
    }
    onSessionSourceChanged: followSessionSource()
    onOwnedSessionChanged: followSessionSource()
    Component.onCompleted: followSessionSource()
    function requestClose() {
        if (!sessionActive) {
            localFailure = "";
            closed();
        } else {
            backRequested();
        }
    }
    function act(action) {
        if (searchModel)
            searchModel.control(action, String(status.session || ""), String(hit.id || ""));
    }
    Rectangle { anchors.fill: parent; color: Theme.bg }
    Column {
        id: header
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: Theme.screenPadding
        spacing: 8
        Row {
            width: parent.width
            spacing: 12
            IconButton { objectName: "searchBack"; icon: "back"; accessibleName: qsTr("Back"); onClicked: screen.requestClose() }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Signal search")
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(22)
                font.weight: Font.Bold
                color: Theme.textPrimary
            }
        }
        Text {
            width: parent.width
            text: sessionActive ? [qsTr("Stopped"), qsTr("Searching"), qsTr("Tuning"), qsTr("Identifying"), qsTr("Listening"), qsTr("Held"), qsTr("Paused"), qsTr("Search error")][phase] : qsTr("Find strong signals across your frequency ranges")
            color: Theme.textSecondary
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(15)
            wrapMode: Text.Wrap
        }
    }
    PlexFlickable {
        id: scroll
        anchors.top: header.bottom
        anchors.topMargin: Theme.gap
        anchors.bottom: footer.top
        anchors.bottomMargin: Theme.gap
        anchors.left: parent.left
        anchors.right: parent.right
        contentHeight: content.height + Theme.gap
        clip: true
        Column {
            id: content
            x: (parent.width - width) / 2
            width: Math.min(Theme.formWidth, parent.width - 2 * Theme.screenPadding)
            spacing: Theme.gap
            Text {
                objectName: "searchMessage"
                width: parent.width
                visible: text.length > 0
                text: [screen.localFailure, screen.failure].filter(function (s) { return s.length > 0; }).join("\n") || screen.status.message || (searchModel ? searchModel.error : "")
                color: Theme.textPrimary
                wrapMode: Text.Wrap
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(14)
                Accessible.role: Accessible.AlertMessage
            }
            Column {
                width: parent.width
                visible: !screen.sessionActive
                spacing: 12
                Text { text: qsTr("Radio"); color: Theme.textPrimary; font.family: Theme.sans; font.pixelSize: Theme.fontSize(17); font.weight: Font.DemiBold }
                Flow {
                    width: parent.width
                    spacing: 10
                    Repeater {
                        model: [{ key: "usb", label: qsTr("USB dongle") }, { key: "airspy", label: "Airspy" }, { key: "rtltcp", label: "RTL-TCP" }]
                        DecodeChip {
                            required property var modelData
                            text: modelData.label
                            selected: screen.sourceType === modelData.key
                            onClicked: screen.sourceType = modelData.key
                        }
                    }
                }
                PlexTextField { id: hostField; width: parent.width; visible: screen.sourceType === "rtltcp"; label: qsTr("Host"); text: screen.host; onTextChanged: screen.host = text }
                PlexTextField { id: portField; width: parent.width; visible: screen.sourceType === "rtltcp"; label: qsTr("Port"); text: String(screen.port); inputMethodHints: Qt.ImhDigitsOnly }
                Text {
                    width: parent.width
                    text: qsTr("RF strength is not a distance measurement. Search uses one radio; each detection is identified, then monitored before searching continues.")
                    color: Theme.textSecondary
                    wrapMode: Text.Wrap
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontSize(14)
                }
            }
            Column {
                width: parent.width
                visible: screen.receiving
                spacing: 10
                Text {
                    width: parent.width
                    text: (screen.hit.freqMHz || "") + " MHz"
                    color: Theme.textPrimary
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(28)
                    wrapMode: Text.Wrap
                }
                Text {
                    objectName: "searchStrength"
                    width: parent.width
                    text: (screen.status.livePowerValid ? qsTr("Now %1 dBFS · detected %2 dBFS").arg(Number(screen.status.livePower).toFixed(1)).arg(Number(screen.hit.power || 0).toFixed(1)) : Number(screen.hit.power || 0).toFixed(1) + qsTr(" dBFS")) + " · " + (screen.hit.range || "")
                    color: Theme.textSecondary
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(14)
                    wrapMode: Text.Wrap
                }
                Text {
                    width: parent.width
                    objectName: "searchIdentification"
                    text: phase === 2 ? qsTr("Tuning to detection…") : screen.hit.confirmed ? screen.hit.protocol + qsTr(" · confirmed") : phase === 3 || screen.status.identifying ? qsTr("Trying digital identification…") : screen.hit.unresolved ? qsTr("NFM monitoring · digital identification unresolved") : screen.hit.am ? qsTr("AM monitoring") : qsTr("NFM monitoring")
                    color: Theme.textPrimary
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontSize(16)
                    wrapMode: Text.Wrap
                }
                Text {
                    width: parent.width
                    visible: text.length > 0
                    text: [screen.hit.access || "", screen.hit.tone || "", screen.hit.targetText || (screen.hit.target ? qsTr("Target %1").arg(screen.hit.target) : ""), screen.hit.sourceText || (screen.hit.source ? qsTr("Source %1").arg(screen.hit.source) : "")].filter(function (s) { return s.length > 0; }).join(" · ")
                    color: Theme.textSecondary
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontSize(14)
                    wrapMode: Text.Wrap
                }
                Text {
                    width: parent.width
                    visible: (screen.hit.knownLabel || "").length > 0
                    text: qsTr("Saved frequency: %1").arg(screen.hit.knownLabel || "")
                    color: Theme.textSecondary
                    wrapMode: Text.Wrap
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontSize(14)
                }
                Flow {
                    width: parent.width
                    spacing: 10
                    OutlineButton { objectName: "searchHold"; text: screen.phase === 5 ? qsTr("Release Hold") : qsTr("Hold"); onClicked: screen.act(3) }
                    OutlineButton { objectName: "searchSkip"; text: qsTr("Skip"); onClicked: screen.act(4) }
                    OutlineButton { text: qsTr("Avoid this session"); onClicked: screen.act(5) }
                    OutlineButton { text: qsTr("Avoid permanently"); onClicked: screen.act(6) }
                    OutlineButton { objectName: "searchSave"; text: qsTr("Save as system"); onClicked: screen.saveDetection(Object.assign({}, screen.hit)) }
                }
            }
            Text {
                width: parent.width
                visible: screen.sessionActive
                text: qsTr("Pass %1 · range %2 · channel %3 of %4").arg(screen.status.pass || 1).arg((screen.status.range || 0) + 1).arg((screen.status.channel || 0) + 1).arg(screen.status.count || 0)
                color: Theme.textSecondary
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(13)
                wrapMode: Text.Wrap
            }
            Text { text: qsTr("Search ranges"); color: Theme.textPrimary; font.family: Theme.sans; font.pixelSize: Theme.fontSize(17); font.weight: Font.DemiBold }
            Text {
                width: parent.width
                visible: !screen.editable
                text: qsTr("Pause to edit ranges. Changes apply when you restart the search.")
                color: Theme.textSecondary
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(13)
                wrapMode: Text.Wrap
            }
            Repeater {
                model: searchModel ? searchModel.ranges : []
                Column {
                    required property var modelData
                    required property int index
                    width: parent.width
                    spacing: 8
                    PlexCheckBox {
                        width: parent.width
                        text: modelData.name + " · " + modelData.startMHz + "–" + modelData.endMHz + " MHz"
                        checked: modelData.enabled
                        enabled: screen.editable
                        onClicked: searchModel.saveRange(index, Object.assign({}, modelData, { enabled: checked }))
                    }
                    Flow {
                        width: parent.width
                        visible: screen.editable
                        spacing: 8
                        OutlineButton { text: qsTr("Edit"); accessibleName: qsTr("Edit %1").arg(modelData.name); onClicked: editor.openFor(index, modelData) }
                        OutlineButton { text: qsTr("Move up"); enabled: index > 0; onClicked: searchModel.moveRange(index, -1) }
                        OutlineButton { text: qsTr("Remove"); onClicked: searchModel.removeRange(index) }
                    }
                }
            }
            OutlineButton { visible: screen.editable; width: parent.width; text: qsTr("Add range"); onClicked: editor.openFor(-1, searchModel.defaultRange()) }
            Text { text: qsTr("Recent detections"); color: Theme.textPrimary; font.family: Theme.sans; font.pixelSize: Theme.fontSize(17); font.weight: Font.DemiBold }
            Text {
                width: parent.width
                visible: !searchModel || searchModel.history.length === 0
                text: qsTr("Detections appear here even when no digital content is decoded.")
                color: Theme.textSecondary
                wrapMode: Text.Wrap
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(14)
            }
            Repeater {
                model: searchModel ? searchModel.history.slice(0, historyLimit) : []
                Column {
                    required property var modelData
                    width: parent.width
                    spacing: 5
                    Text { width: parent.width; text: modelData.freqMHz + " MHz · " + Number(modelData.power).toFixed(1) + " dBFS"; color: Theme.textPrimary; font.family: Theme.mono; font.pixelSize: Theme.fontSize(15); wrapMode: Text.Wrap }
                    Text {
                        width: parent.width
                        text: (modelData.confirmed ? modelData.protocol + qsTr(" · confirmed") : qsTr("RF only")) + (modelData.ended ? qsTr(" · ended") : "") + (modelData.knownLabel ? "\n" + modelData.knownLabel : "")
                        color: Theme.textSecondary
                        font.family: Theme.sans
                        font.pixelSize: Theme.fontSize(13)
                        wrapMode: Text.Wrap
                    }
                    OutlineButton { text: qsTr("Save as system"); accessibleName: qsTr("Save %1 MHz as system").arg(modelData.freqMHz); onClicked: screen.saveDetection(Object.assign({}, modelData)) }
                    Rectangle { width: parent.width; height: 1; color: Theme.panelBorder }
                }
            }
            OutlineButton { text: qsTr("Show more"); visible: searchModel && searchModel.history.length > historyLimit; onClicked: historyLimit += 25 }
            Text { visible: screen.sessionActive && searchModel && (searchModel.sessionAvoids || []).length > 0; text: qsTr("Avoided this session"); color: Theme.textPrimary; font.family: Theme.sans; font.pixelSize: Theme.fontSize(17); font.weight: Font.DemiBold }
            Repeater {
                model: screen.sessionActive && searchModel ? (searchModel.sessionAvoids || []) : []
                OutlineButton {
                    required property var modelData
                    objectName: "searchSessionAvoid"
                    width: parent.width
                    text: qsTr("Stop avoiding · %1 MHz").arg((Number(modelData) / 1e6).toFixed(6))
                    onClicked: searchModel.removeSessionAvoid(Number(modelData))
                }
            }
            Text { visible: searchModel && searchModel.avoids.length > 0; text: qsTr("Permanent avoids"); color: Theme.textPrimary; font.family: Theme.sans; font.pixelSize: Theme.fontSize(17); font.weight: Font.DemiBold }
            Repeater {
                model: searchModel ? searchModel.avoids : []
                OutlineButton {
                    required property var modelData
                    objectName: "searchPermanentAvoid"
                    width: parent.width
                    text: qsTr("Remove avoid · %1 MHz").arg((Number(modelData) / 1e6).toFixed(6))
                    onClicked: searchModel.removeAvoid(Number(modelData))
                }
            }
        }
    }
    property int historyLimit: 25
    Row {
        id: footer
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: Theme.screenPadding
        spacing: 12
        GradientButton {
            objectName: "searchStartPause"
            width: screen.sessionActive ? (parent.width - 12) / 2 : parent.width
            text: !screen.sessionActive ? qsTr("Start search") : screen.phase === 6 ? qsTr("Resume") : qsTr("Pause")
            enabled: searchModel && (!screen.sessionActive || screen.phase !== 7)
            onClicked: {
                if (screen.sessionActive) {
                    if (screen.phase === 6)
                        screen.localFailure = searchModel.arm();
                    else
                        screen.act(1);
                    return;
                }
                var error = searchModel.arm();
                if (error.length) {
                    screen.localFailure = error;
                    screen.startRefused();
                    return;
                }
                var ranges = searchModel.ranges.filter(function (r) { return r.enabled; });
                screen.localFailure = "";
                screen.start(screen.sourceType, screen.host, Number(portField.text), ranges[0].startMHz);
            }
        }
        OutlineButton { objectName: "searchStop"; visible: screen.sessionActive; width: (parent.width - 12) / 2; text: qsTr("Stop"); onClicked: screen.stop() }
    }
    SignalSearchRangeEditor { id: editor; searchModel: screen.searchModel; sourceType: screen.sourceType }
}
