// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import DsdNeo 1.0
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
            channelsSheet.visible = false;
            toneFilterSheet.visible = false;
            receiveFiltersSheet.visible = false;
            serviceNamesSheet.visible = false;
            squelchSheet.visible = false;
            passbandSheet.visible = false;
            scanAvoidSheet.visible = false;
            historyDetail.visible = false;
            otherSlotMenu.visible = false;
            receiverSheet.visible = false;
        }
    }

    // Raised by the header's spectrum button; Main.qml owns the layer.
    signal openSpectrum
    property string scanTargetName: ""
    // Issue #901 r3 M1: set by Main while a start is under way, until it has
    // told monitorView which session to show. The list takes its calls only
    // after, so a start never builds (and then destroys) rows of the session
    // before it.
    property bool callsPending: false
    // Issue #697: the name the session gives the list of the target on air;
    // Main sets it only while the scan has more than one list.
    property string scanListName: ""
    property bool sitesAvailable: false
    signal openSites
    signal openSessionMenu
    // Issue #800: the control the session menu opens from, which takes the
    // focus back when a screen the menu pushed (the map) is left.
    readonly property Item sessionOptions: sessionOptionsButton
    signal openTalkgroups
    // Issue #800: a recent call's "Show on map", with the sheet to return to.
    signal showOnMap(var record, var sheet)
    // The keyboard stop of the call row logged under @p uid, brought into
    // view: where a sheet opened again by uid (Back from the map) returns the
    // focus. The row it was opened from can be gone by then: a covered list
    // holds no rows (issue #782) and builds new ones. Null when not listed.
    function rowFocusFor(uid) {
        var source = callHistory.rowForUid(uid);
        var row = source < 0 ? -1 : monitorView.mapFromSource(callHistory.index(source, 0)).row;
        if (row < 0 || !recentList.model)
            return null;
        // Issue #909: the list's place as the next frame shows it. Below tier
        // 5 it hangs between the header and hero's Column and the call
        // actions' Column, each laid out on its next polish, so a part that
        // changed since the last frame (a scan's timing line) would leave the
        // row contained against the old room, and cut off once the list
        // moves. The list header's Column (which holds both in tier 5) needs
        // no settling: it grows above the rows, not into them.
        Theme.settlePositioners(topGroup);
        Theme.settlePositioners(liveActions);
        recentList.forceLayout();
        recentList.positionViewAtIndex(row, ListView.Contain);
        var item = recentList.itemAtIndex(row);
        return item ? item.node : null;
    }
    // Issue #644: what the channel the operator tuned to can be started as,
    // with its own settings (Main's listenActions), and the ask; Main starts
    // the one there is, or opens the menu of several.
    property var listenActions: []
    signal listenRequested

    // The saved-system map the session was started from (may be null for a
    // network/file quick start).
    property var system: null
    // The standalone saved system the session runs, by UID; empty for a scan
    // list, Explore or a wizard test. The key sheet may write its profile.
    property string sessionSystemUid: ""
    signal systemProfileSaved(string uid)
    // Issue #899 (B7, B11): what the audio-input Squelch sheet saves to, and its saved squelch, from Main.
    property bool squelchSavesToSystem: false
    property string squelchSaveError: ""
    property string squelchLevelSaveError: ""
    property string squelchStoreError: ""
    property string savedSquelch: ""
    property bool savedSquelchCleared: false
    signal squelchSettingRequested(string key, var value)
    signal clearSavedSquelchRequested()
    // A key sheet apply still waiting for the decoder, which outlives the
    // sheet; Main reaches the sheet only through this screen.
    readonly property bool keyApplyActive: keySheet.applyActive
    // After an Activity restart the reattached session has no saved-system map,
    // but the persisted session label still names what is playing — the header
    // should agree with the history rows it sits above. A map without a name (a
    // saved system removed while it plays reads back as an empty map) falls back
    // the same way rather than binding undefined to the header.
    readonly property string sessionName: system && system.name ? system.name : callHistory.sessionLabel.length > 0 ? callHistory.sessionLabel : qsTr("Listening")
    // Issue #697: how many other lists a scan session runs beside its own;
    // Main sets it. The title then says so: "Security + 2 lists".
    property int otherListCount: 0
    function withOtherLists(name) {
        if (screen.otherListCount <= 0)
            return name;
        return screen.otherListCount === 1 ? qsTr("%1 + 1 list").arg(name) : Util.fill(qsTr("%1 + %2 lists"), [name, screen.otherListCount]);
    }
    property string systemName: withOtherLists(sessionName)
    // The title's line is a fixed box (issue #901 B9): a line break in an
    // imported or typed name is drawn as a space (r2 B1), and read as one.
    readonly property string sessionLine: Util.oneLine(sessionName)

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
    // The hero's subline as heard (issue #901 B10): every part the call has,
    // or the line draws, in the order the line draws them, whether or not the
    // line had room for it.
    function heroSublineSpeech() {
        return [heroPrivate || heroPrivateTag.visible ? heroPrivateTag.label : "", heroIdsText.text,
            heroIdentityNote.shown || heroIdentityNote.visible ? heroIdentityNote.text : "",
            heroEmergency || heroEmergencyTag.visible ? heroEmergencyTag.label : "", heroEnc || heroEncTag.visible ? heroEncTag.label : "",
            heroAlgorithm.shown || heroAlgorithm.visible ? heroAlgorithm.text : "",
            heroChannelText.shown || heroChannelText.visible ? heroChannelText.text : ""].filter(function (part) {
            return part.length > 0;
        }).join(", ");
    }

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
    readonly property string otherIdentity: slotIdentity(otherSlot)
    // Slot @p slot's identity line, from that slot's own readings.
    function slotIdentity(slot) {
        if (!metrics || (slot !== 1 && slot !== 2))
            return "";
        var p = "slot" + slot;
        var kind = metrics[p + "CallKind"];
        var name = metrics[p + "CallName"];
        var tg = metrics[p + "TgText"];
        var tgId = metrics[p + "TgId"];
        var tgLabel = metrics[p + "TgLabel"];
        var src = metrics[p + "SrcText"];
        var protocol = metrics[p + "ProtocolLabel"];
        if (Util.callIsPrivate(kind) || Util.callIsAnonymous(tg, tgId, src, protocol))
            return Util.callHeadline(kind, name, tg, tgId, tgLabel, src, protocol);
        if (tgId > 0 && name === tg)
            return Util.talkgroupHeadline(name, tg, tgId);
        if (tg.length === 0)
            return name;
        return name.length > 0 ? name + " · TG " + tg : "TG " + tg;
    }
    // What the strip's node says, from one reading of the lead slot and the
    // other slot's call (r7 K2): stripSlot, stripIdle and the other slot's
    // properties are separate bindings that settle one after another, so a
    // name built from them said "SLOT 2 · idle", the slot still on air, when
    // the leading slot 1 ended and slot 2 took the hero in one publication.
    // Idle, the slot alone (r5 H1); with a call, the slot, its identity and
    // each flag it has, drawn or not (issue #782).
    function stripSpeech() {
        var lead = metrics ? metrics.leadSlot : 0;
        var other = lead === 1 ? 2 : lead === 2 ? 1 : 0;
        if (other === 0 || metrics["slot" + other + "CallState"] !== 2)
            return qsTr("SLOT %1 · idle").arg(lead === 2 ? 1 : 2);
        var p = "slot" + other;
        return [qsTr("SLOT %1").arg(other), Util.oneLine(slotIdentity(other)), Util.callIsPrivate(metrics[p + "CallKind"]) ? otherPrivateTag.label : "", metrics[p + "CallEmergency"] ? otherEmergencyTag.label : "", metrics[p + "CallEnc"] ? otherEncTag.label : ""].filter(function (part) {
            return part.length > 0;
        }).join(", ");
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
    // Issue #644: a hold a frequency entry set is held too; the Hold toggle
    // releases it as it releases any hold.
    readonly property bool scanHeld: metrics ? metrics.scanHold === true || metrics.scanEntryHold === true : false
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
    // The running scan's line: "SCANNING · 3/14 · HOLD · Dispatch". With more
    // than one list (issue #697) the place counts only the targets of the lists
    // that are on, "-" while every list holding the target on air is off and
    // the scan is about to leave it, and the list that names it closes the
    // line.
    function scanningText() {
        var lists = metrics.scanListCount > 1;
        var count = lists ? metrics.scanRotationCount : metrics.scanTargetCount;
        if (count <= 0)
            return "";
        var place = !lists ? metrics.scanTargetOrdinal : metrics.scanRotationOrdinal > 0 ? metrics.scanRotationOrdinal : "-";
        var out = Util.fill(qsTr("SCANNING · %1/%2"), [place, count]) + (screen.scanHeld ? qsTr(" · HOLD") : "");
        if (screen.scanTargetName.length > 0)
            out += " · " + screen.scanTargetName;
        if (lists && screen.scanListName.length > 0)
            out += " · " + screen.scanListName;
        return out;
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
    // Compact view (issue #901, D4) starts the search at 2, so it always has
    // the compact hero, draws no label in any tier (labelCandidate) and has
    // compact rows; the strip, actions and Stop keep their size.
    // Only the size, the fonts, the user's density and which live-strip parts
    // the session shows feed it, never call content, so traffic never moves the
    // screen. A scan's hops between two-slot and single-slot carriers do not
    // move it either: the rotation keeps the slot strip's place
    // (rotationReservesStrip).
    readonly property int tier: {
        var two = 2 * callRowHeight;
        for (var t = compactView ? 2 : 0; t <= 4; ++t) {
            if (callSpaceFor(t) >= two)
                return t;
        }
        return callSpaceFor(4) < callRowHeight ? 5 : 4;
    }
    // Whether tier t draws the RECENT CALLS label: the budget's label part, the
    // label itself, the spacer that stands in for it and where the calls card
    // starts all follow this one rule. The list keeps its accessible name.
    function labelCandidate(t) {
        return t < 4 && !compactView;
    }
    // Wide screens fit every header action beside the title in any tier. Where
    // the host brokers a USB receiver (#647 B16), the actions must also fit the
    // row, measured with the pill at its widest label; else Sites and Spectrum
    // are left to the session menu, as past tier 0, and nothing is stacked.
    readonly property bool headerStacked: headerStackedFor(tier)
    readonly property bool wideHeaderActions: wideHeaderActionsFor(tier)
    function headerStackedFor(t) {
        return t === 0 && width < 500 && headerActionsFit;
    }
    function wideHeaderActionsFor(t) {
        return (t === 0 || width >= 500) && headerActionsFit;
    }
    readonly property bool heroCompact: tier >= 2
    readonly property bool stripInList: tier >= 3
    readonly property bool labelShown: labelCandidate(tier)
    readonly property bool scrollAll: tier >= 5

    // The measured parts of the budget. Each is the height the layout below
    // gives that part, so the budget and the screen cannot disagree.
    readonly property real pillHeight: Math.max(48, 30 * Theme.fontScale)
    // The rows' own height (LineBoxes, issue #901): their fonts' line boxes,
    // at the user's density, whatever their width.
    readonly property real callRowHeight: LineBoxes.callRowHeight(compactView)
    // The widest of each form of age a row shows (Util.shortAge: "now", up
    // to "59m", "23h", "999d", then years up to "99y"), one per line: the
    // rows' time slot, so a ticking age never changes what a row draws.
    readonly property string rowTimeProbe: [qsTr("now"), "59m", "23h", "999d", "99y"].join("\n")
    readonly property real heroMicroHeight: Math.ceil(heroMicroMetrics.height)
    // The hero's subline: its tags, and the line boxes (LineBoxes: Latin, CJK
    // and emoji) of the fonts that draw received or imported text, the ids'
    // and channel's mono 13 and the algorithm's mono 11, so only a script that
    // stands taller than the box is drawn smaller in it (r6 J4: from the base
    // mono face alone, a CJK channel was drawn at 24 px of its 26 under Noto
    // CJK at 2.0). The identity note is app text (no translator is installed),
    // whose sans 13 Latin line the mono box holds; its own box, a CJK line
    // taller than the Latin one, would grow the hero on a desktop host by 1 px
    // at 2.0 for text it never draws.
    readonly property real heroSublineHeight: Math.ceil(Math.max(heroSublineMetrics.height, LineBoxes.lineHeight(Theme.mono, Theme.fontSize(13), Font.Normal), LineBoxes.lineHeight(Theme.mono, Theme.fontSize(11), Font.Normal), heroEncTag.implicitHeight, heroEmergencyTag.implicitHeight, heroPrivateTag.implicitHeight))
    readonly property real heroFullHeight: 2 * 14 + heroMicroHeight + 6 + Math.ceil(Theme.fontSize(40)) + 6 + heroSublineHeight + 6 + Math.max(26, Math.ceil(heroDurationMetrics.height))
    readonly property real heroCompactHeight: 2 * 12 + heroMicroHeight + 4 + Math.ceil(Theme.fontSize(30)) + 4 + heroSublineHeight
    readonly property real stripHeight: {
        var parts = [];
        if (stripPlaceHeld)
            parts.push(otherSlotHost.height);
        if (metrics.scanRotationActive || screen.scanPaused)
            parts.push(scanControls.height);
        if (screen.listenActions.length > 0)
            parts.push(listenControls.height);
        return columnHeight(parts);
    }

    function columnHeight(parts) {
        var total = 0;
        for (var i = 0; i < parts.length; ++i)
            total += parts[i];
        return parts.length > 0 ? total + (parts.length - 1) * Theme.gap : 0;
    }
    function headerBarHeight(t) {
        var lines = headingHeight(t);
        return headerStackedFor(t) ? lines + 8 + pillHeight : Math.max(lines, pillHeight);
    }
    // The header's lines (issue #901 B9/B12) are fixed line boxes, so a scan
    // target, a timing phrase or a roaming state in a script with a taller
    // line (Burmese) never changes the header's height or the tier. The lines
    // that change with traffic take the shared measure (LineBoxes). The
    // title changes only with the session, so its box is the session name's
    // own line in the title font, at least the Latin line it always had
    // (r4 F1: in tier 5, flush with the list's clipped top, a Burmese name
    // lost the top 5 px of its glyphs in the Latin box); a Latin name keeps
    // the header and the tiers as they were. The count's line ("+ N lists")
    // keeps the Latin box. headingHeight(t) works the column out for tier t
    // from fonts and widths, never reading it back, since its make-up follows
    // the tier in force.
    readonly property real titleLineBox: Math.max(titleCountBox, Math.ceil(titleLineProbe.height))
    readonly property real titleCountBox: Math.ceil(titleLatinProbe.height)
    readonly property real headerMetaBox: LineBoxes.lineHeight(Theme.mono, Theme.fontSize(11), Font.Normal)
    // The second-slot strip's identity line (B6); its slot label is in the
    // header lines' face and size.
    readonly property real otherIdentityBox: LineBoxes.lineHeight(Theme.sans, Theme.fontSize(14), Font.DemiBold)
    // The heading's optional lines, kept for the whole of what they stand for
    // (a rotation, a roaming session) so traffic never adds or removes one.
    readonly property bool scanTimingLineShown: metrics.scanTimingVisible || metrics.scanRotationActive
    readonly property bool roamingLineShown: siteRoaming.available && siteRoaming.enabled && sessionSystemUid.length > 0
    function headingHeight(t) {
        var lines = [titleLineBox];
        if (titleTwoLine(t))
            lines.push(titleCountPad(t) + titleCountBox);
        // The scan target line is always there, blank off a scan.
        lines.push(headerMetaBox);
        if (scanTimingLineShown)
            lines.push(headerMetaBox);
        if (roamingLineShown)
            lines.push(headerMetaBox);
        var total = 0;
        for (var h of lines)
            total += h;
        return total + (lines.length - 1) * 3;
    }
    // B12: a scan of several lists is titled "<name> + N lists". Where the
    // title's room in tier t cannot hold the count and a readable part of the
    // name (max(96, 35% of the room), but never more than the whole name, so
    // a short title that fits keeps one line: r2 E1), the count takes a
    // second line of the header's whole width, below the header actions, and
    // the name has the first line to itself, elided. The room is measured
    // with the actions the tier shows and the pill at its widest label, so
    // LIVE turning STOPPING… never changes the form.
    readonly property string titleCountText: withOtherLists("").trim()
    function titleTwoLine(t) {
        if (otherListCount <= 0)
            return false;
        var row = width - 2 * Theme.screenPadding;
        // The heading's own width: its 12 px right margin, stacked or beside
        // the actions.
        var room = row - 12 - (headerStackedFor(t) ? 0 : headerActionsWidthFor(wideHeaderActionsFor(t)));
        var name = Math.min(Math.ceil(titleNameProbe.advanceWidth), Math.max(96, 0.35 * room));
        return room < titleSuffixMetrics.advanceWidth + name;
    }
    // Beside the actions the count line starts below them.
    function titleCountPad(t) {
        return headerStackedFor(t) ? 0 : Math.max(0, pillHeight - titleLineBox);
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
        header.push(labelCandidate(t) ? labelBlock.height : labelSpacer.height);
        return viewport - columnHeight(header);
    }

    // #647 B16: Sites, the session menu, Spectrum and the pill, each as the
    // session would show it, with the pill at the widest label it can show
    // (LIVE, LOST, STARTING…, STOPPING…, or what it says now), so the row never
    // changes make-up as the label changes. Beside the title the row keeps the
    // title's 12 px.
    readonly property real headerActionsWidth: headerActionsWidthFor(true)
    // The same with Sites and Spectrum (@p wide) or without them, as past
    // tier 0 on a phone.
    function headerActionsWidthFor(wide) {
        var widths = [];
        if (wide && sitesAvailable)
            widths.push(siteChooserButton.width);
        widths.push(sessionOptionsButton.width);
        if (wide && metrics.radioInput)
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
        id: titleLatinProbe
        visible: false
        text: "Ag"
        textFormat: Text.PlainText
        font: monitorTitle.font
    }
    // The session name's line in the title font (r4 F1), drawn as the title
    // draws it, one line.
    Text {
        id: titleLineProbe
        visible: false
        text: screen.sessionLine
        maximumLineCount: 1
        textFormat: Text.PlainText
        font: monitorTitle.font
    }
    // The session name's whole width in the title font, for the title's form
    // (titleTwoLine()); never elided, so it follows the name and the font only.
    TextMetrics {
        id: titleNameProbe
        font: monitorTitle.font
        text: screen.sessionLine
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
    // Issue #901: Compact view (AppPrefs ui/compactView), read from the same
    // store, else the app's prefs.
    readonly property bool compactViewSetting: prefsStore && prefsStore.compactView !== undefined ? prefsStore.compactView === true : (typeof prefs !== "undefined" && !!prefs && prefs.compactView === true)
    // The density the Monitor lays out in: the one value the budget, the
    // rows' form and height and the follower's row height all read. It
    // follows the setting while the Monitor is in reach, or hidden (its list
    // then holds no rows). Under a screen or sheet that covers it without
    // hiding it (the session's Settings, where the setting changes) it keeps
    // the density it shows and takes the new one once uncovered (r2 D1): a
    // re-layout of the rows under the cover raised a hide for each row it
    // dropped, each a blocking query of Android's bridge ("Hidden lists").
    readonly property bool densityInReach: Navigation.allows(screen) || !visible
    property bool compactView: false
    Binding {
        target: screen
        property: "compactView"
        value: screen.compactViewSetting
        when: screen.densityInReach
        restoreMode: Binding.RestoreNone
    }
    // Issue #901 (D11): a change of density puts the calls back at their
    // start, the list header in view; the focus stays where it is (a change
    // made under a cover, once it is applied). Deferred so
    // the rows and the tier take their new heights first, by a timer the
    // screen owns, so nothing runs after it closes; the rows are laid out at
    // their new height before the view is placed, or the start it found was
    // the old layout's and the view was left short of the header.
    onCompactViewChanged: listToStart.restart()
    Timer {
        id: listToStart
        interval: 0
        onTriggered: {
            recentList.forceLayout();
            recentList.positionViewAtBeginning();
        }
    }
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
        // Issue #902: its reach for the keyboard (PressGate).
        readonly property bool inReach: readingFlow.Accessible.focusable
        property alias speechStep: readingFlowSpeech.step
        // Issue #902: gated as each single reading is: Qt keeps a static
        // text a stop with SetFocus whatever its flags, so out of reach the
        // group is a plain pane, and one a screen reader's SetFocus gave the
        // keyboard gives it back.
        Accessible.role: !navigationAllowed || !visible ? Accessible.Pane : Accessible.StaticText
        readonly property bool navigationAllowed: Navigation.allows(readingFlow)
        Accessible.ignored: !navigationAllowed || !visible
        Accessible.focusable: navigationAllowed && visible
        Accessible.name: readingFlowSpeech.spoken
        SpeechPacer {
            id: readingFlowSpeech
            group: readingFlow
            shown: !readingFlow.Accessible.ignored
        }
    }
    component ReadingRow: Row {
        id: readingRow
        // Issue #902: its reach for the keyboard (PressGate).
        readonly property bool inReach: readingRow.Accessible.focusable
        property alias speechStep: readingRowSpeech.step
        // Issue #902: gated as each single reading is: Qt keeps a static
        // text a stop with SetFocus whatever its flags, so out of reach the
        // group is a plain pane, and one a screen reader's SetFocus gave the
        // keyboard gives it back.
        Accessible.role: !navigationAllowed || !visible ? Accessible.Pane : Accessible.StaticText
        readonly property bool navigationAllowed: Navigation.allows(readingRow)
        Accessible.ignored: !navigationAllowed || !visible
        Accessible.focusable: navigationAllowed && visible
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
    // decryption state, an ENC LOCKOUT count in magenta, a NO FREQ count in the
    // alert colour (Tier III channels with no frequency), then the site. Only
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
        if (metrics.noFreqCount > 0)
            tokens.push({text: qsTr("NO FREQ %1").arg(metrics.noFreqCount), color: String(Theme.alert)});
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

                // A long list name gives way before "+ 2 lists" does: the count
                // is what tells this session from a scan of the one list.
                TextMetrics {
                    id: titleNameMetrics
                    font: monitorTitle.font
                    text: screen.sessionLine
                    elide: Text.ElideRight
                    elideWidth: Math.max(0, monitorTitle.width - titleSuffixMetrics.advanceWidth)
                }
                TextMetrics {
                    id: titleSuffixMetrics
                    font: monitorTitle.font
                    text: screen.withOtherLists("")
                }
                // B12: the two-line form's name, on a line of its own.
                TextMetrics {
                    id: titleLineMetrics
                    font: monitorTitle.font
                    text: screen.sessionLine
                    elide: Text.ElideRight
                    elideWidth: monitorTitle.width
                }
                ReadableText {
                    id: monitorTitle
                    heading: true
                    readonly property bool twoLine: screen.titleTwoLine(screen.tier)
                    width: parent.width
                    height: screen.titleLineBox
                    verticalAlignment: Text.AlignVCenter
                    objectName: "monitorHeaderTitle"
                    text: twoLine ? titleLineMetrics.elidedText : screen.otherListCount > 0 ? screen.withOtherLists(titleNameMetrics.elidedText) : screen.sessionLine
                    maximumLineCount: 1
                    // Fits its box (r5 H3): a taller script is drawn smaller.
                    fontSizeMode: Text.VerticalFit
                    minimumPixelSize: Math.max(4, Math.floor(font.pixelSize / 3))
                    // The heading is read as one, and whole: the name, then
                    // the count (on the line below it in the two-line form).
                    // Speech has no width, so the name is never the elided
                    // one drawn (r2 C1).
                    Accessible.name: reachable ? screen.withOtherLists(screen.sessionLine) : ""
                    font.family: Theme.sans
                    font.pixelSize: Theme.fontSize(20)
                    font.weight: Font.DemiBold
                    color: Theme.textPrimary
                    elide: Text.ElideRight
                }
                // The count's line in the two-line form: the header's whole
                // width, below the actions when they share the title's line.
                // Read with the heading, so it is no node of its own.
                Item {
                    objectName: "monitorHeaderTitleCountLine"
                    visible: monitorTitle.twoLine
                    width: header.width
                    height: screen.titleCountPad(screen.tier) + screen.titleCountBox

                    Text {
                        objectName: "monitorHeaderTitleCount"
                        fontSizeMode: Text.VerticalFit
                        minimumPixelSize: Math.max(4, Math.floor(font.pixelSize / 3))
                        y: screen.titleCountPad(screen.tier)
                        width: parent.width
                        height: screen.titleCountBox
                        verticalAlignment: Text.AlignVCenter
                        text: screen.titleCountText
                        textFormat: Text.PlainText
                        font: monitorTitle.font
                        color: Theme.textPrimary
                        elide: Text.ElideRight
                    }
                }

                // Paced (issue #782): a scan can hop every 300 ms, and a header named
                // by its live text was read out on every hop. Heard at once when a
                // scan starts or stops or is held or released; otherwise the target
                // it is on is read at most every 5 s.
                LiveText {
                    id: scanTargetLine
                    // Below the header actions (under a two-line title, or at
                    // large text) the header's whole width, as the roaming
                    // line's; beside them the column's (r3 M2: at 320 dp and
                    // 2.0 it kept 119 of 284 px, "SCANNIN…").
                    readonly property bool clearOfActions: header.stacked || y >= headerActions.y + headerActions.height
                    width: clearOfActions ? header.width : parent.width
                    height: screen.headerMetaBox
                    verticalAlignment: Text.AlignVCenter
                    objectName: "scanTargetHeader"
                    step: screen.scanPaused ? "paused" : metrics.scanRotationActive ? ["scan", metrics.scanHold].join("|") : text
                    // A paused scan's line drops its noun, then its count, to
                    // fit; speech has no width, so it hears the whole line
                    // (issue #901 r2 C1).
                    speech: function () {
                        return screen.scanPaused ? Util.oneLine(screen.pausedScanText(Number.POSITIVE_INFINITY)) : scanTargetLine.text;
                    }
                    // One line in its box (r2 B1): a break in a target's or
                    // system's name is a space.
                    text: Util.oneLine(screen.scanPaused ? screen.pausedScanText(width) : screen.scanningText() || (screen.system ? Util.monitorMeta(screen.system) : ""))
                    maximumLineCount: 1
                    // Fits its box (r5 H3): a taller script is drawn smaller.
                    fontSizeMode: Text.VerticalFit
                    minimumPixelSize: Math.max(4, Math.floor(font.pixelSize / 3))
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
                    id: scanTimingRow
                    // As the scan target line (r3 M2: "Followi…").
                    readonly property bool clearOfActions: header.stacked || y >= headerActions.y + headerActions.height
                    width: clearOfActions ? header.width : parent.width
                    height: screen.headerMetaBox
                    verticalAlignment: Text.AlignVCenter
                    objectName: "scanTimingRow"
                    // The countdowns tick; why the scan stays, and its state, are heard at once.
                    // Keyed on the stay itself, not its phrase: the silent-channel skip's
                    // variants of it (#834) come and go within one stay and are heard at the
                    // pace of the countdown. So are the voice gate's own variants of one stay
                    // (Qualify, Voice tail): a sync that starts the qualify window is already
                    // a step of speechStep, and voice ending changes the reason itself.
                    step: [screen.speechStep, metrics.scanTimingVisible, metrics.scanStayReason, metrics.scanDwellState,
                        metrics.scanVisitLive].join("|")
                    // The skip status whose sentence was last said (#834). The line's words are
                    // followed by what a "Quiet check" or "No check: tuner" means once, at the
                    // first rename after the status or its reason changes, not at every rename
                    // a hop makes on a session that cannot check. It cannot go in the
                    // description: Qt's Android bridge sends name and description as one
                    // text on every rename (androidjniaccessibility.cpp,
                    // descriptionForInterface()).
                    property string quietSaid: ""
                    speech: function () {
                        var key = metrics.scanQuietStatus + "/" + metrics.scanQuietReason;
                        if (!metrics.scanQuietText || key === scanTimingRow.quietSaid)
                            return scanTimingRow.text;
                        scanTimingRow.quietSaid = key;
                        return scanTimingRow.text + ". " + metrics.scanQuietText;
                    }
                    // A new rotation says it again. Only that clears the key: a sheet
                    // opened over the Monitor stops the pacer but leaves the line
                    // visible, so the key persists across the sheet and an unchanged
                    // status is not said again once it closes.
                    onVisibleChanged: {
                        if (!visible)
                            quietSaid = "";
                    }
                    // Kept for the whole rotation, blank between stays (#718): the view
                    // publishes and withdraws it with each followed call, and a line
                    // coming and going would move everything below the header.
                    visible: screen.scanTimingLineShown
                    text: {
                        if (!metrics.scanTimingVisible)
                            return "";
                        var out = metrics.scanStayPhrase;
                        if (metrics.scanTimerLive) {
                            out += " " + Util.fill(qsTr("%1s/%2s"), [(metrics.scanTimerRemainingDs / 10).toFixed(1), (metrics.scanTimerSpanMs / 1000).toFixed(1)]);
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
                                ? Util.fill(qsTr("Visit: %1s/%2s"), [(metrics.scanVisitRemainingDs / 10).toFixed(1), (metrics.scanVisitMs / 1000).toFixed(1)])
                                : qsTr("Visit: paused"));
                        }
                        // One line in its box (r2 B1).
                        return Util.oneLine(out);
                    }
                    maximumLineCount: 1
                    // Fits its box (r5 H3): a taller script is drawn smaller.
                    fontSizeMode: Text.VerticalFit
                    minimumPixelSize: Math.max(4, Math.floor(font.pixelSize / 3))
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(11)
                    font.letterSpacing: 0.8
                    color: Theme.textSubdued
                    // On a narrow phone screen, elide long budgets to leave room for controls.
                    elide: Text.ElideRight
                }

                // #786: what GPS roaming is doing, state first and the feature
                // last, so eliding drops the name and never the state; where
                // the line is too narrow for that (measured beside the header
                // actions), the state alone. Kept for the whole of a saved
                // session while roaming is on and blank while it idles, so the
                // header, sized from this column, never changes height. A
                // screen reader hears the sentence, and a change of state is
                // read out once. Switching roaming on or off during the
                // session adds or removes the line by design (the spec shows
                // it while the switch is on): that one change of height, and
                // of tier, is the user's own, never one roaming makes.
                ReadableText {
                    id: roamingHeader
                    objectName: "roamingHeaderLine"
                    readonly property bool problem: siteRoaming.action === SiteRoaming.OpenAppSettings || siteRoaming.action === SiteRoaming.TurnOnLocation || siteRoaming.reason === SiteRoaming.SwitchFailed
                    readonly property bool fullFits: roamingHeaderMetrics.advanceWidth <= width + 0.5
                    // Below the header actions (the title and meta lines are
                    // taller than they are, at any font scale) the line has the
                    // header's whole width, so a pill that widens (STOPPING… in
                    // a switch) never squeezes the state; beside them, the
                    // column's.
                    readonly property bool clearOfActions: header.stacked || y >= headerActions.y + headerActions.height
                    width: clearOfActions ? header.width : parent.width
                    height: screen.headerMetaBox
                    verticalAlignment: Text.AlignVCenter
                    visible: screen.roamingLineShown
                    // One line in its box (r2 B1); the sentence is read whole.
                    text: Util.oneLine(fullFits ? siteRoaming.headerText : siteRoaming.headerShortText)
                    maximumLineCount: 1
                    // Fits its box (r5 H3): a taller script is drawn smaller.
                    fontSizeMode: Text.VerticalFit
                    minimumPixelSize: Math.max(4, Math.floor(font.pixelSize / 3))
                    reachable: navigationAllowed && visible && text.length > 0
                    Accessible.name: reachable ? siteRoaming.statusText : ""
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(11)
                    font.letterSpacing: 0.8
                    color: problem ? Theme.alert : Theme.textSubdued
                    elide: Text.ElideRight
                    TextMetrics {
                        id: roamingHeaderMetrics
                        font: roamingHeader.font
                        text: Util.oneLine(siteRoaming.headerText)
                    }
                    readonly property string step: [siteRoaming.state, siteRoaming.reason, siteRoaming.candidateUid, siteRoaming.destinationUid].join("|")
                    // Once the bindings have settled, and only while it is shown.
                    onStepChanged: Qt.callLater(announceState)
                    function announceState() {
                        if (text.length > 0 && Navigation.presented(roamingHeader))
                            Navigation.announce(roamingSpeech, roamingHeader, siteRoaming.statusText);
                    }
                    QtObject {
                        id: roamingSpeech
                        objectName: "roamingHeaderSpeech"
                        property string announced: ""
                        property int announcements: 0
                    }
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
                    // Issue #902: its reach for the keyboard (PressGate).
                    readonly property bool inReach: Navigation.allows(spectrumPill) && spectrumPill.visible

                    objectName: "openSpectrumButton"
                    // Held while it has the focus (see OutlineButton).
                    activeFocusOnTab: (Navigation.allows(spectrumPill) && visible && enabled) || activeFocus
                    Accessible.role: Accessible.Button
                    Accessible.name: !navigationAllowed || !visible ? "" : qsTr("Spectrum")
                    Accessible.focusable: navigationAllowed && visible
                    readonly property bool navigationAllowed: Navigation.allows(spectrumPill)
                    // Gate first: see OutlineButton.
                    Accessible.ignored: !navigationAllowed || !visible
                    Accessible.onPressAction: activate()
                    // One guarded press for keys and accessibility alike: a key
                    // reaching it under an open sheet, or while it is hidden
                    // with the focus (off a radio, a narrowed header), does
                    // nothing.
                    function activate() {
                        if (enabled && Navigation.allows(spectrumPill) && visible)
                            screen.openSpectrum();
                    }
                    Keys.onPressed: function (event) {
                        if (Navigation.activationPress(event, Window.activeFocusItem))
                            activate();
                    }
                    Keys.onReleased: function (event) {
                        Navigation.activationRelease(event);
                    }
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

                    GatedTapHandler {
                        id: spectrumTap
                        onAllowedTap: screen.openSpectrum()
                    }
                }

                // For a USB receiver session on a brokering host, a button that
                // opens the receiver sheet; otherwise only an indicator.
                Rectangle {
                    id: livePill
                    // Issue #902: its reach for the keyboard (PressGate).
                    readonly property bool inReach: Navigation.allows(livePill) && livePill.visible

                    objectName: "monitorLiveStatus"
                    // Its padding and dot, also read by headerActionsWidth.
                    readonly property int chromeWidth: 24
                    readonly property int dotWidth: 8
                    readonly property bool acts: screen.sessionReceiver.length > 0
                    readonly property bool navigationAllowed: Navigation.allows(livePill)
                    function activate() {
                        if (acts && Navigation.allows(livePill) && livePill.visible)
                            screen.openReceiverSheet();
                    }
                    // Not a tab stop behind a sheet; kept while focused, so a change
                    // under the focus never drops it mid-way.
                    activeFocusOnTab: (acts && navigationAllowed && visible) || activeFocus
                    Accessible.role: !navigationAllowed || !visible ? Accessible.Pane : (acts ? Accessible.Button : Accessible.StaticText)
                    Accessible.name: !navigationAllowed || !visible ? "" : screen.receiverLost ? receiverStatus.accessibleName : liveLabel.text
                    Accessible.focusable: navigationAllowed && visible
                    Accessible.description: !navigationAllowed || !visible ? "" : acts ? qsTr("Shows the receiver") : ""
                    Accessible.ignored: !navigationAllowed || !visible
                    Accessible.onPressAction: activate()
                    Keys.onPressed: function (event) {
                        if (Navigation.activationPress(event, Window.activeFocusItem))
                            activate();
                    }
                    Keys.onReleased: function (event) {
                        Navigation.activationRelease(event);
                    }
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
                            // Off a session, the host's status, which can be a
                            // start failure naming a device or file.
                            textFormat: Text.PlainText
                            font.family: Theme.mono
                            font.pixelSize: Theme.fontSize(11)
                            font.letterSpacing: 1.4
                            color: Theme.textPrimary
                        }
                    }

                    GatedTapHandler {
                        id: liveTap
                        enabled: livePill.acts
                        onAllowedTap: livePill.activate()
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
            // Issue #902: its reach for the keyboard (PressGate).
            readonly property bool inReach: hero.Accessible.focusable
            objectName: "monitorHero"
            width: parent.width
            height: screen.heroCompact ? screen.heroCompactHeight : screen.heroFullHeight
            readonly property int padding: screen.heroCompact ? 12 : 14
            // The call it captions as one item for a screen reader (issue #782):
            // the name the canvas draws, then its subline (ids, tags, algorithm,
            // channel), or what it is waiting for. The duration reads on its
            // own, so the seconds counting do not rename this.
            Accessible.role: !navigationAllowed || !visible ? Accessible.Pane : Accessible.StaticText
            readonly property bool navigationAllowed: Navigation.allows(hero)
            Accessible.ignored: !navigationAllowed || !visible
            // Through a pacer (issue #782): its waiting line follows the lock, which
            // can flap; a call starting or ending is a step and heard at once.
            Accessible.name: heroSpeech.spoken
            Accessible.focusable: navigationAllowed && visible
            SpeechPacer {
                id: heroSpeech
                shown: !hero.Accessible.ignored
                step: screen.speechStep
                read: function () {
                    return [screen.heroSlot === 0 ? heroWaitingText.text : screen.heroHeadline,
                        heroSubline.visible ? screen.heroSublineSpeech() : ""].filter(function (part) {
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
                        // Read by the hero, whose name leads with this headline
                        // while it shows (heroSpeech): a node of its own would read
                        // it twice, and stayed in the tree under every sheet.
                        Accessible.ignored: true

                        onPaint: {
                            var ctx = getContext("2d");
                            ctx.reset();
                            // One line (r2 B1): a break in a name is a space.
                            var label = Util.oneLine(screen.heroHeadline);
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
                        // As tall as its reservation, never its children's
                        // natural heights (r5 H3).
                        height: parent.height

                        // One line of the height reserved for it (issue #901
                        // B10). The ids come first, at min(their width, 96);
                        // then, each only while it still fits, EMERGENCY,
                        // PRIVATE, ENC and the algorithm text, which may be cut
                        // to no less than its "ALG nn" part. The first that does
                        // not fit ends the run, so a lower one is never drawn in
                        // place of a higher one. Then the channel, after every
                        // status drawn, and last the ids grow into what is
                        // left, up to their width. Left to right: PRIVATE, ids, EMERGENCY, ENC,
                        // algorithm, channel. Every word is still in the hero's
                        // accessible text (heroSublineSpeech).
                        width: parent.width
                        visible: screen.heroSlot !== 0
                        spacing: 8

                        readonly property var plan: {
                            var room = width;
                            var gap = spacing;
                            var leads = [];
                            if (heroIdsText.text.length > 0)
                                leads.push({key: "ids", natural: Math.ceil(heroIdsMetrics.advanceWidth)});
                            if (heroIdentityNote.shown)
                                leads.push({key: "note", natural: Math.ceil(heroNoteMetrics.advanceWidth)});
                            var shown = {ids: 0, note: 0, emergency: false, privateCall: false, enc: false, algorithm: 0, channel: 0};
                            var used = 0;
                            var count = 0;
                            for (var lead of leads) {
                                lead.width = Math.min(lead.natural, 96, Math.max(0, room - used - (count > 0 ? gap : 0)));
                                used += (count > 0 ? gap : 0) + lead.width;
                                ++count;
                            }
                            var tags = [{key: "emergency", on: screen.heroEmergency, width: heroEmergencyTag.implicitWidth},
                                        {key: "privateCall", on: screen.heroPrivate, width: heroPrivateTag.implicitWidth},
                                        {key: "enc", on: screen.heroEnc, width: heroEncTag.implicitWidth}];
                            var stopped = false;
                            for (var tag of tags) {
                                if (!tag.on)
                                    continue;
                                if (used + (count > 0 ? gap : 0) + tag.width > room) {
                                    stopped = true;
                                    break;
                                }
                                used += (count > 0 ? gap : 0) + tag.width;
                                ++count;
                                shown[tag.key] = true;
                            }
                            if (!stopped && heroAlgorithm.shown) {
                                var left = room - used - (count > 0 ? gap : 0);
                                var whole = Math.ceil(heroAlgorithmMetrics.advanceWidth);
                                // Cut, it draws its part and the ellipsis.
                                var part = Math.min(whole, Math.ceil(heroAlgorithmPartMetrics.advanceWidth));
                                if (left >= part) {
                                    shown.algorithm = Math.min(whole, left);
                                    used += (count > 0 ? gap : 0) + shown.algorithm;
                                    ++count;
                                } else {
                                    stopped = true;
                                }
                            }
                            // The channel after every status, never in place of
                            // one left out, and before the ids grow: a long name
                            // does not push a short channel off the line.
                            if (!stopped && heroChannelText.shown) {
                                var channelRoom = room - used - (count > 0 ? gap : 0);
                                var channelNatural = Math.ceil(heroChannelMetrics.advanceWidth);
                                if (channelRoom >= Math.min(channelNatural, 48)) {
                                    shown.channel = Math.min(channelNatural, channelRoom);
                                    used += (count > 0 ? gap : 0) + shown.channel;
                                    ++count;
                                }
                            }
                            // Last, what is left: the ids grow to their width.
                            var spare = room - used;
                            for (var grown of leads) {
                                var more = Math.min(grown.natural - grown.width, Math.max(0, spare));
                                grown.width += more;
                                spare -= more;
                                shown[grown.key] = grown.width;
                            }
                            return shown;
                        }
                        TextMetrics {
                            id: heroIdsMetrics
                            font: heroIdsText.font
                            text: heroIdsText.text
                        }
                        TextMetrics {
                            id: heroNoteMetrics
                            font: heroIdentityNote.font
                            text: heroIdentityNote.text
                        }
                        TextMetrics {
                            id: heroAlgorithmMetrics
                            font: heroAlgorithm.font
                            text: heroAlgorithm.text
                        }
                        // "ALG 84" of "ALG 84 · KID 0001", what tells AES from RC4,
                        // with the "…" a cut text ends in: right-eliding fits the
                        // ellipsis inside the width, so "ALG 84" alone came out
                        // "ALG 8…".
                        TextMetrics {
                            id: heroAlgorithmPartMetrics
                            font: heroAlgorithm.font
                            text: heroAlgorithm.text.split(" · ")[0] + "…"
                        }
                        TextMetrics {
                            id: heroChannelMetrics
                            font: heroChannelText.font
                            text: heroChannelText.text
                        }

                        // A private call (#773): the tag leads, then the talker. The
                        // headline already names the other party, so no TG follows.
                        PrivateTag {
                            id: heroPrivateTag
                            objectName: "heroPrivateTag"
                            anchors.verticalCenter: parent.verticalCenter
                            visible: heroSubline.plan.privateCall
                        }

                        // An id that did not decode has no text (the call view leaves it
                        // empty, never "0"), so it is simply left out: an encrypted call
                        // on a conventional channel reports no talkgroup, and the headline
                        // already names the channel.
                        Text {
                            id: heroIdsText
                            objectName: "heroIds"
                            // The subline's reserved height, the line centred and
                            // fitted to it (r5 H3: a Tibetan channel made the Row
                            // 74 px tall in a 34 px reservation).
                            height: screen.heroSublineHeight
                            verticalAlignment: Text.AlignVCenter
                            anchors.verticalCenter: parent.verticalCenter
                            visible: text.length > 0 && heroSubline.plan.ids > 0
                            width: heroSubline.plan.ids
                            elide: Text.ElideRight
                            maximumLineCount: 1
                            // Fits its box (r5 H3): a taller script is drawn smaller.
                            fontSizeMode: Text.VerticalFit
                            minimumPixelSize: Math.max(4, Math.floor(font.pixelSize / 3))
                            // One line, measured and drawn as the same text
                            // (r2 B1): a break in an imported alias is a space,
                            // the words kept for speech.
                            text: {
                                var parts = [];
                                if (!screen.heroPrivate && !screen.heroDuplicate && screen.heroTg.length > 0)
                                    parts.push("TG " + screen.heroTg);
                                if (screen.heroSrc.length > 0)
                                    parts.push("SRC " + screen.heroSrc);
                                return Util.oneLine(parts.join(" · "));
                            }
                            // Target and source text come off the air or from
                            // an imported alias: drawn as written.
                            textFormat: Text.PlainText
                            font.family: Theme.mono
                            font.pixelSize: Theme.fontSize(13)
                            color: Theme.textSecondary
                        }

                        // A TETRA call no party named (#773). TETRA names its parties
                        // in the call setup, so saying the identity did not decode keeps
                        // "TETRA call" from reading as all there is. X2-TDMA and ProVoice
                        // never carry one and get no note. Prose, so the sans face.
                        Text {
                            id: heroIdentityNote
                            fontSizeMode: Text.VerticalFit
                            minimumPixelSize: Math.max(4, Math.floor(font.pixelSize / 3))
                            objectName: "heroIdentityNote"
                            // The subline's reserved height, the line centred and
                            // fitted to it (r5 H3: a Tibetan channel made the Row
                            // 74 px tall in a 34 px reservation).
                            height: screen.heroSublineHeight
                            verticalAlignment: Text.AlignVCenter
                            readonly property bool shown: screen.heroAnonymous && screen.heroProtocol === "TETRA"
                            anchors.verticalCenter: parent.verticalCenter
                            visible: shown && heroSubline.plan.note > 0
                            width: heroSubline.plan.note
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
                            visible: heroSubline.plan.emergency
                        }

                        EncTag {
                            id: heroEncTag
                            objectName: "heroEncTag"
                            anchors.verticalCenter: parent.verticalCenter
                            visible: heroSubline.plan.enc
                        }

                        // The decoded algorithm and key id, when the header said:
                        // AES and RC4 traffic should read differently at a glance.
                        Text {
                            id: heroAlgorithm
                            objectName: "heroAlgorithm"
                            // The subline's reserved height, the line centred and
                            // fitted to it (r5 H3: a Tibetan channel made the Row
                            // 74 px tall in a 34 px reservation).
                            height: screen.heroSublineHeight
                            verticalAlignment: Text.AlignVCenter
                            readonly property bool shown: screen.heroEnc && screen.heroEncText.length > 0
                            anchors.verticalCenter: parent.verticalCenter
                            visible: shown && heroSubline.plan.algorithm > 0
                            width: heroSubline.plan.algorithm
                            elide: Text.ElideRight
                            maximumLineCount: 1
                            // Fits its box (r5 H3): a taller script is drawn smaller.
                            fontSizeMode: Text.VerticalFit
                            minimumPixelSize: Math.max(4, Math.floor(font.pixelSize / 3))
                            text: Util.oneLine(screen.heroEncText)
                            font.family: Theme.mono
                            font.pixelSize: Theme.fontSize(11)
                            color: Theme.textSubdued
                        }

                        // Where the call was heard, when that is not already the
                        // headline above: a listed talkgroup still says which scan
                        // channel it came from, and so does a call headlined by its
                        // other party or its protocol. Last in the row and elided to
                        // what is left of it, so an operator-length name never pushes
                        // the tags off the panel.
                        Text {
                            id: heroChannelText
                            objectName: "heroChannel"
                            // The subline's reserved height, the line centred and
                            // fitted to it (r5 H3: a Tibetan channel made the Row
                            // 74 px tall in a 34 px reservation).
                            height: screen.heroSublineHeight
                            verticalAlignment: Text.AlignVCenter
                            readonly property bool shown: screen.heroChannel.length > 0 && screen.heroChannel !== screen.heroHeadline
                            anchors.verticalCenter: parent.verticalCenter
                            visible: shown && heroSubline.plan.channel > 0
                            width: heroSubline.plan.channel
                            elide: Text.ElideRight
                            maximumLineCount: 1
                            // Fits its box (r5 H3): a taller script is drawn smaller.
                            fontSizeMode: Text.VerticalFit
                            minimumPixelSize: Math.max(4, Math.floor(font.pixelSize / 3))
                            // An imported channel name can hold line separators
                            // (dsd_channel_label_sanitize() keeps them): drawn
                            // and measured on one line (r2 B1).
                            text: "· " + Util.oneLine(screen.heroChannel)
                            // A scan row's typed or imported name.
                            textFormat: Text.PlainText
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
            // One line (issue #901 B6), as high as its tallest line box, tag
            // or button plus a 4-px inset, never under the 56 px it always
            // was. The budget reads this height (stripHeight).
            height: Math.max(56, Math.ceil(Math.max(Theme.minimumTouchSize, screen.otherIdentityBox, screen.headerMetaBox, otherEmergencyTag.implicitHeight, otherPrivateTag.implicitHeight, otherEncTag.implicitHeight)) + 8)
            visible: screen.stripPlaceHeld

            UiPanel {
                id: otherSlotPanel
                anchors.fill: parent
                objectName: "otherSlotPanel"
                visible: screen.stripVisible

                // What the line draws (issue #901 B6), from the room between
                // the content's left edge and Skip and the items' own widths:
                // the identity first, min(its width, 96); then, each only while
                // it still fits, EMERGENCY, the slot label, PRIVATE and ENC.
                // The first that does not fit ends the run, so a lower one is
                // never drawn in place of a higher one. The identity takes what
                // is left and elides. Left to right the line reads label,
                // identity, PRIVATE, EMERGENCY, ENC; what is left out is still
                // in the identity's accessible name. Widths only, never the
                // items' positions, so no binding loops.
                readonly property var plan: {
                    var room = Math.max(0, otherSkip.x - 10 - Theme.cardPadding);
                    var used = Math.min(Math.ceil(otherIdentityMetrics.advanceWidth), 96);
                    var shown = {label: false, emergency: false, privateCall: false, enc: false};
                    var parts = [{key: "emergency", on: screen.otherEmergency, width: otherEmergencyTag.implicitWidth},
                                 {key: "label", on: true, width: otherSlotLabel.implicitWidth},
                                 {key: "privateCall", on: screen.otherPrivate, width: otherPrivateTag.implicitWidth},
                                 {key: "enc", on: screen.otherEnc, width: otherEncTag.implicitWidth}];
                    for (var part of parts) {
                        if (!part.on)
                            continue;
                        if (used + 10 + part.width > room)
                            break;
                        used += 10 + part.width;
                        shown[part.key] = true;
                    }
                    return shown;
                }
                TextMetrics {
                    id: otherIdentityMetrics
                    font: otherIdentityText.font
                    text: otherIdentityText.text
                }

                // The line: the strip's one node (r4 F3), shown with the panel
                // whatever it draws, and as wide as the label, the identity and
                // the tags it reads (r5 H2: a touch on the drawn label found no
                // node). With a call it stops at Skip; idle, Skip and More are
                // hidden and the line is the panel's width, so the idle label
                // has all of it (r6 J2: stopped at Skip's hidden place, "SLOT 2 ·
                // idle" was cut to 102 of its 223 px at 320 dp and 2.0). Idle it
                // reads only the idle state the label draws (r5 H1: an ended
                // call's flags, kept through its hold, are not read); with a
                // call, the slot, the call and its tags, each even where the
                // line has no room to draw it, since the kind of call is not
                // something to drop for a screen reader (issue #782). Built from
                // the readings in one go (stripSpeech()), never from what the
                // children show, so a call ending renames it once (r6 J3: the
                // label changed before the identity hid, and the node was
                // renamed "SLOT 2 · idle, TG 4002", then "SLOT 2 · idle").
                // Unnamed while covered.
                Item {
                    id: otherSlotLine
                    // Issue #902: its reach for the keyboard (PressGate).
                    readonly property bool inReach: otherSlotLine.Accessible.focusable
                    objectName: "otherSlotNode"
                    readonly property bool navigationAllowed: Navigation.allows(otherSlotLine)
                    x: 0
                    width: screen.stripIdle ? parent.width : otherSkip.x
                    height: parent.height
                    // Issue #902: gated as a single reading is (see ReadingFlow):
                    // out of reach the node is a plain pane.
                    Accessible.role: !navigationAllowed || !visible ? Accessible.Pane : Accessible.StaticText
                    Accessible.focusable: navigationAllowed && visible
                    Accessible.ignored: !navigationAllowed || !visible
                    Accessible.name: !navigationAllowed || !visible ? "" : screen.stripSpeech()

                    // Drawn text only, in MicroLabel's look: no node of its own
                    // (r4 F3). It comes and goes with the room the line has, and a
                    // node that did raised a hide or show at each fit change and,
                    // hidden, still renamed with each lead-slot change; the
                    // line's node (otherSlotNode) reads it.
                    Text {
                        id: otherSlotLabel
                        objectName: "otherSlotLabel"
                        // Idle, the label is all the line holds.
                        visible: screen.stripIdle || otherSlotPanel.plan.label
                        anchors.left: parent.left
                        anchors.leftMargin: Theme.cardPadding
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.min(implicitWidth, Math.max(0, parent.width - 2 * Theme.cardPadding))
                        elide: Text.ElideRight
                        text: screen.stripIdle ? qsTr("SLOT %1 · idle").arg(screen.stripSlot) : qsTr("SLOT %1").arg(screen.otherSlot)
                        textFormat: Text.PlainText
                        font.family: Theme.mono
                        font.pixelSize: Theme.fontSize(11)
                        font.letterSpacing: Theme.fontSize(11) * 0.18
                        font.capitalization: Font.AllUppercase
                        color: Theme.textSubdued
                    }

                    // Drawn text: the line's node reads it (r5 H2).
                    Text {
                        id: otherIdentityText
                        objectName: "otherSlotIdentity"
                        visible: !screen.stripIdle
                        anchors.left: otherSlotPanel.plan.label ? otherSlotLabel.right : parent.left
                        anchors.leftMargin: otherSlotPanel.plan.label ? 10 : Theme.cardPadding
                        anchors.right: otherPrivateTag.visible ? otherPrivateTag.left : otherEmergencyTag.visible ? otherEmergencyTag.left : otherEncTag.visible ? otherEncTag.left : parent.right
                        anchors.rightMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        // A fixed line box (issue #901, D10): a name in a taller
                        // script is drawn smaller to fit it (r5 H3).
                        height: screen.otherIdentityBox
                        verticalAlignment: Text.AlignVCenter
                        // One line in its box (r2 B1): a break in a name is a space.
                        text: Util.oneLine(screen.otherIdentity)
                        textFormat: Text.PlainText
                        maximumLineCount: 1
                        // Fits its box (r5 H3): a taller script is drawn smaller.
                        fontSizeMode: Text.VerticalFit
                        minimumPixelSize: Math.max(4, Math.floor(font.pixelSize / 3))
                        font.family: Theme.sans
                        font.pixelSize: Theme.fontSize(14)
                        font.weight: Font.DemiBold
                        color: Theme.textPrimary
                        elide: Text.ElideRight
                    }

                    PrivateTag {
                        id: otherPrivateTag
                        objectName: "otherPrivateTag"
                        visible: !screen.stripIdle && otherSlotPanel.plan.privateCall
                        anchors.right: otherEmergencyTag.visible ? otherEmergencyTag.left : otherEncTag.visible ? otherEncTag.left : parent.right
                        anchors.rightMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                    }

                    EmergencyTag {
                        id: otherEmergencyTag
                        objectName: "otherEmergencyTag"
                        visible: !screen.stripIdle && otherSlotPanel.plan.emergency
                        anchors.right: otherEncTag.visible ? otherEncTag.left : parent.right
                        anchors.rightMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                    }

                    EncTag {
                        id: otherEncTag
                        objectName: "otherEncTag"
                        visible: !screen.stripIdle && otherSlotPanel.plan.enc
                        anchors.right: parent.right
                        anchors.rightMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                    }
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
                    // Unnamed while covered or hidden (r4 F3: a lead-slot change
                    // under the session's Settings renamed it).
                    accessibleName: navigationAllowed && visible ? qsTr("Skip this call on slot %1").arg(screen.otherSlot) : ""
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
                    // Unnamed while covered or hidden, as Skip.
                    accessibleName: navigationAllowed && visible ? qsTr("More actions for slot %1").arg(screen.otherSlot) : ""
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
        // channel") arrive through uiMessage, on the "Signal & site" row's
        // line. While the operator has the scan paused (issue #671) there is
        // nothing on air to hold, avoid or step past, and the one thing to do
        // is resume it.
        Item {
            id: scanControls
            // Named so UI_QT_QML_CALL_LISTS can reach it with findChild().
            objectName: "scanControlsRow"

            width: parent.width
            readonly property int spacing: 10
            visible: metrics.scanRotationActive || screen.scanPaused
            // Hold, Avoid, Prev and Next (issue #666) on the call actions' grid
            // rule: four equal columns when the longest word of any label fits a
            // quarter cell less the 12 px OutlineButton keeps either side, else two
            // rows of two, so no label ever breaks inside a word. One word each
            // keeps the four on one 48 px row on a 320 dp phone at normal text.
            readonly property var labels: [qsTr("Hold"), qsTr("Avoid"), qsTr("Prev"), qsTr("Next")]
            readonly property real widest: {
                // advanceWidth() is a call, which a binding does not track:
                // reading the font re-measures on a scale change.
                var font = scanMetrics.font;
                var widest = 0;
                for (var label of labels)
                    widest = Math.max(widest, scanMetrics.advanceWidth(label));
                return Math.ceil(widest);
            }
            readonly property real longestWord: {
                var font = scanMetrics.font;
                var longest = 0;
                for (var label of labels) {
                    for (var word of label.split(" "))
                        longest = Math.max(longest, scanMetrics.advanceWidth(word));
                }
                return Math.ceil(longest);
            }
            // Sized from the width both of the strip's hosts give it (the screen
            // less its padding), not from the host it sits in: the cells' height
            // is part of the budget that picks the host, and reading the host back
            // closed that loop.
            readonly property real span: screen.width - 2 * Theme.screenPadding
            readonly property int columns: (span - 3 * spacing) / 4 - 24 >= longestWord ? 4 : 2
            // Whole pixels: a cell ending on a fraction loses its right border to
            // the list's clip when the strip is in the list.
            readonly property real cell: Math.floor((span - (columns - 1) * spacing) / columns)
            // What the whole pixels leave over, split either side.
            readonly property real inset: Math.floor((width - columns * cell - (columns - 1) * spacing) / 2)
            // One height for every cell, from the widest label.
            readonly property int labelLines: widest <= cell - 24 ? 1 : 2
            readonly property real cellHeight: Math.max(Theme.minimumTouchSize, Math.ceil(labelLines * scanMetrics.lineSpacing) + 24)
            // A paused scan shows the one Resume.
            readonly property int rows: screen.scanPaused ? 1 : Math.ceil(4 / columns)
            height: rows * cellHeight + (rows - 1) * spacing

            FontMetrics {
                id: scanMetrics
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(15)
                font.weight: Font.DemiBold
            }

            OutlineButton {
                objectName: "scanResumeButton"

                // The columns' width, so the row keeps its edges.
                x: scanControls.inset
                width: scanControls.columns * scanControls.cell + (scanControls.columns - 1) * scanControls.spacing
                height: scanControls.cellHeight
                maximumLineCount: scanControls.labelLines
                wrapMode: Text.WordWrap
                visible: screen.scanPaused
                text: qsTr("Resume scan")
                // Not twice: a -Y resume already waiting on its tune reads paused.
                enabled: decoderHost.running && metrics.scanResumable
                onClicked: commands.resumeScan()
            }

            OutlineButton {
                objectName: "scanHoldButton"

                x: scanControls.inset + (0 % scanControls.columns) * (scanControls.cell + scanControls.spacing)
                y: Math.floor(0 / scanControls.columns) * (scanControls.cellHeight + scanControls.spacing)
                width: scanControls.cell
                height: scanControls.cellHeight
                maximumLineCount: scanControls.labelLines
                wrapMode: Text.WordWrap
                // A toggle with one label, its hold shown as checked like Hold TG's
                // (#718): "Release scan" took two lines on a phone.
                text: qsTr("Hold")
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

                x: scanControls.inset + (1 % scanControls.columns) * (scanControls.cell + scanControls.spacing)
                y: Math.floor(1 / scanControls.columns) * (scanControls.cellHeight + scanControls.spacing)
                width: scanControls.cell
                height: scanControls.cellHeight
                maximumLineCount: scanControls.labelLines
                wrapMode: Text.WordWrap
                text: qsTr("Avoid")
                // The call actions below avoid a talkgroup; this one, the channel.
                accessibleName: qsTr("Avoid this scan channel")
                visible: !screen.scanPaused
                enabled: decoderHost.running
                onClicked: commands.avoidCurrentChannel()
            }

            OutlineButton {
                objectName: "scanPrevButton"

                x: scanControls.inset + (2 % scanControls.columns) * (scanControls.cell + scanControls.spacing)
                y: Math.floor(2 / scanControls.columns) * (scanControls.cellHeight + scanControls.spacing)
                width: scanControls.cell
                height: scanControls.cellHeight
                maximumLineCount: scanControls.labelLines
                wrapMode: Text.WordWrap
                text: qsTr("Prev")
                accessibleName: qsTr("Previous scan channel")
                visible: !screen.scanPaused
                enabled: decoderHost.running
                onClicked: commands.previousChannel()
            }

            OutlineButton {
                objectName: "scanNextButton"

                x: scanControls.inset + (3 % scanControls.columns) * (scanControls.cell + scanControls.spacing)
                y: Math.floor(3 / scanControls.columns) * (scanControls.cellHeight + scanControls.spacing)
                width: scanControls.cell
                height: scanControls.cellHeight
                maximumLineCount: scanControls.labelLines
                wrapMode: Text.WordWrap
                text: qsTr("Next")
                accessibleName: qsTr("Next scan channel")
                visible: !screen.scanPaused
                enabled: decoderHost.running
                onClicked: commands.nextChannel()
            }
        }

        // Issue #644: the channel the operator tuned to, when it is a scan-list
        // row or a saved system, and the way to listen to it with its own
        // settings. The name line goes where the button already says what it
        // starts ("Start “County”"). Sized from its parts' own heights, which
        // a hidden host does not zero (see stripHeight), at the width both of
        // the strip's hosts give it, as scanControls is: its height is part of
        // the budget that picks the host.
        Item {
            id: listenControls
            objectName: "monitorListen"
            width: screen.width - 2 * Theme.screenPadding
            readonly property bool named: screen.listenActions.length === 1 && screen.listenActions[0].key === "saved"
            height: (named ? 0 : listenNames.implicitHeight + 6) + listenButton.implicitHeight
            visible: screen.listenActions.length > 0

            Text {
                id: listenNames
                objectName: "monitorListenNames"
                width: parent.width
                visible: !listenControls.named
                text: Util.listenNames(screen.listenActions)
                textFormat: Text.PlainText
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(12)
                color: Theme.textSecondary
                elide: Text.ElideRight
            }

            OutlineButton {
                id: listenButton
                objectName: "monitorListenButton"
                y: listenControls.named ? 0 : listenNames.implicitHeight + 6
                width: parent.width
                height: implicitHeight
                text: Util.listenText(screen.listenActions)
                accessibleName: Util.listenAccessibleName(screen.listenActions)
                enabled: decoderHost.running
                onClicked: screen.listenRequested()
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
        // Its model is monitorView, given by recentPlace below only while the
        // screen shows (issue #901, B8).
        //
        // The calls there are, which the empty state reads: the model's count
        // while hidden, when the list has no model and counts none, and the
        // list's own while shown. The view's count moves in step with its rows;
        // the model's runs ahead of it on a clear, and the footer resized by it
        // then had the view cancel a row it was still building
        // (DelegateModel::cancel).
        readonly property int callCount: model ? count : monitorView.count
        // No current row. A ListView is a focus scope that hands focus to its
        // current item as rows land, which would pull keyboard focus off the
        // disclosure in its header; Tab moves between rows instead.
        currentIndex: -1
        // Handed a model that holds rows (recentPlace's attach, a session
        // starting with calls kept from earlier ones), the view makes row 0
        // current, and once the session's filter took that row away the
        // next call to land became current and took the keyboard (issue
        // #901 r2 D2). Every attach puts it back to none.
        onModelChanged: currentIndex = -1
        keyNavigationEnabled: false

        // Scrollable by assistive technology, as the body it replaced was.
        Accessible.role: Accessible.Pane
        Accessible.name: !navigationAllowed || !visible ? "" : qsTr("Recent calls")
        Accessible.focusable: false
        readonly property bool navigationAllowed: Navigation.allows(recentList)
        Accessible.ignored: !navigationAllowed || !visible
        function scrollPage(direction) {
            if (!enabled || !visible || !Navigation.allows(recentList))
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
            height: recentList.callCount === 0 ? Math.max(screen.callRowHeight, emptyText.implicitHeight + 24) : 6

            ReadableText {
                id: emptyText
                anchors.centerIn: parent
                width: parent.width - 2 * Theme.cardPadding
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                visible: recentList.callCount === 0
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
            onActivated: historyDetail.open(callHistory.recordFor(model.uid))
            // Inset by the card's border, so a row's divider never paints over it.
            x: 1
            width: ListView.view.width - 2
            compact: screen.compactView
            timeWidthProbe: screen.rowTimeProbe
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
                // not printed, the access code (an analog reception's tone)
                // follows the ids, the channel follows the duration unless it
                // is already the name, and the frequency closes the line, the
                // first thing a narrow phone elides.
                var meta = [];
                if (model.tg > 0)
                    meta.push(Util.targetMeta(model.tg, model.gi));
                if (model.src > 0 || model.srcName)
                    meta.push("SRC " + Util.sourceText(model.src, model.srcName));
                var code = model.analog ? model.receivedTone : model.accessCode;
                if (code.length > 0)
                    meta.push(code);
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
            // A call's meta line says "encrypted" (a notice's does not); the
            // tag and the age are said by the row (r2 C2).
            metaSaysEnc: model.kind !== 1
            emergency: model.emergency
            hasRecording: model.hasRecording
            // Issue #909: the call a History playlist is on.
            playback: Util.playlistMark(decoderHost, model.uid)
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
        rowHeight: screen.callRowHeight
    }

    // Issue #901 (B8): hidden, the list holds no rows ("Hidden lists" in
    // docs/code_map.md, issue #782). Main keeps the Monitor hidden under Home
    // whenever no session runs, with the kept calls in its model, and each row
    // built there was announced to accessibility clients. A screen pushed over
    // the session (its Settings, the map) covers the Monitor without hiding it,
    // so the rows stay. It is hidden only between sessions, and a session shows
    // its calls from the newest, so no place is kept (no keyOf).
    KeepPlace {
        id: recentPlace
        list: recentList
        model: monitorView
        ready: !screen.callsPending
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
                // Issue #901 (D9): the engine's answer to the last command
                // ("Output: Muted", "Output: open failed") takes the summary's
                // line while it lasts, as plain text in cyan; without it a tap
                // that failed inside the engine reads as a button that did
                // nothing. Then the summary returns. The line is a fixed box
                // (DisclosureRow, D10), so neither moves the list header.
                readonly property bool answering: metrics.uiMessage.length > 0
                subtitle: answering ? metrics.uiMessage : screen.detailsSummary.markup
                subtitleReading: true
                subtitleTextFormat: answering ? Text.PlainText : Text.StyledText
                subtitleColor: answering ? Theme.cyan : Theme.textSecondary
                accessibleDescription: screen.detailsSummary.plain
                // The answer is said as it is while it shows. The summary's SNR
                // moves every tick: read at the readings' pace (issue #782).
                Accessible.description: !answering ? detailsSpeech.spoken : !detailsRow.navigationAllowed || !detailsRow.visible ? "" : metrics.uiMessage
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
                // Issue #902: its reach for the keyboard (PressGate).
                readonly property bool inReach: siteRow.Accessible.focusable
                objectName: "siteRow"
                width: parent.width
                height: Math.max(48, siteLabel.implicitHeight + 20)
                visible: metrics.siteLine.length > 0
                opacity: metrics.siteConfirmed ? 1 : 0.55
                // Held while it has the focus (see OutlineButton).
                activeFocusOnTab: (Navigation.allows(siteRow) && visible) || activeFocus
                Accessible.role: Accessible.Button
                Accessible.name: !navigationAllowed || !visible ? "" : metrics.siteLine
                Accessible.focusable: navigationAllowed && visible
                readonly property bool navigationAllowed: Navigation.allows(siteRow)
                Accessible.ignored: !navigationAllowed || !visible
                Accessible.onPressAction: activate()
                function activate() {
                    if (visible && Navigation.allows(siteRow))
                        siteSheet.visible = true;
                }
                Keys.onPressed: function (event) {
                    if (Navigation.activationPress(event, Window.activeFocusItem))
                        activate();
                }
                Keys.onReleased: function (event) {
                    Navigation.activationRelease(event);
                }
                FocusFrame {}
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
                GatedTapHandler {
                    onAllowedTap: siteRow.activate()
                }
            }

            // Discussion #900 F3: the Tier III Channels sheet, while the live
            // channel list holds channels of the user's (set or hidden), or a
            // save is owed for the session's system whatever the live lists
            // hold (its Try saving again is in the sheet), and the NO FREQ
            // reading, which opens the same sheet, is hidden: one entry at a
            // time. Before the site names itself there is no Site row, and
            // with every grant given a frequency no NO FREQ reading: this stays
            // the way to correct a saved frequency, by tap, key or screen reader.
            UiPanel {
                id: channelsRow
                // Issue #902: its reach for the keyboard (PressGate).
                readonly property bool inReach: Navigation.allows(channelsRow) && channelsRow.visible
                objectName: "monitorChannelsRow"
                width: parent.width
                height: Math.max(Theme.minimumTouchSize, channelsLabel.implicitHeight + 20)
                visible: (metrics.dmrUserChannels === true || channelsSheet.saveOwedHere) && metrics.noFreqCount === 0
                // Held while it has the focus (see OutlineButton).
                activeFocusOnTab: (Navigation.allows(channelsRow) && visible) || activeFocus
                Accessible.role: Accessible.Button
                Accessible.name: !navigationAllowed || !visible ? "" : qsTr("Tier III channels")
                Accessible.focusable: navigationAllowed && visible
                readonly property bool navigationAllowed: Navigation.allows(channelsRow)
                Accessible.ignored: !navigationAllowed || !visible
                Accessible.onPressAction: activate()
                function activate() {
                    if (visible && Navigation.allows(channelsRow))
                        channelsSheet.open();
                }
                Keys.onPressed: function (event) {
                    if (Navigation.activationPress(event, Window.activeFocusItem))
                        activate();
                }
                Keys.onReleased: function (event) {
                    Navigation.activationRelease(event);
                }
                FocusFrame {}
                Text {
                    id: channelsLabel
                    objectName: "monitorChannelsText"
                    anchors.centerIn: parent
                    width: parent.width - 24
                    text: qsTr("Tier III channels ›")
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(12)
                    color: Theme.textPrimary
                }
                GatedTapHandler {
                    onAllowedTap: channelsRow.activate()
                }
            }

            UiPanel {
                id: decryptionRow
                // Issue #902: its reach for the keyboard (PressGate).
                readonly property bool inReach: Navigation.allows(decryptionRow) && decryptionRow.visible
                objectName: "openKeySheetButton"
                width: parent.width
                height: Math.max(Theme.minimumTouchSize, decryptionLabel.implicitHeight + 20)
                enabled: decoderHost.running
                // Held while it has the focus (see OutlineButton).
                activeFocusOnTab: (Navigation.allows(decryptionRow) && visible && enabled) || activeFocus
                Accessible.role: Accessible.Button
                Accessible.name: !navigationAllowed || !visible ? "" : qsTr("Decryption · %1").arg(keySheet.effectiveState)
                Accessible.focusable: navigationAllowed && visible
                readonly property bool navigationAllowed: Navigation.allows(decryptionRow)
                Accessible.ignored: !navigationAllowed || !visible
                Accessible.onPressAction: activate()
                function activate() {
                    if (enabled && Navigation.allows(decryptionRow) && decryptionRow.visible)
                        keySheet.open();
                }
                Keys.onPressed: function (event) {
                    if (Navigation.activationPress(event, Window.activeFocusItem))
                        activate();
                }
                Keys.onReleased: function (event) {
                    Navigation.activationRelease(event);
                }
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
                GatedTapHandler {
                    onAllowedTap: decryptionRow.activate()
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
                    objectName: "monitorTunedNames"
                    width: Math.min(implicitWidth, parent.width)
                    visible: text.length > 0
                    text: {
                        if (typeof knownFrequencies === "undefined")
                            return "";
                        var revision = knownFrequencies.revision;
                        return knownFrequencies.tunedNames(metrics.centerFreqHz, metrics.channelBandwidthHz).join(" / ");
                    }
                    // Saved names, typed or imported.
                    textFormat: Text.PlainText
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

            // Discussion #900 F3: the DMR Tier III channels the site granted calls
            // on that have no frequency here, so those calls are not followed. The
            // alert colour: calls are being missed, and a frequency fixes it.
            // Hidden at zero. Unlike the readings around it, it goes somewhere:
            // a tap, a key or a screen reader's press opens the Channels sheet.
            // Its count is read at the readings' pace (issue #782, SpeechPacer).
            // The tap reaches past its line, up to a full touch target.
            Item {
                id: noFreqRow
                // Issue #902: its reach for the keyboard (PressGate).
                readonly property bool inReach: Navigation.allows(noFreqRow) && noFreqRow.visible
                objectName: "noFreqRow"
                readonly property int count: metrics.noFreqCount
                width: noFreqLine.implicitWidth
                height: noFreqLine.implicitHeight
                visible: count > 0
                function activate() {
                    if (visible && Navigation.allows(noFreqRow))
                        channelsSheet.open();
                }
                // Held while it has the focus (see OutlineButton).
                activeFocusOnTab: (Navigation.allows(noFreqRow) && visible) || activeFocus
                Accessible.role: Accessible.Button
                Accessible.name: noFreqSpeech.spoken
                Accessible.focusable: navigationAllowed && visible
                readonly property bool navigationAllowed: Navigation.allows(noFreqRow)
                Accessible.ignored: !navigationAllowed || !visible
                Accessible.onPressAction: activate()
                Keys.onPressed: function (event) {
                    if (Navigation.activationPress(event, Window.activeFocusItem))
                        activate();
                }
                Keys.onReleased: function (event) {
                    Navigation.activationRelease(event);
                }
                FocusFrame {}
                SpeechPacer {
                    id: noFreqSpeech
                    shown: !noFreqRow.Accessible.ignored
                    step: screen.speechStep
                    read: function () {
                        return noFreqRow.count === 1 ? qsTr("1 channel without a frequency")
                            : qsTr("%1 channels without a frequency").arg(noFreqRow.count);
                    }
                }

                Row {
                    id: noFreqLine
                    spacing: 5

                    // Read as the row's name, the count in words.
                    Text {
                        objectName: "noFreqLabel"
                        text: qsTr("NO FREQ")
                        font.family: Theme.mono
                        font.pixelSize: Theme.fontSize(11)
                        color: Theme.textSubdued
                    }

                    Text {
                        objectName: "noFreqValue"

                        text: noFreqRow.count.toString()
                        font.family: Theme.mono
                        font.pixelSize: Theme.fontSize(11)
                        color: Theme.alert
                    }

                    Caret {
                        anchors.verticalCenter: parent.verticalCenter
                        rotation: -90
                    }
                }

                GatedTapHandler {
                    margin: Math.max(0, (Theme.minimumTouchSize - noFreqRow.height) / 2)
                    onAllowedTap: noFreqRow.activate()
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
            // avoided for the session, with the list of them and the way to put
            // them back. Shown while there is something to show or clear (issue
            // #672): avoids a stopped -Y left behind, which the -T control-channel
            // hunts still skip, and a paused scan's, but not a 0 while a rotation
            // runs with none. A count the list cannot vouch for reads "?". The
            // [avoided] marker is the trunk-scan fallback: every alternate failed
            // to retune and the receiver stayed on a target that was avoided. A
            // Flow, as the squelch row is, so the buttons move under the reading
            // at large text.
            ReadingFlow {
                id: scanAvoidRow
                // Named so UI_QT_QML_CALL_LISTS can reach it with findChild().
                objectName: "scanAvoidRow"
                speechStep: screen.speechStep

                width: parent.width
                spacing: 8
                visible: metrics.scanAvoidsManageable === true
                    && (metrics.scanAvoidCount > 0 || metrics.scanAvoidsTotalKnown !== true)

                Text {
                    height: scanAvoidShowButton.height
                    verticalAlignment: Text.AlignVCenter
                    text: metrics.scanTargetAvoided ? qsTr("SCAN AVOIDS [avoided]") : qsTr("SCAN AVOIDS")
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(11)
                    color: Theme.textSubdued
                }

                Text {
                    objectName: "scanAvoidValue"

                    height: scanAvoidShowButton.height
                    verticalAlignment: Text.AlignVCenter
                    text: metrics.scanAvoidsTotalKnown === true ? metrics.scanAvoidCount.toString() : "?"
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(11)
                    color: Theme.cyan
                }

                OutlineButton {
                    id: scanAvoidShowButton
                    objectName: "scanAvoidShowButton"

                    text: qsTr("Show")
                    accessibleName: qsTr("Show avoided scan channels")
                    enabled: decoderHost.running
                    onClicked: scanAvoidSheet.open()
                }

                // The default touch height, and the width of its label at any
                // text scale, so it neither falls under 48 px nor breaks "Clear"
                // in two. Named for what it clears, like the scan buttons.
                OutlineButton {
                    objectName: "scanAvoidClearButton"

                    text: qsTr("Clear")
                    accessibleName: qsTr("Clear scan avoids")
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
        onShowOnMapRequested: function (record) {
            screen.showOnMap(record, historyDetail);
        }
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
        savesToSystem: screen.squelchSavesToSystem
        saveError: screen.squelchSaveError
        squelchSaveError: screen.squelchLevelSaveError
        squelchStoreError: screen.squelchStoreError
        savedSquelch: screen.savedSquelch
        savedSquelchCleared: screen.savedSquelchCleared
        onSettingRequested: function (key, value) { screen.squelchSettingRequested(key, value); }
        onClearSavedSquelchRequested: screen.clearSavedSquelchRequested()
    }

    // #621: the passband a rigctl peer is asked for on audio input, over the monitor like the squelch editor.
    PassbandSheet {
        id: passbandSheet
        z: 10
    }

    // #672: the avoided channels with Put back, over the monitor like the squelch editor.
    ScanAvoidSheet {
        id: scanAvoidSheet
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
                id: holdTalkgroupButton
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
                // The state the press asked for (issue #782), not the engine's: a second press before the engine's
                // state arrives is read as a release, so it sends one.
                onClicked: commands.holdTalkgroup(holdTalkgroupButton.shownChecked ? screen.heroTgId : 0)
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
            // Issue #902: its reach for the keyboard (PressGate).
            readonly property bool inReach: Navigation.allows(lostLine) && lostLine.visible
            objectName: "monitorReceiverLostLine"
            visible: lostLineHost.shown
            enabled: !keySheet.visible
            readonly property bool navigationAllowed: Navigation.allows(lostLine)
            function activate() {
                if (visible && enabled && Navigation.allows(lostLine))
                    screen.openReceiverSheet();
            }
            activeFocusOnTab: (lostLineHost.shown && navigationAllowed && visible) || activeFocus
            Accessible.role: Accessible.Button
            Accessible.name: !navigationAllowed || !visible ? "" : receiverStatus.accessibleName
            Accessible.focusable: navigationAllowed && visible
            Accessible.description: !navigationAllowed || !visible ? "" : qsTr("The session is finishing")
            Accessible.ignored: !navigationAllowed || !visible
            Accessible.onPressAction: activate()
            Keys.onPressed: function (event) {
                if (Navigation.activationPress(event, Window.activeFocusItem))
                    activate();
            }
            Keys.onReleased: function (event) {
                Navigation.activationRelease(event);
            }
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

            GatedTapHandler {
                id: lostLineTap
                onAllowedTap: lostLine.activate()
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
        // A GPS roaming switch's Stopping (#786) keeps Stop listening: its
        // uploads can take minutes, and the user's Stop then ends the session
        // for good instead of moving it to the next site.
        readonly property bool switchStopping: decoderHost.switchStopping === true
        text: decoderHost.sessionState === 3 && !switchStopping ? qsTr("Stopping…") : screen.system && screen.system.sourceType === "file" ? qsTr("Stop replay") : qsTr("Stop listening")
        enabled: (!decoderHost.transitioning || switchStopping) && !keySheet.visible
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
        onChannelsRequested: channelsSheet.open()
    }

    // WP-F2: read-only network sheet, opened from SiteSheet.
    NetworkSheet {
        id: networkSheet
        z: 100
    }

    // Discussion #900 F3: the DMR Tier III channels, opened from SiteSheet and
    // the NO FREQ reading.
    ChannelsSheet {
        id: channelsSheet
        z: 100
    }
}
