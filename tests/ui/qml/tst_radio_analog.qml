// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest

// Issue #525: the Radio sheet's NFM channel width. Under the analog preset it
// shows the width in force -- the one the engine reports, "DSP-limited" when the
// DSP rate bounds it, "default" when none is configured -- and its stepper edits
// the configured width through the common channel plans. On PCM input the width
// cannot act, so the stepper is disabled with the reason beside it. The NFM chip
// selects the analog preset.
Item {
    width: 420
    height: 1100

    Loader {
        id: loader
        anchors.fill: parent
        source: uiDir + "/RadioSheet.qml"
    }

    TestCase {
        name: "RadioAnalog"
        when: windowShown

        readonly property var sheet: loader.item
        // DSDCFG_MODE_ANALOG, through the production flag mapping.
        readonly property int analogMode: commands.decodeModeForFlag("-fA")

        function init() {
            verify(sheet !== null, "RadioSheet must load");
            testContext.setHostRunning(true);
            testContext.resetCommands();
            testContext.setMetric("radioInput", true);
            sheet.open();
        }

        function cleanup() {
            testContext.setMetric("decodeMode", 1);
            testContext.setMetric("analogBandwidthHz", 0);
            testContext.setMetric("analogBandwidthDspLimited", false);
            testContext.setMetric("analogBandwidthConfiguredHz", 0);
            testContext.setMetric("radioInput", true);
            if (sheet) {
                sheet.forgetRequests();
                sheet.visible = false;
            }
            testContext.setHostRunning(false);
        }

        // What the engine publishes for an analog session: the width in force, the
        // DSP-limited flag and the configured width (0 = default).
        function analogSession(widthHz, limited, configuredHz) {
            testContext.setMetric("analogBandwidthHz", widthHz);
            testContext.setMetric("analogBandwidthDspLimited", limited);
            testContext.setMetric("analogBandwidthConfiguredHz", configuredHz);
            testContext.setMetric("decodeMode", analogMode);
            tryVerify(function () { return metrics.decodeMode === analogMode });
        }

        function test_hidden_outside_the_analog_preset() {
            verify(analogMode >= 0, "-fA maps to a decode preset");
            verify(!findChild(sheet, "radioAnalogSection").visible, "the analog section showed on a digital preset");
        }

        function test_nfm_chip_selects_the_analog_preset() {
            var chip = findChild(sheet, "radioDecode_NFM");
            verify(chip !== null, "the NFM chip is missing");
            verify(!chip.selected, "NFM showed as chosen on the auto preset");
            chip.clicked();
            compare(testContext.lastDecodeMode(), analogMode);
            analogSession(16000, false, 0);
            verify(chip.selected, "the NFM chip does not follow the engine's analog preset");
        }

        function test_reading_agrees_with_the_engine() {
            analogSession(12500, false, 12500);
            verify(findChild(sheet, "radioAnalogSection").visible);
            compare(findChild(sheet, "radioAnalogBandwidthValue").text, "12.5 kHz");
            analogSession(16000, false, 0);
            compare(findChild(sheet, "radioAnalogBandwidthValue").text, "16 kHz (default)");
            // The legacy default below a 20 kHz DSP rate: the rate bounds the channel.
            analogSession(10800, true, 0);
            compare(findChild(sheet, "radioAnalogBandwidthValue").text, "10.8 kHz (DSP-limited)");
        }

        function test_stepper_edits_the_configured_width() {
            analogSession(12500, false, 12500);
            findChild(sheet, "radioAnalogBandwidthUp").clicked();
            compare(testContext.lastNfmBandwidthHz(), 16000);
            // The request stands in for the reading until the engine answers.
            compare(findChild(sheet, "radioAnalogBandwidthValue").text, "16 kHz");
            sheet.forgetRequests();
            findChild(sheet, "radioAnalogBandwidthDown").clicked();
            compare(testContext.lastNfmBandwidthHz(), 11250);
            sheet.forgetRequests();
            // From the default, whatever the DSP rate leaves: the steps start at 16 kHz.
            analogSession(10800, true, 0);
            findChild(sheet, "radioAnalogBandwidthUp").clicked();
            compare(testContext.lastNfmBandwidthHz(), 20000);
        }

        function test_stepper_stops_at_the_ends_of_the_range() {
            analogSession(25000, false, 25000);
            verify(!findChild(sheet, "radioAnalogBandwidthUp").enabled, "a step past 25 kHz was offered");
            var calls = testContext.nfmBandwidthCalls();
            sheet.stepAnalogWidth(1);
            compare(testContext.nfmBandwidthCalls(), calls);
            analogSession(8000, false, 8000);
            verify(!findChild(sheet, "radioAnalogBandwidthDown").enabled, "a step below 8 kHz was offered");
        }

        function test_pcm_input_disables_the_stepper_and_says_why() {
            analogSession(16000, false, 0);
            testContext.setMetric("radioInput", false);
            tryVerify(function () { return metrics.radioInput === false });
            verify(!findChild(sheet, "radioAnalogBandwidthUp").enabled);
            verify(!findChild(sheet, "radioAnalogBandwidthDown").enabled);
            verify(findChild(sheet, "radioAnalogBandwidthNote").visible, "no reason shown for the disabled control");
            var calls = testContext.nfmBandwidthCalls();
            sheet.stepAnalogWidth(1);
            compare(testContext.nfmBandwidthCalls(), calls);
        }
    }
}
