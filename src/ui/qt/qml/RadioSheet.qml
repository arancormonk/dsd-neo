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
Rectangle {
    id: sheet

    objectName: "radioSheet"

    anchors.fill: parent
    visible: false
    color: Qt.alpha("#000000", 0.5)

    // 0 dB is the tuner's automatic gain, not silence — worth saying, because
    // "0" next to a signal that vanished reads as a mistake otherwise.
    readonly property bool autoGain: metrics.tunerGainDb <= 0

    function open() {
        visible = true
    }

    /** Nudge the tuner gain, staying inside what an R820T actually offers. */
    function stepGain(delta) {
        var next = metrics.tunerGainDb + delta
        if (next < 0)
            next = 0
        if (next > 49)
            next = 49
        commands.setTunerGain(next)
    }

    /** Nudge the crystal correction. Real dongles land within about ±100 ppm. */
    function stepPpm(delta) {
        var next = metrics.ppm + delta
        if (next < -200)
            next = -200
        if (next > 200)
            next = 200
        commands.setPpm(next)
    }

    /** Nudge the squelch. Coarse steps: this is a threshold, not a measurement. */
    function stepSquelch(delta) {
        var next = metrics.squelchDb + delta
        if (next < -140)
            next = -140
        if (next > 0)
            next = 0
        commands.setSquelchDb(next)
    }

    // Tapping the scrim dismisses; tapping the panel must not. A TapHandler on
    // the panel cannot express that, because handlers never take exclusive grabs
    // and both would fire — which closed this panel on every press of every
    // control inside it. So the scrim decides by where the tap landed.
    TapHandler {
        onTapped: function (eventPoint) {
            if (!sheet.hitsPanel(eventPoint.position.x, eventPoint.position.y))
                sheet.visible = false
        }
    }

    /** Whether a point in this sheet's coordinates lies on the panel. */
    function hitsPanel(x, y) {
        var p = sheet.mapToItem(panel, x, y)
        return p.x >= 0 && p.y >= 0 && p.x <= panel.width && p.y <= panel.height
    }

    UiPanel {
        id: panel

        objectName: "radioSheetPanel"
        anchors.centerIn: parent
        width: parent.width - 2 * Theme.screenPadding
        height: column.height + 2 * Theme.cardPadding

        Column {
            id: column

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.margins: Theme.cardPadding
            spacing: 14

            MicroLabel {
                text: qsTr("Radio")
            }

            // ---- Gain ----
            Item {
                width: parent.width
                height: 34

                Text {
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Gain")
                    font.family: Theme.sans
                    font.pixelSize: 14
                    color: Theme.textSecondary
                }

                Row {
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 10

                    Text {
                        objectName: "radioGainValue"
                        anchors.verticalCenter: parent.verticalCenter
                        width: 74
                        horizontalAlignment: Text.AlignRight
                        text: sheet.autoGain ? qsTr("auto") : metrics.tunerGainDb + " dB"
                        font.family: Theme.mono
                        font.pixelSize: 14
                        color: Theme.textPrimary
                    }

                    OutlineButton {
                        objectName: "radioGainDown"
                        width: 44
                        height: 34
                        text: "−"
                        onClicked: sheet.stepGain(-1)
                    }

                    OutlineButton {
                        objectName: "radioGainUp"
                        width: 44
                        height: 34
                        text: "+"
                        onClicked: sheet.stepGain(1)
                    }
                }
            }

            // ---- Squelch ----
            Item {
                width: parent.width
                height: 34

                Text {
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Squelch")
                    font.family: Theme.sans
                    font.pixelSize: 14
                    color: Theme.textSecondary
                }

                Row {
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 10

                    Text {
                        objectName: "radioSquelchValue"
                        anchors.verticalCenter: parent.verticalCenter
                        width: 74
                        horizontalAlignment: Text.AlignRight
                        text: Math.round(metrics.squelchDb) + " dB"
                        font.family: Theme.mono
                        font.pixelSize: 14
                        color: Theme.textPrimary
                    }

                    OutlineButton {
                        objectName: "radioSquelchDown"
                        width: 44
                        height: 34
                        text: "−"
                        onClicked: sheet.stepSquelch(-5)
                    }

                    OutlineButton {
                        objectName: "radioSquelchUp"
                        width: 44
                        height: 34
                        text: "+"
                        onClicked: sheet.stepSquelch(5)
                    }
                }
            }

            // ---- PPM ----
            // The dongle's crystal error. Chosen once when the system was added,
            // by someone who had no way to tell whether it was right: the symptom
            // is a frequency offset the decoder cannot close, and that only shows
            // up with a signal on screen. Which is here.
            Item {
                width: parent.width
                height: 34

                Text {
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("PPM")
                    font.family: Theme.sans
                    font.pixelSize: 14
                    color: Theme.textSecondary
                }

                Row {
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 10

                    Text {
                        objectName: "radioPpmValue"
                        anchors.verticalCenter: parent.verticalCenter
                        width: 74
                        horizontalAlignment: Text.AlignRight
                        text: String(metrics.ppm)
                        font.family: Theme.mono
                        font.pixelSize: 14
                        color: Theme.textPrimary
                    }

                    OutlineButton {
                        objectName: "radioPpmDown"
                        width: 44
                        height: 34
                        text: "−"
                        onClicked: sheet.stepPpm(-1)
                    }

                    OutlineButton {
                        objectName: "radioPpmUp"
                        width: 44
                        height: 34
                        text: "+"
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
                font.pixelSize: 14
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
                onSelected: function (index) { commands.setModulation(index) }
            }

            // ---- Decode ----
            Text {
                text: qsTr("Listening for")
                font.family: Theme.sans
                font.pixelSize: 14
                color: Theme.textSecondary
            }

            Flow {
                width: parent.width
                spacing: 8

                Repeater {
                    // The simulcast entry is a modulation choice wearing a decode
                    // chip's clothes; it has its own control above, so it would
                    // appear here as a duplicate that changes nothing.
                    model: Util.DECODE_MODES.filter(function (m) { return m.flag !== "-mq" })

                    DecodeChip {
                        required property var modelData

                        readonly property int mode: commands.decodeModeForFlag(modelData.flag)

                        objectName: "radioDecode_" + modelData.short
                        text: modelData.short
                        selected: mode >= 0 && mode === metrics.decodeMode
                        onClicked: {
                            if (mode >= 0)
                                commands.setDecodeMode(mode)
                        }
                    }
                }
            }

            OutlineButton {
                objectName: "radioSheetDone"
                width: parent.width
                text: qsTr("Done")
                onClicked: sheet.visible = false
            }
        }
    }
}
