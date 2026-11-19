// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

// Issue #800: the words the map screen, its List and its sheets say a position
// in. One place for them, so the List row and the detail sheet never describe
// the same fix two ways. Ages come from the model (PositionModel), which reads
// them on the clock each fix was stamped on; nothing here reads a clock.

.pragma library

// "now", "45 s", "12 min", "2 h 5 min", "3 d": how long ago, for a row's end.
// Empty when not known (-1, or a stopped replay's fix).
function ageText(seconds) {
    if (typeof seconds !== "number" || !isFinite(seconds) || seconds < 0)
        return ""
    var s = Math.floor(seconds)
    if (s < 5)
        return qsTr("now")
    if (s < 60)
        return qsTr("%1 s").arg(s)
    if (s < 3600)
        return qsTr("%1 min").arg(Math.floor(s / 60))
    if (s < 86400) {
        var h = Math.floor(s / 3600)
        var m = Math.floor((s % 3600) / 60)
        return m > 0 ? qsTr("%1 h %2 min").arg(h).arg(m) : qsTr("%1 h").arg(h)
    }
    return qsTr("%1 d").arg(Math.floor(s / 86400))
}

// "45 s ago", "just now"; a known time without an age ("at 12:04:33"), else "".
function agoText(seconds, timeText) {
    var age = ageText(seconds)
    if (age === qsTr("now"))
        return qsTr("just now")
    if (age.length > 0)
        return qsTr("%1 ago").arg(age)
    return timeText && timeText.length > 0 ? qsTr("at %1").arg(timeText) : ""
}

// The name a track goes by: its label, else its callsign, else its radio ID,
// exactly as the map's labels name it. An anonymous track has no name it can be
// trusted with.
function trackName(track) {
    if (!track || track.gap)
        return ""
    if (track.anonymous)
        return qsTr("Unconfirmed radio")
    if (track.label && track.label.length > 0)
        return track.label
    if (track.sourceText && track.sourceText.length > 0)
        return track.sourceText
    // The bare number, as the map's label shows it, so the two can be matched.
    if (track.sourceId !== undefined && track.sourceId > 0)
        return String(Math.round(track.sourceId))
    return qsTr("Unnamed radio")
}

// The states a fix is in, in the words the detail sheet and the List use.
// Each is also a visual variable on the map; here none depends on seeing it.
function stateWords(track) {
    var words = []
    if (!track || track.gap)
        return words
    if (track.emergency)
        words.push(qsTr("Emergency"))
    if (track.validityUnknown)
        words.push(qsTr("Fix may be old or invalid"))
    if (track.anonymous)
        words.push(qsTr("Radio not confirmed"))
    if (track.provisional)
        words.push(qsTr("Format not confirmed by a published specification"))
    if (track.weakCheck)
        words.push(qsTr("Weak error check"))
    return words
}

// The states of one report as its own mark draws them (final review M1): a
// fix that may be invalid (the open ring), a format no published
// specification confirms (the double outline), a weak error check.
function fixWords(fix) {
    var words = []
    if (!fix)
        return words
    if (fix.validityUnknown)
        words.push(qsTr("Fix may be old or invalid"))
    if (fix.provisional)
        words.push(qsTr("Format not confirmed by a published specification"))
    if (fix.weakCheck)
        words.push(qsTr("Weak error check"))
    return words
}

// What the List says of a track's open ring beside its current position:
// that it is there and may be invalid, and its own format when unconfirmed
// (its double outline), in one line.
function ringWords(ring) {
    if (!ring || ring.latitude === undefined)
        return ""
    var text = ring.newest ? qsTr("Newer report may be old or invalid") : qsTr("An earlier report may be old or invalid")
    if (ring.provisional)
        text = qsTr("%1, format not confirmed by a published specification").arg(text)
    if (ring.weakCheck)
        text = qsTr("%1, weak error check").arg(text)
    return text
}

function degreesText(value, positive, negative) {
    if (typeof value !== "number" || !isFinite(value))
        return ""
    return Math.abs(value).toFixed(5) + "° " + (value >= 0 ? positive : negative)
}

// "22.50000° N, 45.00000° E".
function coordinateText(lat, lon) {
    var a = degreesText(lat, qsTr("N"), qsTr("S"))
    var b = degreesText(lon, qsTr("E"), qsTr("W"))
    return a.length > 0 && b.length > 0 ? a + ", " + b : ""
}

// Metres in the unit shown: "12 m", "40 ft", "1.2 km", "0.8 mi".
function lengthText(metres, metric) {
    if (typeof metres !== "number" || !isFinite(metres) || metres < 0)
        return ""
    if (metric)
        return metres >= 1000 ? qsTr("%1 km").arg((metres / 1000).toFixed(1)) : qsTr("%1 m").arg(Math.round(metres))
    var feet = metres / 0.3048
    return feet >= 1000 ? qsTr("%1 mi").arg((feet / 5280).toFixed(1)) : qsTr("%1 ft").arg(Math.round(feet))
}

// "Accurate to 12 m", "Accurate to 12 m or worse" for a lower bound.
function accuracyText(metres, lowerBound, metric) {
    var length = lengthText(metres, metric)
    if (length.length === 0 || metres <= 0)
        return ""
    return lowerBound ? qsTr("Accurate to %1 or worse").arg(length) : qsTr("Accurate to %1").arg(length)
}

// 0.1 km/h in the unit shown; "at least" for a lower bound.
function speedText(dkmh, lowerBound, metric) {
    if (typeof dkmh !== "number" || !isFinite(dkmh) || dkmh < 0)
        return ""
    var kmh = dkmh / 10
    var value = metric ? qsTr("%1 km/h").arg(Math.round(kmh)) : qsTr("%1 mph").arg(Math.round(kmh / 1.609344))
    return lowerBound ? qsTr("Speed at least %1").arg(value) : qsTr("Speed %1").arg(value)
}

// 0.1 degree. An ambiguous heading is LIP's direction of travel 15, which
// means 337.5° or that the direction is not known: both are said, and no
// other bearing is.
function headingText(ddeg, ambiguous) {
    if (typeof ddeg !== "number" || !isFinite(ddeg) || ddeg < 0)
        return ""
    var deg = Math.round(ddeg / 10) % 360
    var names = [qsTr("north"), qsTr("northeast"), qsTr("east"), qsTr("southeast"), qsTr("south"), qsTr("southwest"), qsTr("west"), qsTr("northwest")]
    var text = qsTr("Heading %1° (%2)").arg(deg).arg(names[Math.round(deg / 45) % 8])
    return ambiguous ? qsTr("%1, or the direction is not known").arg(text) : text
}

// Decimetres in the unit shown.
function altitudeText(dm, metric) {
    if (typeof dm !== "number" || !isFinite(dm))
        return ""
    var metres = dm / 10
    return metric ? qsTr("Altitude %1 m").arg(Math.round(metres)) : qsTr("Altitude %1 ft").arg(Math.round(metres / 0.3048))
}

// Great-circle metres between two positions in degrees (display only).
function distanceMetres(lat1, lon1, lat2, lon2) {
    var r = Math.PI / 180
    var dLat = (lat2 - lat1) * r
    var dLon = (lon2 - lon1) * r
    var a = Math.sin(dLat / 2) * Math.sin(dLat / 2) + Math.cos(lat1 * r) * Math.cos(lat2 * r) * Math.sin(dLon / 2) * Math.sin(dLon / 2)
    return 6371000 * 2 * Math.atan2(Math.sqrt(a), Math.sqrt(1 - a))
}

// Whether @p host is OpenStreetMap's own tile server (tile.openstreetmap.org,
// or one of its lettered names), whose tiles may be shown only with its credit.
function isOsmTileHost(host) {
    var h = String(host || "").toLowerCase()
    return h === "tile.openstreetmap.org" || /\.tile\.openstreetmap\.org$/.test(h)
}

// Whether @p attribution gives what the tile server at @p host asks for:
// OpenStreetMap's tiles need @p osmCredit in it (more may follow); any other
// server, any credit that is not empty.
function creditGiven(host, attribution, osmCredit) {
    var text = String(attribution || "").trim()
    if (text.length === 0)
        return false
    return !isOsmTileHost(host) || text.indexOf(osmCredit) >= 0
}

// The credit the map shows for @p host: the saved one, or @p osmCredit in
// front of it when OpenStreetMap's tiles show and the saved one leaves it out.
function shownCredit(host, attribution, osmCredit) {
    var text = String(attribution || "").trim()
    if (!isOsmTileHost(host) || text.indexOf(osmCredit) >= 0)
        return text
    return text.length > 0 ? osmCredit + " · " + text : osmCredit
}
