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

// Flags a saved system may still carry from an older catalog. They keep their
// saved behavior (the session-args builder splices the stored flag verbatim);
// this map only keeps their card label honest.
var LEGACY_DECODE_LABELS = {
    "-f1": "P25"
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
