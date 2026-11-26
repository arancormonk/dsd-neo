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
// staying beside it; wider, everything shares one line. Where even that line
// is too narrow for "Call 88 of 88 · part 8 of 8 · 0:00 / 0:00" (a phone at a
// large text size), the time or the status takes a third line, so it is
// never the part the elide takes.
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
    // Wanted: its button pauses (B4: an interruption carries on by itself, so
    // it reads Pause too).
    readonly property bool wanted: playState === 1 || playState === 2 || (playState === 3 && interrupted)
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
    function placeText() {
        if (!live || callIndex < 0 || callCount <= 0)
            return "";
        var parts = decoderHost.playbackPartCount;
        if (parts > 1)
            return Util.fill(qsTr("Call %1 of %2 · part %3 of %4"), [Util.countText(callIndex + 1), Util.countText(callCount), decoderHost.playbackPartOrdinal, parts]);
        return Util.fill(qsTr("Call %1 of %2"), [Util.countText(callIndex + 1), Util.countText(callCount)]);
    }
    // The position reads the length once it reaches the end, as the sheet's.
    function timeAt(ms) {
        return durationMs > 0 && ms >= durationMs ? Util.playbackLengthText(durationMs) : Util.playbackTimeText(ms);
    }
    function timeText(ms) {
        return durationMs > 0 ? Util.fill(qsTr("%1 / %2"), [timeAt(ms), Util.playbackLengthText(durationMs)]) : timeAt(ms);
    }
    // What it is doing, when that is more than playing: in place of the time.
    function statusText() {
        if (notice.length > 0)
            return notice;
        if (!live)
            return "";
        if (interrupted)
            return qsTr("Paused for another app's audio");
        if (playState === 1)
            return qsTr("Opening…");
        if (playState === 3) {
            var reason = decoderHost.playbackPausedReason;
            return reason === "noisy" ? qsTr("Paused: headphones unplugged") : reason === "unavailable" ? qsTr("Can't play right now.") : qsTr("Paused");
        }
        if (status.skipCount > 0 && status.skipCall === callIndex)
            return status.skippedText(status.skipCount);
        return "";
    }
    // The line under the name, and the third line where there is one.
    readonly property string lineText: {
        if (!live)
            return notice;
        var state = statusText();
        var place = placeText();
        if (threeLines)
            return place.length > 0 ? place : state;
        if (state.length > 0)
            return state;
        return place.length > 0 ? place + " · " + timeText(positionMs) : timeText(positionMs);
    }
    readonly property string thirdLineText: {
        if (!threeLines || placeText().length === 0)
            return "";
        var state = statusText();
        return state.length > 0 ? state : timeText(positionMs);
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
        return [title, placeText(), statusText().length > 0 ? statusText() : spoken].filter(function (part) {
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
        if (Navigation.announce(bar, showButton, status.eventText))
            status.heard(id);
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
    TextMetrics {
        id: partsProbe
        font: bar.lineFont
        text: qsTr("Call 88 of 88 · part 8 of 8 · 0:00 / 0:00")
    }
    readonly property real stackedTextWidth: width - sideInset - controlInset - 48 - Theme.gap
    readonly property bool threeLines: stacked && stackedTextWidth < Math.ceil(partsProbe.advanceWidth)
    readonly property real textBoxHeight: Math.ceil(nameMetrics.height) + (threeLines ? 2 : 1) * (3 + Math.ceil(lineMetrics.height))
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
        y: bar.padding + (bar.stacked ? 0 : (bar.height - 2 * bar.padding - height) / 2)
        width: Math.max(0, (bar.stacked ? bar.closeX : bar.previousX) - Theme.gap + 6 - x)
        height: bar.stacked ? bar.topRow : Math.max(bar.textBoxHeight, 48)

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
                height: Math.ceil(lineMetrics.height)
                verticalAlignment: Text.AlignVCenter
                text: bar.lineText
                textFormat: Text.PlainText
                font: bar.lineFont
                color: Theme.textSecondary
                elide: Text.ElideRight
            }
            Text {
                objectName: "nowPlayingThirdLine"
                visible: bar.threeLines
                width: parent.width
                height: Math.ceil(lineMetrics.height)
                verticalAlignment: Text.AlignVCenter
                text: bar.thirdLineText
                textFormat: Text.PlainText
                font: bar.lineFont
                color: Theme.textSecondary
                elide: Text.ElideRight
            }
        }
        // Ended: how, in the name's type, on as many lines as the same box holds.
        Text {
            objectName: "nowPlayingResult"
            visible: !!bar.end
            x: 6
            width: parent.width - 12
            anchors.verticalCenter: parent.verticalCenter
            text: bar.resultText
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            maximumLineCount: Math.max(1, Math.floor(bar.textBoxHeight / nameMetrics.height))
            elide: Text.ElideRight
            font: bar.nameFont
            color: bar.failedEnd && bar.notice.length === 0 ? Theme.alert : Theme.textPrimary
        }

        GatedTapHandler {
            id: showTap
            gesturePolicy: TapHandler.ReleaseWithinBounds
            onAllowedTap: showButton.activate()
        }
    }

    MapControlButton {
        id: previousButton
        objectName: "nowPlayingPrevious"
        framed: false
        shape: "previous"
        accessibleName: qsTr("Previous call")
        enabled: bar.live
        x: bar.previousX
        y: bar.transportY + 2
        onClicked: decoderHost.previousPlayback()
    }
    PlayCircle {
        id: playButton
        objectName: "nowPlayingPlay"
        featured: bar.wanted
        showPause: bar.wanted
        enabled: bar.live
        accessibleName: bar.wanted ? qsTr("Pause") : qsTr("Resume")
        x: bar.playX
        y: bar.transportY
        onClicked: bar.togglePlay()
    }
    MapControlButton {
        id: nextButton
        objectName: "nowPlayingNext"
        framed: false
        shape: "next"
        accessibleName: qsTr("Next call")
        // Not offered at the last call.
        enabled: bar.live && bar.callIndex >= 0 && bar.callIndex < bar.callCount - 1
        x: bar.nextX
        y: bar.transportY + 2
        onClicked: decoderHost.nextPlayback()
    }
    IconButton {
        id: closeButton
        objectName: "nowPlayingClose"
        icon: "close"
        accessibleName: bar.live ? qsTr("Stop playing") : qsTr("Close")
        x: bar.closeX
        y: bar.stacked ? bar.padding + (bar.topRow - height) / 2 : (bar.height - height) / 2
        onClicked: bar.close()
    }
}
