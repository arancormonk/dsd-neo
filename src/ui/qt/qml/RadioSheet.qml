// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import "Util.js" as Util

// The four settings that decide whether a signal on the waterfall turns into
// audio, put where the waterfall is. Getting them wrong looks identical to
// nothing being on the air, and until now the only way to change them was to
// stop, edit the system and start again — which loses the thing you were
// looking at.
//
// Every control reads the engine, not its own last request: a command can be
// refused, and on Android the service holding these outlives this process, so a
// panel remembering what it asked for would drift from the radio.
//
// With one bounded exception, below: a request that has been sent and not yet
// answered stands in for the reading while it is outstanding. metrics is a
// mirror republished every 250 ms, and these commands are coalescible setters —
// so five taps inside one poll would each read the same stale value, compute the
// same next one, and merge in the queue into a single step. The pending value
// expires after requestTtlMs whatever happens, which is how a refused command
// comes back to what the radio is actually on.
ModalSheet {
    id: sheet

    objectName: "radioSheet"
    panelObjectName: "radioSheetPanel"
    spacing: 14
    readonly property bool sessionRunning: decoderHost.running
    onSessionRunningChanged: {
        if (!sessionRunning) {
            visible = false;
            forgetRequests();
        }
    }

    // Long enough for several 250 ms polls plus the queue drain — an accepted
    // request is normally reflected well inside it, and a refused one is not
    // left standing for long enough to read as accepted.
    readonly property int requestTtlMs: 1500

    // NaN means "no outstanding request; the engine's reading is the truth".
    property real pendingGain: NaN
    property real pendingPpm: NaN
    property real pendingSquelch: NaN
    property real pendingAnalogWidth: NaN

    // What each control steps from and displays.
    readonly property bool airspyActive: metrics.airspy !== undefined && metrics.airspy.gain_mode !== undefined
    readonly property int gainDb: isNaN(pendingGain) ? metrics.tunerGainDb : pendingGain
    readonly property int ppm: isNaN(pendingPpm) ? metrics.ppm : pendingPpm
    // A scan row or target can carry its own squelch (--squelch-db). While it is on
    // air the readout is the row's, and the buttons edit the configured default
    // beneath it: an edit made to the row would be gone the moment the scanner
    // moved on. Whether each level is off is the engine's decision, not the sign
    // of its dB reading: a full-scale default also reads 0 dB and gates everything.
    readonly property bool squelchRowOverride: metrics.squelchRowOverride === true
    readonly property real baseSquelchDb: squelchRowOverride ? metrics.configuredSquelchDb : metrics.squelchDb
    readonly property bool baseSquelchOff: squelchRowOverride
        ? metrics.configuredSquelchOff : metrics.squelchOff
    readonly property real squelchDb: isNaN(pendingSquelch) ? baseSquelchDb : pendingSquelch
    // Off is a state of its own, not a very low threshold: squelchDb bottoms out
    // at the -120 dB display floor either way. A pending request speaks for
    // itself, since 0 is what the engine reads as "switch it off".
    readonly property bool squelchOff: isNaN(pendingSquelch) ? baseSquelchOff : pendingSquelch >= 0
    // The threshold in force comes first; with a row override that is the row's,
    // and the default being edited is named below it.
    readonly property string squelchReading: squelchRowOverride
        ? squelchText(metrics.effectiveSquelchOff, metrics.effectiveSquelchDb)
        : squelchText(squelchOff, squelchDb)

    // 0 dB is the tuner's automatic gain, not silence — worth saying, because
    // "0" next to a signal that vanished reads as a mistake otherwise.
    readonly property bool autoGain: gainDb <= 0

    // Issue #525: the NFM channel width, shown under the analog preset. The
    // stepper edits the configured width (0 is the default), and the reading is
    // the width in force as the engine spells it for every frontend: what the
    // front end reports while the monitor runs, with "DSP-limited" when the DSP
    // rate rather than the filter bounds it.
    readonly property int analogMode: commands.decodeModeForFlag("-fA")
    readonly property bool analogPreset: analogMode >= 0 && metrics.decodeMode === analogMode
    // The width is the radio front end's filter; PCM audio arrives demodulated.
    readonly property bool analogWidthEditable: metrics.radioInput === true
    readonly property int analogWidthConfigured: isNaN(pendingAnalogWidth)
        ? metrics.analogBandwidthConfiguredHz : pendingAnalogWidth
    // Under another preset an explicit width stays editable on a radio, as the
    // terminal row does: a switch to NFM is held to it, and where the device or
    // the capture forces a DSP rate that cannot filter it, the refusal says to
    // narrow it, which has to be possible before the switch.
    readonly property bool analogWidthOffered: analogPreset
        || (analogWidthEditable && (metrics.analogBandwidthConfiguredHz > 0 || !isNaN(pendingAnalogWidth)))
    // An explicit width steps from itself. The default steps from the width in
    // force, which below a 20 kHz DSP rate is the width the rate leaves rather
    // than 16 kHz, so the first step from there is one the rate can take.
    readonly property int analogWidthStepFrom: {
        if (analogWidthConfigured > 0)
            return analogWidthConfigured;
        return metrics.analogBandwidthHz > 0 ? metrics.analogBandwidthHz : Util.NFM_DEFAULT_WIDTH_HZ;
    }
    // The widest width the DSP rate filters (the running stream's, or with none
    // the rate an RTL-SDR input's DSP bandwidth sets; 0 = not known): the steps
    // skip what the engine would refuse.
    readonly property int analogWidthMax: metrics.analogBandwidthMaxHz > 0 ? metrics.analogBandwidthMaxHz : 0
    readonly property bool analogWidthCanNarrow: analogWidthEditable
        && Util.nextNfmWidth(analogWidthStepFrom, -1, analogWidthMax) > 0
    readonly property bool analogWidthCanWiden: analogWidthEditable
        && Util.nextNfmWidth(analogWidthStepFrom, 1, analogWidthMax) > 0
    // A request stands in for the reading until the engine answers, spelled as
    // the setting it is ("12.5 kHz", or "default" for 0).
    // Outside the preset no width is in force, so the setting stands in.
    readonly property string analogWidthReading: {
        if (!isNaN(pendingAnalogWidth))
            return pendingAnalogWidth > 0 ? Util.widthKhzText(pendingAnalogWidth) : qsTr("default");
        if (!analogPreset)
            return analogWidthConfigured > 0 ? Util.widthKhzText(analogWidthConfigured) : qsTr("default");
        return metrics.analogBandwidthReading;
    }

    function open() {
        // Whatever was outstanding belongs to the last time this was open, and on
        // Android the service may have been driven from elsewhere since.
        forgetRequests();
        visible = true;
    }

    /** A squelch reading as the panel prints it. */
    function squelchText(off, db) {
        return off ? qsTr("off") : Math.round(db) + " dB";
    }

    /** Drop every outstanding request and go back to reading the engine. */
    function forgetRequests() {
        pendingGain = NaN;
        pendingPpm = NaN;
        pendingSquelch = NaN;
        pendingAnalogWidth = NaN;
        gainTtl.stop();
        ppmTtl.stop();
        squelchTtl.stop();
        analogWidthTtl.stop();
    }

    /**
     * Step the NFM channel width through the common channel plans. The engine
     * refuses a width the DSP rate cannot filter, with a message, and keeps the
     * one it had; the pending value then expires back to the reading.
     */
    function stepAnalogWidth(direction) {
        if (!(direction > 0 ? analogWidthCanWiden : analogWidthCanNarrow))
            return;
        var next = Util.nextNfmWidth(analogWidthStepFrom, direction, analogWidthMax);
        pendingAnalogWidth = next;
        analogWidthTtl.restart();
        commands.setNfmBandwidthHz(next);
    }

    /**
     * Return the NFM channel width to the unset default (0). Not a step to
     * 16 kHz: the default keeps its own rule (the channel filter runs only at
     * DSP rates of 20 kHz or more) and a save leaves it out of the config, so a
     * later default reaches it.
     */
    function resetAnalogWidth() {
        if (!analogWidthEditable || analogWidthConfigured <= 0)
            return;
        pendingAnalogWidth = 0;
        analogWidthTtl.restart();
        commands.setNfmBandwidthHz(0);
    }

    /**
     * Nudge the tuner gain, staying inside what an R820T actually offers.
     *
     * A step that lands where the setting already is sends nothing: every
     * accepted gain command restarts the dongle (svc_rtl_set_gain sets
     * rtl_needs_restart), which drops audio and the spectrum — too much to spend
     * on a button press at the end of the range that changes no setting.
     */
    function stepGain(delta) {
        var next = gainDb + delta;
        if (next < 0)
            next = 0;
        if (next > 49)
            next = 49;
        if (next === gainDb)
            return;
        pendingGain = next;
        gainTtl.restart();
        commands.setTunerGain(next);
    }

    /** Nudge the crystal correction. Real dongles land within about ±100 ppm. */
    function stepPpm(delta) {
        var next = ppm + delta;
        if (next < -200)
            next = -200;
        if (next > 200)
            next = 200;
        if (next === ppm)
            return;
        pendingPpm = next;
        ppmTtl.restart();
        commands.setPpm(next);
    }

    /**
     * Nudge the squelch. Coarse steps: this is a threshold, not a measurement.
     *
     * Off sits one step below the floor. The floor itself is the readback's own:
     * pwr_to_dB() clamps what comes back at -120 dB, so a threshold below that
     * cannot be displayed. Stepping down from the floor therefore switches the
     * squelch off — which the engine spells 0 — rather than pinning at a value
     * the button can no longer move, and stepping up from off lands back on the
     * floor. Above it, -5 dB is the last real threshold, because 0 is off.
     */
    function stepSquelch(delta) {
        var cur = squelchOff ? -125 : squelchDb;
        var next = cur + delta;
        if (next < -120) {
            if (squelchOff)
                return;
            next = 0;
        } else if (next > -5) {
            next = -5;
        }
        // The reading is a float that has been through dB_to_pwr and back, so
        // "already there" is a tolerance, not an equality.
        if (!squelchOff && Math.abs(next - squelchDb) < 0.001)
            return;
        pendingSquelch = next;
        squelchTtl.restart();
        commands.setSquelchDb(next);
    }

    Timer {
        id: gainTtl

        interval: sheet.requestTtlMs
        onTriggered: sheet.pendingGain = NaN
    }

    Timer {
        id: ppmTtl

        interval: sheet.requestTtlMs
        onTriggered: sheet.pendingPpm = NaN
    }

    Timer {
        id: squelchTtl

        interval: sheet.requestTtlMs
        onTriggered: sheet.pendingSquelch = NaN
    }

    Timer {
        id: analogWidthTtl

        interval: sheet.requestTtlMs
        onTriggered: sheet.pendingAnalogWidth = NaN
    }

    MicroLabel {
        text: qsTr("Radio")
    }

    AirspyControls {
        width: parent.width
        visible: sheet.airspyActive
        settings: metrics.airspy || ({})
        onEdited: function(key, value) { commands.setAirspy(key, value); }
    }
    Column {
        visible: !sheet.airspyActive
        width: parent.width
        spacing: 8
        Text {
            text: qsTr("Gain")
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSize(14)
        }
        Row {
            width: parent.width
            spacing: 10
            Text {
                objectName: "radioGainValue"
                width: parent.width - 116
                anchors.verticalCenter: parent.verticalCenter
                text: sheet.autoGain ? qsTr("auto") : sheet.gainDb + " dB"
                color: Theme.textPrimary
                font.family: Theme.mono
                font.pixelSize: Theme.fontSize(14)
            }
            OutlineButton {
                objectName: "radioGainDown"
                width: 48
                text: "−"
                accessibleName: qsTr("Decrease Gain")
                onClicked: sheet.stepGain(-1)
            }
            OutlineButton {
                objectName: "radioGainUp"
                width: 48
                text: "+"
                accessibleName: qsTr("Increase Gain")
                onClicked: sheet.stepGain(1)
            }
        }
    }

    Column {
        width: parent.width
        spacing: 8
        Text {
            text: qsTr("Squelch")
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSize(14)
        }
        Row {
            width: parent.width
            spacing: 10
            Text {
                objectName: "radioSquelchValue"
                width: parent.width - 116
                anchors.verticalCenter: parent.verticalCenter
                text: sheet.squelchReading
                // Read aloud as the terminal prints it, row note and default included.
                Accessible.role: Accessible.StaticText
                Accessible.name: sheet.squelchRowOverride ? metrics.squelchReadout : sheet.squelchReading
                color: Theme.textPrimary
                font.family: Theme.mono
                font.pixelSize: Theme.fontSize(14)
            }
            OutlineButton {
                objectName: "radioSquelchDown"
                width: 48
                text: "−"
                accessibleName: qsTr("Decrease Squelch")
                onClicked: sheet.stepSquelch(-5)
            }
            OutlineButton {
                objectName: "radioSquelchUp"
                width: 48
                text: "+"
                accessibleName: qsTr("Increase Squelch")
                onClicked: sheet.stepSquelch(5)
            }
        }
        // Shown only while the row on air overrides the squelch: says the row owns the
        // reading above, and which default the buttons are changing.
        Row {
            objectName: "radioSquelchRowNote"
            visible: sheet.squelchRowOverride
            spacing: 8
            Rectangle {
                objectName: "radioSquelchRowBadge"
                implicitWidth: rowBadge.implicitWidth + 14
                implicitHeight: Math.max(20, rowBadge.implicitHeight + 8)
                anchors.verticalCenter: parent.verticalCenter
                radius: 5
                color: "transparent"
                border.width: 1
                border.color: Theme.controlBorder

                Text {
                    id: rowBadge
                    anchors.centerIn: parent
                    text: qsTr("row")
                    font.family: Theme.mono
                    font.pixelSize: Theme.fontSize(10)
                    font.letterSpacing: 1
                    color: Theme.cyan
                }
            }
            Text {
                objectName: "radioSquelchDefault"
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("default %1").arg(sheet.squelchText(sheet.squelchOff, sheet.squelchDb))
                color: Theme.textSecondary
                font.family: Theme.mono
                font.pixelSize: Theme.fontSize(12)
            }
        }
    }

    Column {
        visible: !sheet.airspyActive
        width: parent.width
        spacing: 8
        Text {
            text: qsTr("PPM")
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSize(14)
        }
        Row {
            width: parent.width
            spacing: 10
            Text {
                objectName: "radioPpmValue"
                width: parent.width - 116
                anchors.verticalCenter: parent.verticalCenter
                text: String(sheet.ppm)
                color: Theme.textPrimary
                font.family: Theme.mono
                font.pixelSize: Theme.fontSize(14)
            }
            OutlineButton {
                objectName: "radioPpmDown"
                width: 48
                text: "−"
                accessibleName: qsTr("Decrease PPM")
                onClicked: sheet.stepPpm(-1)
            }
            OutlineButton {
                objectName: "radioPpmUp"
                width: 48
                text: "+"
                accessibleName: qsTr("Increase PPM")
                onClicked: sheet.stepPpm(1)
            }
        }
    }

    Rectangle {
        width: parent.width
        height: 1
        color: Theme.divider
    }

    // ---- Modulation ----
    // Its own control rather than a decode chip, because it answers a
    // different question: not what to look for, but how the site sends
    // it. A simulcast P25 system reads as noise on C4FM and never locks,
    // and that is the single most common reason a real signal decodes
    // nothing.
    Text {
        text: qsTr("Modulation")
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(14)
        color: Theme.textSecondary
    }

    // GFSK is the third choice because it is a state the session can
    // already be in — the DMR and EDACS/ProVoice presets select it — and
    // a two-entry control could only show it as C4FM, then offer no way
    // back to it once the user tried something else.
    SegmentedControl {
        objectName: "radioModulation"
        width: parent.width
        model: [qsTr("C4FM"), qsTr("QPSK / simulcast"), qsTr("GFSK")]
        currentIndex: metrics.modulation
        onSelected: function (index) {
            commands.setModulation(index);
        }
    }

    // ---- Decode ----
    Text {
        text: qsTr("Listening for")
        font.family: Theme.sans
        font.pixelSize: Theme.fontSize(14)
        color: Theme.textSecondary
    }

    Flow {
        width: parent.width
        spacing: 8

        Repeater {
            // The simulcast entry is a modulation choice wearing a decode
            // chip's clothes; it has its own control above, so it would
            // appear here as a duplicate that changes nothing.
            model: Util.DECODE_MODES.filter(function (m) {
                return m.flag !== "-mq";
            })

            DecodeChip {
                required property var modelData

                readonly property int mode: commands.decodeModeForFlag(modelData.flag)

                objectName: "radioDecode_" + modelData.short
                text: modelData.short
                selected: mode >= 0 && mode === metrics.decodeMode
                onClicked: {
                    if (mode >= 0)
                        commands.setDecodeMode(mode);
                }
            }
        }
    }

    // ---- Analog ----
    // The NFM channel filter's width: the full RF passband the monitor keeps,
    // not the tuner or the audio bandwidth. It has to fit the DSP rate.
    Column {
        objectName: "radioAnalogSection"
        visible: sheet.analogWidthOffered
        width: parent.width
        spacing: 8
        Text {
            text: qsTr("NFM channel width")
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSize(14)
        }
        Row {
            objectName: "radioAnalogBandwidth"
            width: parent.width
            spacing: 10
            Text {
                objectName: "radioAnalogBandwidthValue"
                width: parent.width - 116
                anchors.verticalCenter: parent.verticalCenter
                text: sheet.analogWidthReading
                color: Theme.textPrimary
                font.family: Theme.mono
                font.pixelSize: Theme.fontSize(14)
            }
            OutlineButton {
                objectName: "radioAnalogBandwidthDown"
                width: 48
                text: "−"
                accessibleName: qsTr("Narrower Channel")
                enabled: sheet.analogWidthCanNarrow
                onClicked: sheet.stepAnalogWidth(-1)
            }
            OutlineButton {
                objectName: "radioAnalogBandwidthUp"
                width: 48
                text: "+"
                accessibleName: qsTr("Wider Channel")
                enabled: sheet.analogWidthCanWiden
                onClicked: sheet.stepAnalogWidth(1)
            }
        }
        // Back to the unset default, offered while an explicit width is set.
        OutlineButton {
            objectName: "radioAnalogBandwidthDefault"
            visible: sheet.analogWidthConfigured > 0
            width: parent.width
            text: qsTr("Use the default width")
            accessibleName: qsTr("Default Channel Width")
            enabled: sheet.analogWidthEditable
            onClicked: sheet.resetAnalogWidth()
        }
        // Outside the preset: what the setting is for.
        Text {
            objectName: "radioAnalogBandwidthIdleNote"
            visible: !sheet.analogPreset && sheet.analogWidthEditable
            width: parent.width
            wrapMode: Text.WordWrap
            text: qsTr("Used when NFM is chosen, which needs a width the DSP rate can filter.")
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSize(12)
        }
        // Why the stepper is greyed out, rather than leaving a dead control.
        Text {
            objectName: "radioAnalogBandwidthNote"
            visible: !sheet.analogWidthEditable
            width: parent.width
            wrapMode: Text.WordWrap
            text: qsTr("The channel width filters a radio input; this audio arrives already demodulated.")
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSize(12)
        }
    }

    OutlineButton {
        objectName: "radioSheetDone"
        width: parent.width
        text: qsTr("Done")
        onClicked: sheet.visible = false
    }
}
