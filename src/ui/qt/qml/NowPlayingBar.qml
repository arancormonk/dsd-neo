// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import "Util.js" as Util

// Issue #909: the History playlist, docked at the bottom of both History
// screens: the call's name over "Call 3 of 41 · 0:12 / 0:31" (or what it is
// doing), a 2 px progress line on its top edge, then Previous call,
// Pause/Resume, Next call and ✕. The text is one button, Show in list: the
// call's row scrolled into view and its sheet opened, following what plays.
// Once the playlist ends the bar keeps how it ended until ✕ (which then only
// dismisses it) or the next playlist. It follows Main's `playlistStatus`
// (PlaylistStatus), which holds the playlist and its events: this bar reads
// an event out when its screen shows, nothing covers it and the app is
// active, so a call change is never read over the audio and an event is
// read once whichever History screen shows it.
//
// One height for every state: from its fonts and its width only. Where the
// text would have less than a "Call 88 of 88 · 0:00 / 0:00" beside the four
// controls (a phone), the controls take their own line under the text, ✕
// staying beside it; wider, everything shares one line. The line under the
// name wraps, between its parts ("Call 3 of 41", "part 2 of 3", the time),
// never inside one, on at most two lines: its box is the two lines (or one,
// where every status fits one), whatever it says, so the bar keeps its
// height. A status longer than that is elided on the bar and read whole in
// Show in list's description. Once ended, the result takes the transport's
// room as well, the transport gone.
Rectangle {
    id: bar

    property var status: null
    // Brings the row logged under a uid into view and opens its sheet; false
    // when the list does not show it now (HistoryScreen.showPlaying()).
    property var showRow: null
    // Navigation.announce()'s record of what was read out.
    property string announced: ""
    property int announcements: 0
    // Show in list's answer when the row is not listed, in place of the
    // status until the playlist moves on.
    property string notice: ""

    readonly property bool shown: decoderHost.playbackSupported === true && !!status && status.shown
    // The host's readings are this playlist's (PlaylistStatus lets a playlist
    // go as soon as they are not, so this holds while it plays).
    readonly property bool live: shown && status.live && decoderHost.playbackGeneration === status.generation
    readonly property var end: shown && status.ended ? status.ended : null
    readonly property int playState: live ? decoderHost.playbackState : 0
    readonly property bool interrupted: live && decoderHost.playbackInterrupted === true
    // Opening with nothing wanted (paused while it opens, or Next while
    // paused): the player names why it pauses; it reads paused, with Resume.
    readonly property bool pausedOpening: playState === 1 && String(decoderHost.playbackPausedReason || "").length > 0
    // Wanted: its button pauses (B4: an interruption carries on by itself, so
    // it reads Pause too; R49: so does a start asked for and not yet made, 5,
    // while its position holds).
    readonly property bool wanted: (playState === 1 && !pausedOpening) || playState === 2 || playState === 5 || (playState === 3 && interrupted)
    readonly property real durationMs: live ? Math.max(0, Number(decoderHost.playbackDurationMs) || 0) : 0
    readonly property real positionMs: live ? Math.max(0, durationMs > 0 ? Math.min(decoderHost.playbackPositionMs, durationMs) : decoderHost.playbackPositionMs) : 0
    readonly property string title: live ? decoderHost.playbackTitle : end ? end.title : ""
    readonly property int callIndex: live ? decoderHost.playbackCallIndex : -1
    readonly property int callCount: live ? decoderHost.playbackCallCount : 0
    readonly property bool failedEnd: !!end && (end.reason === "start" || end.reason === "unplayable" || end.reason === "failed")

    visible: shown
    color: Theme.panel
    height: implicitHeight
    implicitHeight: stacked ? 2 * padding + topRow + rowGap + 52 : 2 * padding + Math.max(textBoxHeight, 52)

    // "Call 3 of 41", with "· part 2 of 3" for a call of several parts.
    // Where it is: the call counter, and the part counter of a call in
    // parts, each its own segment so the line can break between them.
    function callText() {
        if (!live || callIndex < 0 || callCount <= 0)
            return "";
        return Util.fill(qsTr("Call %1 of %2"), [Util.countText(callIndex + 1), Util.countText(callCount)]);
    }
    function partText() {
        var parts = decoderHost.playbackPartCount;
        if (callText().length === 0 || parts <= 1)
            return "";
        return Util.fill(qsTr("part %1 of %2"), [decoderHost.playbackPartOrdinal, parts]);
    }
    function placeText() {
        return [callText(), partText()].filter(function (part) {
            return part.length > 0;
        }).join(" · ");
    }
    // The position reads the length once it reaches the end, as the sheet's.
    function timeAt(ms) {
        return durationMs > 0 && ms >= durationMs ? Util.playbackLengthText(durationMs) : Util.playbackTimeText(ms);
    }
    function timeText(ms) {
        return durationMs > 0 ? Util.fill(qsTr("%1 / %2"), [timeAt(ms), Util.playbackLengthText(durationMs)]) : timeAt(ms);
    }
    // What it is doing, when that is more than playing: in place of the time.
    // @p spoken: the words Show in list's description reads, where the bar
    // shows a shorter line (a refused resume).
    function statusText(spoken) {
        if (notice.length > 0)
            return notice;
        if (!live)
            return "";
        if (interrupted)
            return qsTr("Paused for another app's audio");
        if (playState === 3 || pausedOpening) {
            var reason = decoderHost.playbackPausedReason;
            if (reason === "unavailable")
                return spoken === true ? status.unavailableText : qsTr("Paused: another app has the audio.");
            return reason === "noisy" ? qsTr("Paused: headphones unplugged") : qsTr("Paused");
        }
        if (playState === 1)
            return qsTr("Opening…");
        // The player's skip notice: the recordings skipped before this call
        // played, while it is the call playing (the player knows which call
        // followed them, whenever the app looked).
        var skips = Number(decoderHost.playbackSkipNoticeCount) || 0;
        if (skips > 0 && Number(decoderHost.playbackSkipNoticeCall) === callIndex)
            return status.skippedText(skips);
        return "";
    }
    // Parts joined so a wrap falls only between them, after a dot (as the
    // detail sheet's site line wraps).
    function wholeParts(parts) {
        return parts.filter(function (part) {
            return part.length > 0;
        }).map(function (part) {
            return part.replace(/ /g, "\u00a0");
        }).join("\u00a0· ");
    }
    // The line under the name: what it is doing, else where it is.
    readonly property string lineText: {
        if (!live)
            return notice;
        var state = statusText();
        return state.length > 0 ? state : wholeParts([callText(), partText(), timeText(positionMs)]);
    }
    readonly property string resultText: notice.length > 0 ? notice : end ? status.endedText(end) : ""
    // Show in list's description: the call, where it is, and what it is doing.
    function descriptionText() {
        if (end)
            return [end.title, resultText].filter(function (part) {
                return part.length > 0;
            }).join(", ");
        if (!live)
            return "";
        var spoken = durationMs > 0 ? Util.fill(qsTr("%1 of %2"), [timeAt(positionMs), Util.playbackLengthText(durationMs)]) : timeAt(positionMs);
        var doing = statusText(true);
        return [title, placeText(), doing.length > 0 ? doing : spoken].filter(function (part) {
            return part.length > 0;
        }).join(", ");
    }

    function showInList() {
        var key = live ? decoderHost.playbackKey : end ? end.key : "";
        var uid = Number(key);
        if (uid > 0 && typeof showRow === "function" && showRow(uid))
            return;
        notice = qsTr("This call isn't in the list now.");
        Navigation.announce(bar, showButton, notice);
    }
    // What a tap on each control acts on (GatedTapHandler.pressKey): a tap
    // acts on what its press began on, or on nothing once that changed while
    // the press was held. Pause/Resume: whether it would pause, and the
    // playlist (a headset paused it meanwhile: the tap does not resume it).
    // Previous, Next and ✕: the playlist, the action, and whether it has
    // ended, so a press held on one playlist never moves or stops the next one
    // (nor a press on an ended result stop a new one).
    function playKey() {
        return [wanted, status ? status.generation : 0].join("|");
    }
    function controlKey(action) {
        return [status ? status.generation : 0, action, !!end].join("|");
    }
    // Show in list: the playlist and the call it names (a press held while the
    // playlist moves on never opens the next call's row).
    function showKey() {
        return [status ? status.generation : 0, live ? decoderHost.playbackKey : end ? end.key : ""].join("|");
    }
    function closeAction() {
        return live ? "stop" : "dismiss";
    }
    function togglePlay() {
        if (!live)
            return;
        if (wanted)
            decoderHost.pausePlayback();
        else
            decoderHost.resumePlayback();
    }
    function close() {
        if (!status)
            return;
        if (live)
            status.stop();
        else
            status.dismiss();
    }
    // The notice is an answer to one press: the playlist moving on ends it.
    readonly property string noticeScope: [status ? status.generation : 0, callIndex, live ? decoderHost.playbackPartOrdinal : 0, playState, !!end].join("|")
    onNoticeScopeChanged: notice = ""

    // ---- Reading its events out (PlaylistStatus) ----
    readonly property bool canSay: shown && visible && Navigation.allows(showButton) && !!status && status.appActive
    onCanSayChanged: sayLater.restart()
    function sayEvent() {
        if (!status || status.eventHeard || !canSay)
            return;
        var id = status.eventId;
        // A refused resume the sheet over the bar read out already: heard.
        if (status.eventSaidElsewhere) {
            status.heard(id);
            return;
        }
        if (Navigation.announce(bar, showButton, status.eventText)) {
            status.heard(id);
            if (status.eventRefusal)
                Navigation.refusalSaid = Navigation.refusalKey(status.generation, status.eventRefusals);
        }
    }
    Connections {
        target: bar.status
        ignoreUnknownSignals: true
        function onEventIdChanged() {
            sayLater.restart();
        }
    }
    // Once every binding has the new event, and after the host read the
    // platform on a return to the foreground; a Timer, which dies with the
    // bar, rather than Qt.callLater.
    Timer {
        id: sayLater
        interval: 0
        onTriggered: bar.sayEvent()
    }

    // ---- Accessibility: one group, its controls gated as every control is ----
    readonly property bool reachable: Navigation.allows(bar) && visible
    Accessible.role: Accessible.Grouping
    Accessible.name: reachable ? qsTr("Now playing") : ""
    Accessible.ignored: !reachable
    // The position at the screen reader's pace (issue #782); a call change is
    // heard on its next period, never as a step, so nothing is read over the
    // call starting.
    SpeechPacer {
        id: spokenPacer
        shown: bar.reachable
        step: [bar.status ? bar.status.generation : 0, bar.playState, bar.interrupted, !!bar.end, bar.notice].join("|")
        read: function () {
            return bar.descriptionText();
        }
    }

    // ---- Layout ----
    readonly property real padding: 8
    readonly property real sideInset: Theme.screenPadding
    // A glyph's 48 px target sits closer to the edge, its glyph near the inset.
    readonly property real controlInset: 6
    readonly property real controlGap: 4
    readonly property font nameFont: Qt.font({family: Theme.sans, pixelSize: Theme.fontSize(15), weight: Font.DemiBold})
    readonly property font lineFont: Qt.font({family: Theme.sans, pixelSize: Theme.fontSize(13), features: {"tnum": 1}})
    FontMetrics {
        id: nameMetrics
        font: bar.nameFont
    }
    FontMetrics {
        id: lineMetrics
        font: bar.lineFont
    }
    TextMetrics {
        id: lineProbe
        font: bar.lineFont
        text: qsTr("Call 88 of 88 · 0:00 / 0:00")
    }
    // The longest things the line under the name can say, wrapped at its
    // width: its box is as tall as the tallest, never fewer than one line nor
    // more than two.
    readonly property var lineProbes: [qsTr("Paused: another app has the audio."), status ? status.skippedText(88) : "",
        qsTr("Paused for another app's audio"), qsTr("Paused: headphones unplugged"), qsTr("This call isn't in the list now."),
        wholeParts([qsTr("Call %1 of %2").arg("888").arg("888"), qsTr("part %1 of %2").arg("8").arg("8"), qsTr("%1 / %2").arg("00:00").arg("00:00")])]
    // The line's width while it plays (never the wider one an ended bar has),
    // so the box, and the bar, keep one height.
    readonly property real lineWidth: Math.max(0, (stacked ? closeX : previousX) - Theme.gap + 6 - showButton.x - 12)
    readonly property real lineBoxHeight: {
        var tallest = Math.ceil(lineMetrics.height);
        for (var i = 0; i < probes.count; i++) {
            var probe = probes.itemAt(i);
            if (probe)
                tallest = Math.max(tallest, Math.ceil(probe.implicitHeight));
        }
        return Math.min(tallest, 2 * Math.ceil(lineMetrics.height));
    }
    readonly property real textBoxHeight: Math.ceil(nameMetrics.height) + 3 + lineBoxHeight
    readonly property real transportWidth: 48 + controlGap + 52 + controlGap + 48
    readonly property bool stacked: width - sideInset - controlInset - transportWidth - 2 * controlGap - 48 - Theme.gap < Math.ceil(lineProbe.advanceWidth)
    readonly property real topRow: Math.max(textBoxHeight, 48)
    readonly property real rowGap: 2
    // Stacked, the transport line is centred, its targets further apart.
    readonly property real stackedGap: 20
    // Each control's place, from the bar's width alone (never from each other's).
    readonly property real closeX: width - controlInset - 48
    readonly property real previousX: stacked ? (width - transportWidth - 2 * (stackedGap - controlGap)) / 2
                                              : closeX - 2 * controlGap - transportWidth
    readonly property real playX: previousX + 48 + (stacked ? stackedGap : controlGap)
    readonly property real nextX: playX + 52 + (stacked ? stackedGap : controlGap)
    readonly property real transportY: stacked ? padding + topRow + rowGap : (height - 52) / 2
    // The text area: the top row beside ✕ (or the line, beside the transport),
    // and once ended the transport's room too.
    readonly property real textRight: (stacked || end ? closeX : previousX) - Theme.gap + 6
    readonly property real textHeight: end && stacked ? topRow + rowGap + 52 : stacked ? topRow : Math.max(textBoxHeight, 52)

    // The result's type: the name's, unless the longest result would not fit
    // the room in it (a wide bar is one row high).
    readonly property bool resultInNameFont: resultProbe.implicitHeight <= textHeight
    readonly property font resultFont: resultInNameFont ? nameFont : lineFont

    // The top edge: a hairline, and the part's progress over it.
    Rectangle {
        width: parent.width
        height: 1
        color: Theme.divider
    }
    Rectangle {
        objectName: "nowPlayingProgress"
        visible: bar.live && bar.durationMs > 0
        width: Math.round(parent.width * (bar.durationMs > 0 ? Math.min(1, bar.positionMs / bar.durationMs) : 0))
        height: 2
        color: Theme.cyan
    }

    Item {
        id: showButton
        objectName: "nowPlayingShowInList"
        // Issue #902: its reach for the keyboard (PressGate).
        readonly property bool inReach: showButton.Accessible.focusable
        readonly property bool navigationAllowed: Navigation.allows(showButton)

        x: bar.sideInset - 6
        y: bar.padding
        width: Math.max(0, bar.textRight - x)
        height: bar.textHeight

        // Held while it has the focus (see OutlineButton).
        activeFocusOnTab: (navigationAllowed && visible && enabled) || activeFocus
        Accessible.role: Accessible.Button
        Accessible.name: !navigationAllowed || !visible ? "" : qsTr("Show in list")
        Accessible.description: !navigationAllowed || !visible ? "" : spokenPacer.spoken
        Accessible.focusable: navigationAllowed && visible
        Accessible.ignored: !navigationAllowed || !visible
        Accessible.onPressAction: activate()
        function activate() {
            if (!enabled || !Navigation.allows(showButton) || !visible)
                return;
            Navigation.commitComposing(true);
            bar.showInList();
        }
        Keys.onPressed: function (event) {
            if (Navigation.activationPress(event, Window.activeFocusItem))
                activate();
        }
        Keys.onReleased: function (event) {
            Navigation.activationRelease(event);
        }
        FocusFrame {}
        Rectangle {
            anchors.fill: parent
            radius: Theme.radiusButton
            color: showTap.pressed ? Theme.chipSelectedFill : "transparent"
        }

        // Playing: the call's name over where it is, or what it is doing.
        Column {
            visible: !bar.end
            x: 6
            width: parent.width - 12
            anchors.verticalCenter: parent.verticalCenter
            spacing: 3

            Text {
                objectName: "nowPlayingTitle"
                width: parent.width
                height: Math.ceil(nameMetrics.height)
                verticalAlignment: Text.AlignVCenter
                // A received name: drawn as written, on one line.
                text: Util.oneLine(bar.title)
                textFormat: Text.PlainText
                font: bar.nameFont
                color: Theme.textPrimary
                elide: Text.ElideRight
            }
            Text {
                objectName: "nowPlayingLine"
                width: parent.width
                height: bar.lineBoxHeight
                text: bar.lineText
                textFormat: Text.PlainText
                font: bar.lineFont
                color: Theme.textSecondary
                // Between words only, so the line breaks between its parts.
                wrapMode: Text.WordWrap
                maximumLineCount: Math.max(1, Math.floor(bar.lineBoxHeight / lineMetrics.height))
                elide: Text.ElideRight
            }
        }
        // Ended: how, over the text's and the transport's room, in the name's
        // type where the longest result fits there.
        Text {
            objectName: "nowPlayingResult"
            visible: !!bar.end
            x: 6
            width: parent.width - 12
            anchors.verticalCenter: parent.verticalCenter
            text: bar.resultText
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            maximumLineCount: Math.max(1, Math.floor(parent.height / (bar.resultInNameFont ? nameMetrics.height : lineMetrics.height)))
            elide: Text.ElideRight
            font: bar.resultFont
            color: bar.failedEnd && bar.notice.length === 0 ? Theme.alert : Theme.textPrimary
        }

        GatedTapHandler {
            id: showTap
            gesturePolicy: TapHandler.ReleaseWithinBounds
            pressKey: function () {
                return bar.showKey();
            }
            onAllowedTap: showButton.activate()
        }
    }

    // ✕'s place when stacked: after the text area, before the transport.
    Item {
        id: topRowSlot
    }
    MapControlButton {
        id: previousButton
        objectName: "nowPlayingPrevious"
        framed: false
        shape: "previous"
        accessibleName: qsTr("Previous call")
        visible: !bar.end
        enabled: bar.live
        x: bar.previousX
        y: bar.transportY + 2
        pressKey: function () {
            return bar.controlKey("previous");
        }
        onClicked: decoderHost.previousPlayback()
    }
    PlayCircle {
        id: playButton
        objectName: "nowPlayingPlay"
        featured: bar.wanted
        showPause: bar.wanted
        visible: !bar.end
        enabled: bar.live
        accessibleName: bar.wanted ? qsTr("Pause") : qsTr("Resume")
        x: bar.playX
        y: bar.transportY
        pressKey: function () {
            return bar.playKey();
        }
        onClicked: bar.togglePlay()
    }
    MapControlButton {
        id: nextButton
        objectName: "nowPlayingNext"
        framed: false
        shape: "next"
        accessibleName: qsTr("Next call")
        visible: !bar.end
        // Not offered at the last call.
        enabled: bar.live && bar.callIndex >= 0 && bar.callIndex < bar.callCount - 1
        x: bar.nextX
        y: bar.transportY + 2
        pressKey: function () {
            return bar.controlKey("next");
        }
        onClicked: decoderHost.nextPlayback()
    }
    // The keyboard's order is what is seen (Tab, and a screen reader's swipe):
    // stacked, ✕ ends the top row, before the transport under it; on one line
    // it comes last. ✕ sits in the slot that gives it that place in the
    // bar's children, which both orders follow.
    Item {
        id: lastSlot
    }
    IconButton {
        id: closeButton
        parent: bar.stacked ? topRowSlot : lastSlot
        objectName: "nowPlayingClose"
        icon: "close"
        accessibleName: bar.live ? qsTr("Stop playing") : qsTr("Close")
        x: bar.closeX
        // Beside the top row's text; once ended, centred on the result, which
        // then fills the transport's room too.
        y: bar.stacked ? bar.padding + ((bar.end ? bar.textHeight : bar.topRow) - height) / 2 : (bar.height - height) / 2
        pressKey: function () {
            return bar.controlKey(bar.closeAction());
        }
        onClicked: bar.close()
    }

    // The probes the line's box (lineBoxHeight) and the result's type
    // (resultInNameFont) are measured from: laid out, never drawn or read.
    Item {
        opacity: 0
        width: bar.lineWidth
        Accessible.ignored: true
        Text {
            id: resultProbe
            // The result's width: the text area's once ended.
            width: Math.max(0, bar.closeX - Theme.gap + 6 - showButton.x - 12)
            text: bar.status ? bar.status.unavailableText : ""
            textFormat: Text.PlainText
            font: bar.nameFont
            wrapMode: Text.Wrap
            Accessible.ignored: true
        }
        Repeater {
            id: probes
            model: bar.lineProbes
            Text {
                required property string modelData
                width: bar.lineWidth
                text: modelData
                textFormat: Text.PlainText
                font: bar.lineFont
                wrapMode: Text.WordWrap
                Accessible.ignored: true
            }
        }
    }
}
