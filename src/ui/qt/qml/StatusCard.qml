// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Live signal and per-slot call state. Only ever on screen while a session is, so it
// never has to render a plausible-looking row of zeros for a decoder that is not
// running — see MetricsModel::clear().
GroupBox {
    id: card

    title: qsTr("Status")

    // The front end takes a moment to open the device, and a grid of zeros during that
    // window reads as a fault rather than as progress. The slot text is the test rather
    // than the symbol rate: MetricsModel::clear() empties it and only a published
    // snapshot fills it back in, so it means "the engine has reported something" for
    // every input, not just the ones that carry a symbol clock.
    readonly property bool waiting: metrics.slot1Text.length === 0 && metrics.slot2Text.length === 0

    ColumnLayout {
        anchors.fill: parent
        spacing: 4

        Label {
            Layout.fillWidth: true
            visible: card.waiting
            opacity: 0.7
            text: qsTr("waiting for signal…")
        }

        GridLayout {
            Layout.fillWidth: true
            visible: !card.waiting
            columns: 2
            columnSpacing: 8
            rowSpacing: 2

            // Sample delivery, symbol clock and output rate are all published by the
            // RTL stream, so they describe a tuner too — see the note on the SNR rows.
            Label { text: qsTr("Stream"); visible: metrics.radioInput }
            Label {
                Layout.fillWidth: true
                visible: metrics.radioInput
                font.family: monoFontFamily
                text: (metrics.streamActive ? qsTr("active") : qsTr("idle"))
                      + "  " + metrics.symbolRateHz + " sym/s  " + metrics.outputRateHz + " Hz"
            }

            // Signal quality, carrier lock, frequency offset and tuner gain are
            // properties of a tuner. A PCM feed or a file has none, and no estimator
            // reports on one, so these rows are omitted rather than dashed out: a
            // column of "—" reads as a decoder that is failing to measure, not as one
            // that has nothing to measure.
            Label { text: qsTr("SNR"); visible: metrics.radioInput }
            Label {
                Layout.fillWidth: true
                visible: metrics.radioInput
                font.family: monoFontFamily
                // An estimator that has not reported has no number to show; the C4FM
                // estimator reads nothing on a GFSK stream, for instance.
                text: (metrics.snrValid ? metrics.snrDb.toFixed(1) + " dB" : "—")
                      + "   " + (metrics.carrierLock ? qsTr("lock") : qsTr("no lock"))
                      + "   " + metrics.cfoHz.toFixed(0) + " Hz"
            }

            Label { text: qsTr("Gain"); visible: metrics.radioInput }
            Label {
                Layout.fillWidth: true
                visible: metrics.radioInput
                font.family: monoFontFamily
                text: metrics.tunerGainText.length > 0 ? metrics.tunerGainText : "—"
            }

            Label { text: qsTr("Slot 1") }
            Label {
                Layout.fillWidth: true
                elide: Text.ElideRight
                font.family: monoFontFamily
                text: metrics.slot1Text.length > 0 ? metrics.slot1Text : "—"
            }

            Label { text: qsTr("Slot 2") }
            Label {
                Layout.fillWidth: true
                elide: Text.ElideRight
                font.family: monoFontFamily
                text: metrics.slot2Text.length > 0 ? metrics.slot2Text : "—"
            }
        }

        Label {
            Layout.fillWidth: true
            visible: metrics.messageText.length > 0
            wrapMode: Text.Wrap
            text: metrics.messageText
        }
    }
}
