// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import "Util.js" as Util

Item {
    id: screen
    objectName: "signalSearchScreen"
    property var searchModel: null
    property bool sessionActive: false
    // Main sets this while the search stops for a handoff (Explore here, a
    // saved system): the screen says so and its controls are disabled until it
    // closes; Back cancels the handoff and gives them back.
    property bool leaving: false
    // With the controls disabled the screen itself holds focus, so Back still
    // reaches Main.
    onLeavingChanged: {
        if (leaving)
            forceActiveFocus();
    }
    // While a session runs, Back from anywhere on the screen is Main's session
    // policy (as requestClose() delegates it); nothing above the screen would
    // take a key a control left. Idle, the key goes on as before.
    Keys.onEscapePressed: function (event) {
        event.accepted = sessionActive || leaving;
        if (event.accepted)
            backRequested();
    }
    Keys.onBackPressed: function (event) {
        event.accepted = sessionActive || leaving;
        if (event.accepted)
            backRequested();
    }
    // Main binds this to its failure text (connection, USB access and
    // configuration failures, and a receiver that ended the search, worded
    // for this screen). Never assigned here: an assignment would replace
    // that binding, and no later failure would reach this screen, which covers
    // Home's banner.
    property string failure: ""
    // This screen's own refusal of a Start, Resume (searchModel.arm()) or
    // handoff, shown first until the next attempt or until Back closes it; the
    // host's failure, else the search's later message or the model's error,
    // follows it.
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
    // Issue #782: what makes the search's moving lines worth hearing at once
    // (SpeechPacer): a new hit or a change of state. Between these the
    // channel count, the live level and the call details are re-read at most
    // every few seconds.
    readonly property string speechStep: [sessionActive, phase, receiving, status.identifying === true, status.pass || 0,
        status.rangeName || "", hit.id || "", hit.freqMHz || "", hit.confirmed === true, hit.protocol || ""].join("|")
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
    // Leave the search for an Explore session on a detection, or for a saved
    // system it matched; Main ends the search first.
    signal exploreDetection(var hit)
    signal startSavedSystem(string uid)
    // The saved systems a detection's matches name (Main's savedSystems).
    property var systems: typeof savedSystems !== "undefined" ? savedSystems : null
    readonly property bool menuOpen: detectionMenu.visible
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
    /** Start a search on the screen's receiver: arm the ranges, then ask Main
     * to start the session. Main's retry of a failed search runs this too. */
    function startSearch() {
        if (!searchModel)
            return;
        var error = searchModel.arm();
        if (error.length) {
            localFailure = error;
            startRefused();
            return;
        }
        var ranges = searchModel.ranges.filter(function (r) { return r.enabled; });
        localFailure = "";
        start(sourceType, host, Number(portField.text), ranges[0].startMHz);
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
    // lines, so the pass and channel count always show. The line count is
    // reserved from the width and the font scale alone: two, or three where
    // the widest pass, bounds and channel count with a shortened name would
    // not fit two (a narrow phone at the largest text).
    readonly property int statusLines: {
        var font = statusMetrics.font;
        var width = statusText.width - 1;
        if (width <= 0)
            return 2;
        var widest = [qsTr("Pass %1").arg("8888"), "…", qsTr("%1–%2 MHz").arg("8888.888888").arg("8888.888888"),
                      qsTr("channel %1 of %2").arg("888888").arg("888888")].join(" · ");
        return wrappedLines(widest, width) <= 2 ? 2 : 3;
    }
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
        var lines = statusLines;
        if (width <= 0 || wrappedLines(compose(name), width) <= lines)
            return compose(name);
        var step = statusMetrics.advanceWidth("M");
        for (var budget = statusMetrics.advanceWidth(name) - step; budget > 0; budget -= step) {
            var shorter = statusMetrics.elidedText(name, Qt.ElideRight, budget);
            if (wrappedLines(compose(shorter), width) <= lines)
                return compose(shorter);
        }
        if (wrappedLines(compose("…"), width) <= lines)
            return compose("…");
        // Bounds wider than any a receiver tunes: the progress stays whole.
        return [pass, channel].join(" · ");
    }
    /** Lines @p text takes at @p width in the status line's font, or in
     * @p metrics, wrapped at spaces as the line wraps it (a word wider than
     * a line breaks anywhere). */
    function wrappedLines(text, width, metrics) {
        var m = metrics || statusMetrics;
        var space = m.advanceWidth(" ");
        var lines = 1, x = 0;
        for (var word of text.split(" ")) {
            var w = m.advanceWidth(word);
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
    readonly property string identificationLine: leaving ? qsTr("Stopping the search…") : !receiving ? phaseLine : phase === 2 ? qsTr("Tuning to detection…") : hit.confirmed ? hit.protocol + qsTr(" · confirmed") : phase === 3 || status.identifying ? qsTr("Trying digital identification…") : hit.unresolved ? qsTr("NFM · digital unresolved") : hit.am ? qsTr("AM monitoring") : qsTr("NFM monitoring")
    readonly property string identificationFull: !leaving && unresolvedShown ? qsTr("NFM monitoring · digital identification unresolved") : identificationLine
    readonly property var knownNames: hit.knownNames || ((hit.knownLabel || "").length > 0 ? [hit.knownLabel] : [])
    /** A detection's checked details in reading order: its site, access code
     * and tone, the call (slot, target, source), then the call's encryption. */
    function detailText(h) {
        return [h.site || "", h.access || "", h.tone || "", h.slot > 0 ? qsTr("Slot %1").arg(h.slot) : "", h.targetText || (h.target ? qsTr("Target %1").arg(h.target) : ""), h.sourceText || (h.source ? qsTr("Source %1").arg(h.source) : ""), h.crypt || ""].filter(function (s) { return s.length > 0; }).join(" · ");
    }
    /** @p text to show wrapped: every space inside a value is made
     * non-breaking, so lines break only after a " · " and a value ("SYS
     * 1183", "KID 0001") never splits, nor does a line start with a
     * separator. A value wider than @p width in @p metrics keeps its spaces
     * and wraps at its words; with no @p metrics every value is kept whole.
     * Line breaks stay. Screen readers get the plain text. */
    function keepValues(text, width, metrics) {
        return text.split("\n").map(function (line) {
            return line.split(" · ").map(function (value) {
                return metrics && metrics.advanceWidth(value) > width ? value : value.replace(/ /g, "\u00a0");
            }).join("\u00a0· ");
        }).join("\n");
    }
    /** @p values joined for @p lines lines of @p width (keepValues() text
     * wraps only at its breakable spaces, which wrappedLines() counts): all
     * of them, or as many leading ones as fit followed by "…", so a value
     * is never cut.
     * Only a first value that alone needs more lines is left to the Text's
     * own elision. */
    function fitValues(values, width, lines, metrics) {
        var all = keepValues(values.join(" · "), width, metrics);
        if (width <= 0 || wrappedLines(all, width, metrics) <= lines)
            return all;
        for (var k = values.length - 1; k > 0; --k) {
            var shown = keepValues(values.slice(0, k).concat(["…"]).join(" · "), width, metrics);
            if (wrappedLines(shown, width, metrics) <= lines)
                return shown;
        }
        return all;
    }
    // The Now panel's actions. "Hold" and "Release Hold" share one cell, so a
    // Hold toggle never reflows the grid; `labels` lists every label a cell can
    // show, and the widest of them sizes every cell.
    readonly property var actionCells: [
        { name: "searchHold", label: phase === 5 ? qsTr("Release Hold") : qsTr("Hold"), labels: [qsTr("Hold"), qsTr("Release Hold")], action: 3 },
        { name: "searchSkip", label: qsTr("Skip"), action: 4 },
        { name: "searchAvoidSession", label: qsTr("Avoid this session"), action: 5 },
        { name: "searchAvoidPermanent", label: qsTr("Avoid permanently"), action: 6 },
        { name: "searchSave", label: qsTr("Save as system"), save: true },
        { name: "searchMore", label: qsTr("More"), more: true, accessibleName: qsTr("More actions for this detection") }
    ]
    /** The saved systems @p h matched, once each, in match order: the ones still saved. */
    function savedTargets(h) {
        var seen = {}, found = [];
        for (var match of (h.matches || [])) {
            var uid = String(match.systemUid || "");
            if (!uid.length || seen[uid] || !systems)
                continue;
            seen[uid] = true;
            var system = systems.getByUid(uid);
            if (system && system.uid)
                found.push({ uid: uid, name: system.name || qsTr("Saved system"), avoided: !!system.avoidSite });
        }
        return found;
    }
    /** What can be done with detection @p h: leaving for it from the Now
     * panel's More, or everything from its history row (@p all). */
    function detectionActions(h, all) {
        var items = [{ text: qsTr("Explore here"), key: "explore", objectName: "searchActionExplore" }];
        var targets = savedTargets(h);
        for (var i = 0; i < targets.length; ++i) {
            var target = targets[i];
            items.push({ text: qsTr("Start “%1”").arg(target.name), key: "start", uid: target.uid, objectName: "searchActionStart" + i, enabled: !target.avoided,
                description: target.avoided ? qsTr("Avoided; remove the avoid in its site list to start it") : "" });
        }
        if (!all)
            return items;
        items.push({ text: qsTr("Save as system"), key: "save", objectName: "searchActionSave" });
        if (ownedSession && phase !== 0)
            items.push({ text: qsTr("Avoid this session"), key: "avoidSession", objectName: "searchActionAvoidSession" });
        if (permanentlyAvoided(h))
            items.push({ text: qsTr("Remove permanent avoid"), key: "removePermanent", objectName: "searchActionRemovePermanent" });
        else
            items.push({ text: qsTr("Avoid permanently"), key: "avoidPermanent", objectName: "searchActionAvoidPermanent" });
        return items;
    }
    function hitHz(h) {
        return Number(h.frequency) || Math.round(Number(h.freqMHz || 0) * 1e6);
    }
    function permanentlyAvoided(h) {
        var hz = hitHz(h);
        return !!searchModel && (searchModel.avoids || []).some(function (f) { return Number(f) === hz; });
    }
    function openDetectionMenu(h, all) {
        detectionMenu.target = Object.assign({}, h);
        detectionMenu.title = qsTr("%1 MHz").arg(Util.mhzText(hitHz(h)));
        detectionMenu.actions = detectionActions(h, all);
        detectionMenu.open();
    }
    FontMetrics { id: statusMetrics; font.family: Theme.sans; font.pixelSize: Theme.fontSize(13) }
    FontMetrics { id: frequencyMetrics; font.family: Theme.mono; font.pixelSize: Theme.fontSize(28) }
    FontMetrics { id: strengthMetrics; font.family: Theme.mono; font.pixelSize: Theme.fontSize(14) }
    FontMetrics { id: identificationMetrics; font.family: Theme.sans; font.pixelSize: Theme.fontSize(16) }
    FontMetrics { id: detailMetrics; font.family: Theme.sans; font.pixelSize: Theme.fontSize(14) }
    FontMetrics { id: historyMetrics; font.family: Theme.sans; font.pixelSize: Theme.fontSize(13) }
    FontMetrics { id: actionMetrics; font.family: Theme.sans; font.pixelSize: Theme.fontSize(15); font.weight: Font.DemiBold }
    Rectangle { anchors.fill: parent; color: Theme.bg }
    Column {
        id: header
        // The menu's rows take passive taps: nothing under it may take one too.
        enabled: !screen.menuOpen && !screen.leaving
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: Theme.screenPadding
        spacing: 8
        Row {
            width: parent.width
            spacing: 12
            IconButton { objectName: "searchBack"; icon: "back"; accessibleName: qsTr("Back"); onClicked: screen.requestClose() }
            ReadableText {
                heading: true
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Signal search")
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(22)
                font.weight: Font.Bold
                color: Theme.textPrimary
            }
        }
        // While a session runs: where the search is. Its lines (statusLines) are
        // always kept for it, so a longer range name or channel count never
        // moves the page. Idle, the slot is the text's own height: its height,
        // not implicitHeight, whose getter lays out pending text and changes
        // itself under the binding reading it (a binding loop).
        Item {
            width: parent.width
            height: screen.sessionActive ? screen.statusLines * Math.ceil(statusMetrics.lineSpacing) : statusText.height
            Text {
                id: statusText
                objectName: "searchStatusLine"
                width: parent.width
                text: screen.sessionActive ? screen.statusLine : qsTr("Find strong signals across your frequency ranges")
                color: Theme.textSecondary
                font.family: Theme.sans
                font.pixelSize: screen.sessionActive ? Theme.fontSize(13) : Theme.fontSize(15)
                wrapMode: Text.Wrap
                maximumLineCount: screen.sessionActive ? screen.statusLines : 1000
                elide: Text.ElideRight
                Accessible.role: Accessible.StaticText
                Accessible.name: statusSpeech.spoken
                SpeechPacer {
                    id: statusSpeech
                    shown: statusText.visible
                    step: screen.speechStep
                    read: function () {
                        return statusText.text;
                    }
                }
            }
        }
    }
    PlexFlickable {
        id: scroll
        objectName: "searchScroll"
        anchors.top: header.bottom
        anchors.topMargin: Theme.gap
        anchors.bottom: footer.top
        anchors.bottomMargin: Theme.gap
        anchors.left: parent.left
        anchors.right: parent.right
        contentHeight: content.height + Theme.gap
        clip: true
        enabled: !screen.menuOpen && !screen.leaving
        Column {
            id: content
            x: (parent.width - width) / 2
            width: Math.min(Theme.formWidth, parent.width - 2 * Theme.screenPadding)
            spacing: Theme.gap
            // The message sits above the setup while idle; while a session runs
            // it moves under the Now panel, so a refusal that appears or clears
            // as a detection arrives never moves the panel.
            Item {
                id: idleMessageSlot
                objectName: "searchIdleMessageSlot"
                width: parent.width
                height: searchMessage.height
                visible: !screen.sessionActive && searchMessage.text.length > 0
            }
            Column {
                width: parent.width
                visible: !screen.sessionActive
                spacing: 12
                ReadableText { heading: true; text: qsTr("Radio"); color: Theme.textPrimary; font.family: Theme.sans; font.pixelSize: Theme.fontSize(17); font.weight: Font.DemiBold }
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
                ReadableText {
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
                        id: strengthSlot
                        width: parent.width
                        // One line, or two where the widest reading would not fit
                        // one at this width and font scale: a level is never cut
                        // off, and the height still depends on nothing else.
                        readonly property real textWidth: width - liveCue.width - 8
                        readonly property int lines: {
                            // advanceWidth() is a call, which a binding does not
                            // track: reading the font re-measures on a scale change.
                            var font = strengthMetrics.font;
                            return strengthMetrics.advanceWidth(qsTr("Now %1 · detected %2 dBFS").arg("-100.0").arg("-100.0")) <= textWidth ? 1 : 2;
                        }
                        height: lines * Math.ceil(strengthMetrics.lineSpacing)
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
                            // Beside the first line, where "Now" is.
                            y: (Math.ceil(strengthMetrics.lineSpacing) - height) / 2
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
                            width: strengthSlot.textWidth
                            // Both levels, nothing else: the range is in the status
                            // line and in the accessible description.
                            text: !screen.receiving ? "" : screen.status.livePowerValid ? (strengthSlot.lines > 1 ? qsTr("Now %1 dBFS\ndetected %2 dBFS") : qsTr("Now %1 · detected %2 dBFS")).arg(Util.fixed(screen.status.livePower, 1)).arg(Util.fixed(screen.hit.power || 0, 1)) : Util.fixed(screen.hit.power || 0, 1) + qsTr(" dBFS")
                            color: Theme.textSecondary
                            font.family: Theme.mono
                            font.pixelSize: Theme.fontSize(14)
                            wrapMode: Text.Wrap
                            maximumLineCount: strengthSlot.lines
                            elide: Text.ElideRight
                            id: strengthText
                            readonly property string spoken: strengthSpeech.spoken
                            Accessible.name: spoken
                            Accessible.description: spoken.length > 0 && (screen.hit.range || "").length > 0 ? spoken + " · " + screen.hit.range : spoken
                            Accessible.ignored: text.length === 0
                            // The live level moves every tick (issue #782).
                            SpeechPacer {
                                id: strengthSpeech
                                shown: strengthText.text.length > 0
                                step: screen.speechStep
                                read: function () {
                                    return strengthText.text.replace("\n", " · ");
                                }
                            }
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
                            // The hint belongs to a search that is running or paused;
                            // a stopped or failed one shows only its phase and message.
                            readonly property string fullText: !screen.receiving ? ([1, 2, 6].indexOf(screen.phase) >= 0 ? qsTr("Strong signals open here to be identified and monitored.") : "") : screen.detailText(screen.hit)
                            // The values that fit both lines, ending "· …" when
                            // some do not; elision is only the last resort.
                            text: {
                                // advanceWidth() is a call, which a binding does not
                                // track: reading the font re-measures on a scale change.
                                var font = detailMetrics.font;
                                return screen.receiving && fullText.length > 0 ? screen.fitValues(fullText.split(" · "), width, 2, detailMetrics) : fullText;
                            }
                            color: Theme.textSecondary
                            font.family: Theme.sans
                            font.pixelSize: Theme.fontSize(14)
                            wrapMode: Text.Wrap
                            maximumLineCount: 2
                            elide: Text.ElideRight
                            id: searchDetailText
                            Accessible.name: detailSpeech.spoken
                            Accessible.description: detailSpeech.spoken
                            Accessible.ignored: text.length === 0
                            // A monitored call's talkgroup and source change with each call (issue #782).
                            SpeechPacer {
                                id: detailSpeech
                                shown: searchDetailText.text.length > 0
                                step: screen.speechStep
                                read: function () {
                                    return searchDetailText.fullText;
                                }
                            }
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
                    // scale, else two when every word of every label fits a
                    // cell (no word breaks mid-way), else one; as many label
                    // lines as the longest label any cell can show wraps to at
                    // the cell width. It works for any number of cells.
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
                        // The widest single word any cell can show.
                        readonly property real widestWord: {
                            var font = actionMetrics.font;
                            var widest = 0;
                            for (var cell of screen.actionCells) {
                                for (var label of (cell.labels || [cell.label])) {
                                    for (var word of label.split(" "))
                                        widest = Math.max(widest, actionMetrics.advanceWidth(word));
                                }
                            }
                            return Math.ceil(widest);
                        }
                        readonly property int columns: (width - 2 * spacing) / 3 >= widest + 2 * Theme.cardPadding ? 3
                                                     : (width - spacing) / 2 - 24 >= widestWord ? 2 : 1
                        readonly property real cellWidth: (width - (columns - 1) * spacing) / columns
                        // OutlineButton keeps 12 px either side of its label, which
                        // wraps at spaces as wrappedLines() counts.
                        readonly property int labelLines: {
                            var font = actionMetrics.font;
                            var lines = 1;
                            for (var cell of screen.actionCells) {
                                for (var label of (cell.labels || [cell.label]))
                                    lines = Math.max(lines, screen.wrappedLines(label, cellWidth - 24, actionMetrics));
                            }
                            return lines;
                        }
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
                                accessibleName: cell.accessibleName || cell.label
                                onClicked: {
                                    if (cell.more)
                                        screen.openDetectionMenu(screen.hit, false);
                                    else if (cell.save)
                                        screen.saveDetection(Object.assign({}, screen.hit));
                                    else
                                        screen.act(cell.action);
                                }
                            }
                        }
                    }
                }
            }
            Item {
                id: sessionMessageSlot
                objectName: "searchSessionMessageSlot"
                width: parent.width
                height: searchMessage.height
                visible: screen.sessionActive && searchMessage.text.length > 0
                Text {
                    id: searchMessage
                    objectName: "searchMessage"
                    parent: screen.sessionActive ? sessionMessageSlot : idleMessageSlot
                    width: parent.width
                    // This screen's own refusal comes first and never hides what
                    // the search says now: the host's failure, else the search's
                    // message or the model's error.
                    text: [screen.localFailure, screen.failure || screen.status.message || (searchModel ? searchModel.error : "")].filter(function (s) { return s.length > 0; }).join("\n")
                    color: Theme.textPrimary
                    wrapMode: Text.Wrap
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontSize(14)
                    Accessible.role: Accessible.AlertMessage
                    // A role alone reads nothing (issue #782): the message is its name.
                    Accessible.name: text
                }
            }
            Loader {
                width: parent.width
                active: screen.sessionActive
                visible: active
                sourceComponent: recentDetections
            }
            ReadableText { heading: true; visible: !screen.sessionActive; text: qsTr("Search ranges"); color: Theme.textPrimary; font.family: Theme.sans; font.pixelSize: Theme.fontSize(17); font.weight: Font.DemiBold }
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
                ReadableText {
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
            ReadableText { heading: true; visible: screen.sessionActive && searchModel && (searchModel.sessionAvoids || []).length > 0; text: qsTr("Avoided this session"); color: Theme.textPrimary; font.family: Theme.sans; font.pixelSize: Theme.fontSize(17); font.weight: Font.DemiBold }
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
            ReadableText { heading: true; visible: searchModel && searchModel.avoids.length > 0; text: qsTr("Permanent avoids"); color: Theme.textPrimary; font.family: Theme.sans; font.pixelSize: Theme.fontSize(17); font.weight: Font.DemiBold }
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
            ReadableText { heading: true; text: qsTr("Recent detections"); color: Theme.textPrimary; font.family: Theme.sans; font.pixelSize: Theme.fontSize(17); font.weight: Font.DemiBold }
            ReadableText {
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
                    Item {
                        width: parent.width
                        height: Math.max(rowText.height, rowActions.height)
                        Column {
                            id: rowText
                            width: parent.width - rowActions.width - 8
                            spacing: 5
                            ReadableText { objectName: "searchHistoryReading"; width: parent.width; text: screen.keepValues(modelData.freqMHz + " MHz · " + Util.fixed(modelData.power, 1) + " dBFS"); color: Theme.textPrimary; font.family: Theme.mono; font.pixelSize: Theme.fontSize(15); wrapMode: Text.Wrap }
                            Text {
                                width: parent.width
                                objectName: "searchHistoryDetail"
                                // The site and encryption the detection was
                                // checked with, under its verdict.
                                readonly property string checked: [modelData.site || "", modelData.crypt || ""].filter(function (s) { return s.length > 0; }).join(" · ")
                                readonly property string fullText: (modelData.confirmed ? modelData.protocol + qsTr(" · confirmed") : qsTr("RF only")) + (modelData.ended ? qsTr(" · ended") : "") + (checked ? "\n" + checked : "") + (modelData.knownLabel ? "\n" + modelData.knownLabel : "")
                                text: {
                                    var font = historyMetrics.font;
                                    return screen.keepValues(fullText, width, historyMetrics);
                                }
                                Accessible.name: fullText.replace(/\n/g, " · ")
                                color: Theme.textSecondary
                                font.family: Theme.sans
                                font.pixelSize: Theme.fontSize(13)
                                wrapMode: Text.Wrap
                            }
                        }
                        IconButton {
                            id: rowActions
                            objectName: "searchHistoryActions"
                            anchors.right: parent.right
                            icon: "more"
                            accessibleName: qsTr("Actions for %1 MHz").arg(Util.mhzText(screen.hitHz(modelData)))
                            onClicked: screen.openDetectionMenu(modelData, true)
                        }
                    }
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
        enabled: !screen.menuOpen && !screen.leaving
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
                screen.startSearch();
            }
        }
        OutlineButton { objectName: "searchStop"; visible: screen.sessionActive; width: (parent.width - 12) / 2; text: qsTr("Stop"); onClicked: screen.stop() }
    }
    SignalSearchRangeEditor { id: editor; searchModel: screen.searchModel; sourceType: screen.sourceType }
    // One menu for a detection, from its history row or the Now panel's More.
    // It acts on the snapshot it opened with: the row or the hit can change
    // underneath while it is open.
    ActionMenu {
        id: detectionMenu
        objectName: "searchDetectionMenu"
        property var target: ({})
        onTriggered: function (index) {
            // Qualified: the action grid's id is "actions".
            var item = detectionMenu.actions[index];
            var h = detectionMenu.target;
            if (item.key === "explore")
                screen.exploreDetection(Object.assign({}, h));
            else if (item.key === "start")
                screen.startSavedSystem(item.uid);
            else if (item.key === "save")
                screen.saveDetection(Object.assign({}, h));
            else if (item.key === "avoidSession")
                searchModel.avoidFrequency(String(screen.status.session || ""), screen.hitHz(h));
            else if (item.key === "avoidPermanent")
                searchModel.avoidPermanently(String(screen.status.session || ""), screen.hitHz(h));
            else if (item.key === "removePermanent")
                searchModel.removeAvoid(screen.hitHz(h));
        }
    }
}
