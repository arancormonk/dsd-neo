// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import "Util.js" as Util
import "MapText.js" as MapText

// Issue #800: what the map knows about one radio's position, opened from its
// marker or its List row; or, for a History row's "Show on map" pin, what the
// row saved and what it did not. History's detail sheet in grammar: plain
// lines, then the actions. Every state the marker draws on its own axis
// (freshness, a source not confirmed, a fix that may be invalid, a format no
// published specification confirms, an emergency) is said here in words, so
// nothing depends on seeing the marker.
//
// It follows its track while open: a new fix, a label learned, ages moving.
// It describes each fix the map draws with that report's own states and
// readings (final review M1): the shown position, then an open ring's report.
// Every line is a fixed slot (issue #782): nothing is built or destroyed
// while it is open, a line comes and goes only by `visible`, and its words
// change in place. The ages move every second on their own (`ages`), read at
// a screen reader's pace (LiveText); the rest renames only when the fix
// changes. A track cleared while open keeps the last copy read and says so.
ModalSheet {
    id: sheet

    // The track shown (0 for a pin), the last copy read of it without its
    // ages, and its ages ({ageSeconds, unknownAgeSeconds, reportAgeSeconds}).
    property double trackId: 0
    property var track: ({})
    property var ages: ({})
    property bool trackGone: false
    // A History row's pin: {name, when, tg, src, sourceName, position: {...}}.
    property var pin: null
    // Whether the map follows this track now, and whether the sheet was opened
    // from the List (it then also offers to show the radio on the map).
    property bool following: false
    property bool fromList: false
    // How much of the screen's bottom the panel leaves clear: the map's
    // attribution, which stays readable above any sheet (spec 4.4).
    property real bottomReserve: 0
    // The device's position for "N km from you", when known, and whether it is
    // the live one (following) rather than the saved one-shot fix, or the
    // live feed's last fix gone stale (no longer "you").
    property bool deviceKnown: false
    property bool deviceLive: false
    property bool deviceLastKnown: false
    property double deviceLatitude: 0
    property double deviceLongitude: 0

    signal followRequested(double trackId)
    signal stopFollowRequested
    signal showOnMapRequested(double trackId)

    objectName: "mapPositionSheet"
    accessibleName: pin ? qsTr("Saved position") : qsTr("Position details")
    // The panel is centred in the room above the reserve.
    bottomInset: bottomReserve

    readonly property bool metric: typeof prefs !== "undefined" && prefs ? prefs.metricUnits !== false : true
    readonly property var pinPosition: pin && pin.position ? pin.position : ({})

    function open(id) {
        pin = null;
        trackId = id;
        trackGone = false;
        take(positions.trackFor(id));
        visible = true;
    }
    function openPin(record) {
        trackId = 0;
        track = ({});
        ages = ({});
        trackGone = false;
        pin = record;
        visible = true;
    }
    function reread() {
        if (!visible || trackId <= 0)
            return;
        var fresh = positions.trackFor(trackId);
        if (fresh.trackId === undefined) {
            // Gone (evicted, cleared): its last fixes stay, but ages read on
            // the model's clock stop with it, so the times are said as times
            // ("Received at 12:04:33"), never as a frozen "just now".
            trackGone = true;
            ages = ({});
            return;
        }
        take(fresh);
    }
    readonly property var ageKeys: ["ageSeconds", "unknownAgeSeconds", "reportAgeSeconds", "shown", "currentShown", "unknownShown"]
    // A copy read: its ages go to `ages`, the rest to `track`, each only when
    // it changed, so a tick of the clock touches the age lines alone.
    function take(fresh) {
        var rest = Object.assign({}, fresh);
        var moved = {};
        for (var key of ageKeys) {
            moved[key] = fresh[key];
            delete rest[key];
        }
        if (JSON.stringify(rest) !== JSON.stringify(track))
            track = rest;
        if (JSON.stringify(moved) !== JSON.stringify(ages))
            ages = moved;
    }
    Connections {
        target: positions
        enabled: sheet.visible && sheet.trackId > 0
        function onTracksChanged() {
            sheet.reread();
        }
        function onAgesChanged() {
            sheet.reread();
        }
    }

    readonly property string title: pin ? (pin.name || pin.sourceName || qsTr("Saved position")) : MapText.trackName(track)
    // Final review M1: the sheet describes the fixes the map draws, each with
    // its own report's states and readings. The first block is the shown
    // position: the filled marker's fix (currentFix), else the open ring's
    // when there is no other, else a History pin's. The second is the open
    // ring's report when the track has both.
    readonly property var current: track.currentFix || ({})
    readonly property var ring: track.unknownFix || ({})
    // What the canvas draws now under the age filter: the words follow the
    // drawn marks (a filtered-out fix is not described as on the map).
    readonly property bool currentDrawn: current.latitude !== undefined && ages.currentShown === true
    readonly property bool ringDrawn: ring.latitude !== undefined && ages.unknownShown === true
    // A track the filter hides altogether (the filter changed while the
    // sheet is open) keeps describing the fixes it has.
    readonly property bool hasCurrent: currentDrawn || (!ringDrawn && current.latitude !== undefined)
    readonly property bool hasRing: ringDrawn || (!currentDrawn && ring.latitude !== undefined)
    // Whether the map draws any mark for the track now.
    readonly property bool drawnNow: currentDrawn || ringDrawn
    readonly property var shownFix: pin ? pinPosition : hasCurrent ? current : ring
    readonly property double shownAge: pin ? -1 : hasCurrent ? (Number(ages.ageSeconds) >= 0 ? ages.ageSeconds : -1) : (Number(ages.unknownAgeSeconds) >= 0 ? ages.unknownAgeSeconds : -1)
    // The track's own states, which belong to no one report: an emergency (the
    // marker's alert colour) and a source that could not be confirmed (the
    // dashed outline).
    readonly property var trackStates: {
        var t = pin ? pinPosition : track;
        return [t.emergency ? qsTr("Emergency") : "", t.anonymous ? qsTr("Radio not confirmed") : ""];
    }

    // Who a report was sent to: each report names its own (review r1), so
    // the one said is the shown report's, never the track's newest. An
    // anonymous report keeps a recipient its checked provenance named.
    function recipientOf(fix) {
        if (!fix || !(fix.targetId > 0))
            return "";
        return fix.targetGroup ? qsTr("Talkgroup %1").arg(Util.idText(fix.targetId)) : qsTr("Sent to radio %1").arg(Util.idText(fix.targetId));
    }
    // Who sent it, in fixed slots, "" for one not known: label or the row's
    // name, callsign, radio ID, and who the shown report was sent to.
    function identitySlots() {
        // A History row's target is its call's, a talkgroup or a radio: the
        // row's own kind says which (gi), else the saved position's flag.
        if (pin)
            return [pin.sourceName && pin.sourceName !== title ? pin.sourceName : "", "",
                    pin.src > 0 ? qsTr("Radio ID %1").arg(Util.idText(pin.src)) : "",
                    Util.targetLine(pin.tg, pin.gi === 0 || pin.gi === 1 ? pin.gi : pinPosition.targetGroup === true ? 0 : -1)];
        if (track.anonymous)
            return [qsTr("The radio that sent this could not be confirmed, so it has no name or trail."), "", "", recipientOf(shownFix)];
        return [track.label && track.label.length > 0 && track.label !== title ? qsTr("Label %1").arg(track.label) : "",
                track.sourceText && track.sourceText.length > 0 && track.sourceText !== title ? qsTr("Callsign %1").arg(track.sourceText) : "",
                track.sourceId > 0 ? qsTr("Radio ID %1").arg(Util.idText(track.sourceId)) : "",
                recipientOf(shownFix)];
    }
    readonly property var identityLines: identitySlots()

    // The radio's name, the one heading.
    ReadableText {
        objectName: "mapPositionTitle"
        heading: true
        width: parent.width
        text: sheet.title
        wrapMode: Text.Wrap
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(20)
        font.weight: Font.DemiBold
        color: sheet.track.emergency || (sheet.pin && sheet.pinPosition.emergency) ? Theme.alert : Theme.textPrimary
    }

    // The track's states: an emergency in the alert colour and weight (the
    // word is the state, the colour only repeats it).
    Repeater {
        model: 2
        ReadableText {
            required property int index
            objectName: "mapTrackState" + index
            width: parent.width
            visible: text.length > 0
            text: sheet.trackStates[index]
            wrapMode: Text.Wrap
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(15)
            font.weight: Font.DemiBold
            color: index === 0 ? Theme.alert : Theme.textPrimary
        }
    }
    Repeater {
        model: 4
        ReadableText {
            required property int index
            objectName: "mapIdentityLine" + index
            width: parent.width
            visible: text.length > 0
            text: sheet.identityLines[index] || ""
            wrapMode: Text.Wrap
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(15)
            color: Theme.textPrimary
        }
    }

    // The shown position, then the open ring's report beside it. Both blocks
    // are always built; the second shows only when the track has both.
    FixDetails {
        objectName: "mapShownFix"
        width: parent.width
        heading: sheet.pin ? qsTr("Saved position") : sheet.hasRing && sheet.hasCurrent ? qsTr("Position on the map") : qsTr("Position")
        fix: sheet.shownFix
        ageSeconds: sheet.shownAge
        reportAgeSeconds: sheet.shownFix.newest ? (Number(sheet.ages.reportAgeSeconds) >= 0 ? sheet.ages.reportAgeSeconds : -1) : -1
        stepKey: String(sheet.trackId) + ":" + (sheet.shownFix.receivedTime || 0)
        receivedWhen: sheet.pin ? (sheet.pinPosition.receivedTime > 0 ? sheet.pinPosition.receivedTime : (sheet.pin.when || 0)) : -1
    }
    FixDetails {
        objectName: "mapRingFix"
        width: parent.width
        visible: !sheet.pin && sheet.hasCurrent && sheet.hasRing
        heading: sheet.ring.newest ? qsTr("Newer report") : qsTr("Earlier report")
        fix: sheet.hasCurrent ? sheet.ring : ({})
        // Its own recipient, when it is not the one the shown report names.
        recipient: sheet.recipientOf(sheet.ring) !== sheet.recipientOf(sheet.shownFix) ? sheet.recipientOf(sheet.ring) : ""
        ageSeconds: Number(sheet.ages.unknownAgeSeconds) >= 0 ? sheet.ages.unknownAgeSeconds : -1
        reportAgeSeconds: sheet.ring.newest ? (Number(sheet.ages.reportAgeSeconds) >= 0 ? sheet.ages.reportAgeSeconds : -1) : -1
        stepKey: String(sheet.trackId) + ":ring:" + (sheet.ring.receivedTime || 0)
        receivedWhen: -1
    }

    // One report: a small section heading, its states, when it was heard and
    // measured (paced: ages move), and its readings, each line a fixed slot.
    component FixDetails: Column {
        id: block

        property string heading: ""
        property var fix: ({})
        property double ageSeconds: -1
        property double reportAgeSeconds: -1
        property string stepKey: ""
        // Who this report was sent to, when the identity lines above do not
        // already say it.
        property string recipient: ""
        // A History pin's receive time (Unix s): said as a date; -1 for a live fix.
        property double receivedWhen: -1
        readonly property bool hasFix: fix.latitude !== undefined
        readonly property var states: MapText.fixWords(fix)
        readonly property string away: hasFix && sheet.deviceKnown ? MapText.lengthText(MapText.distanceMetres(sheet.deviceLatitude, sheet.deviceLongitude, fix.latitude, fix.longitude), sheet.metric) : ""
        readonly property var lines: [fix.kindText && fix.kindText.length > 0 ? qsTr("Report type %1").arg(fix.kindText) : "",
            hasFix ? recipient : "",
            hasFix ? qsTr("Position %1").arg(MapText.coordinateText(fix.latitude, fix.longitude)) : "",
            // Final review M5: the live position while following, else the saved one.
            away.length > 0 ? (sheet.deviceLive ? qsTr("%1 from you").arg(away) : sheet.deviceLastKnown ? qsTr("%1 from your last known position").arg(away)
                : qsTr("%1 from your saved location").arg(away)) : "",
            MapText.accuracyText(fix.errorM, fix.errorLowerBound, sheet.metric),
            MapText.speedText(fix.speedDkmh, fix.speedLowerBound, sheet.metric),
            MapText.headingText(fix.headingDdeg, fix.headingAmbiguous),
            MapText.altitudeText(fix.altitudeDm, sheet.metric),
            // Zero is a reported count (no satellites in view), unlike none.
            fix.sats === undefined ? "" : fix.sats === 1 ? qsTr("1 satellite") : qsTr("%1 satellites").arg(fix.sats),
            fix.freqHz > 0 ? qsTr("Frequency %1").arg(Util.fmtMhz(fix.freqHz)) : ""]

        visible: hasFix
        spacing: 12
        topPadding: 6

        MicroLabel {
            objectName: block.objectName + "Heading"
            width: parent.width
            text: block.heading
        }
        Repeater {
            model: 3
            ReadableText {
                required property int index
                objectName: block.objectName + "State" + index
                width: block.width
                visible: text.length > 0
                text: block.states[index] || ""
                wrapMode: Text.Wrap
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(15)
                font.weight: Font.DemiBold
                color: Theme.textPrimary
            }
        }
        LiveText {
            objectName: block.objectName + "Received"
            width: parent.width
            visible: text.length > 0
            step: block.stepKey
            text: {
                if (block.receivedWhen > 0)
                    return qsTr("Received %1").arg(Qt.formatDateTime(new Date(block.receivedWhen * 1000), "yyyy-MM-dd hh:mm:ss"));
                if (block.receivedWhen === 0 || !block.hasFix)
                    return "";
                // The receive time with its date when not today, as "Measured at".
                var at = MapText.clockText(block.fix.receivedTime, savedSystems.localDay) || block.fix.timeText || "";
                var ago = MapText.agoText(block.ageSeconds, at);
                return ago.length > 0 ? qsTr("Received %1").arg(ago) + (block.ageSeconds >= 0 && at.length > 0 ? " · " + at : "") : "";
            }
            wrapMode: Text.Wrap
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(15)
            color: Theme.textPrimary
        }
        LiveText {
            objectName: block.objectName + "Measured"
            width: parent.width
            visible: text.length > 0
            step: block.stepKey + ":" + (block.fix.fixTime || 0)
            // With its date when not today (MapText.clockText()).
            readonly property string at: block.fix.fixTime > 0 ? MapText.clockText(block.fix.fixTime, savedSystems.localDay) : ""
            // A report with no time of its own (LIP) may still say how old the
            // position was when sent: that bound, not nothing.
            text: at.length === 0 ? MapText.sentAgeText(block.fix.ageAtSendMaxSeconds) : block.reportAgeSeconds >= 0 ? qsTr("Measured %1").arg(MapText.agoText(block.reportAgeSeconds, at)) : qsTr("Measured at %1").arg(at)
            wrapMode: Text.Wrap
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(15)
            color: Theme.textPrimary
        }
        Repeater {
            model: 10
            ReadableText {
                required property int index
                objectName: block.objectName + "Line" + index
                width: block.width
                visible: text.length > 0
                text: block.lines[index] || ""
                wrapMode: Text.Wrap
                font.family: Theme.sans
                font.pixelSize: Theme.fontSize(15)
                color: Theme.textPrimary
            }
        }
    }

    // What a History row keeps, and what the map would show for a live track.
    ReadableText {
        objectName: "mapPinLimits"
        width: parent.width
        visible: sheet.pin !== null
        text: qsTr("Saved with this History row: one position. History does not keep the radio's trail, speed, heading or altitude.")
        wrapMode: Text.Wrap
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(13)
        color: Theme.textSubdued
    }
    ReadableText {
        objectName: "mapPositionGone"
        width: parent.width
        visible: sheet.trackGone
        text: qsTr("This radio is no longer on the map. These were its last details.")
        wrapMode: Text.Wrap
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(13)
        color: Theme.textSubdued
    }
    ReadableText {
        objectName: "mapPositionFiltered"
        width: parent.width
        visible: !sheet.pin && !sheet.trackGone && !sheet.drawnNow
        text: qsTr("The age filter hides this radio on the map now. Choose All to see or follow it there.")
        wrapMode: Text.Wrap
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(13)
        color: Theme.textSubdued
    }

    // An anonymous report is a single point: every report gets its own row,
    // so there is nothing that would move it. It can be shown, not followed.
    OutlineButton {
        objectName: "mapPositionFollow"
        width: parent.width
        visible: !sheet.pin && !sheet.track.anonymous
        // Starting a follow needs a mark on the map (as Show on map); one
        // already running stays stoppable while the filter hides it.
        enabled: !sheet.trackGone && (sheet.following || sheet.drawnNow)
        checkable: true
        checked: sheet.following
        text: sheet.following ? qsTr("Stop following") : qsTr("Follow")
        accessibleName: sheet.following ? qsTr("Stop following %1").arg(sheet.title) : qsTr("Follow %1 on the map").arg(sheet.title)
        onClicked: {
            if (sheet.following)
                sheet.stopFollowRequested();
            else
                sheet.followRequested(sheet.trackId);
            sheet.visible = false;
        }
    }
    OutlineButton {
        objectName: "mapPositionShowOnMap"
        width: parent.width
        visible: !sheet.pin && sheet.fromList
        // Only while the map draws a mark for it: the age filter can hide
        // both while the sheet stays open, and the camera would find nothing.
        enabled: !sheet.trackGone && sheet.drawnNow
        text: qsTr("Show on map")
        onClicked: {
            sheet.showOnMapRequested(sheet.trackId);
            sheet.visible = false;
        }
    }
    OutlineButton {
        objectName: "mapPositionClose"
        width: parent.width
        text: qsTr("Close")
        onClicked: sheet.visible = false
    }
}
