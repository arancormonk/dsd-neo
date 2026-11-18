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
// The ages are paced for a screen reader (LiveText); the rest renames only
// when the fix changes. A track cleared while open keeps the last copy read
// and says so.
ModalSheet {
    id: sheet

    // The track shown (0 for a pin), and the last copy read of it.
    property double trackId: 0
    property var track: ({})
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
    // Centred, the panel stops twice the reserve short so its bottom clears it.
    maximumHeight: Math.max(0, Math.min(height, keyboardTop) - 2 * Theme.screenPadding - 2 * bottomReserve)

    readonly property bool metric: typeof prefs !== "undefined" && prefs ? prefs.metricUnits !== false : true
    readonly property var pinPosition: pin && pin.position ? pin.position : ({})

    function open(id) {
        pin = null;
        trackId = id;
        trackGone = false;
        track = positions.trackFor(id);
        visible = true;
    }
    function openPin(record) {
        trackId = 0;
        track = ({});
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
        if (JSON.stringify(fresh) !== JSON.stringify(track))
            track = fresh;
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
    readonly property double newestAge: MapText.newestAge(track)
    readonly property var stateLines: pin ? MapText.stateWords(pinPosition) : MapText.stateWords(track)
    readonly property bool hasFix: pin ? pinPosition.latitude !== undefined : (track.hasPosition === true || track.hasUnknownFix === true)
    // Where the fix shown is: a pin's, else the current position, else the
    // open ring's; 0 until there is one (hasFix says whether there is).
    readonly property double shownLatitude: Number(pin ? pinPosition.latitude : track.hasPosition ? track.latitude : track.unknownLatitude) || 0
    readonly property double shownLongitude: Number(pin ? pinPosition.longitude : track.hasPosition ? track.longitude : track.unknownLongitude) || 0

    function identityLines() {
        var lines = [];
        if (pin) {
            if (pin.sourceName && pin.sourceName !== title)
                lines.push(pin.sourceName);
            if (pin.src > 0)
                lines.push(qsTr("Radio ID %1").arg(Util.idText(pin.src)));
            if (pin.tg > 0)
                lines.push(qsTr("Talkgroup %1").arg(Util.idText(pin.tg)));
            return lines;
        }
        if (track.anonymous)
            return [qsTr("The radio that sent this could not be confirmed, so it has no name or trail.")];
        if (track.label && track.label.length > 0 && track.label !== title)
            lines.push(qsTr("Label %1").arg(track.label));
        if (track.sourceText && track.sourceText.length > 0 && track.sourceText !== title)
            lines.push(qsTr("Callsign %1").arg(track.sourceText));
        if (track.sourceId > 0)
            lines.push(qsTr("Radio ID %1").arg(Util.idText(track.sourceId)));
        if (track.targetId > 0)
            lines.push(track.targetGroup ? qsTr("Talkgroup %1").arg(Util.idText(track.targetId)) : qsTr("Sent to radio %1").arg(Util.idText(track.targetId)));
        return lines;
    }
    function reportLines() {
        var lines = [];
        var kind = pin ? pinPosition.kindText : track.kindText;
        if (kind && kind.length > 0)
            lines.push(qsTr("Report type %1").arg(kind));
        if (hasFix)
            lines.push(qsTr("Position %1").arg(MapText.coordinateText(shownLatitude, shownLongitude)));
        if (hasFix && deviceKnown) {
            var away = MapText.lengthText(MapText.distanceMetres(deviceLatitude, deviceLongitude, shownLatitude, shownLongitude), metric);
            if (away.length > 0)
                lines.push(qsTr("%1 from your saved location").arg(away));
        }
        var accuracy = pin ? MapText.accuracyText(pinPosition.errorM, pinPosition.errorLowerBound, metric) : MapText.accuracyText(track.errorM, track.errorLowerBound, metric);
        if (accuracy.length > 0)
            lines.push(accuracy);
        if (pin)
            return lines;
        var parts = [MapText.speedText(track.speedDkmh, track.speedLowerBound, metric), MapText.headingText(track.headingDdeg, track.headingAmbiguous), MapText.altitudeText(track.altitudeDm, metric)];
        for (var i = 0; i < parts.length; i++) {
            if (parts[i].length > 0)
                lines.push(parts[i]);
        }
        if (track.sats > 0)
            lines.push(qsTr("%1 satellites").arg(track.sats));
        if (track.freqHz > 0)
            lines.push(qsTr("Frequency %1").arg(Util.fmtMhz(track.freqHz)));
        return lines;
    }
    // Every property the two read is a dependency of this binding.
    readonly property var lines: identityLines().concat(reportLines())

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
        model: sheet.stateLines
        ReadableText {
            required property string modelData
            required property int index
            objectName: "mapPositionState" + index
            width: parent.width
            text: modelData
            wrapMode: Text.Wrap
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(15)
            font.weight: Font.DemiBold
            color: modelData === qsTr("Emergency") ? Theme.alert : Theme.textPrimary
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
        text: !sheet.pin && sheet.track.fixTime > 0 ? qsTr("Measured %1").arg(MapText.agoText(sheet.track.reportAgeSeconds, Qt.formatDateTime(new Date(sheet.track.fixTime * 1000), "hh:mm:ss"))) : ""
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
        text: !sheet.pin && sheet.track.validityUnknown && sheet.track.hasPosition ? qsTr("Last valid position received %1").arg(MapText.agoText(sheet.track.ageSeconds, "")) : ""
        wrapMode: Text.Wrap
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(15)
        color: Theme.textPrimary
    }

    Repeater {
        model: sheet.lines
        ReadableText {
            required property string modelData
            width: parent.width
            text: modelData
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
