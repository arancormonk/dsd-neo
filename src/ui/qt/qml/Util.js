// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
.pragma library

// Decode chip catalog: label ↔ CLI flags. The empty flag is the engine's own
// default (P25 Phase 1+2, DMR and YSF enabled together), so "Auto" passes
// nothing. Each entry carries the whole flag set its system type needs — the
// user picks what they are listening to, never a demodulator: a simulcast P25
// site needs QPSK (`-mq`) or it reads at ~1 dB and never locks, so that travels
// with the chip rather than surfacing as a "modulation" question.
var DECODE_MODES = [
    {
        label: "Auto — P25/DMR/YSF", short: "Auto", flag: "",
        hint: "Figures it out — good first choice for most systems."
    },
    {
        // -ft, not -f1: the statewide/county systems this chip is pitched at are
        // commonly Phase 2, and -f1 sets frame_p25p2=0 — every Phase 2 voice
        // grant would tune and stay permanently mute. -ft keeps the P25p1
        // control channel plus both voice phases.
        label: "P25", short: "P25", flag: "-ft",
        hint: "Standard P25 — most statewide and county digital systems."
    },
    {
        // -mq alone, not -f1 -mq: QPSK is what LSM needs, and the engine's default
        // decode set already covers both P25 Phase 1 and Phase 2.
        label: "P25 Simulcast", short: "P25 LSM", flag: "-mq",
        hint: "Simulcast P25 (LSM) — pick this when standard P25 never locks or sounds garbled."
    },
    {
        label: "DMR", short: "DMR", flag: "-fs",
        hint: "DMR — businesses, ham repeaters and some public safety."
    },
    {
        label: "NXDN48", short: "NXDN48", flag: "-fi",
        hint: "NXDN 4800 — NEXEDGE and IDAS narrowband."
    },
    {
        label: "NXDN96", short: "NXDN96", flag: "-fn",
        hint: "NXDN 9600 — wideband NEXEDGE."
    },
    {
        label: "D-STAR", short: "D-STAR", flag: "-fd",
        hint: "D-STAR — ham digital voice."
    },
    {
        label: "YSF", short: "YSF", flag: "-fy",
        hint: "Yaesu System Fusion — ham digital voice."
    },
    {
        label: "M17", short: "M17", flag: "-fz",
        hint: "M17 — open-source ham digital voice."
    }
]

// Flags a saved system may carry that the chip catalog above does not offer:
// one left over from an older catalog, and the composite forms the
// RadioReference import picks on the user's behalf. They keep their saved
// behavior (the session-args builder splices the stored flag verbatim); this map
// only keeps their card label honest, since decodeLabel() matches DECODE_MODES
// on the whole flag string and would otherwise read "Auto".
//
// EDACS deliberately stays out of DECODE_MODES: that array is the user-pickable
// catalog, and these are flags the importer chooses. The custom-AFS forms
// (-fh344, -fH434) still read "Auto" — they go through extraArgs.
var LEGACY_DECODE_LABELS = {
    "-f1": "P25",
    "-fh": "EDACS",
    "-fH": "EDACS",
    "-fe": "EDACS EA",
    "-fE": "EDACS EA",
    "-ft -^": "P25",
    "-mq -^": "P25 LSM",
    "-fs -Y": "DMR Scan",
    "-fi -Y": "NXDN48 Scan",
    "-fn -Y": "NXDN96 Scan",
    "-ft -Y": "P25 Scan",
    "-mq -Y": "P25 LSM Scan"
}

// Where digital voice actually lives, for someone exploring who does not yet know
// a single local frequency. The same US-centric assumption the decode chips make
// ("most statewide and county digital systems"); anyone elsewhere types a
// frequency instead, which is why the catalog is a convenience and not the only
// way to move.
//
// `start` is where a chip drops you: a busy part of the band rather than its
// bottom edge, so the first screen of waterfall usually has something on it.
// `low`/`high` also bound the sweep — it wraps inside the band it began in rather
// than walking off into spectrum nobody asked about.
var BANDS = [
    { label: "VHF", low: 136.0e6, high: 174.0e6, start: 154.0e6 },
    { label: "UHF", low: 380.0e6, high: 470.0e6, start: 453.0e6 },
    { label: "700", low: 763.0e6, high: 806.0e6, start: 770.0e6 },
    { label: "800", low: 806.0e6, high: 869.0e6, start: 855.0e6 }
]

// The tuner's usable range. R820T/R828D — the tuner in essentially every RTL
// dongle — covers 24-1766 MHz, and RTL over USB or TCP is the only radio this
// app builds with: both Android presets set DSD_ENABLE_SOAPYSDR=OFF
// (CMakePresets.json:215,242). Not a hard contract even so — the engine is the
// authority and refuses an out-of-range tune with its own message. This only
// stops the rail offering frequencies nothing can reach.
var TUNER_LOW_HZ = 24.0e6
var TUNER_HIGH_HZ = 1766.0e6

// The band containing hz, or null when tuned outside all of them. nextStepHz()
// is the only caller that treats a null result as a fallback, and what it falls
// back to is unbounded stepping — off-band there is no window to wrap a step
// against, so it simply keeps going.
function bandFor(hz) {
    for (var i = 0; i < BANDS.length; i++) {
        if (hz >= BANDS[i].low && hz < BANDS[i].high)
            return BANDS[i]
    }
    return null
}

// The MHz text for a frequency, without a unit: "769.76875", "851.0125".
//
// Five decimals, not four. 12.5 kHz and 6.25 kHz channel plans put real
// frequencies on the fifth decimal — 769.76875 is a control channel someone is
// listening to — and four would render it as 769.7687, a frequency the radio is
// not on. The trailing zero is dropped so the commoner 4-decimal channels do not
// carry a digit that means nothing.
function mhzText(hz) {
    var s = (hz / 1.0e6).toFixed(5)
    return s.charAt(s.length - 1) === "0" ? s.substring(0, s.length - 1) : s
}

// The same number with its unit: "769.76875 MHz".
function fmtMhz(hz) {
    return mhzText(hz) + " MHz"
}

function decodeLabel(flag) {
    for (var i = 0; i < DECODE_MODES.length; i++) {
        if (DECODE_MODES[i].flag === flag)
            return DECODE_MODES[i].short
    }
    if (LEGACY_DECODE_LABELS[flag] !== undefined)
        return LEGACY_DECODE_LABELS[flag]
    return "Auto"
}

function decodeHint(flag) {
    for (var i = 0; i < DECODE_MODES.length; i++) {
        if (DECODE_MODES[i].flag === flag)
            return DECODE_MODES[i].hint
    }
    return ""
}

// The one-line mono meta under a saved system's name: "851.375 MHz · P25 trunked"
// on the dongle, "851.375 MHz · RTL-TCP · P25 trunked" on a networked tuner.
function systemMeta(sys) {
    var parts = []
    if (sys.sourceType === "usb" || sys.sourceType === "rtltcp") {
        if (sys.freqMhz && sys.freqMhz.length > 0)
            parts.push(sys.freqMhz + " MHz")
        // Ahead of the decode label rather than after it. The card's meta elides
        // from the right, and a trailing marker was the first thing cut — which
        // left a networked tuner and a dongle on the same frequency reading
        // identically, the one case the marker exists to tell apart. Cutting the
        // decode tail instead costs the least: it repeats the chip the user
        // picked, while the source does not appear anywhere else on the card.
        if (sys.sourceType === "rtltcp")
            parts.push("RTL-TCP")
        var decode = decodeLabel(sys.decodeFlag)
        parts.push(sys.trunking ? decode + " trunked" : decode)
    } else if (sys.sourceType === "udp") {
        parts.push("UDP :" + sys.port)
        parts.push(decodeLabel(sys.decodeFlag))
    } else if (sys.sourceType === "tcp") {
        parts.push("TCP " + sys.host + ":" + sys.port)
        parts.push(decodeLabel(sys.decodeFlag))
    } else {
        var path = sys.filePath || ""
        parts.push(path.substring(path.lastIndexOf('/') + 1))
        parts.push(decodeLabel(sys.decodeFlag))
    }
    return parts.join(" · ")
}

// "Heard 2 minutes ago" / "Heard yesterday" / "Never heard".
function heardText(lastHeardSecs) {
    if (!lastHeardSecs || lastHeardSecs <= 0)
        return qsTr("Never heard")
    var delta = Math.floor(Date.now() / 1000) - lastHeardSecs
    if (delta < 60)
        return qsTr("Heard just now")
    if (delta < 3600) {
        var minutes = Math.floor(delta / 60)
        return minutes === 1 ? qsTr("Heard a minute ago") : qsTr("Heard %1 minutes ago").arg(minutes)
    }
    if (delta < 86400) {
        var hours = Math.floor(delta / 3600)
        return hours === 1 ? qsTr("Heard an hour ago") : qsTr("Heard %1 hours ago").arg(hours)
    }
    if (delta < 172800)
        return qsTr("Heard yesterday")
    return qsTr("Heard %1 days ago").arg(Math.floor(delta / 86400))
}

// Compact age for a call row's right edge: "1m", "2h", "3d".
function shortAge(whenSecs) {
    var delta = Math.floor(Date.now() / 1000) - whenSecs
    if (delta < 60)
        return qsTr("now")
    if (delta < 3600)
        return Math.floor(delta / 60) + "m"
    if (delta < 86400)
        return Math.floor(delta / 3600) + "h"
    return Math.floor(delta / 86400) + "d"
}

// "0:07" — the monitor timer and call durations share this shape.
function fmtDuration(secs) {
    if (secs === undefined || secs === null || secs < 0)
        return ""
    var m = Math.floor(secs / 60)
    var s = secs % 60
    return m + ":" + (s < 10 ? "0" + s : s)
}

// The argv a saved system starts with is built by the C++ SessionArgsBuilder
// (the `sessionArgs` context property): the ':'-delimited rtl specs it
// assembles silently mistune a session when malformed, so they are built and
// validated in host-tested code, not here.

// Uppercased mono meta for the monitor header: "851.375 MHZ · TRUNKED · USB".
function monitorMeta(sys) {
    var parts = []
    if ((sys.sourceType === "usb" || sys.sourceType === "rtltcp") && sys.freqMhz && sys.freqMhz.length > 0)
        parts.push(sys.freqMhz + " MHz")
    if (sys.trunking)
        parts.push(qsTr("trunked"))
    if (sys.sourceType === "usb")
        parts.push("USB")
    else if (sys.sourceType === "rtltcp")
        parts.push("RTL-TCP")
    else if (sys.sourceType === "udp")
        parts.push("UDP")
    else if (sys.sourceType === "tcp")
        parts.push("TCP")
    else
        parts.push(qsTr("file"))
    return parts.join(" · ").toUpperCase()
}
