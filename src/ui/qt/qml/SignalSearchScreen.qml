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
    readonly property bool receiving: receivingIn(status)
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
            setPort(source.port || 1234);
        }
    }
    // The port field keeps what was typed until told otherwise, so every
    // assignment from outside sets the field as well as the property.
    function setPort(value) {
        port = value;
        portField.text = String(value);
    }
    /** Fill the receiver as Home opens the screen; that is not an edit. */
    function seed(type, hostName, portNumber) {
        sourceType = type;
        host = hostName;
        setPort(portNumber);
        openedSource = sourceValue();
        resetView();
    }
    // The receiver as the screen opened (or as a stopped session left it):
    // closing while idle asks only once it differs.
    property string openedSource: ""
    function sourceValue() {
        var tcp = sourceType === "rtltcp";
        return JSON.stringify([sourceType, tcp ? host : "", tcp ? portField.text : ""]);
    }
    onSessionActiveChanged: {
        if (!sessionActive)
            openedSource = sourceValue();
    }
    onSessionSourceChanged: followSessionSource()
    onOwnedSessionChanged: followSessionSource()
    Component.onCompleted: followSessionSource()
    function requestClose() {
        if (sessionActive) {
            backRequested();
            return;
        }
        if (openedSource.length && sourceValue() !== openedSource) {
            discard.ask(function () {
                screen.localFailure = "";
                screen.closed();
            });
            return;
        }
        localFailure = "";
        closed();
    }
    DiscardDialog {
        id: discard
    }
    function act(action) {
        if (searchModel)
            searchModel.control(action, String(status.session || ""), String(hit.id || ""));
    }
    // ---- What the screen shows while a session runs ----
    // The range being searched, as the engine names it from its own
    // configuration: never the retained detection's, which can belong to an
    // earlier range, and a paused edit shows only once Resume applies it.
    // The range name alone gives way when the line would need more than its
    // two lines, so the channel count always shows.
    readonly property string statusLine: {
        var font = statusMetrics.font;
        var pass = qsTr("Pass %1").arg(status.pass || 1);
        var channel = qsTr("channel %1 of %2").arg((status.channel || 0) + 1).arg(status.count || 0);
        var name = status.rangeName || "";
        if (name.length === 0)
            return [pass, channel].join(" · ");
        var bounds = qsTr("%1–%2 MHz").arg(status.rangeStartMHz || "").arg(status.rangeEndMHz || "");
        var compose = function (n) { return [pass, n, bounds, channel].join(" · "); };
        var width = statusText.width - 1;
        if (width <= 0 || wrappedLines(compose(name), width) <= 2)
            return compose(name);
        var step = statusMetrics.advanceWidth("M");
        for (var budget = statusMetrics.advanceWidth(name) - step; budget > 0; budget -= step) {
            var shorter = statusMetrics.elidedText(name, Qt.ElideRight, budget);
            if (wrappedLines(compose(shorter), width) <= 2)
                return compose(shorter);
        }
        return compose("…");
    }
    /** Lines @p text takes at @p width in the status line's font, wrapped at
     * spaces as the line wraps it (a word wider than a line breaks anywhere). */
    function wrappedLines(text, width) {
        var space = statusMetrics.advanceWidth(" ");
        var lines = 1, x = 0;
        for (var word of text.split(" ")) {
            var w = statusMetrics.advanceWidth(word);
            if (x > 0 && x + space + w <= width) {
                x += space + w;
                continue;
            }
            if (x > 0)
                ++lines;
            lines += Math.floor(w / width);
            x = w % width;
        }
        return lines;
    }
    // Bumped by every accepted live measurement, so a fresh reading of an
    // unchanged level still marks the cue.
    readonly property var liveSeq: status.liveSeq || 0
    readonly property bool liveShown: receiving && !!status.livePowerValid
    /** Whether @p s is receiving a detection: its hit is live and the phase is a visit's. */
    function receivingIn(s) {
        var h = s.hit || {};
        return sessionActive && !!h.id && h.id !== "0" && !h.ended && [2, 3, 4, 5].indexOf(s.phase || 0) >= 0;
    }
    readonly property int enabledRanges: searchModel ? searchModel.ranges.filter(function (r) { return r.enabled; }).length : 0
    // Ranges are setup: collapsed while a search runs, unless opened here;
    // paused or idle, they are what the screen is for and stay open.
    property bool rangesExpanded: false
    readonly property bool rangesShown: editable || rangesExpanded
    property int historyLimit: 5
    /** Back to how the screen first shows: on every open and every new session. */
    function resetView() {
        rangesExpanded = false;
        historyLimit = 5;
    }
    readonly property string sessionKey: String(status.session || "")
    onSessionKeyChanged: resetView()
    onVisibleChanged: {
        if (!visible)
            resetView();
    }
    onLiveSeqChanged: {
        // Read from status itself: `receiving` and liveShown may not have
        // caught up with an update that brought the hit and its first
        // measurement together.
        if (receivingIn(status) && status.livePowerValid)
            liveCue.mark();
    }
    readonly property string phaseLine: [qsTr("Stopped"), (status.rangeName || "").length > 0 ? qsTr("Searching %1…").arg(status.rangeName) : qsTr("Searching…"), qsTr("Tuning…"), qsTr("Trying digital identification…"), qsTr("Listening"), qsTr("Held"), qsTr("Paused"), qsTr("Search error")][phase] || ""
    // The panel's one identification line is short enough for a phone; the
    // full phrase is its accessible description.
    readonly property bool unresolvedShown: receiving && phase !== 2 && !hit.confirmed && !(phase === 3 || status.identifying) && !!hit.unresolved
    readonly property string identificationLine: !receiving ? phaseLine : phase === 2 ? qsTr("Tuning to detection…") : hit.confirmed ? hit.protocol + qsTr(" · confirmed") : phase === 3 || status.identifying ? qsTr("Trying digital identification…") : hit.unresolved ? qsTr("NFM · digital unresolved") : hit.am ? qsTr("AM monitoring") : qsTr("NFM monitoring")
    readonly property string identificationFull: unresolvedShown ? qsTr("NFM monitoring · digital identification unresolved") : identificationLine
    readonly property var knownNames: hit.knownNames || ((hit.knownLabel || "").length > 0 ? [hit.knownLabel] : [])
    // The Now panel's actions. "Hold" and "Release Hold" share one cell, so a
    // Hold toggle never reflows the grid; `labels` lists every label a cell can
    // show, and the widest of them sizes every cell.
    readonly property var actionCells: [
        { name: "searchHold", label: phase === 5 ? qsTr("Release Hold") : qsTr("Hold"), labels: [qsTr("Hold"), qsTr("Release Hold")], action: 3 },
        { name: "searchSkip", label: qsTr("Skip"), action: 4 },
        { name: "searchAvoidSession", label: qsTr("Avoid this session"), action: 5 },
        { name: "searchAvoidPermanent", label: qsTr("Avoid permanently"), action: 6 },
        { name: "searchSave", label: qsTr("Save as system"), save: true }
    ]
    FontMetrics { id: statusMetrics; font.family: Theme.sans; font.pixelSize: Theme.fontSize(13) }
    FontMetrics { id: frequencyMetrics; font.family: Theme.mono; font.pixelSize: Theme.fontSize(28) }
    FontMetrics { id: strengthMetrics; font.family: Theme.mono; font.pixelSize: Theme.fontSize(14) }
    FontMetrics { id: identificationMetrics; font.family: Theme.sans; font.pixelSize: Theme.fontSize(16) }
    FontMetrics { id: detailMetrics; font.family: Theme.sans; font.pixelSize: Theme.fontSize(14) }
    FontMetrics { id: actionMetrics; font.family: Theme.sans; font.pixelSize: Theme.fontSize(15); font.weight: Font.DemiBold }
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
        // While a session runs: where the search is. Two lines are always kept
        // for it, so a longer range name or channel count never moves the page.
        Item {
            width: parent.width
            height: screen.sessionActive ? 2 * Math.ceil(statusMetrics.lineSpacing) : statusText.implicitHeight
            Text {
                id: statusText
                objectName: "searchStatusLine"
                width: parent.width
                text: screen.sessionActive ? screen.statusLine : qsTr("Find strong signals across your frequency ranges")
                color: Theme.textSecondary
                font.family: Theme.sans
                font.pixelSize: screen.sessionActive ? Theme.fontSize(13) : Theme.fontSize(15)
                wrapMode: Text.Wrap
                maximumLineCount: screen.sessionActive ? 2 : 1000
                elide: Text.ElideRight
                Accessible.role: Accessible.StaticText
                Accessible.name: text
            }
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
                PlexTextField { id: portField; objectName: "searchPort"; width: parent.width; visible: screen.sourceType === "rtltcp"; label: qsTr("Port"); text: String(screen.port); inputMethodHints: Qt.ImhDigitsOnly }
                Text {
                    width: parent.width
                    text: qsTr("RF strength is not a distance measurement. Search uses one radio; each detection is identified, then monitored before searching continues.")
                    color: Theme.textSecondary
                    wrapMode: Text.Wrap
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontSize(14)
                }
            }
            // ---- Now: what is on the air, and what to do with it ----
            // Present for the whole session at one height, a function of width
            // and font scale only: every line keeps its own fixed height (empty
            // or not), and the actions keep their space while hidden, so the
            // page below never moves as detections come and go.
            UiPanel {
                id: nowPanel
                objectName: "searchNowPanel"
                width: parent.width
                height: nowColumn.height + 2 * Theme.cardPadding
                visible: screen.sessionActive
                Column {
                    id: nowColumn
                    x: Theme.cardPadding
                    y: Theme.cardPadding
                    width: parent.width - 2 * Theme.cardPadding
                    spacing: 6
                    Text {
                        objectName: "searchFrequency"
                        width: parent.width
                        height: Math.ceil(frequencyMetrics.lineSpacing)
                        // Between detections the slot says what it is for, in
                        // the sans face so it never reads as a frequency.
                        text: screen.receiving ? (screen.hit.freqMHz || "") + " MHz" : qsTr("Waiting for a signal")
                        color: screen.receiving ? Theme.textPrimary : Theme.textSecondary
                        font.family: screen.receiving ? Theme.mono : Theme.sans
                        font.pixelSize: Theme.fontSize(28)
                        // Every digit of a frequency matters: it shrinks to fit
                        // rather than eliding.
                        fontSizeMode: Text.HorizontalFit
                        minimumPixelSize: Theme.fontSize(14)
                        verticalAlignment: Text.AlignVCenter
                        Accessible.role: Accessible.StaticText
                        Accessible.name: text
                    }
                    Item {
                        width: parent.width
                        height: Math.ceil(strengthMetrics.lineSpacing)
                        // Marks each fresh live measurement: the level of a steady
                        // carrier can read the same for a long time.
                        Rectangle {
                            id: liveCue
                            objectName: "searchLiveCue"
                            property int marks: 0
                            function mark() {
                                marks++;
                                pulse.restart();
                            }
                            width: 7
                            height: 7
                            radius: 3.5
                            anchors.verticalCenter: parent.verticalCenter
                            visible: screen.liveShown
                            color: Theme.cyan
                            opacity: 0.4
                            Accessible.ignored: true
                            SequentialAnimation {
                                id: pulse
                                NumberAnimation { target: liveCue; property: "opacity"; to: 1.0; duration: 50 }
                                NumberAnimation { target: liveCue; property: "opacity"; to: 0.4; duration: 170; easing.type: Easing.OutCubic }
                            }
                        }
                        Text {
                            objectName: "searchStrength"
                            // The cue's slot is kept for the whole visit, so the
                            // text does not move as a live level comes and goes.
                            x: screen.receiving ? liveCue.width + 8 : 0
                            width: parent.width - x
                            height: parent.height
                            text: !screen.receiving ? "" : (screen.status.livePowerValid ? qsTr("Now %1 dBFS · detected %2 dBFS").arg(Number(screen.status.livePower).toFixed(1)).arg(Number(screen.hit.power || 0).toFixed(1)) : Number(screen.hit.power || 0).toFixed(1) + qsTr(" dBFS")) + " · " + (screen.hit.range || "")
                            color: Theme.textSecondary
                            font.family: Theme.mono
                            font.pixelSize: Theme.fontSize(14)
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                            Accessible.name: text
                            Accessible.description: text
                            Accessible.ignored: text.length === 0
                        }
                    }
                    Text {
                        objectName: "searchIdentification"
                        width: parent.width
                        height: Math.ceil(identificationMetrics.lineSpacing)
                        text: screen.identificationLine
                        color: Theme.textPrimary
                        font.family: Theme.sans
                        font.pixelSize: Theme.fontSize(16)
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                        Accessible.name: text
                        Accessible.description: screen.identificationFull
                        Accessible.ignored: text.length === 0
                    }
                    Item {
                        width: parent.width
                        height: 2 * Math.ceil(detailMetrics.lineSpacing)
                        Text {
                            objectName: "searchDetail"
                            width: parent.width
                            text: !screen.receiving ? qsTr("Strong signals open here to be identified and monitored.") : [screen.hit.access || "", screen.hit.tone || "", screen.hit.targetText || (screen.hit.target ? qsTr("Target %1").arg(screen.hit.target) : ""), screen.hit.sourceText || (screen.hit.source ? qsTr("Source %1").arg(screen.hit.source) : "")].filter(function (s) { return s.length > 0; }).join(" · ")
                            color: Theme.textSecondary
                            font.family: Theme.sans
                            font.pixelSize: Theme.fontSize(14)
                            wrapMode: Text.Wrap
                            maximumLineCount: 2
                            elide: Text.ElideRight
                            Accessible.name: text
                            Accessible.description: text
                            Accessible.ignored: text.length === 0
                        }
                    }
                    // Two names, then how many more: the names elide, the count
                    // never does. The full list is in the accessible text here
                    // and in the detection's history row.
                    Item {
                        width: parent.width
                        height: Math.ceil(detailMetrics.lineSpacing)
                        readonly property int more: screen.receiving ? Math.max(0, screen.knownNames.length - 2) : 0
                        Text {
                            id: savedNames
                            objectName: "searchSaved"
                            width: Math.min(implicitWidth, parent.width - (savedMore.visible ? savedMore.implicitWidth + 4 : 0))
                            height: parent.height
                            text: !screen.receiving || screen.knownNames.length === 0 ? "" : qsTr("Saved: %1").arg(screen.knownNames.slice(0, 2).join(" / "))
                            color: Theme.textSecondary
                            font.family: Theme.sans
                            font.pixelSize: Theme.fontSize(14)
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                            readonly property string fullText: text.length > 0 ? qsTr("Saved frequency: %1").arg(screen.knownNames.join(", ")) : ""
                            Accessible.name: fullText
                            Accessible.description: fullText
                            Accessible.ignored: text.length === 0
                        }
                        Text {
                            id: savedMore
                            objectName: "searchSavedMore"
                            x: savedNames.width + 4
                            height: parent.height
                            visible: parent.more > 0
                            text: qsTr("+%1").arg(parent.more)
                            color: Theme.textSecondary
                            font.family: Theme.sans
                            font.pixelSize: Theme.fontSize(14)
                            verticalAlignment: Text.AlignVCenter
                            // Counted in the names' accessible text.
                            Accessible.ignored: true
                        }
                    }
                    // A deterministic grid: three equal columns when every cell
                    // fits the widest label any cell can show at this font
                    // scale, else two; one label line, or two when that widest
                    // label needs them. It works for any number of cells.
                    Item {
                        id: actions
                        objectName: "searchActions"
                        readonly property int spacing: 10
                        readonly property real widest: {
                            // advanceWidth() is a call, which a binding does not
                            // track: reading the font re-measures on a scale change.
                            var font = actionMetrics.font;
                            var widest = 0;
                            for (var cell of screen.actionCells) {
                                for (var label of (cell.labels || [cell.label]))
                                    widest = Math.max(widest, actionMetrics.advanceWidth(label));
                            }
                            return Math.ceil(widest);
                        }
                        readonly property int columns: (width - 2 * spacing) / 3 >= widest + 2 * Theme.cardPadding ? 3 : 2
                        readonly property real cellWidth: (width - (columns - 1) * spacing) / columns
                        // OutlineButton keeps 12 px either side of its label.
                        readonly property int labelLines: widest <= cellWidth - 24 ? 1 : 2
                        readonly property real cellHeight: Math.max(Theme.minimumTouchSize, Math.ceil(labelLines * actionMetrics.lineSpacing) + 24)
                        readonly property int rows: Math.ceil(screen.actionCells.length / columns)
                        width: parent.width
                        height: rows * cellHeight + Math.max(0, rows - 1) * spacing
                        Repeater {
                            model: screen.actionCells.length
                            OutlineButton {
                                required property int index
                                readonly property var cell: screen.actionCells[index]
                                objectName: cell.name
                                x: (index % actions.columns) * (actions.cellWidth + actions.spacing)
                                y: Math.floor(index / actions.columns) * (actions.cellHeight + actions.spacing)
                                width: actions.cellWidth
                                height: actions.cellHeight
                                maximumLineCount: actions.labelLines
                                // Hidden, not removed, while nothing is received.
                                visible: screen.receiving
                                text: cell.label
                                onClicked: {
                                    if (cell.save)
                                        screen.saveDetection(Object.assign({}, screen.hit));
                                    else
                                        screen.act(cell.action);
                                }
                            }
                        }
                    }
                }
            }
            Loader {
                width: parent.width
                active: screen.sessionActive
                visible: active
                sourceComponent: recentDetections
            }
            Text { visible: !screen.sessionActive; text: qsTr("Search ranges"); color: Theme.textPrimary; font.family: Theme.sans; font.pixelSize: Theme.fontSize(17); font.weight: Font.DemiBold }
            UiPanel {
                width: parent.width
                height: rangesToggle.height
                visible: screen.sessionActive
                DisclosureRow {
                    id: rangesToggle
                    objectName: "searchRangesToggle"
                    title: qsTr("Search ranges · %1 enabled").arg(screen.enabledRanges)
                    // Paused, the ranges are open for editing and the row is
                    // their heading; it does not take focus as they open.
                    expandable: !screen.editable
                    expanded: screen.rangesShown
                    showCaret: !screen.editable
                    tapEnabled: !screen.editable
                    // Not a control while paused: the heading of the open ranges.
                    Accessible.role: screen.editable ? Accessible.Heading : Accessible.Button
                    accessibleName: screen.editable ? qsTr("Search ranges, %1 enabled").arg(screen.enabledRanges) : screen.rangesExpanded ? qsTr("Hide search ranges") : qsTr("Show search ranges, %1 enabled").arg(screen.enabledRanges)
                    onTapped: screen.rangesExpanded = !screen.rangesExpanded
                }
            }
            Column {
                objectName: "searchRanges"
                width: parent.width
                visible: !screen.sessionActive || screen.rangesShown
                spacing: Theme.gap
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
            }
            Loader {
                width: parent.width
                active: !screen.sessionActive
                visible: active
                sourceComponent: recentDetections
            }
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
    // Recent detections: after Now while a search runs, after the ranges while
    // idle. Five, then Show more adds the next 25.
    Component {
        id: recentDetections
        Column {
            spacing: Theme.gap
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
                model: searchModel ? searchModel.history.slice(0, screen.historyLimit) : []
                Column {
                    required property var modelData
                    objectName: "searchHistoryRow"
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
            OutlineButton { objectName: "searchShowMore"; text: qsTr("Show more"); visible: !!searchModel && searchModel.history.length > screen.historyLimit; onClicked: screen.historyLimit += 25 }
        }
    }
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
