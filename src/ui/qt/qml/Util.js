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
// saved behavior (buildArgs splices the stored flag verbatim); this map only
// keeps their card label honest.
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

// The one-line mono meta under a saved system's name: "851.375 MHz · P25 trunked".
function systemMeta(sys) {
    var parts = []
    if (sys.sourceType === "usb" || sys.sourceType === "rtltcp") {
        if (sys.freqMhz && sys.freqMhz.length > 0)
            parts.push(sys.freqMhz + " MHz")
        var decode = decodeLabel(sys.decodeFlag)
        parts.push(sys.trunking ? decode + " trunked" : decode)
        if (sys.sourceType === "rtltcp")
            parts.push("rtl-tcp")
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

// Whether a saved system's frequency field parses as a positive MHz value.
// Number() rejects trailing junk ("851.375M" → NaN) where parseFloat would not.
function freqValid(freqMhz) {
    if (freqMhz === undefined || freqMhz === null)
        return false
    var mhz = Number(String(freqMhz).trim())
    return isFinite(mhz) && mhz > 0
}

// The CLI-shaped argv a saved system starts with, or null when the system cannot
// produce a sane one. Reusing the CLI parser is what buys the whole option
// surface; per-system overrides fall back to the app-wide defaults from prefs
// (-1 / empty string mean "no override"). A malformed frequency must fail here,
// not downstream: dsd_parse_freq_hz reads a garbage spec as 0 Hz and the session
// would come up silently mistuned with no diagnostic.
function buildArgs(sys, prefs) {
    if ((sys.sourceType === "usb" || sys.sourceType === "rtltcp") && !freqValid(sys.freqMhz))
        return null

    var args = ["--frontend", "none"]

    var gain = (sys.gainDb !== undefined && sys.gainDb >= 0) ? sys.gainDb : prefs.gainDb
    var ppm = String((sys.ppm !== undefined && sys.ppm !== "") ? sys.ppm : String(prefs.ppm)).trim()
    // The wizard's IntValidator accepts an explicit '+' sign that the check
    // below would refuse; it means the same thing, so drop it rather than make
    // "+5" a saved system that can never start.
    if (ppm.charAt(0) === '+')
        ppm = ppm.substring(1)
    // PPM is the one override persisted as a raw string, and it is spliced
    // verbatim into the ':'-delimited spec below — like the frequency, a
    // malformed value must fail here, not downstream as a silently unapplied
    // correction. (gain/bw go through parseInt at commit and fall back on NaN.)
    if ((sys.sourceType === "usb" || sys.sourceType === "rtltcp") && !/^-?\d+$/.test(ppm))
        return null
    var bw = (sys.bandwidthKhz !== undefined && sys.bandwidthKhz > 0) ? sys.bandwidthKhz : prefs.bandwidthKhz
    var bias = sys.biasTee || prefs.biasTee
    var tail = ":" + sys.freqMhz + "M:" + gain + ":" + ppm + ":" + bw + ":0:2"

    if (sys.sourceType === "usb") {
        var spec = "rtl:0" + tail
        if (bias)
            spec += ":bias"
        args.push("-i", spec)
    } else if (sys.sourceType === "rtltcp") {
        // The engine parses a trailing bias token on rtltcp specs exactly as it
        // does on rtl ones; a remote dongle feeding an LNA needs it just as much.
        var tcpSpec = "rtltcp:" + sys.host + ":" + sys.port + tail
        if (bias)
            tcpSpec += ":bias"
        args.push("-i", tcpSpec)
    } else if (sys.sourceType === "udp") {
        args.push("-i", "udp:0.0.0.0:" + sys.port)
    } else if (sys.sourceType === "tcp") {
        args.push("-i", "tcp:" + sys.host + ":" + sys.port)
    } else {
        args.push("-i", sys.filePath)
    }

    args.push("-o", "pulse")

    if (sys.decodeFlag && sys.decodeFlag.length > 0) {
        // A chip may carry several flags, so split rather than push whole.
        var flags = sys.decodeFlag.split(/\s+/)
        for (var f = 0; f < flags.length; f++)
            args.push(flags[f])
    }
    if (sys.trunking)
        args.push("-T")
    if (prefs.skipEncrypted)
        args.push("--enc-lockout")
    if (prefs.autoPpm)
        args.push("--auto-ppm")

    var extra = ((sys.extraArgs || "") + " " + (prefs.extraArgs || "")).trim()
    if (extra.length > 0) {
        var parts = extra.split(/\s+/)
        for (var i = 0; i < parts.length; i++)
            args.push(parts[i])
    }
    return args
}

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
