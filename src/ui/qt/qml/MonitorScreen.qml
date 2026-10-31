// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import "Util.js" as Util

// The live session: who you are hearing now, the calls you just heard, and one
// way out. Replaces the home content while a session is active.
Item {
    id: screen

    onVisibleChanged: {
        if (!visible) {
            keySheet.visible = false;
            siteSheet.visible = false;
            networkSheet.visible = false;
            toneFilterSheet.visible = false;
            receiveFiltersSheet.visible = false;
            serviceNamesSheet.visible = false;
            squelchSheet.visible = false;
            passbandSheet.visible = false;
            historyDetail.visible = false;
            otherSlotMenu.visible = false;
            receiverSheet.visible = false;
        }
    }

    // Raised by the header's spectrum button; Main.qml owns the layer.
    signal openSpectrum
    property string scanTargetName: ""
    property bool sitesAvailable: false
    signal openSites
    signal openSessionMenu
    signal openTalkgroups

    // The saved-system map the session was started from (may be null for a
    // network/file quick start).
    property var system: null
    // The standalone saved system the session runs, by UID; empty for a scan
    // list, Explore or a wizard test. The key sheet may write its profile.
    property string sessionSystemUid: ""
    signal systemProfileSaved(string uid)
    // After an Activity restart the reattached session has no saved-system map,
    // but the persisted session label still names what is playing — the header
    // should agree with the history rows it sits above. A map without a name (a
    // saved system removed while it plays reads back as an empty map) falls back
    // the same way rather than binding undefined to the header.
    property string systemName: system && system.name ? system.name : callHistory.sessionLabel.length > 0 ? callHistory.sessionLabel : qsTr("Listening")

    // Which slot the hero shows: the earliest active call keeps the headline, then a
    // recently ended one; exact start ties and ended calls prefer the lower slot.
    // dsd_app_lead_slot() compares exact starts so this panel and the Android notification
    // agree despite different polling rates. An earlier call gaining identity late takes
    // the headline once, using its original start.
    readonly property int heroSlot: metrics ? metrics.leadSlot : 0
    readonly property bool heroActive: heroSlot === 1 ? metrics.slot1CallState === 2 : heroSlot === 2 ? metrics.slot2CallState === 2 : false
    readonly property string heroName: heroSlot === 1 ? metrics.slot1CallName : heroSlot === 2 ? metrics.slot2CallName : ""
    // The scan channel the hero call was heard on (a -Y row name or a trunk-scan
    // target id); empty when the receiver is not scanning.
    readonly property string heroChannel: heroSlot === 1 ? metrics.slot1Channel : heroSlot === 2 ? metrics.slot2Channel : ""
    readonly property string heroTg: heroSlot === 1 ? metrics.slot1TgText : heroSlot === 2 ? metrics.slot2TgText : ""
    readonly property string heroSrc: heroSlot === 1 ? metrics.slot1SrcText : heroSlot === 2 ? metrics.slot2SrcText : ""
    readonly property double heroTgId: heroSlot === 1 ? metrics.slot1TgId : heroSlot === 2 ? metrics.slot2TgId : 0
    // Issue #773: a private call headlines its other party under a PRIVATE tag, and a
    // call no party named headlines its protocol ("TETRA call").
    readonly property int heroKind: heroSlot === 1 ? metrics.slot1CallKind : heroSlot === 2 ? metrics.slot2CallKind : 0
    readonly property string heroTgLabel: heroSlot === 1 ? metrics.slot1TgLabel : heroSlot === 2 ? metrics.slot2TgLabel : ""
    readonly property string heroProtocol: heroSlot === 1 ? metrics.slot1ProtocolLabel : heroSlot === 2 ? metrics.slot2ProtocolLabel : ""
    readonly property bool heroPrivate: heroSlot !== 0 && Util.callIsPrivate(heroKind)
    readonly property bool heroAnonymous: heroSlot !== 0 && !heroPrivate && Util.callIsAnonymous(heroTg, heroTgId, heroSrc, heroProtocol)
    readonly property bool heroDuplicate: heroTgId > 0 && heroName === heroTg
    readonly property string heroHeadline: Util.callHeadline(heroKind, heroName, heroTg, heroTgId, heroTgLabel, heroSrc, heroProtocol)
    readonly property bool heroEmergency: heroSlot === 1 ? metrics.slot1CallEmergency : heroSlot === 2 ? metrics.slot2CallEmergency : false
    readonly property bool otherEmergency: otherSlot === 1 ? metrics.slot1CallEmergency : otherSlot === 2 ? metrics.slot2CallEmergency : false
    readonly property bool heroEnc: heroSlot === 1 ? metrics.slot1CallEnc : heroSlot === 2 ? metrics.slot2CallEnc : false
    readonly property string heroEncText: heroSlot === 1 ? metrics.slot1EncText : heroSlot === 2 ? metrics.slot2EncText : ""
    readonly property int heroSeconds: heroSlot === 1 ? metrics.slot1CallSeconds : heroSlot === 2 ? metrics.slot2CallSeconds : 0

    // The hero shows one slot, but TDMA carries two: when the other slot is also
    // live it gets a slim strip of its own, or that call is invisible and cannot
    // be skipped.
    readonly property int otherSlot: heroSlot === 1 ? 2 : heroSlot === 2 ? 1 : 0
    readonly property bool otherActive: otherSlot === 1 ? metrics.slot1CallState === 2 : otherSlot === 2 ? metrics.slot2CallState === 2 : false
    // The second-slot strip: kept in place while the decoder says the carrier is two-slot TDMA, so the other slot's
    // calls coming and going never move the screen below it, and shown for an active second call whatever the
    // carrier is known to be. With no call it stands for slot 2; with slot 2 leading, for slot 1.
    readonly property bool stripVisible: metrics.twoSlotCarrier || otherActive
    // A scan rotation that mixes two-slot and single-slot carriers (issue #782): once it has visited a two-slot one,
    // the strip's place stays reserved for the rest of the rotation, empty on a single-slot carrier, so the hops never
    // move the scan buttons under it, the list or the tier. Set by the handlers below, never by a binding on itself;
    // the rotation ending and a new session clear it. A new session is seen as an edge of decoderHost.running, the
    // only session boundary the host publishes: a stop, then a start. A restart that kept running true would carry
    // the reservation over, which costs at most a blank strip until that rotation ends.
    property bool rotationReservesStrip: false
    readonly property bool stripRotating: metrics.scanRotationActive
    readonly property bool stripTwoSlot: metrics.twoSlotCarrier
    function noteStripReserve() {
        if (!stripRotating)
            rotationReservesStrip = false;
        else if (stripTwoSlot)
            rotationReservesStrip = true;
    }
    onStripRotatingChanged: noteStripReserve()
    onStripTwoSlotChanged: noteStripReserve()
    Component.onCompleted: noteStripReserve()
    Connections {
        target: decoderHost
        function onRunningChanged() {
            screen.rotationReservesStrip = false;
            screen.noteStripReserve();
        }
    }
    // The strip's place in the layout: what the budget measures and what the scene shows, from this one value.
    readonly property bool stripPlaceHeld: stripVisible || rotationReservesStrip
    readonly property bool stripIdle: !otherActive
    readonly property int stripSlot: heroSlot === 2 ? 1 : 2
    readonly property string otherName: otherSlot === 1 ? metrics.slot1CallName : otherSlot === 2 ? metrics.slot2CallName : ""
    readonly property string otherTg: otherSlot === 1 ? metrics.slot1TgText : otherSlot === 2 ? metrics.slot2TgText : ""
    readonly property double otherTgId: otherSlot === 1 ? metrics.slot1TgId : otherSlot === 2 ? metrics.slot2TgId : 0
    readonly property bool otherEnc: otherSlot === 1 ? metrics.slot1CallEnc : otherSlot === 2 ? metrics.slot2CallEnc : false
    readonly property int otherKind: otherSlot === 1 ? metrics.slot1CallKind : otherSlot === 2 ? metrics.slot2CallKind : 0
    readonly property string otherTgLabel: otherSlot === 1 ? metrics.slot1TgLabel : otherSlot === 2 ? metrics.slot2TgLabel : ""
    readonly property string otherSrc: otherSlot === 1 ? metrics.slot1SrcText : otherSlot === 2 ? metrics.slot2SrcText : ""
    readonly property string otherProtocol: otherSlot === 1 ? metrics.slot1ProtocolLabel : otherSlot === 2 ? metrics.slot2ProtocolLabel : ""
    readonly property bool otherPrivate: otherSlot !== 0 && Util.callIsPrivate(otherKind)
    // The strip's one line: a talkgroup call names its talkgroup beside its name, as
    // before; a private call and a call no party named read as the hero would.
    readonly property string otherIdentity: {
        if (otherPrivate || Util.callIsAnonymous(otherTg, otherTgId, otherSrc, otherProtocol))
            return Util.callHeadline(otherKind, otherName, otherTg, otherTgId, otherTgLabel, otherSrc, otherProtocol);
        if (otherTgId > 0 && otherName === otherTg)
            return Util.talkgroupHeadline(otherName, otherTg, otherTgId);
        if (otherTg.length === 0)
            return otherName;
        return otherName.length > 0 ? otherName + " · TG " + otherTg : "TG " + otherTg;
    }

    component AvoidAction: OutlineButton {
        property int heroSlot: 0
        property bool running: false
        // A private call's target is a radio, which the decoder will not lock out as a talkgroup.
        property bool privateCall: false
        text: metrics.persistTgLockouts ? qsTr("Lock out") : qsTr("Avoid TG")
        accessibleName: privateCall ? qsTr("A private call has no talkgroup to lock out") : metrics.persistTgLockouts ? qsTr("Lock out talkgroup and save") : qsTr("Avoid talkgroup for this session")
        enabled: running && heroSlot !== 0 && !privateCall
        onClicked: commands.lockoutSlot(heroSlot === 2 ? 1 : 0)
    }

    // Ticks the recent-calls age labels ("now", "1m", "2h") once a minute:
    // metrics.decodeNowMs is read live but only announces a change while the
    // decoder publishes, so without this a row's age freezes at whatever it said
    // when the decoder last spoke. Same device as HomeScreen's heardTick.
    property int ageTick: 0

    Timer {
        interval: 60000
        running: screen.visible
        repeat: true
        onTriggered: screen.ageTick++
    }

    // Engine truth, not local mirrors: commands only enqueue a request, and on
    // Android the service (which owns both states) outlives the Activity — a
    // relaunched UI must show where mute and hold actually stand, or its buttons
    // run inverted against the live session.
    readonly property bool muted: metrics ? metrics.audioMuted : false
    readonly property bool holding: metrics ? metrics.heldTg > 0 : false
    readonly property bool scanHeld: metrics ? metrics.scanHold : false
    // Issue #671: a scan the operator paused, by a typed frequency or Explore.
    // It owns nothing until resumed, so the header says so in place of the
    // rotation, and the scan controls give way to Resume scan.
    readonly property bool scanPaused: metrics ? metrics.scanPaused : false
    // The paused header that fits @a width, losing its noun before its number:
    // "SCAN PAUSED · 12 channels", then "SCAN PAUSED · 12", then the state alone.
    // The count is the line's reason to exist, and elision would cut it first.
    function pausedScanText(width) {
        if (!screen.scanPaused)
            return "";
        // A -Y resume waits on its first channel's tune before the scan runs.
        var state = metrics.scanResumable ? qsTr("SCAN PAUSED") : qsTr("RESUMING SCAN");
        var n = metrics.scanEntryCount;
        if (n <= 0)
            return state;
        var size = metrics.scanKind === 2 ? (n === 1 ? qsTr("1 target") : qsTr("%1 targets").arg(n)) : (n === 1 ? qsTr("1 channel") : qsTr("%1 channels").arg(n));
        // Read so a text-size change re-fits the line, as scanControls' cells do.
        var font = scanHeaderMetrics.font;
        for (var text of [state + " · " + size, state + " · " + n]) {
            if (scanHeaderMetrics.advanceWidth(text) <= width)
                return text;
        }
        return state;
    }
    FontMetrics {
        id: scanHeaderMetrics
        font.family: Theme.mono
        font.pixelSize: Theme.fontSize(11)
        font.letterSpacing: 0.8
    }

    // #647 D1a: the session's USB receiver, where the host brokers it (Android).
    // LOST as soon as the host says the session's receiver was unplugged or
    // stopped sending, while the engine still winds down (running stays true
    // through its cleanup). The desktop opens its receiver itself and shows
    // none of this.
    DeviceStatus {
        id: receiverStatus
        host: decoderHost
    }
    readonly property bool receiverLost: decoderHost.localDeviceBrokered === true && (receiverStatus.state === "lost" || receiverStatus.state === "fault")
    // The local receiver the session runs on: the saved system's, or the lost
    // one's own after an Activity recreation, which leaves no system map. Empty
    // for rtl_tcp, network audio and files, whose pill stays an indicator.
    readonly property string sessionReceiver: decoderHost.localDeviceBrokered !== true ? "" : receiverLost ? receiverStatus.kind : system && (system.sourceType === "usb" || system.sourceType === "airspy") ? system.sourceType : ""
    readonly property string liveLabel: receiverLost ? qsTr("LOST") : decoderHost.running ? (system && system.sourceType === "file" ? qsTr("PLAYING") : qsTr("LIVE")) : decoderHost.statusText.toUpperCase()
    function openReceiverSheet() {
        receiverSheet.openFor(receiverLost);
    }

    onHeroHeadlineChanged: heroText.requestPaint()
    onHeroCompactChanged: heroText.requestPaint()

    // ---- Layout (issue #718): calls first ----
    //
    // Top to bottom: the header, the hero, the live strip (the other slot, then
    // the scan controls), one list, the call actions and Stop. The list is the
    // one thing that scrolls. Its header holds the "Signal & site" disclosure,
    // the readings it folds away and the RECENT CALLS label; its rows are the
    // recent calls.
    //
    // The tier is the first of these that leaves at least two whole call rows on
    // screen below the list header, with the list at its top and the disclosure
    // closed, measured at the current size and fonts:
    //   0  stacked header, full hero, live strip fixed;
    //   1  header actions on the title line (Sites and Spectrum are in the
    //      session menu);
    //   2  compact hero;
    //   3  live strip in the list header, above the disclosure;
    //   4  no RECENT CALLS label;
    //   5  even 4 leaves less than one row: only Stop stays put and everything
    //      else scrolls with the calls, so every control stays reachable.
    // Only the size, the fonts and which live-strip parts the session shows feed
    // it, never call content, so traffic never moves the screen. A scan's hops
    // between two-slot and single-slot carriers do not move it either: the
    // rotation keeps the slot strip's place (rotationReservesStrip).
    readonly property int tier: {
        var two = 2 * callRowHeight;
        for (var t = 0; t <= 4; ++t) {
            if (callSpaceFor(t) >= two)
                return t;
        }
        return callSpaceFor(4) < callRowHeight ? 5 : 4;
    }
    // Wide screens fit every header action beside the title in any tier. Where
    // the host brokers a USB receiver (#647 B16), the actions must also fit the
    // row, measured with the pill at its widest label; else Sites and Spectrum
    // are left to the session menu, as past tier 0, and nothing is stacked.
    readonly property bool headerStacked: tier === 0 && width < 500 && headerActionsFit
    readonly property bool wideHeaderActions: (tier === 0 || width >= 500) && headerActionsFit
    readonly property bool heroCompact: tier >= 2
    readonly property bool stripInList: tier >= 3
    readonly property bool labelShown: tier < 4
    readonly property bool scrollAll: tier >= 5

    // The measured parts of the budget. Each is the height the layout below
    // gives that part, so the budget and the screen cannot disagree.
    readonly property real pillHeight: Math.max(48, 30 * Theme.fontScale)
    readonly property real callRowHeight: Math.max(Theme.rowHeight, Math.ceil(rowNameProbe.implicitHeight + 3 + rowMetaProbe.implicitHeight) + 24)
    readonly property real heroMicroHeight: Math.ceil(heroMicroMetrics.height)
    readonly property real heroSublineHeight: Math.ceil(Math.max(heroSublineMetrics.height, heroEncTag.implicitHeight, heroEmergencyTag.implicitHeight, heroPrivateTag.implicitHeight))
    readonly property real heroFullHeight: 2 * 14 + heroMicroHeight + 6 + Math.ceil(Theme.fontSize(40)) + 6 + heroSublineHeight + 6 + Math.max(26, Math.ceil(heroDurationMetrics.height))
    readonly property real heroCompactHeight: 2 * 12 + heroMicroHeight + 4 + Math.ceil(Theme.fontSize(30)) + 4 + heroSublineHeight
    readonly property real stripHeight: {
        var parts = [];
        if (stripPlaceHeld)
            parts.push(otherSlotHost.height);
        if (metrics.scanRotationActive || screen.scanPaused)
            parts.push(scanControls.height);
        return columnHeight(parts);
    }

    function columnHeight(parts) {
        var total = 0;
        for (var i = 0; i < parts.length; ++i)
            total += parts[i];
        return parts.length > 0 ? total + (parts.length - 1) * Theme.gap : 0;
    }
    function headerBarHeight(t) {
        var stacked = t === 0 && width < 500 && headerActionsFit;
        return stacked ? heading.implicitHeight + 8 + pillHeight : Math.max(heading.implicitHeight, pillHeight);
    }
    function heroHeight(t) {
        return t >= 2 ? heroCompactHeight : heroFullHeight;
    }
    // The list's viewport in tier t, less its header with the disclosure closed.
    function callSpaceFor(t) {
        var fixedTop = Theme.screenPadding + headerBarHeight(t) + 12 + heroHeight(t);
        var fixedStrip = t < 3 && stripHeight > 0 ? Theme.gap + stripHeight : 0;
        var fixedBottom = 14 + liveActions.height + 8 + stopButton.height + stopButton.anchors.bottomMargin;
        var viewport = height - fixedTop - fixedStrip - Theme.gap - fixedBottom;
        var header = [];
        if (t >= 3 && stripHeight > 0)
            header.push(stripHeight);
        header.push(detailsCard.height);
        header.push(t < 4 ? labelBlock.height : labelSpacer.height);
        return viewport - columnHeight(header);
    }

    // #647 B16: Sites, the session menu, Spectrum and the pill, each as the
    // session would show it, with the pill at the widest label it can show
    // (LIVE, LOST, STARTING…, STOPPING…, or what it says now), so the row never
    // changes make-up as the label changes. Beside the title the row keeps the
    // title's 12 px.
    readonly property real headerActionsWidth: {
        var widths = [];
        if (sitesAvailable)
            widths.push(siteChooserButton.width);
        widths.push(sessionOptionsButton.width);
        if (metrics.radioInput)
            widths.push(spectrumPill.width);
        // The pill's padding, dot and gap are constants: its Row only resizes on
        // polish, after the label has already changed.
        widths.push(livePill.chromeWidth + livePill.dotWidth + liveRow.spacing + liveLabelProbe.implicitWidth);
        var total = (widths.length - 1) * headerActions.spacing;
        for (var w of widths)
            total += w;
        return total;
    }
    readonly property bool headerActionsFit: !decoderHost.localDeviceBrokered || headerActionsWidth <= width - 2 * Theme.screenPadding - (width < 500 ? 0 : 12)

    // Probes for the budget: never shown, only measured.
    // One line per label: a plain text's width is its widest line.
    Text {
        id: liveLabelProbe
        visible: false
        // The transition words are the host's, upper-cased; LIVE and LOST are ours.
        text: [qsTr("LIVE"), qsTr("LOST"), "STARTING…", "STOPPING…", screen.liveLabel].join("\n")
        textFormat: Text.PlainText
        font: liveLabel.font
    }
    Text {
        id: rowNameProbe
        visible: false
        text: "Ag"
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(16)
        font.weight: Font.DemiBold
    }
    Text {
        id: rowMetaProbe
        visible: false
        text: "Ag"
        font.family: Theme.mono
        font.pixelSize: Theme.fontSize(12)
    }
    FontMetrics {
        id: heroMicroMetrics
        font.family: Theme.mono
        font.pixelSize: Theme.fontSize(11)
    }
    FontMetrics {
        id: heroSublineMetrics
        font.family: Theme.mono
        font.pixelSize: Theme.fontSize(13)
    }
    FontMetrics {
        id: heroDurationMetrics
        font.family: Theme.mono
        font.pixelSize: Theme.fontSize(24)
        font.weight: Font.Medium
    }

    // The disclosure starts closed and remembers how it was left, on this
    // device only (AppPrefs monitor/detailsExpanded). The preference is the
    // state: a tap writes it and the binding follows, so it never parts from
    // what is stored. prefsStore is the app's AppPrefs; a test may hand in its
    // own. With none, a local value stands in.
    property var prefsStore: typeof prefs !== "undefined" ? prefs : null
    property bool localDetailsExpanded: false
    property bool detailsExpanded: prefsStore && prefsStore.monitorDetailsExpanded !== undefined ? prefsStore.monitorDetailsExpanded === true : localDetailsExpanded
    function setDetailsExpanded(on) {
        if (prefsStore && prefsStore.monitorDetailsExpanded !== undefined)
            prefsStore.monitorDetailsExpanded = on;
        else
            localDetailsExpanded = on;
    }

    // A group of readings read as one node, named by what it shows at the
    // readings' pace (issue #782, SpeechPacer); `speechStep` is what is heard
    // at once. The labels and values inside stay plain.
    component ReadingFlow: Flow {
        id: readingFlow
        property alias speechStep: readingFlowSpeech.step
        Accessible.role: Accessible.StaticText
        readonly property bool navigationAllowed: Navigation.allows(readingFlow)
        Accessible.ignored: !navigationAllowed || !visible
        Accessible.name: readingFlowSpeech.spoken
        SpeechPacer {
            id: readingFlowSpeech
            group: readingFlow
            shown: !readingFlow.Accessible.ignored
        }
    }
    component ReadingRow: Row {
        id: readingRow
        property alias speechStep: readingRowSpeech.step
        Accessible.role: Accessible.StaticText
        readonly property bool navigationAllowed: Navigation.allows(readingRow)
        Accessible.ignored: !navigationAllowed || !visible
        Accessible.name: readingRowSpeech.spoken
        SpeechPacer {
            id: readingRowSpeech
            group: readingRow
            shown: !readingRow.Accessible.ignored
        }
    }

    // What makes a moving reading worth hearing at once (issue #782; SpeechPacer):
    // a call starting or ending on either slot, a change of lock or sync, and a
    // scan held or released (off a scan, a new frequency). A scan's hop alone
    // is not one (Util.tuneStep). Between these, readings are re-read at most
    // every 5 s.
    readonly property string speechStep: [Util.tuneStep(metrics), metrics.carrierLock, metrics.syncedHere,
        metrics.syncLabel, heroSlot, heroTgId, heroSrc, otherSlot, otherTgId].join("|")

    // The disclosure's one-line summary, most important first: the radio's SNR,
    // decode quality (a codec fault in the alert colour ahead of it), the
    // decryption state, an ENC LOCKOUT count in magenta, then the site. Only
    // readings the rows below already show; a fault is said here, never by
    // opening the rows.
    readonly property var detailsSummary: {
        var tokens = [];
        if (metrics.radioInput && metrics.snrValid)
            tokens.push({text: qsTr("SNR %1 dB").arg(Util.fixed(metrics.snrDb, 1)), color: ""});
        if (metrics.tetraVisible && metrics.tetraCodecFault && metrics.tetraCodecText.length > 0)
            tokens.push({text: qsTr("CODEC") + " " + metrics.tetraCodecText, color: String(Theme.alert)});
        if (metrics.qualityValid) {
            if (metrics.ccFecValid)
                tokens.push({text: qsTr("CC FEC") + " " + metrics.ccFecOkPct.toFixed(0) + "%", color: ""});
            else if (metrics.voiceErrsValid)
                tokens.push({text: qsTr("VOICE") + " " + metrics.voiceErrsPerFrame.toFixed(1) + " err/fr", color: ""});
            else if (metrics.rsValid)
                tokens.push({text: qsTr("RS") + " " + metrics.rsOkPct.toFixed(0) + "%", color: ""});
            else if (metrics.lastFrameErrsValid)
                tokens.push({text: qsTr("ERR") + " " + metrics.lastFrameErrs + "/" + metrics.lastFrameErrs2, color: ""});
        }
        // "Not yet known" says nothing a closed summary needs.
        if (keySheet.effectiveState !== qsTr("Not yet known"))
            tokens.push({text: keySheet.effectiveState, color: ""});
        if (metrics.encLockoutCount > 0)
            tokens.push({text: qsTr("ENC LOCKOUT %1").arg(metrics.encLockoutCount), color: String(Theme.magenta)});
        var site = Util.siteShortForm(metrics.siteLine);
        if (site.length > 0)
            tokens.push({text: site, color: ""});
        return Util.summaryMarkup(tokens);
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.bg
    }

    // Header and hero: fixed at the top, or the first thing in the list in tier 5.
    Column {
        id: topGroup
        parent: screen.scrollAll ? topSlot : topHost
        width: parent ? parent.width : 0
        spacing: 12

        // Header
        Item {
            id: header
            objectName: "monitorHeader"
            enabled: !keySheet.visible

            width: parent.width
            // Stacked only in tier 0 on a phone; otherwise the actions share the
            // title line and the heading elides to what is left of it.
            readonly property bool stacked: screen.headerStacked
            height: screen.headerBarHeight(screen.tier)

            Column {
                id: heading
                anchors.left: parent.left
                anchors.right: header.stacked ? parent.right : headerActions.left
                anchors.rightMargin: 12
                anchors.top: parent.top
                spacing: 3

                ReadableText {
                    heading: true
                    width: parent.width
                    objectName: "monitorHeaderTitle"
                    text: screen.systemName
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontSize(20)
                    font.weight: Font.DemiBold
                    color: Theme.textPrimary
                    elide: Text.ElideRight
                }

                // Paced (issue #782): a scan can hop every 300 ms, and a header named
                // by its live text was read out on every hop. Heard at once when a
                // scan starts or stops or is held or released; otherwise the target
                // it is on is read at most every 5 s.
                LiveText {
                    width: parent.width
                    objectName: "scanTargetHeader"
                    step: screen.scanPaused ? "paused" : metrics.scanTargetCount > 0 ? ["scan", metrics.scanHold].join("|") : text
                    text: screen.scanPaused ? screen.pausedScanText(width) : metrics.scanTargetCount > 0 ? qsTr("SCANNING · %1/%2").arg(metrics.scanTargetOrdinal).arg(metrics.scanTargetCount) + (metrics.scanHold ? qsTr(" · HOLD") : "") + (screen.scanTargetName.length > 0 ? " · " + screen.scanTargetName : "") : screen.system ? Util.monitorMeta(screen.system) : ""
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(11)
                    font.letterSpacing: 0.8
                    color: Theme.textSubdued
                    elide: Text.ElideRight
                }

                // Why the rotation is staying on the row above and how long is left
                // (#508). The same grammar as the terminal's `| Scan Timing:` row --
                // one space before the countdown, two between groups, seconds to one
                // decimal place -- so a reader moving between the two surfaces reads
                // the same line. Which budgets appear at all was decided in
                // app-control; a zero here means "the view withheld it".
                LiveText {
                    width: parent.width
                    objectName: "scanTimingRow"
                    // The countdowns tick; why the scan stays, and its state, are heard at once.
                    step: [screen.speechStep, metrics.scanTimingVisible, metrics.scanStayPhrase, metrics.scanDwellState,
                        metrics.scanVisitLive].join("|")
                    // Kept for the whole rotation, blank between stays (#718): the view
                    // publishes and withdraws it with each followed call, and a line
                    // coming and going would move everything below the header.
                    visible: metrics.scanTimingVisible || metrics.scanRotationActive
                    text: {
                        if (!metrics.scanTimingVisible)
                            return "";
                        var out = metrics.scanStayPhrase;
                        if (metrics.scanTimerLive) {
                            out += " " + qsTr("%1s/%2s").arg((metrics.scanTimerRemainingDs / 10).toFixed(1)).arg((metrics.scanTimerSpanMs / 1000).toFixed(1));
                        }
                        if (metrics.scanDwellMs > 0) {
                            out += "  " + qsTr("dwell %1s").arg((metrics.scanDwellMs / 1000).toFixed(1));
                            // DSD_APP_SCAN_DWELL_SUSPENDED / _PAUSED: disarmed while
                            // something else holds the row, versus stopped by the
                            // operator. Neither is a dwell that expired.
                            if (metrics.scanDwellState === 2) {
                                out += " " + qsTr("(suspended)");
                            } else if (metrics.scanDwellState === 3) {
                                out += " " + qsTr("(paused)");
                            }
                        }
                        if (metrics.scanHoldMs > 0) {
                            out += "  " + qsTr("hold %1s").arg((metrics.scanHoldMs / 1000).toFixed(1));
                        }
                        if (metrics.scanHangMs > 0) {
                            out += "  " + qsTr("hang %1s").arg((metrics.scanHangMs / 1000).toFixed(1));
                        }
                        // Last, the ceiling on the whole visit (#507). A word rather than
                        // 0.0s while a hold suspends it: a zero countdown would read as a
                        // visit that just ran out.
                        if (metrics.scanVisitMs > 0) {
                            out += "  " + (metrics.scanVisitLive
                                ? qsTr("Visit: %1s/%2s").arg((metrics.scanVisitRemainingDs / 10).toFixed(1)).arg((metrics.scanVisitMs / 1000).toFixed(1))
                                : qsTr("Visit: paused"));
                        }
                        return out;
                    }
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(11)
                    font.letterSpacing: 0.8
                    color: Theme.textSubdued
                    // On a narrow phone screen, elide long budgets to leave room for controls.
                    elide: Text.ElideRight
                }
            }

            Row {
                id: headerActions
                anchors.right: parent.right
                anchors.top: header.stacked ? heading.bottom : parent.top
                anchors.topMargin: header.stacked ? 8 : 0
                spacing: 8

                // On a phone past tier 0, Sites and Spectrum are in the session menu,
                // which carries both on the same conditions.
                OutlineButton {
                    id: siteChooserButton
                    objectName: "runningSiteChooserButton"
                    visible: screen.sitesAvailable && screen.wideHeaderActions
                    // One height for the whole row, which the budget measures.
                    height: screen.pillHeight
                    text: qsTr("Sites ›")
                    onClicked: screen.openSites()
                }

                IconButton {
                    id: sessionOptionsButton
                    objectName: "sessionOptionsButton"
                    icon: "more"
                    accessibleName: qsTr("Session options")
                    onClicked: screen.openSessionMenu()
                }

                // Only an RTL front end has a band around the tuned frequency to show;
                // a PCM feed or a file has nothing to draw.
                Rectangle {
                    id: spectrumPill

                    objectName: "openSpectrumButton"
                    activeFocusOnTab: Navigation.allows(spectrumPill) && enabled
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("Spectrum")
                    readonly property bool navigationAllowed: Navigation.allows(spectrumPill)
                    Accessible.ignored: !navigationAllowed
                    Accessible.onPressAction: {
                        if (Navigation.allows(spectrumPill))
                            screen.openSpectrum();
                    }
                    Keys.onReturnPressed: screen.openSpectrum()
                    Keys.onSpacePressed: screen.openSpectrum()
                    FocusFrame {}
                    visible: metrics.radioInput && screen.wideHeaderActions
                    width: spectrumLabel.implicitWidth + 22
                    height: screen.pillHeight
                    radius: Theme.radiusButton
                    color: spectrumTap.pressed ? Qt.alpha(Theme.cyan, 0.08) : Theme.panel
                    border.width: 1
                    border.color: Theme.panelBorder

                    Behavior on color {
                        ColorAnimation {
                            duration: 120
                        }
                    }

                    Text {
                        id: spectrumLabel

                        anchors.centerIn: parent
                        text: qsTr("SPECTRUM")
                        font.family: Theme.mono
                        font.pixelSize: Theme.fontSize(11)
                        font.letterSpacing: 1.4
                        color: Theme.textSecondary
                    }

                    TapHandler {
                        id: spectrumTap
                        onTapped: screen.openSpectrum()
                    }
                }

                // For a USB receiver session on a brokering host, a button that
                // opens the receiver sheet; otherwise only an indicator.
                Rectangle {
                    id: livePill

                    objectName: "monitorLiveStatus"
                    // Its padding and dot, also read by headerActionsWidth.
                    readonly property int chromeWidth: 24
                    readonly property int dotWidth: 8
                    readonly property bool acts: screen.sessionReceiver.length > 0
                    readonly property bool navigationAllowed: Navigation.allows(livePill)
                    function activate() {
                        if (acts && Navigation.allows(livePill))
                            screen.openReceiverSheet();
                    }
                    // Not a tab stop behind a sheet; kept while focused, so a change
                    // under the focus never drops it mid-way.
                    activeFocusOnTab: (acts && navigationAllowed) || activeFocus
                    Accessible.role: acts ? Accessible.Button : Accessible.StaticText
                    Accessible.name: screen.receiverLost ? receiverStatus.accessibleName : liveLabel.text
                    Accessible.description: acts ? qsTr("Shows the receiver") : ""
                    Accessible.ignored: !navigationAllowed
                    Accessible.onPressAction: activate()
                    Keys.onReturnPressed: activate()
                    Keys.onEnterPressed: activate()
                    Keys.onSpacePressed: activate()
                    FocusFrame {}
                    width: liveRow.implicitWidth + chromeWidth
                    height: screen.pillHeight
                    radius: Theme.radiusButton
                    color: liveTap.pressed && acts ? Qt.alpha(Theme.cyan, 0.08) : Theme.panel
                    border.width: 1
                    border.color: Theme.panelBorder

                    Behavior on color {
                        ColorAnimation {
                            duration: 120
                        }
                    }

                    Row {
                        id: liveRow
                        anchors.centerIn: parent
                        spacing: 7

                        Item {
                            width: livePill.dotWidth
                            height: livePill.dotWidth
                            anchors.verticalCenter: parent.verticalCenter

                            Rectangle {
                                anchors.centerIn: parent
                                width: 18
                                height: 18
                                radius: 9
                                visible: Theme.dark && decoderHost.running && !screen.receiverLost
                                color: Qt.alpha(Theme.cyan, 0.25)
                            }

                            Rectangle {
                                objectName: "monitorLiveDot"
                                anchors.centerIn: parent
                                width: livePill.dotWidth
                                height: livePill.dotWidth
                                radius: livePill.dotWidth / 2
                                color: screen.receiverLost ? Theme.alert : decoderHost.running ? Theme.cyan : Theme.textSubdued
                            }
                        }

                        Text {
                            id: liveLabel
                            objectName: "monitorLiveLabel"
                            text: screen.liveLabel
                            font.family: Theme.mono
                            font.pixelSize: Theme.fontSize(11)
                            font.letterSpacing: 1.4
                            color: Theme.textPrimary
                        }
                    }

                    TapHandler {
                        id: liveTap
                        enabled: livePill.acts
                        onTapped: livePill.activate()
                    }
                }
            }
        }

        // Hero: now hearing. Its height is reserved from its fixed lines and never
        // follows content: the label, one name line, the subline and, in the full
        // form, the meter and duration. Waiting, a call and its end share the same
        // box, so the list below never moves between them.
        UiPanel {
            id: hero
            objectName: "monitorHero"
            width: parent.width
            height: screen.heroCompact ? screen.heroCompactHeight : screen.heroFullHeight
            readonly property int padding: screen.heroCompact ? 12 : 14
            // The call it captions as one item for a screen reader (issue #782):
            // the name the canvas draws, then its subline (ids, tags, algorithm,
            // channel), or what it is waiting for. The duration reads on its
            // own, so the seconds counting do not rename this.
            Accessible.role: Accessible.StaticText
            readonly property bool navigationAllowed: Navigation.allows(hero)
            Accessible.ignored: !navigationAllowed || !visible
            // Through a pacer (issue #782): its waiting line follows the lock, which
            // can flap; a call starting or ending is a step and heard at once.
            Accessible.name: heroSpeech.spoken
            SpeechPacer {
                id: heroSpeech
                shown: !hero.Accessible.ignored
                step: screen.speechStep
                read: function () {
                    return [screen.heroSlot === 0 ? heroWaitingText.text : screen.heroHeadline,
                        heroSubline.visible ? Util.shownText(heroSubline) : ""].filter(function (part) {
                        return part.length > 0;
                    }).join(", ");
                }
            }

            // Faint diagonal cyan→magenta wash over the panel.
            Rectangle {
                anchors.fill: parent
                anchors.margins: 1
                radius: Theme.radiusPanel - 1
                rotation: 0
                gradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop {
                        position: 0.0
                        color: Qt.alpha(Theme.cyan, 0.07)
                    }
                    GradientStop {
                        position: 1.0
                        color: Qt.alpha(Theme.magenta, 0.06)
                    }
                }
            }

            Column {
                x: Theme.cardPadding
                y: hero.padding
                width: parent.width - 2 * Theme.cardPadding
                spacing: screen.heroCompact ? 4 : 6

                MicroLabel {
                    height: screen.heroMicroHeight
                    text: qsTr("Now hearing")
                }

                // The name line. In the compact form the duration ends it, as the
                // meter row is gone.
                Item {
                    id: heroNameLine
                    objectName: "heroNameLine"
                    width: parent.width
                    height: Math.ceil(Theme.fontSize(screen.heroCompact ? 30 : 40))

                    // Gradient-filled talkgroup name, drawn so the fill can follow the
                    // cyan→magenta ramp per glyph run.
                    Canvas {
                        id: heroText
                        objectName: "heroHeadlineText"

                        width: parent.width - (heroCompactDuration.visible ? heroCompactDuration.width + 12 : 0)
                        height: parent.height
                        visible: screen.heroSlot !== 0
                        // Painted text has no text of its own for a screen reader.
                        Accessible.role: Accessible.StaticText
                        Accessible.name: screen.heroHeadline

                        onPaint: {
                            var ctx = getContext("2d");
                            ctx.reset();
                            var label = screen.heroHeadline;
                            ctx.font = "bold " + Theme.fontSize(screen.heroCompact ? 25 : 31) + "px \"" + Theme.sans + "\"";
                            ctx.textBaseline = "middle";
                            var gradient = ctx.createLinearGradient(0, 0, Math.max(ctx.measureText(label).width, 1), 0);
                            gradient.addColorStop(0, String(Theme.cyan));
                            gradient.addColorStop(1, String(Theme.magenta));
                            ctx.fillStyle = gradient;
                            ctx.fillText(label, 0, height / 2, width);
                        }

                        Connections {
                            target: Theme
                            function onDarkChanged() {
                                heroText.requestPaint();
                            }
                        }
                    }

                    Text {
                        id: heroWaitingText
                        objectName: "heroWaiting"
                        visible: screen.heroSlot === 0
                        width: parent.width
                        height: parent.height
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                        // A locked carrier means the site is there and quiet; only the
                        // absence of one is "no signal".
                        text: metrics.carrierLock ? qsTr("waiting for a call…") : qsTr("waiting for signal…")
                        font.family: Theme.sans
                        font.pixelSize: Theme.fontSize(20)
                        color: Theme.textSubdued
                    }

                    LiveText {
                        id: heroCompactDuration
                        objectName: "heroCompactDuration"
                        step: screen.speechStep
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        visible: screen.heroCompact && screen.heroSlot !== 0
                        text: Util.fmtDuration(screen.heroSeconds)
                        font.family: Theme.mono
                        font.pixelSize: Theme.fontSize(14)
                        font.weight: Font.Medium
                        color: Theme.textPrimary
                    }
                }

                Item {
                    width: parent.width
                    height: screen.heroSublineHeight

                    Row {
                        id: heroSubline

                        // Give encryption indicators priority, then fit the identity
                        // and trailing channel into the remaining space.
                        width: parent.width
                        visible: screen.heroSlot !== 0
                        spacing: 8

                        // A private call (#773): the tag leads, then the talker. The
                        // headline already names the other party, so no TG follows.
                        PrivateTag {
                            id: heroPrivateTag
                            objectName: "heroPrivateTag"
                            anchors.verticalCenter: parent.verticalCenter
                            visible: screen.heroPrivate
                        }

                        // An id that did not decode has no text (the call view leaves it
                        // empty, never "0"), so it is simply left out: an encrypted call
                        // on a conventional channel reports no talkgroup, and the headline
                        // already names the channel.
                        Text {
                            objectName: "heroIds"
                            anchors.verticalCenter: parent.verticalCenter
                            visible: text.length > 0
                            width: Math.max(0, Math.min(implicitWidth, heroSubline.width - (heroPrivateTag.visible ? heroPrivateTag.width + heroSubline.spacing : 0) - (heroEmergencyTag.visible ? heroEmergencyTag.width + heroSubline.spacing : 0) - (heroEncTag.visible ? heroEncTag.width + heroSubline.spacing : 0) - (heroAlgorithm.visible ? heroAlgorithm.width + heroSubline.spacing : 0)))
                            elide: Text.ElideRight
                            text: {
                                var parts = [];
                                if (!screen.heroPrivate && !screen.heroDuplicate && screen.heroTg.length > 0)
                                    parts.push("TG " + screen.heroTg);
                                if (screen.heroSrc.length > 0)
                                    parts.push("SRC " + screen.heroSrc);
                                return parts.join(" · ");
                            }
                            font.family: Theme.mono
                            font.pixelSize: Theme.fontSize(13)
                            color: Theme.textSecondary
                        }

                        // A TETRA call no party named (#773). TETRA names its parties
                        // in the call setup, so saying the identity did not decode keeps
                        // "TETRA call" from reading as all there is. X2-TDMA and ProVoice
                        // never carry one and get no note. Prose, so the sans face.
                        Text {
                            objectName: "heroIdentityNote"
                            anchors.verticalCenter: parent.verticalCenter
                            visible: screen.heroAnonymous && screen.heroProtocol === "TETRA"
                            width: Math.max(0, Math.min(implicitWidth, heroSubline.width - (heroEmergencyTag.visible ? heroEmergencyTag.width + heroSubline.spacing : 0) - (heroEncTag.visible ? heroEncTag.width + heroSubline.spacing : 0) - (heroAlgorithm.visible ? heroAlgorithm.width + heroSubline.spacing : 0)))
                            elide: Text.ElideRight
                            text: qsTr("identity not decoded")
                            font.family: Theme.sans
                            font.pixelSize: Theme.fontSize(13)
                            color: Theme.textSecondary
                        }

                        // The hero must say when the call it is captioning is
                        // encrypted — hearing silence over a normal-looking talkgroup
                        // otherwise reads as the decoder failing.
                        EmergencyTag {
                            id: heroEmergencyTag
                            objectName: "heroEmergencyTag"
                            anchors.verticalCenter: parent.verticalCenter
                            visible: screen.heroEmergency
                        }

                        EncTag {
                            id: heroEncTag
                            objectName: "heroEncTag"
                            anchors.verticalCenter: parent.verticalCenter
                            visible: screen.heroEnc
                        }

                        // The decoded algorithm and key id, when the header said:
                        // AES and RC4 traffic should read differently at a glance.
                        Text {
                            id: heroAlgorithm
                            objectName: "heroAlgorithm"
                            anchors.verticalCenter: parent.verticalCenter
                            visible: screen.heroEnc && screen.heroEncText.length > 0
                            width: Math.max(0, Math.min(implicitWidth, heroSubline.width - (heroPrivateTag.visible ? heroPrivateTag.width + heroSubline.spacing : 0) - (heroEmergencyTag.visible ? heroEmergencyTag.width + heroSubline.spacing : 0) - (heroEncTag.visible ? heroEncTag.width + heroSubline.spacing : 0)))
                            elide: Text.ElideRight
                            text: screen.heroEncText
                            font.family: Theme.mono
                            font.pixelSize: Theme.fontSize(11)
                            color: Theme.textSubdued
                        }

                        // Where the call was heard, when that is not already the
                        // headline above: a listed talkgroup still says which scan
                        // channel it came from, and so does a call headlined by its
                        // other party or its protocol. Last in the row and elided to what is left of it,
                        // so an operator-length name never pushes the ENC tag off the
                        // panel; x is laid out from the siblings before it, so the
                        // width binding cannot loop.
                        Text {
                            objectName: "heroChannel"
                            anchors.verticalCenter: parent.verticalCenter
                            visible: screen.heroChannel.length > 0 && screen.heroChannel !== screen.heroHeadline
                            width: Math.max(0, Math.min(implicitWidth, heroSubline.width - x))
                            elide: Text.ElideRight
                            text: "· " + screen.heroChannel
                            font.family: Theme.mono
                            font.pixelSize: Theme.fontSize(13)
                            color: Theme.textSecondary
                        }
                    }
                }
            }

            LevelMeter {
                visible: !screen.heroCompact
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                anchors.leftMargin: Theme.cardPadding
                anchors.bottomMargin: hero.padding
                active: screen.heroActive
            }

            LiveText {
                objectName: "heroDuration"
                step: screen.speechStep
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.rightMargin: Theme.cardPadding
                anchors.bottomMargin: hero.padding
                text: Util.fmtDuration(screen.heroSeconds)
                visible: !screen.heroCompact && screen.heroSlot !== 0
                font.family: Theme.mono
                font.pixelSize: Theme.fontSize(24)
                font.weight: Font.Medium
                color: Theme.textPrimary
            }
        }
    }


    // The live strip: the other TDMA slot, then the scan controls, each only when
    // the session has them. Fixed under the hero in tiers 0-2; in the list header,
    // above the disclosure, from tier 3.
    Column {
        objectName: "monitorLiveStrip"
        parent: screen.stripInList ? stripSlot : stripHost
        width: parent ? parent.width : 0
        spacing: Theme.gap
        enabled: !keySheet.visible

        // The concurrent TDMA call on the non-hero slot: identity plus its own
        // call controls, so a second conversation is never invisible or untouchable.
        // On a two-slot carrier the strip holds its place while that slot is idle.
        // The host holds the place itself: while it is shown the scan controls
        // below never move, and during a rotation that has visited a two-slot
        // carrier it stays, blank, on a single-slot one (issue #782). A plain
        // Item, so holding the place says nothing to an accessibility client;
        // only the panel inside comes and goes with the slot.
        Item {
            id: otherSlotHost
            objectName: "otherSlotHost"
            width: parent.width
            height: 56
            visible: screen.stripPlaceHeld

            UiPanel {
                id: otherSlotPanel
                anchors.fill: parent
                objectName: "otherSlotPanel"
                visible: screen.stripVisible

                MicroLabel {
                    id: otherSlotLabel
                    objectName: "otherSlotLabel"
                    anchors.left: parent.left
                    anchors.leftMargin: Theme.cardPadding
                    anchors.verticalCenter: parent.verticalCenter
                    text: screen.stripIdle ? qsTr("SLOT %1 · idle").arg(screen.stripSlot) : qsTr("SLOT %1").arg(screen.otherSlot)
                }

                ReadableText {
                    id: otherIdentityText
                    objectName: "otherSlotIdentity"
                    visible: !screen.stripIdle
                    // With its tags, which sit beside it (issue #782).
                    Accessible.name: Accessible.ignored ? "" : [text, otherEmergencyTag.visible ? Util.shownText(otherEmergencyTag) : "", otherEncTag.visible ? Util.shownText(otherEncTag) : ""].filter(function (part) {
                        return part.length > 0;
                    }).join(", ")
                    anchors.left: otherSlotLabel.right
                    anchors.leftMargin: 10
                    anchors.right: otherPrivateTag.visible ? otherPrivateTag.left : otherEmergencyTag.visible ? otherEmergencyTag.left : otherEncTag.visible ? otherEncTag.left : otherSkip.left
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    text: screen.otherIdentity
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontSize(14)
                    font.weight: Font.DemiBold
                    color: Theme.textPrimary
                    elide: Text.ElideRight
                }

                // The call's name outranks its tag on this narrow line: the tag shows only
                // where the whole identity still fits beside it and the ENC and EMERGENCY
                // tags, which keep their places. Measured from the identity's natural
                // width and the siblings' fixed edges, so no width binding loops.
                PrivateTag {
                    id: otherPrivateTag
                    objectName: "otherPrivateTag"
                    readonly property real room: otherSkip.x - 10 - (otherEncTag.visible ? otherEncTag.width + 10 : 0) - (otherEmergencyTag.visible ? otherEmergencyTag.width + 10 : 0) - (otherSlotLabel.x + otherSlotLabel.width + 10)
                    visible: !screen.stripIdle && screen.otherPrivate && otherIdentityText.implicitWidth + 10 + implicitWidth <= room
                    anchors.right: otherEmergencyTag.visible ? otherEmergencyTag.left : otherEncTag.visible ? otherEncTag.left : otherSkip.left
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                }

                EmergencyTag {
                    id: otherEmergencyTag
                    objectName: "otherEmergencyTag"
                    visible: !screen.stripIdle && screen.otherEmergency
                    anchors.right: otherEncTag.visible ? otherEncTag.left : otherSkip.left
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                }

                EncTag {
                    id: otherEncTag
                    visible: !screen.stripIdle && screen.otherEnc
                    anchors.right: otherSkip.left
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                }

                TextMetrics {
                    id: otherSkipMetrics
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontSize(15)
                    font.weight: Font.DemiBold
                    text: otherSkip.text
                }

                OutlineButton {
                    id: otherSkip
                    objectName: "otherSlotSkip"
                    anchors.right: otherMore.left
                    anchors.rightMargin: 4
                    anchors.verticalCenter: parent.verticalCenter
                    // Grows with large text rather than cutting "Skip" short (the label keeps 12 px a side).
                    width: Math.max(70, Math.ceil(otherSkipMetrics.advanceWidth) + 32)
                    maximumLineCount: 1
                    implicitHeight: 48
                    height: 48
                    text: qsTr("Skip")
                    accessibleName: qsTr("Skip this call on slot %1").arg(screen.otherSlot)
                    // Idle: nothing to skip. Hidden, its place kept so the strip reads the same either way.
                    visible: !screen.stripIdle
                    enabled: decoderHost.running
                    onClicked: commands.skipSlot(screen.otherSlot === 2 ? 1 : 0)
                }

                IconButton {
                    id: otherMore
                    objectName: "otherSlotMore"
                    anchors.right: parent.right
                    anchors.rightMargin: 4
                    anchors.verticalCenter: parent.verticalCenter
                    width: 48
                    height: 48
                    icon: "more"
                    accessibleName: qsTr("More actions for slot %1").arg(screen.otherSlot)
                    visible: !screen.stripIdle
                    enabled: decoderHost.running
                    onClicked: otherSlotMenu.openForOtherSlot()
                }
            }
        }

        // Actions on the scan rotation (#380): the -Y list or the trunk-scan
        // targets, whichever is running. Gated on the rotation, not on the
        // tuner gate: plain trunking owns the tuner too and has nothing to hold
        // or avoid at channel scope. Hold reads the engine back like Hold TG
        // does, and the engine's refusals ("Cannot avoid the last usable scan
        // channel") arrive through uiMessage below. While the operator has the
        // scan paused (issue #671) there is nothing on air to hold, avoid or
        // step past, and the one thing to do is resume it.
        Row {
            id: scanControls
            // Named so UI_QT_QML_CALL_LISTS can reach it with findChild().
            objectName: "scanControlsRow"

            width: parent.width
            spacing: 10
            visible: metrics.scanRotationActive || screen.scanPaused
            // Whole pixels: a cell ending on a fraction loses its right border to
            // the list's clip when the strip is in the list. Sized from the width
            // both of the strip's hosts give it (the screen less its padding), not
            // from the host it sits in: the cells' height is part of the budget
            // that picks the host, and reading the host back closed that loop.
            readonly property real cell: Math.floor((screen.width - 2 * Theme.screenPadding - 20) / 3)
            leftPadding: Math.floor((width - 3 * cell - 20) / 2)
            // One height for the three, from the widest label: a label that needs
            // two lines on a narrow phone takes them in every cell.
            readonly property real cellHeight: {
                var font = scanMetrics.font;
                var widest = 0;
                for (var label of [qsTr("Hold scan"), qsTr("Avoid"), qsTr("Next")])
                    widest = Math.max(widest, scanMetrics.advanceWidth(label));
                var lines = widest <= cell - 24 ? 1 : 2;
                return Math.max(Theme.minimumTouchSize, Math.ceil(lines * scanMetrics.lineSpacing) + 24);
            }
            height: cellHeight

            FontMetrics {
                id: scanMetrics
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(15)
                font.weight: Font.DemiBold
            }

            OutlineButton {
                objectName: "scanResumeButton"

                // The three cells' width, so the row keeps its edges.
                width: 3 * scanControls.cell + 2 * scanControls.spacing
                height: scanControls.cellHeight
                visible: screen.scanPaused
                text: qsTr("Resume scan")
                // Not twice: a -Y resume already waiting on its tune reads paused.
                enabled: decoderHost.running && metrics.scanResumable
                onClicked: commands.resumeScan()
            }

            OutlineButton {
                objectName: "scanHoldButton"

                width: scanControls.cell
                height: scanControls.cellHeight
                // A toggle with one label, its hold shown as checked like Hold TG's
                // (#718): "Release scan" took two lines on a phone.
                text: qsTr("Hold scan")
                accessibleName: qsTr("Hold the scan here")
                checkable: true
                checked: screen.scanHeld
                visible: !screen.scanPaused
                commandToggle: true
                enabled: decoderHost.running
                onClicked: commands.toggleScanHold()
            }

            OutlineButton {
                objectName: "scanAvoidButton"

                width: scanControls.cell
                height: scanControls.cellHeight
                text: qsTr("Avoid")
                visible: !screen.scanPaused
                enabled: decoderHost.running
                onClicked: commands.avoidCurrentChannel()
            }

            OutlineButton {
                objectName: "scanNextButton"

                width: scanControls.cell
                height: scanControls.cellHeight
                text: qsTr("Next")
                visible: !screen.scanPaused
                enabled: decoderHost.running
                onClicked: commands.nextChannel()
            }
        }
    }


    // Hosts for the fixed parts. Each takes its part's height while the part is
    // fixed and collapses to nothing once the part moves into the list.
    Item {
        id: topHost
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: Theme.screenPadding
        height: screen.scrollAll ? 0 : topGroup.height
    }

    Item {
        id: stripHost
        anchors.top: topHost.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: Theme.screenPadding
        anchors.rightMargin: Theme.screenPadding
        anchors.topMargin: !screen.stripInList && screen.stripHeight > 0 ? Theme.gap : 0
        height: screen.stripInList ? 0 : screen.stripHeight
    }

    Item {
        id: actionsHost
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: lostLineHost.top
        anchors.leftMargin: Theme.screenPadding
        anchors.rightMargin: Theme.screenPadding
        anchors.bottomMargin: screen.scrollAll ? 0 : 8
        height: screen.scrollAll ? 0 : liveActions.height
    }

    // The one scrolling surface: the list header, then the recent calls.
    ListView {
        id: recentList

        // Named so UI_QT_QML_CALL_LISTS can reach it with findChild().
        objectName: "recentCallsList"

        enabled: !keySheet.visible
        anchors.top: stripHost.bottom
        anchors.topMargin: screen.scrollAll ? 0 : Theme.gap
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: actionsHost.top
        anchors.leftMargin: Theme.screenPadding
        anchors.rightMargin: Theme.screenPadding
        anchors.bottomMargin: 14
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        model: monitorView
        // No current row. A ListView is a focus scope that hands focus to its
        // current item as rows land, which would pull keyboard focus off the
        // disclosure in its header; Tab moves between rows instead.
        currentIndex: -1
        keyNavigationEnabled: false

        // Scrollable by assistive technology, as the body it replaced was.
        Accessible.role: Accessible.Pane
        Accessible.name: qsTr("Recent calls")
        Accessible.focusable: false
        readonly property bool navigationAllowed: Navigation.allows(recentList)
        Accessible.ignored: !navigationAllowed || !visible
        function scrollPage(direction) {
            if (!enabled || !Navigation.allows(recentList))
                return;
            contentY = Math.max(originY, Math.min(originY + contentHeight - height, contentY + direction * Math.max(48, height * 0.8)));
        }
        Accessible.onScrollDownAction: scrollPage(1)
        Accessible.onScrollUpAction: scrollPage(-1)
        Accessible.onNextPageAction: scrollPage(1)
        Accessible.onPreviousPageAction: scrollPage(-1)

        // The content is listHeader below, declared outside so the rest of the
        // screen can reach its rows by id.
        header: Item {
            width: recentList.width
            height: listHeader.height
        }

        // Closes the calls card, or says why it is empty.
        footer: Item {
            width: recentList.width
            height: recentList.count === 0 ? Math.max(screen.callRowHeight, emptyText.implicitHeight + 24) : 6

            ReadableText {
                id: emptyText
                anchors.centerIn: parent
                width: parent.width - 2 * Theme.cardPadding
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                visible: recentList.count === 0
                text: qsTr("Calls will appear here as they land.")
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(13)
                color: Theme.textSubdued
            }
        }

        // The calls card: from the RECENT CALLS label (or the first row without
        // it) to the end of the list. In the view's contentItem, so it scrolls
        // with the rows it sits behind.
        UiPanel {
            objectName: "recentCallsPanel"
            parent: recentList.contentItem
            z: -1
            width: recentList.width
            y: recentList.headerItem ? recentList.headerItem.y + (screen.labelShown ? labelBlock.y : recentList.headerItem.height) : 0
            height: Math.max(0, recentList.originY + recentList.contentHeight - y)
        }

        delegate: CallRow {
            interactive: true
            onActivated: historyDetail.open({
                uid: model.uid,
                name: model.name,
                when: model.when,
                systemName: model.systemName,
                systemUid: model.systemUid,
                channel: model.channel,
                tg: model.tg,
                src: model.src,
                sourceName: model.srcName,
                emergency: model.emergency,
                enc: model.enc,
                durationSecs: model.durationSecs,
                detail: model.detail,
                freqHz: model.freqHz,
                accessCode: model.accessCode,
                accessCodeText: model.accessCodeText,
                serviceName: model.serviceName || "",
                receivedTone: model.receivedTone || ""
            })
            // Inset by the card's border, so a row's divider never paints over it.
            x: 1
            width: ListView.view.width - 2
            name: model.name
            metaText: {
                // A notice keeps its payload first, the pane being too
                // narrow for its ids, then names what every row names: the
                // access code, the channel it was heard on (its title drops
                // it), and the frequency last (issue #575).
                if (model.kind === 1) {
                    var parts = [model.detail.length > 0 ? model.detail : qsTr("data message")];
                    if (model.accessCode.length > 0)
                        parts.push(model.accessCode);
                    if (model.channel.length > 0 && model.channel !== model.name)
                        parts.push(model.channel);
                    if (model.freqHz > 0)
                        parts.push(Util.fmtMhz(model.freqHz));
                    return parts.join(" · ");
                }
                // Same meta rules as the history row: a zero talkgroup is
                // not printed, the access code follows the ids, the channel
                // follows the duration unless it is already the name, and
                // the frequency closes the line, the first thing a narrow
                // phone elides.
                var meta = [];
                if (model.tg > 0)
                    meta.push("TG " + model.tg);
                if (model.src > 0 || model.srcName)
                    meta.push("SRC " + Util.sourceText(model.src, model.srcName));
                if (model.accessCode.length > 0)
                    meta.push(model.accessCode);
                if (model.enc)
                    meta.push(qsTr("encrypted"));
                if (model.durationSecs >= 0)
                    meta.push(Util.fmtDuration(model.durationSecs));
                if (model.channel.length > 0 && model.channel !== model.name)
                    meta.push(model.channel);
                if (model.freqHz > 0)
                    meta.push(Util.fmtMhz(model.freqHz));
                return meta.join(" · ");
            }
            // Aged on the decode clock the row's when is stamped on. ageTick
            // forces the minute-by-minute refresh while the decoder is quiet.
            rightText: (screen.ageTick, Util.shortAge(model.when, metrics.decodeNowMs))
            enc: model.enc
            emergency: model.emergency
            hasRecording: model.hasRecording
        }
    }

    // Same rule as the history log: the list keeps showing the call that just
    // ended, minus the new-calls pill, which the hero above already covers. Its
    // newest row is the first call under the header, and the disclosure holds
    // its place on screen while the readings open and close below it.
    FollowLatest {
        objectName: "recentCallsFollow"
        list: recentList
        headerAnchor: detailsCard
    }

    // Keyboard focus moving onto a row, or into the header, scrolls it into view.
    function revealListFocus() {
        var window = screen.Window.window;
        var focus = window ? window.activeFocusItem : null;
        if (focus && screen.visible && Navigation.contains(recentList.contentItem, focus))
            Theme.revealFocus(recentList, recentList.contentItem, focus);
    }
    Connections {
        target: screen.Window.window
        function onActiveFocusItemChanged() {
            Qt.callLater(screen.revealListFocus);
        }
    }

    // The list header. Slots first for the parts that move into the list in the
    // tiers that need it, then the disclosure, its readings and the label.
    Column {
        id: listHeader
        objectName: "monitorListHeader"
        parent: recentList.headerItem
        width: recentList.width
        spacing: Theme.gap

        Item {
            id: topSlot
            width: parent.width
            visible: screen.scrollAll
            height: topGroup.height
        }

        // Sized from the session, not from the strip it holds: an Item hides its
        // children, and a strip with no visible parts measures nothing.
        Item {
            id: stripSlot
            width: parent.width
            visible: screen.stripInList && screen.stripHeight > 0
            height: screen.stripHeight
        }

        Item {
            id: actionsSlot
            width: parent.width
            visible: screen.scrollAll
            height: liveActions.height
        }

        // "Signal & site": the readings the monitor carries beside the calls,
        // folded into one row whose summary says what matters most.
        UiPanel {
            id: detailsCard
            objectName: "monitorDetailsCard"
            width: parent.width
            height: detailsRow.height

            DisclosureRow {
                id: detailsRow
                objectName: "monitorDetailsRow"
                width: parent.width
                title: qsTr("Signal & site")
                subtitle: screen.detailsSummary.markup
                subtitleReading: true
                subtitleTextFormat: Text.StyledText
                subtitleColor: Theme.textSecondary
                accessibleDescription: screen.detailsSummary.plain
                // The summary's SNR moves every tick: read at the readings' pace (issue #782).
                Accessible.description: detailsSpeech.spoken
                SpeechPacer {
                    id: detailsSpeech
                    shown: !detailsRow.Accessible.ignored
                    step: screen.speechStep
                    read: function () {
                        return detailsRow.accessibleDescription;
                    }
                }
                verticalPadding: 16
                expandable: true
                expanded: screen.detailsExpanded
                onTapped: screen.setDetailsExpanded(!screen.detailsExpanded)
            }
        }

        // The engine's answer to the last command ("Output: Muted", "Output:
        // open failed") — without it a tap that failed inside the engine reads
        // as a button that did nothing.
        ReadableText {
            width: parent.width
            visible: metrics.uiMessage.length > 0
            text: metrics.uiMessage
            font.family: Theme.mono
            font.pixelSize: Theme.fontSize(12)
            color: Theme.cyan
            elide: Text.ElideRight
        }

        // Each group of readings below (a label and its value, a strip of
        // them) is one item for a screen reader, named by the words it shows
        // (issue #782); its buttons name themselves. A reading that moves
        // renames its group, which Android speaks only while that group is
        // focused; the labels and values inside stay plain, so a value whose
        // width changes moves nothing a screen reader is told about.
        Column {
            id: detailsColumn
            objectName: "monitorDetails"
            width: parent.width
            visible: screen.detailsExpanded
            spacing: Theme.gap

            // WP-F1: site identity, first of the readings.
            UiPanel {
                id: siteRow
                objectName: "siteRow"
                width: parent.width
                height: Math.max(48, siteLabel.implicitHeight + 20)
                visible: metrics.siteLine.length > 0
                opacity: metrics.siteConfirmed ? 1 : 0.55
                Accessible.role: Accessible.Button
                Accessible.name: metrics.siteLine
                readonly property bool navigationAllowed: Navigation.allows(siteRow)
                Accessible.ignored: !navigationAllowed
                Accessible.onPressAction: {
                    if (Navigation.allows(siteRow))
                        siteSheet.visible = true;
                }
                Text {
                    id: siteLabel
                    anchors.centerIn: parent
                    width: parent.width - 24
                    text: metrics.siteLine + "  ›"
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(12)
                    color: Theme.textPrimary
                }
                TapHandler {
                    onTapped: siteSheet.visible = true
                }
            }

            UiPanel {
                id: decryptionRow
                objectName: "openKeySheetButton"
                width: parent.width
                height: Math.max(Theme.minimumTouchSize, decryptionLabel.implicitHeight + 20)
                enabled: decoderHost.running
                activeFocusOnTab: Navigation.allows(decryptionRow) && enabled
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Decryption · %1").arg(keySheet.effectiveState)
                readonly property bool navigationAllowed: Navigation.allows(decryptionRow)
                Accessible.ignored: !navigationAllowed || !visible
                Accessible.onPressAction: activate()
                function activate() {
                    if (enabled && Navigation.allows(decryptionRow))
                        keySheet.open();
                }
                Keys.onReturnPressed: activate()
                Keys.onSpacePressed: activate()
                FocusFrame {}
                Text {
                    id: decryptionLabel
                    objectName: "decryptionStatusText"
                    anchors.centerIn: parent
                    width: parent.width - 24
                    text: qsTr("Decryption · %1 ›").arg(keySheet.effectiveState)
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(12)
                    color: Theme.textPrimary
                }
                TapHandler {
                    onTapped: decryptionRow.activate()
                }
            }

            // Signal strip — tuner truths, only when a tuner exists.
            //
            // Flow, not Row: every reading's width moves with its value (a CFO can
            // run to "-1234 Hz", an SNR to "-10.0 dB"), and an unconstrained Row ran
            // the last reading off the side of a 411 dp phone with no way to reach
            // it. Each reading is one child, so a wrap drops a whole reading to the
            // next line and never splits a label from its value. Whitespace groups
            // them, as in the design — separator glyphs would strand at a line head.
            ReadingFlow {
                objectName: "signalStrip"
                speechStep: screen.speechStep
                width: parent.width
                visible: metrics.radioInput
                spacing: 10

                // Where the receiver is tuned: the frequency a plain session sits
                // on, a scan target's park frequency, or the voice channel while a
                // trunked target follows a call (issue #575). First, because it
                // is what the rest of the strip is a reading of. Here and not in
                // the header, which on a phone would elide the target's name to
                // make room for it.
                Row {
                    objectName: "monitorFreq"
                    spacing: 5
                    visible: metrics.centerFreqHz > 0

                    Text {
                        text: qsTr("FREQ")
                        font.family: Theme.mono
                        font.pixelSize: Theme.fontSize(11)
                        color: Theme.textSubdued
                    }

                    Text {
                        objectName: "monitorFreqValue"
                        text: Util.fmtMhz(metrics.centerFreqHz)
                        font.family: Theme.mono
                        font.pixelSize: Theme.fontSize(11)
                        color: Theme.textSecondary
                    }
                }

                Text {
                    width: Math.min(implicitWidth, parent.width)
                    visible: text.length > 0
                    text: {
                        if (typeof knownFrequencies === "undefined")
                            return "";
                        var revision = knownFrequencies.revision;
                        return knownFrequencies.tunedNames(metrics.centerFreqHz, metrics.channelBandwidthHz).join(" / ");
                    }
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontSize(12)
                    color: Theme.textSecondary
                    elide: Text.ElideRight
                }

                Row {
                    spacing: 5

                    Text {
                        text: qsTr("SNR")
                        font.family: Theme.mono
                        font.pixelSize: Theme.fontSize(11)
                        color: Theme.textSubdued
                    }

                    Text {
                        objectName: "monitorSnrValue"
                        text: metrics.snrValid ? Util.fixed(metrics.snrDb, 1) + " dB" : "—"
                        font.family: Theme.mono
                        font.pixelSize: Theme.fontSize(11)
                        color: metrics.snrValid ? Theme.cyan : Theme.textSubdued
                    }
                }

                Row {
                    spacing: 5
                    // The dot shows the lock by colour only; the strip says it (issue #782).
                    readonly property string spokenText: metrics.carrierLock ? qsTr("Carrier lock") : qsTr("No carrier lock")

                    Text {
                        text: qsTr("LOCK")
                        font.family: Theme.mono
                        font.pixelSize: Theme.fontSize(11)
                        color: Theme.textSubdued
                    }

                    Rectangle {
                        width: 6
                        height: 6
                        radius: 3
                        anchors.verticalCenter: parent.verticalCenter
                        color: metrics.carrierLock ? Theme.cyan : Theme.textSubdued
                    }
                }

                Text {
                    objectName: "monitorCfo"
                    text: "CFO " + Util.fixed(metrics.cfoHz, 0) + " Hz"
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(11)
                    color: Theme.textSubdued
                }

                Text {
                    text: qsTr("GAIN") + " " + metrics.tunerGainText
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(11)
                    color: Theme.textSubdued
                }

                // Sample delivery from the tuner — "no samples" and "no signal" are
                // different faults, and the terminal UI always told them apart.
                Row {
                    spacing: 5

                    Text {
                        text: qsTr("STREAM")
                        font.family: Theme.mono
                        font.pixelSize: Theme.fontSize(11)
                        color: Theme.textSubdued
                    }

                    Text {
                        text: metrics.streamActive ? qsTr("ACTIVE") : qsTr("IDLE")
                        font.family: Theme.mono
                        font.pixelSize: Theme.fontSize(11)
                        color: metrics.streamActive ? Theme.cyan : Theme.textSubdued
                    }
                }
            }

            // Decode quality also applies to PCM, network and file sources.
            ReadingFlow {
                objectName: "decodeQualityRow"
                speechStep: screen.speechStep
                width: parent.width
                visible: metrics.qualityValid
                spacing: 10

                Text {
                    objectName: "ccFecQuality"
                    visible: metrics.ccFecValid
                    text: qsTr("CC FEC") + " " + metrics.ccFecOkPct.toFixed(0) + "%"
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(11)
                    color: Theme.textSubdued
                }
                Text {
                    objectName: "voiceErrsQuality"
                    visible: metrics.voiceErrsValid
                    text: qsTr("VOICE") + " " + metrics.voiceErrsPerFrame.toFixed(1) + " err/fr"
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(11)
                    color: Theme.cyan
                }
                Text {
                    objectName: "rsQuality"
                    visible: metrics.rsValid
                    text: qsTr("RS") + " " + metrics.rsOkPct.toFixed(0) + "%"
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(11)
                    color: Theme.textSubdued
                }
                Text {
                    objectName: "lastFrameQuality"
                    visible: metrics.lastFrameErrsValid
                    text: qsTr("ERR") + " " + metrics.lastFrameErrs + "/" + metrics.lastFrameErrs2
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(11)
                    color: Theme.textSubdued
                }
            }

            // The TETRA rows, from app-control's tetra_view, which the terminal's
            // Call Info lines share: the working mode with the air encryption,
            // the network (its colour code in decimal, as the call history names
            // one), where call following stands, the control and traffic
            // channels, the last MM status and the speech codec's health. Shown
            // only under the TETRA preset once a block has decoded, each part
            // only once the decoder has something for it. Cyan for signal
            // readings and magenta for encryption, as on the rows around it, and
            // the alert colour for a codec that cannot play speech. Flows, as the
            // rows below are, so a long reading moves under its label on a narrow
            // phone rather than running off it.
            ReadingFlow {
                // Named so UI_QT_QML_CALL_LISTS can reach it with findChild().
                objectName: "monitorTetra"
                speechStep: screen.speechStep

                width: parent.width
                spacing: 5
                visible: metrics.tetraVisible

                Text {
                    text: qsTr("TETRA")
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(11)
                    color: Theme.textSubdued
                }

                Text {
                    objectName: "monitorTetraMode"

                    width: Math.min(implicitWidth, parent.width)
                    text: metrics.tetraModeText
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(11)
                    color: metrics.tetraEncrypted ? Theme.magenta : Theme.cyan
                }

                Text {
                    objectName: "monitorTetraNetwork"

                    width: Math.min(implicitWidth, parent.width)
                    visible: text.length > 0
                    text: metrics.tetraNetworkText
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(11)
                    color: Theme.textSecondary
                }
            }

            // The cell's broadcast security class, SCKN and authentication, and
            // the loaded key that applies to the carrier being received; in
            // direct mode the transmission's security class. Magenta, as
            // encryption is on the rows around it.
            ReadingFlow {
                objectName: "monitorTetraSecurity"
                speechStep: screen.speechStep

                width: parent.width
                spacing: 5
                visible: metrics.tetraVisible && metrics.tetraSecurityText.length > 0

                Text {
                    text: qsTr("SECURITY")
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(11)
                    color: Theme.textSubdued
                }

                Text {
                    objectName: "monitorTetraSecurityValue"

                    width: Math.min(implicitWidth, parent.width)
                    text: metrics.tetraSecurityText
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(11)
                    color: Theme.magenta
                }
            }

            // Each label and its reading are children of the Flow, so a long
            // reading moves under its label rather than off the body.
            ReadingFlow {
                objectName: "monitorTetraChannels"
                speechStep: screen.speechStep

                width: parent.width
                spacing: 5
                visible: metrics.tetraVisible && (metrics.tetraTrunkText.length > 0
                                                  || metrics.tetraControlText.length > 0
                                                  || metrics.tetraTrafficText.length > 0)

                Text {
                    visible: metrics.tetraTrunkText.length > 0
                    text: qsTr("TRUNK")
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(11)
                    color: Theme.textSubdued
                }

                Text {
                    objectName: "monitorTetraTrunk"

                    visible: text.length > 0
                    text: metrics.tetraTrunkText
                    textFormat: Text.PlainText
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(11)
                    color: metrics.tetraOnTraffic ? Theme.cyan : Theme.textSubdued
                }

                Text {
                    visible: metrics.tetraControlText.length > 0
                    text: qsTr("CONTROL")
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(11)
                    color: Theme.textSubdued
                }

                Text {
                    objectName: "monitorTetraControl"

                    width: Math.min(implicitWidth, parent.width)
                    visible: text.length > 0
                    text: metrics.tetraControlText
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(11)
                    color: Theme.textSecondary
                }

                Text {
                    visible: metrics.tetraTrafficText.length > 0
                    text: qsTr("TRAFFIC")
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(11)
                    color: Theme.textSubdued
                }

                Text {
                    objectName: "monitorTetraTraffic"

                    width: Math.min(implicitWidth, parent.width)
                    visible: text.length > 0
                    text: metrics.tetraTrafficText
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(11)
                    color: Theme.textSecondary
                }
            }

            ReadingFlow {
                objectName: "monitorTetraMmStatus"
                speechStep: screen.speechStep

                width: parent.width
                spacing: 5
                visible: metrics.tetraVisible && metrics.tetraMmStatusText.length > 0

                Text {
                    text: qsTr("MM STATUS")
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(11)
                    color: Theme.textSubdued
                }

                Text {
                    objectName: "monitorTetraMmStatusValue"

                    width: Math.min(implicitWidth, parent.width)
                    text: metrics.tetraMmStatusText
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(11)
                    color: Theme.textSecondary
                }
            }

            ReadingFlow {
                objectName: "monitorTetraCodec"
                speechStep: screen.speechStep

                width: parent.width
                spacing: 5
                visible: metrics.tetraVisible && metrics.tetraCodecText.length > 0

                Text {
                    text: qsTr("CODEC")
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(11)
                    color: Theme.textSubdued
                }

                Text {
                    objectName: "monitorTetraCodecValue"

                    width: Math.min(implicitWidth, parent.width)
                    text: metrics.tetraCodecText
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(11)
                    color: metrics.tetraCodecFault ? Theme.alert : Theme.cyan
                }
            }

            // Why an empty log can still be a working decoder. On an almost entirely
            // encrypted site the control channel decodes, every grant is declined and
            // no call is ever logged, which is indistinguishable from a decoder that
            // stopped. Outside the strip above and its tuner gate on purpose: this is
            // decode truth, not tuner truth, and a network source meets it just as
            // often.
            //
            // The ledger's size, not a tally of refusals: a control channel repeats a
            // grant update every few hundred ms while a call is up, so counting
            // refused grants read 150 where about a dozen transmissions had happened.
            // The ledger counts targets, and only from voice confirmed undecryptable.
            // Magenta, not cyan: cyan is signal health here, magenta is encryption,
            // same as the ENC tags on the rows below. Hidden at zero, so a site with
            // no encrypted traffic never carries a permanent 0.
            ReadingRow {
                // Named so UI_QT_QML_CALL_LISTS can reach it with findChild().
                objectName: "encLockoutRow"
                speechStep: screen.speechStep

                spacing: 5
                visible: metrics.encLockoutCount > 0

                Text {
                    text: qsTr("ENC LOCKOUT")
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(11)
                    color: Theme.textSubdued
                }

                Text {
                    objectName: "encLockoutValue"

                    text: metrics.encLockoutCount.toString()
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(11)
                    color: Theme.magenta
                }
            }

            // The sub-audible tone or code the analog FM monitor hears (#522, #523):
            // "CTCSS 100.0 Hz" or "DCS D023N / D047I" (both spellings of the code's
            // signal) once locked, "detecting" while a carrier is being evaluated, "none"
            // when it carries no supported tone or code, an em dash with no carrier.
            // Shown only while the FM monitor runs, which app-control decides for this
            // row and the terminal alike. Cyan once locked (rxToneStatus 3 is
            // DSD_APP_RX_TONE_LOCKED), like the other signal readings.
            //
            // Flow, not Row: on a narrow phone with larger text the label and a code's
            // two spellings do not fit on one line, and an unconstrained Row ran the
            // second spelling off the clipped body with no way to reach it. The value
            // then moves under the label whole; it wraps between its words only if
            // even it alone is wider than the body.
            ReadingFlow {
                // Named so UI_QT_QML_CALL_LISTS can reach it with findChild().
                objectName: "monitorRxTone"
                speechStep: screen.speechStep

                width: parent.width
                spacing: 5
                visible: metrics.rxToneVisible

                Text {
                    objectName: "monitorRxToneLabel"

                    text: qsTr("RECEIVED TONE")
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(11)
                    color: Theme.textSubdued
                }

                Text {
                    objectName: "monitorRxToneValue"

                    width: Math.min(implicitWidth, parent.width)
                    text: metrics.rxToneText
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(11)
                    color: metrics.rxToneStatus === 3 ? Theme.cyan : Theme.textSubdued
                }
            }

            // The CTCSS/DCS receive policy in force (#527), beside the received tone
            // but never fed by it: "allow 100.0 Hz/D023N", marked "(row; default X)",
            // naming the configured policy X it shadows, while a scan row's own policy
            // runs, and while a carrier is heard what the policy does
            // with it: "passing" in cyan (toneFilterGate 2 is
            // DSD_ANALOG_TONE_GATE_ALLOWED), "muted: checking tone", "muted: not
            // allowed" or "muted: no tone" in magenta. Shown while a policy is in force
            // where detection runs, which app-control decides for this row and the
            // terminal alike, and with no policy too wherever detection runs, reading
            // "off", so its Edit button can open the live editor. A Flow for the received
            // row's reason: a long list, its verdict and the button move under the label
            // on a narrow phone rather than run off it.
            ReadingFlow {
                // Named so UI_QT_QML_CALL_LISTS can reach it with findChild().
                objectName: "monitorToneFilter"
                speechStep: screen.speechStep

                width: parent.width
                spacing: 5
                visible: metrics.toneFilterVisible || metrics.toneFilterEditable

                Text {
                    objectName: "monitorToneFilterLabel"

                    text: qsTr("TONE FILTER")
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(11)
                    color: Theme.textSubdued
                }

                Text {
                    objectName: "monitorToneFilterValue"

                    width: Math.min(implicitWidth, parent.width)
                    text: metrics.rxToneConfiguredText
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(11)
                    color: Theme.textSubdued
                }

                Text {
                    objectName: "monitorToneFilterStatus"

                    width: Math.min(implicitWidth, parent.width)
                    visible: text.length > 0
                    text: metrics.toneFilterStatusText
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(11)
                    color: metrics.toneFilterGate === 2 ? Theme.cyan : Theme.magenta
                }

                // The live editor (#527): it edits the configured policy, which a scan
                // row with its own keeps in force while on air. Its own size, a full
                // touch target that grows with the text: the Flow moves it under the
                // row's text when it does not fit beside it.
                OutlineButton {
                    objectName: "monitorToneFilterEdit"

                    text: qsTr("Edit")
                    accessibleName: qsTr("Edit tone filter")
                    enabled: decoderHost.running
                    onClicked: toneFilterSheet.openEditor()
                }
            }

            Flow {
                width: parent.width
                spacing: 8
                OutlineButton {
                    objectName: "monitorReceiveFilters"
                    text: qsTr("Receive filters")
                    enabled: decoderHost.running
                    onClicked: receiveFiltersSheet.openEditor()
                }
                OutlineButton {
                    objectName: "monitorServiceNames"
                    text: qsTr("Service names")
                    enabled: decoderHost.running
                    onClicked: serviceNamesSheet.openEditor()
                }
            }
            ReadableText {
                objectName: "monitorReceiveStatus"
                width: parent.width
                text: metrics.receiveStatus || ""
                visible: text.length > 0
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSize(13)
            }

            // The squelch on audio input (#628): a level, or the noise squelch it
            // learns from the input, read as the terminal's SQL field spells it
            // ("noise +10 dB (learning)", "-50.0 dB", "(off: no band above
            // voice)"). On a radio input the Radio sheet has the squelch; here,
            // with no tuner, the monitor is where it is set. A Flow, as the
            // tone filter row is, so the button moves under a long reading.
            ReadingFlow {
                // Named so UI_QT_QML_CALL_LISTS can reach it with findChild().
                objectName: "monitorSquelch"
                // The squelch opening or closing is heard at once too.
                speechStep: screen.speechStep + "|" + (metrics.squelchGateOpen === true)

                width: parent.width
                spacing: 5
                visible: metrics.squelchAudioInput === true

                Text {
                    objectName: "monitorSquelchLabel"

                    text: qsTr("SQUELCH")
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(11)
                    color: Theme.textSubdued
                }

                Text {
                    objectName: "monitorSquelchValue"

                    width: Math.min(implicitWidth, parent.width)
                    text: metrics.squelchReadout
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(11)
                    color: Theme.textSubdued
                }

                OutlineButton {
                    objectName: "monitorSquelchEdit"

                    text: qsTr("Edit")
                    accessibleName: qsTr("Edit squelch")
                    enabled: decoderHost.running
                    onClicked: squelchSheet.openEditor()
                }
            }

            // The passband on audio input with a rigctl peer (#621): the peer
            // demodulates the audio, so the analog width is the passband it is
            // asked for, read as the terminal's Passband field spells it ("NFM
            // 12.5 kHz (-B)", "NFM peer's own"). On a radio input the Radio
            // sheet has the width; here, with no tuner, the monitor is where it
            // is set. A Flow, as the squelch row is, so the button moves under a
            // long reading.
            ReadingFlow {
                // Named so UI_QT_QML_CALL_LISTS can reach it with findChild().
                objectName: "monitorPassband"
                speechStep: screen.speechStep

                width: parent.width
                spacing: 5
                visible: metrics.peerPassband === true
                    && (metrics.nfmBandwidthOffered === true || metrics.amBandwidthOffered === true)

                Text {
                    objectName: "monitorPassbandLabel"

                    text: qsTr("PASSBAND")
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(11)
                    color: Theme.textSubdued
                }

                Text {
                    objectName: "monitorPassbandValue"

                    width: Math.min(implicitWidth, parent.width)
                    text: passbandSheet.summary
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(11)
                    color: Theme.textSubdued
                }

                OutlineButton {
                    objectName: "monitorPassbandEdit"

                    text: qsTr("Edit")
                    accessibleName: qsTr("Edit passband")
                    enabled: decoderHost.running
                    onClicked: passbandSheet.openEditor()
                }
            }

            // Why the scan stopped moving (#380): channels or targets the operator
            // avoided for the session, with the way to put them back. Hidden at zero
            // and outside any rotation, so an idle session never carries a 0. The
            // [avoided] marker is the trunk-scan fallback: every alternate failed to
            // retune and the receiver stayed on a target that was avoided.
            ReadingRow {
                // Named so UI_QT_QML_CALL_LISTS can reach it with findChild().
                objectName: "scanAvoidRow"
                speechStep: screen.speechStep

                spacing: 8
                visible: metrics.scanRotationActive && metrics.scanAvoidCount > 0

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: metrics.scanTargetAvoided ? qsTr("SCAN AVOIDS [avoided]") : qsTr("SCAN AVOIDS")
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(11)
                    color: Theme.textSubdued
                }

                Text {
                    objectName: "scanAvoidValue"

                    anchors.verticalCenter: parent.verticalCenter
                    text: metrics.scanAvoidCount.toString()
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(11)
                    color: Theme.cyan
                }

                OutlineButton {
                    objectName: "scanAvoidClearButton"

                    width: 72
                    implicitHeight: 28
                    text: qsTr("Clear")
                    enabled: decoderHost.running
                    onClicked: commands.clearScanAvoids()
                }
            }
        }

        Item {
            id: labelBlock
            width: parent.width
            visible: screen.labelShown
            height: Theme.cardPadding + recentLabel.implicitHeight + 10

            MicroLabel {
                id: recentLabel
                objectName: "recentCallsLabel"
                x: Theme.cardPadding
                y: Theme.cardPadding
                text: qsTr("Recent calls")
            }
        }

        // Without the label, keeps the calls card a gap below the rows above. One
        // pixel tall: a Column does not space an item of no height.
        Item {
            id: labelSpacer
            width: parent.width
            height: 1
            visible: !screen.labelShown
        }
    }

    HistoryDetailSheet {
        id: historyDetail
        objectName: "monitorHistoryDetail"
    }

    ActionMenu {
        id: otherSlotMenu
        objectName: "otherSlotMenu"
        property int menuSlot: 0
        property double menuTgId: 0
        property bool menuPersist: false
        readonly property bool persistTgLockouts: metrics.persistTgLockouts
        property bool menuHeld: false
        property bool menuPrivate: false
        readonly property bool heldHere: menuTgId > 0 && menuHeld
        title: qsTr("Slot %1").arg(menuSlot)
        onPersistTgLockoutsChanged: {
            if (visible && persistTgLockouts !== menuPersist)
                visible = false;
        }

        function openForOtherSlot() {
            menuSlot = screen.otherSlot;
            menuTgId = screen.otherTgId;
            menuPrivate = screen.otherPrivate;
            menuPersist = metrics.persistTgLockouts;
            menuHeld = metrics.heldTg === menuTgId;
            open();
        }
        function closeIfCallChanged() {
            if (visible && (screen.otherSlot !== menuSlot || screen.otherTgId !== menuTgId || !screen.otherActive))
                visible = false;
        }
        Connections {
            target: screen
            function onOtherSlotChanged() { otherSlotMenu.closeIfCallChanged(); }
            function onOtherTgIdChanged() { otherSlotMenu.closeIfCallChanged(); }
            function onOtherActiveChanged() { otherSlotMenu.closeIfCallChanged(); }
        }
        actions: [
            {
                objectName: "otherSlotHold",
                text: heldHere ? qsTr("Release hold") : menuPrivate ? qsTr("Hold radio %1").arg(Util.idText(menuTgId)) : qsTr("Hold TG %1").arg(Util.idText(menuTgId)),
                description: menuPrivate ? qsTr("Only calls with this radio are heard; the other slot goes quiet") : qsTr("Only this talkgroup is heard; the other slot goes quiet"),
                enabled: decoderHost.running && (heldHere || menuTgId > 0)
            },
            {
                objectName: "otherSlotAvoid",
                text: menuPersist ? qsTr("Lock out") : qsTr("Avoid TG"),
                description: menuPrivate ? qsTr("A private call has no talkgroup to lock out") : menuPersist ? qsTr("Lock out talkgroup and save") : qsTr("Avoid talkgroup for this session"),
                enabled: decoderHost.running && menuSlot !== 0 && !menuPrivate
            }
        ]
        onTriggered: function (index) {
            if (index === 0)
                commands.holdTalkgroup(heldHere ? 0 : menuTgId);
            else if (index === 1)
                commands.lockoutSlot(menuSlot === 2 ? 1 : 0);
        }
    }

    ReceivePolicySheet {
        id: receiveFiltersSheet
    }
    ReceivePolicySheet {
        id: serviceNamesSheet
        namesEditor: true
    }

    // #527: the live tone-filter editor, over the monitor like the key sheet.
    ToneFilterSheet {
        id: toneFilterSheet
        z: 10
    }

    // #628: the squelch on audio input, over the monitor like the tone-filter editor.
    SquelchSheet {
        id: squelchSheet
        z: 10
    }

    // #621: the passband a rigctl peer is asked for on audio input, over the monitor like the squelch editor.
    PassbandSheet {
        id: passbandSheet
        z: 10
    }

    // WP-D2: the modal consumes input above every monitor control.
    KeySheet {
        id: keySheet
        protocol: Util.decryptionProtocol(screen.system ? screen.system.decodeFlag : "")
        targetName: screen.scanTargetName
        systemUid: screen.sessionSystemUid
        systemName: screen.system ? screen.system.name || "" : ""
        onSystemProfileSaved: function (uid) {
            screen.systemProfileSaved(uid);
        }
        z: 10
    }

    // The call actions: Mute · Hold TG · Skip · Avoid, one row when they fit.
    // Fixed above Stop, or in the list header in tier 5.
    Column {
        id: liveActions
        objectName: "monitorLiveActions"
        parent: screen.scrollAll ? actionsSlot : actionsHost
        width: parent ? parent.width : 0
        spacing: 10
        enabled: !keySheet.visible
        // A deterministic grid: four equal columns when the longest word of any
        // label fits a quarter cell (less the 12 px OutlineButton keeps either
        // side), else two. Labels break only at a space, into one line or two
        // for every cell alike, so no row is ragged and no word splits. The
        // toggles keep one label each and show their state as checked, so the
        // widest word is "Avoid", which fits four columns on a 360 dp phone.
        readonly property var labels: [qsTr("Mute"), qsTr("Hold TG"), qsTr("Skip"), qsTr("Avoid TG"), qsTr("Lock out")]
        readonly property real widest: {
            // advanceWidth() is a call, which a binding does not track:
            // reading the font re-measures on a scale change.
            var font = actionMetrics.font;
            var widest = 0;
            for (var label of labels)
                widest = Math.max(widest, actionMetrics.advanceWidth(label));
            return Math.ceil(widest);
        }
        readonly property real longestWord: {
            var font = actionMetrics.font;
            var longest = 0;
            for (var label of labels) {
                for (var word of label.split(" "))
                    longest = Math.max(longest, actionMetrics.advanceWidth(word));
            }
            return Math.ceil(longest);
        }
        readonly property int columns: (width - 3 * spacing) / 4 - 24 >= longestWord ? 4 : 2
        // Whole pixels, for the same reason as the scan controls' cells.
        readonly property real cell: Math.floor((width - (columns - 1) * spacing) / columns)
        // What the whole pixels leave over, split either side.
        readonly property real inset: Math.floor((width - columns * cell - (columns - 1) * spacing) / 2)
        // OutlineButton keeps 12 px either side of its label.
        readonly property int labelLines: widest <= cell - 24 ? 1 : 2
        readonly property real cellHeight: Math.max(Theme.minimumTouchSize, Math.ceil(labelLines * actionMetrics.lineSpacing) + 24)

        FontMetrics {
            id: actionMetrics
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(15)
            font.weight: Font.DemiBold
        }

        Item {
            objectName: "monitorCallActions"
            readonly property int rows: Math.ceil(4 / liveActions.columns)
            width: parent.width
            height: rows * liveActions.cellHeight + (rows - 1) * liveActions.spacing

            // Engine truth: checked follows the snapshot after the command applies.
            OutlineButton {
                objectName: "muteButton"
                x: liveActions.inset + (0 % liveActions.columns) * (liveActions.cell + liveActions.spacing)
                y: Math.floor(0 / liveActions.columns) * (liveActions.cellHeight + liveActions.spacing)
                width: liveActions.cell
                height: liveActions.cellHeight
                maximumLineCount: liveActions.labelLines
                wrapMode: Text.WordWrap
                text: qsTr("Mute")
                accessibleName: qsTr("Mute audio")
                checkable: true
                checked: screen.muted
                commandToggle: true
                enabled: decoderHost.running
                onClicked: commands.toggleMute()
            }

            OutlineButton {
                objectName: "holdTalkgroupButton"
                x: liveActions.inset + (1 % liveActions.columns) * (liveActions.cell + liveActions.spacing)
                y: Math.floor(1 / liveActions.columns) * (liveActions.cellHeight + liveActions.spacing)
                width: liveActions.cell
                height: liveActions.cellHeight
                maximumLineCount: liveActions.labelLines
                wrapMode: Text.WordWrap
                // A private call holds its radio: the decoder's hold matches either party of a private call.
                text: screen.heroPrivate ? qsTr("Hold") : qsTr("Hold TG")
                accessibleName: screen.heroPrivate ? qsTr("Hold calls with radio %1").arg(Util.idText(screen.heroTgId)) : qsTr("Hold this talkgroup")
                checkable: true
                checked: screen.holding
                commandToggle: true
                // Callsigns and dial strings have no numeric talkgroup to hold.
                enabled: decoderHost.running && (screen.holding || screen.heroTgId > 0)
                onClicked: commands.holdTalkgroup(screen.holding ? 0 : screen.heroTgId)
            }

            OutlineButton {
                objectName: "skipButton"
                x: liveActions.inset + (2 % liveActions.columns) * (liveActions.cell + liveActions.spacing)
                y: Math.floor(2 / liveActions.columns) * (liveActions.cellHeight + liveActions.spacing)
                width: liveActions.cell
                height: liveActions.cellHeight
                maximumLineCount: liveActions.labelLines
                wrapMode: Text.WordWrap
                text: qsTr("Skip")
                accessibleName: qsTr("Skip this call")
                enabled: decoderHost.running && screen.heroSlot !== 0
                onClicked: commands.skipSlot(screen.heroSlot === 2 ? 1 : 0)
            }

            // The talkgroup list is in the session menu on every size.
            AvoidAction {
                objectName: "avoidButton"
                heroSlot: screen.heroSlot
                running: decoderHost.running
                privateCall: screen.heroPrivate
                x: liveActions.inset + (3 % liveActions.columns) * (liveActions.cell + liveActions.spacing)
                y: Math.floor(3 / liveActions.columns) * (liveActions.cellHeight + liveActions.spacing)
                width: liveActions.cell
                height: liveActions.cellHeight
                maximumLineCount: liveActions.labelLines
                wrapMode: Text.WordWrap
            }
        }
    }

    // #647 D1a: in tier 5 the header, and the pill with it, scrolls with the
    // calls, so a lost receiver is also said here, fixed above Stop, and opens
    // the pill's sheet. Outside the budget: it shows only where the budget has
    // already left Stop alone in place, so the tier never moves with it, and
    // it takes no room while hidden.
    Item {
        id: lostLineHost
        readonly property bool shown: screen.scrollAll && screen.receiverLost
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: stopButton.top
        anchors.leftMargin: Theme.screenPadding
        anchors.rightMargin: Theme.screenPadding
        anchors.bottomMargin: shown ? 8 : 0
        height: shown ? lostLine.height : 0

        Rectangle {
            id: lostLine
            objectName: "monitorReceiverLostLine"
            visible: lostLineHost.shown
            enabled: !keySheet.visible
            readonly property bool navigationAllowed: Navigation.allows(lostLine)
            function activate() {
                if (visible && enabled && Navigation.allows(lostLine))
                    screen.openReceiverSheet();
            }
            activeFocusOnTab: (lostLineHost.shown && navigationAllowed) || activeFocus
            Accessible.role: Accessible.Button
            Accessible.name: receiverStatus.accessibleName
            Accessible.description: qsTr("The session is finishing")
            Accessible.ignored: !navigationAllowed || !visible
            Accessible.onPressAction: activate()
            Keys.onReturnPressed: activate()
            Keys.onEnterPressed: activate()
            Keys.onSpacePressed: activate()
            FocusFrame {}
            width: parent.width
            height: Math.max(screen.pillHeight, lostLineLabel.implicitHeight + 16)
            radius: Theme.radiusButton
            color: lostLineTap.pressed ? Qt.alpha(Theme.cyan, 0.08) : Theme.panel
            border.width: 1
            border.color: Theme.panelBorder

            Behavior on color {
                ColorAnimation {
                    duration: 120
                }
            }

            Rectangle {
                x: 12
                anchors.verticalCenter: parent.verticalCenter
                width: 8
                height: 8
                radius: 4
                color: Theme.alert
            }

            Text {
                id: lostLineLabel
                x: 12 + 8 + 7
                anchors.verticalCenter: parent.verticalCenter
                text: receiverStatus.pillText
                font.family: Theme.mono
                font.pixelSize: Theme.fontSize(11)
                font.letterSpacing: 1.4
                color: Theme.textPrimary
            }

            Text {
                anchors.left: lostLineLabel.right
                anchors.leftMargin: 10
                anchors.right: lostLineChevron.left
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Session finishing")
                elide: Text.ElideRight
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(13)
                color: Theme.textSubdued
            }

            Text {
                id: lostLineChevron
                anchors.right: parent.right
                anchors.rightMargin: 14
                anchors.verticalCenter: parent.verticalCenter
                text: "›"
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(17)
                color: Theme.textSubdued
            }

            TapHandler {
                id: lostLineTap
                onTapped: lostLine.activate()
            }
        }
    }

    GradientButton {
        id: stopButton
        objectName: "stopListeningButton"

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: Theme.screenPadding
        anchors.bottomMargin: 22
        // 3 is DecoderHost.Stopping. The host publishes it the moment Stop is
        // tapped (#718), so the tap reads as taken while the engine winds down.
        text: decoderHost.sessionState === 3 ? qsTr("Stopping…") : screen.system && screen.system.sourceType === "file" ? qsTr("Stop replay") : qsTr("Stop listening")
        enabled: !decoderHost.transitioning && !keySheet.visible
        onClicked: decoderHost.stop()
    }
    // #647 D1a: the session's receiver. Healthy: its name and status. Lost:
    // what happened, and Stop now, the user's Stop, which also drops uploads
    // still waiting (#718 D4) while Stop itself waits on the cleanup. It
    // answers for the state it opened in: a change while it is up closes it,
    // so OK never turns into Stop now under a finger.
    ConfirmDialog {
        id: receiverSheet
        objectName: "monitorReceiverSheet"
        z: 10
        property bool lostView: false
        title: receiverStatus.headline
        message: lostView ? qsTr("The session is finishing: its last calls are still being saved and uploaded. Stop now ends it at once and drops any uploads still waiting.") : receiverStatus.hostText || receiverStatus.detailText
        confirmText: lostView ? qsTr("Stop now") : qsTr("OK")
        showCancel: lostView
        destructive: lostView
        function openFor(lost) {
            lostView = lost;
            open();
        }
        onConfirmed: {
            if (lostView)
                decoderHost.stop();
        }
        Connections {
            target: screen
            function onReceiverLostChanged() {
                receiverSheet.cancel();
            }
        }
    }

    // WP-F1: owned by Monitor, above its scrolling list and stop control.
    SiteSheet {
        id: siteSheet
        z: 10
        onNetworkRequested: networkSheet.open()
    }

    // WP-F2: read-only network sheet, opened from SiteSheet.
    NetworkSheet {
        id: networkSheet
        z: 100
    }
}
