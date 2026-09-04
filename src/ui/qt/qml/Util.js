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
        // trunked: an explicit P25 pick almost certainly means a trunked
        // system, so the wizard suggests turning call-following on. Only these
        // two chips carry it — DMR and NXDN trunk too, but conventional use is
        // common enough there that neither answer is a safe suggestion.
        label: "P25", short: "P25", flag: "-ft", trunked: true,
        hint: "Standard P25 — most statewide and county digital systems."
    },
    {
        // -mq alone, not -f1 -mq: QPSK is what LSM needs, and the engine's default
        // decode set already covers both P25 Phase 1 and Phase 2.
        label: "P25 Simulcast", short: "P25 LSM", flag: "-mq", trunked: true,
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
// `trunked` marks the bands where a bare carrier is far more often a trunked
// control channel than a conventional voice channel: 700 and 800 are where the
// statewide and county P25 systems the decode chips are pitched at live, and
// 851.375 — the wizard's own frequency prefill — is one. VHF and UHF stay out
// because conventional repeater pairs are at least as common there, so neither
// answer would be a safe suggestion. See suggestsTrunking().
var BANDS = [
    { label: "VHF", low: 136.0e6, high: 174.0e6, start: 154.0e6 },
    { label: "UHF", low: 380.0e6, high: 470.0e6, start: 453.0e6 },
    { label: "700", low: 763.0e6, high: 806.0e6, start: 770.0e6, trunked: true },
    { label: "800", low: 806.0e6, high: 869.0e6, start: 855.0e6, trunked: true }
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

// The catalog entry a chip flag names, or null. One lookup for every question
// asked of the catalog, so a flag that has to be matched some other way — the
// importer's composite forms are already the reason LEGACY_DECODE_LABELS exists
// — is taught here once rather than in each accessor.
function findDecodeMode(flag) {
    for (var i = 0; i < DECODE_MODES.length; i++) {
        if (DECODE_MODES[i].flag === flag)
            return DECODE_MODES[i]
    }
    return null
}

// LEGACY_DECODE_LABELS is a plain object, so a bare `[flag] !== undefined` also
// answers for every Object.prototype member: a saved decodeFlag of "constructor",
// "toString" or "__proto__" would hand a Function back to callers that expect a
// label string and render it into a chip. hasOwnProperty is what keeps the lookup
// to the twelve rows actually declared.
function legacyDecodeLabel(flag) {
    if (typeof flag !== "string")
        return null
    if (!Object.prototype.hasOwnProperty.call(LEGACY_DECODE_LABELS, flag))
        return null
    return LEGACY_DECODE_LABELS[flag]
}

function decodeLabel(flag) {
    var mode = findDecodeMode(flag)
    if (mode !== null)
        return mode.short
    var legacy = legacyDecodeLabel(flag)
    if (legacy !== null)
        return legacy
    return "Auto"
}

function decodeHint(flag) {
    var mode = findDecodeMode(flag)
    if (mode !== null)
        return mode.hint
    // A composite the catalog does not offer still gets a line: this row going
    // blank was half of what made an imported system read as "nothing chosen".
    if (legacyDecodeLabel(flag) !== null)
        return "Saved with this system — tap another chip to change it."
    return ""
}

// The chip row to render for `flag`: the catalog, with the entry a composite
// flag refines swapped for the composite itself, or the composite appended when
// the catalog has no entry it refines (EDACS, which is kept out on purpose).
//
// The importer picks flags DECODE_MODES deliberately does not offer — "-mq -^",
// "-fs -Y", the EDACS forms — and the row matches on the whole flag string, so
// they used to select nothing at all. Pointing them at their base chip instead
// would be worse than the blank row: the base carries the SHORT flag, so one
// tap would silently drop the "-^" or the "-Y" the import added. The chip the
// user sees therefore carries the WHOLE flag. Tapping it changes nothing;
// tapping any other replaces it cleanly.
//
// Swapping rather than appending also keeps the row honest: "-mq -^" would
// otherwise sit next to "P25 Simulcast" as a second, near-identical chip.
function decodeChips(flag) {
    if (findDecodeMode(flag) !== null)
        return DECODE_MODES
    var label = legacyDecodeLabel(flag)
    if (label === null)
        return DECODE_MODES

    var out = []
    var placed = false
    for (var i = 0; i < DECODE_MODES.length; i++) {
        var m = DECODE_MODES[i]
        // `m.flag + " "` and not a bare prefix: Auto's empty flag would match
        // everything, and "-f1" must not be read as refining "-f".
        if (!placed && m.flag !== "" && flag.indexOf(m.flag + " ") === 0) {
            out.push({ label: label, short: label, flag: flag,
                       trunked: m.trunked, hint: decodeHint(flag) })
            placed = true
        } else if (!placed && m.label === label) {
            // A legacy ALIAS rather than a refinement: "-f1" carries the same
            // label as the catalog's "-ft" P25 chip but is not "-ft "-prefixed,
            // so appending it would put two chips reading "P25" side by side -
            // and WizardScreen.qml names the delegate after the label, so the two
            // would share an objectName as well. Swap, exactly as a refinement does.
            out.push({ label: label, short: label, flag: flag,
                       trunked: m.trunked, hint: decodeHint(flag) })
            placed = true
        } else {
            out.push(m)
        }
    }
    if (!placed)
        out.push({ label: label, short: label, flag: flag, hint: decodeHint(flag) })
    return out
}

// Whether the wizard should suggest turning call-following on, for a user who
// has not answered "is it trunked?" themselves.
//
// The chip is the stronger signal and answers alone whenever it names a system
// type. The Auto chip names none, so the frequency is all there is to go on —
// and that case is not academic: Auto is the default selection, so a user who
// accepts the wizard's own 851.375 prefill never taps a chip at all. Reading
// that as "not trunked" hands the decoder a control channel with call-following
// off, which locks on and plays nothing, with no error to explain the silence.
//
// @a hz may be NaN (an empty or half-typed field); bandFor() returns null for
// it, so the answer is simply "no suggestion".
//
// Flags outside the catalog (the RadioReference import's composite forms) never
// reach this: the import carries the database's own trunking answer instead.
function suggestsTrunking(flag, hz) {
    var mode = findDecodeMode(flag)
    if (mode === null)
        // Unreachable for the composites: the only two writers that can put one
        // in decodeFlag - applyRadioReference() and openForEdit() - both call
        // answerTrunking() with it, and refreshTrunkingSuggestion() (the sole
        // caller here) returns early once trunkingAnswered is set. Tapping the
        // composite chip decodeChips() splices in cannot reach it either, since
        // that chip only exists while decodeFlag already holds the composite.
        // If either writer ever stops answering, resolve the composite the way
        // decodeChips() does rather than widening findDecodeMode().
        return false
    if (mode.trunked === true)
        return true
    // An explicit system type that does not carry `trunked` has answered for
    // itself — DMR and NXDN trunk too, but conventional use is common enough
    // there that the band must not override the user's own pick.
    if (mode.flag !== "")
        return false
    var band = bandFor(hz)
    return band !== null && band.trunked === true
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
