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
    // The device's position for "N km from you", when known.
    property bool deviceKnown: false
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
            trackGone = true;
            return;
        }
        take(fresh);
    }
    readonly property var ageKeys: ["ageSeconds", "unknownAgeSeconds", "reportAgeSeconds", "shown"]
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
    readonly property double newestAge: MapText.newestAge(Object.assign({}, track, ages))
    // The states in fixed slots: emergency, validity, source, format, check.
    readonly property var stateSlots: {
        var t = pin ? pinPosition : track;
        return [t.emergency ? qsTr("Emergency") : "", t.validityUnknown ? qsTr("Fix may be old or invalid") : "",
                t.anonymous ? qsTr("Radio not confirmed") : "",
                t.provisional ? qsTr("Format not confirmed by a published specification") : "",
                t.weakCheck ? qsTr("Weak error check") : ""];
    }
    readonly property bool hasFix: pin ? pinPosition.latitude !== undefined : (track.hasPosition === true || track.hasUnknownFix === true)
    // Where the fix shown is: a pin's, else the current position, else the
    // open ring's; 0 until there is one (hasFix says whether there is).
    readonly property double shownLatitude: Number(pin ? pinPosition.latitude : track.hasPosition ? track.latitude : track.unknownLatitude) || 0
    readonly property double shownLongitude: Number(pin ? pinPosition.longitude : track.hasPosition ? track.longitude : track.unknownLongitude) || 0

    // The detail lines in fixed slots, "" for one not known: who (label or
    // the row's name, callsign, radio ID, talkgroup or target), then the
    // report (format, position, distance, accuracy, speed, heading, altitude,
    // satellites, frequency).
    function identitySlots() {
        if (pin)
            return [pin.sourceName && pin.sourceName !== title ? pin.sourceName : "", "",
                    pin.src > 0 ? qsTr("Radio ID %1").arg(Util.idText(pin.src)) : "",
                    pin.tg > 0 ? qsTr("Talkgroup %1").arg(Util.idText(pin.tg)) : ""];
        if (track.anonymous)
            return [qsTr("The radio that sent this could not be confirmed, so it has no name or trail."), "", "", ""];
        return [track.label && track.label.length > 0 && track.label !== title ? qsTr("Label %1").arg(track.label) : "",
                track.sourceText && track.sourceText.length > 0 && track.sourceText !== title ? qsTr("Callsign %1").arg(track.sourceText) : "",
                track.sourceId > 0 ? qsTr("Radio ID %1").arg(Util.idText(track.sourceId)) : "",
                track.targetId > 0 ? (track.targetGroup ? qsTr("Talkgroup %1").arg(Util.idText(track.targetId)) : qsTr("Sent to radio %1").arg(Util.idText(track.targetId))) : ""];
    }
    function reportSlots() {
        var kind = pin ? pinPosition.kindText : track.kindText;
        var away = hasFix && deviceKnown ? MapText.lengthText(MapText.distanceMetres(deviceLatitude, deviceLongitude, shownLatitude, shownLongitude), metric) : "";
        var t = pin ? ({}) : track;
        return [kind && kind.length > 0 ? qsTr("Report type %1").arg(kind) : "",
                hasFix ? qsTr("Position %1").arg(MapText.coordinateText(shownLatitude, shownLongitude)) : "",
                away.length > 0 ? qsTr("%1 from your saved location").arg(away) : "",
                pin ? MapText.accuracyText(pinPosition.errorM, pinPosition.errorLowerBound, metric) : MapText.accuracyText(t.errorM, t.errorLowerBound, metric),
                MapText.speedText(t.speedDkmh, t.speedLowerBound, metric), MapText.headingText(t.headingDdeg, t.headingAmbiguous),
                MapText.altitudeText(t.altitudeDm, metric), t.sats > 0 ? qsTr("%1 satellites").arg(t.sats) : "",
                t.freqHz > 0 ? qsTr("Frequency %1").arg(Util.fmtMhz(t.freqHz)) : ""];
    }
    // Every property the two read is a dependency of this binding.
    readonly property var lineSlots: identitySlots().concat(reportSlots())
    readonly property int lineSlotCount: 13

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

    // The states, each a line of words: an emergency in the alert colour and
    // weight (the word is the state, the colour only repeats it).
    Repeater {
        model: 5
        ReadableText {
            required property int index
            objectName: "mapPositionState" + index
            width: parent.width
            visible: text.length > 0
            text: sheet.stateSlots[index]
            wrapMode: Text.Wrap
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(15)
            font.weight: Font.DemiBold
            color: index === 0 ? Theme.alert : Theme.textPrimary
        }
    }

    // When it was heard and when the radio says it was measured: ages move,
    // so these two are read at a screen reader's pace.
    LiveText {
        objectName: "mapPositionReceived"
        width: parent.width
        visible: text.length > 0
        step: String(sheet.trackId) + ":" + (sheet.track.receivedTime || 0)
        text: {
            if (sheet.pin) {
                var at = sheet.pinPosition.receivedTime > 0 ? sheet.pinPosition.receivedTime : sheet.pin.when;
                return at > 0 ? qsTr("Received %1").arg(Qt.formatDateTime(new Date(at * 1000), "yyyy-MM-dd hh:mm:ss")) : "";
            }
            var ago = MapText.agoText(sheet.newestAge, sheet.track.timeText);
            return ago.length > 0 ? qsTr("Received %1").arg(ago) + (sheet.newestAge >= 0 && sheet.track.timeText ? " · " + sheet.track.timeText : "") : "";
        }
        wrapMode: Text.Wrap
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(15)
        color: Theme.textPrimary
    }
    LiveText {
        objectName: "mapPositionReported"
        width: parent.width
        visible: text.length > 0
        step: String(sheet.trackId) + ":" + (sheet.track.fixTime || 0)
        text: !sheet.pin && sheet.track.fixTime > 0 ? qsTr("Measured %1").arg(MapText.agoText(sheet.ages.reportAgeSeconds, Qt.formatDateTime(new Date(sheet.track.fixTime * 1000), "hh:mm:ss"))) : ""
        wrapMode: Text.Wrap
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(15)
        color: Theme.textPrimary
    }
    // A newer report that may be invalid stands apart from the last valid one.
    LiveText {
        objectName: "mapPositionLastValid"
        width: parent.width
        visible: text.length > 0
        step: String(sheet.trackId) + ":" + (sheet.track.receivedTime || 0)
        // A stopped replay's fix has no age: then the line says only that it is kept.
        readonly property string ago: MapText.agoText(sheet.ages.ageSeconds, "")
        text: !sheet.pin && sheet.track.validityUnknown && sheet.track.hasPosition ? (ago.length > 0 ? qsTr("Last valid position received %1").arg(ago) : qsTr("The last valid position is still shown")) : ""
        wrapMode: Text.Wrap
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(15)
        color: Theme.textPrimary
    }

    Repeater {
        model: sheet.lineSlotCount
        ReadableText {
            required property int index
            objectName: "mapPositionLine" + index
            width: parent.width
            visible: text.length > 0
            text: sheet.lineSlots[index] || ""
            wrapMode: Text.Wrap
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(15)
            color: Theme.textPrimary
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

    OutlineButton {
        objectName: "mapPositionFollow"
        width: parent.width
        visible: !sheet.pin
        enabled: !sheet.trackGone
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
        enabled: !sheet.trackGone
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
