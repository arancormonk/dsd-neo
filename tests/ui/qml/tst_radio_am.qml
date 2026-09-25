// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest

// Issue #524: the Radio sheet's AM decode chip. On a radio input it selects the
// AM preset (-fM) and reads back as selected once the engine reports that mode.
// On audio that arrives already demodulated the engine refuses AM, so the chip is
// not offered there, and a note says why rather than leaving a dead control.
//
// Under the AM preset the sheet's analog channel width section edits the AM
// width: a stepper over the common AM steps that skips what the DSP rate cannot
// filter, a way back to the default, and the width in force as app_control's
// analog width view spells it for every frontend (the same section the NFM
// width uses under -fA).
//
// An explicit width of the kind the configured preset does not run (AM under
// -fA or a digital mode, NFM under -fM) keeps a control of its own on a radio:
// a switch between the kinds is held to it, and where the device forces a DSP
// rate that cannot filter it, the refusal says to narrow it before the switch.
Item {
    width: 420
    height: 900

    Loader {
        id: loader
        anchors.fill: parent
        source: uiDir + "/RadioSheet.qml"
    }

    TestCase {
        name: "RadioAm"
        when: windowShown

        readonly property var sheet: loader.item

        function init() {
            verify(sheet !== null, "RadioSheet must load");
            testContext.setHostRunning(true);
            testContext.resetCommands();
            sheet.open();
        }

        function cleanup() {
            testContext.setMetric("radioInput", true);
            testContext.setMetric("decodeMode", 1);
            testContext.setMetric("analogBandwidthHz", 0);
            testContext.setMetric("analogBandwidthDspLimited", false);
            testContext.setMetric("analogBandwidthConfiguredHz", 0);
            testContext.setMetric("nfmBandwidthConfiguredHz", 0);
            testContext.setMetric("amBandwidthConfiguredHz", 0);
            testContext.setMetric("analogBandwidthMaxHz", 0);
            testContext.setMetric("analogBandwidthReading", "");
            if (sheet) {
                sheet.forgetRequests();
                sheet.visible = false;
            }
            testContext.setHostRunning(false);
        }

        function test_am_chip_selects_the_am_preset() {
            var chip = findChild(sheet, "radioDecode_AM");
            verify(chip !== null, "the Radio sheet offers no AM chip");
            verify(chip.enabled, "the AM chip is disabled on a radio input");
            verify(!findChild(sheet, "radioDecodeIqNote").visible, "the PCM note shows on a radio input");
            verify(!chip.selected);
            chip.clicked();
            compare(testContext.lastDecodeMode(), commands.decodeModeForFlag("-fM"));
            verify(testContext.lastDecodeMode() > 0, "-fM names no preset");
            testContext.setMetric("decodeMode", commands.decodeModeForFlag("-fM"));
            tryVerify(function () { return chip.selected });
        }

        function test_am_chip_is_not_offered_on_pcm_input() {
            testContext.setMetric("radioInput", false);
            tryVerify(function () { return metrics.radioInput === false });
            var chip = findChild(sheet, "radioDecode_AM");
            verify(chip !== null);
            verify(!chip.enabled, "the AM chip is offered on PCM input");
            verify(findChild(sheet, "radioDecodeIqNote").visible, "no note says why AM is unavailable");
            verify(findChild(sheet, "radioDecode_DMR").enabled, "the digital chips must stay available");
        }

        /* The sheet on the AM preset, as the engine publishes it: the width in force, the configured width (0 = the
           default), the widest width the DSP rate filters (0 = not known) and the view's reading of them. AM's
           default is never DSP-limited. */
        function onAm(widthHz, configuredHz, maxHz) {
            testContext.setMetric("analogBandwidthHz", widthHz);
            testContext.setMetric("analogBandwidthConfiguredHz", configuredHz);
            testContext.setMetric("amBandwidthConfiguredHz", configuredHz);
            testContext.setMetric("analogBandwidthMaxHz", maxHz === undefined ? 0 : maxHz);
            testContext.setMetric("analogBandwidthReading",
                                  (widthHz / 1000) + " kHz" + (configuredHz === 0 ? " (default)" : ""));
            testContext.setMetric("decodeMode", commands.decodeModeForFlag("-fM"));
            tryVerify(function () { return findChild(sheet, "radioAnalogSection").visible });
        }

        function valueText() {
            return findChild(sheet, "radioAnalogBandwidthValue").text;
        }

        function test_am_width_section_under_the_am_preset() {
            verify(!findChild(sheet, "radioAnalogSection").visible, "the width shows on a digital preset");
            testContext.setMetric("analogBandwidthHz", 16000);
            testContext.setMetric("analogBandwidthReading", "16 kHz (default)");
            testContext.setMetric("decodeMode", commands.decodeModeForFlag("-fA"));
            tryVerify(function () { return findChild(sheet, "radioAnalogSection").visible });
            compare(findChild(sheet, "radioAnalogSectionTitle").text, "NFM channel width");
            onAm(6000, 0);
            compare(findChild(sheet, "radioAnalogSectionTitle").text, "AM channel width");
            compare(valueText(), "6 kHz (default)");
        }

        function test_am_width_steps_through_the_common_widths() {
            onAm(6000, 0);
            findChild(sheet, "radioAnalogBandwidthUp").clicked();
            compare(testContext.lastAmBandwidthHz(), 8000);
            compare(testContext.nfmBandwidthCalls(), 0, "the AM step went to the NFM command");
            compare(valueText(), "8 kHz", "the request stands in for the reading");
            sheet.forgetRequests();
            onAm(8000, 8000);
            tryCompare(findChild(sheet, "radioAnalogBandwidthValue"), "text", "8 kHz");
            findChild(sheet, "radioAnalogBandwidthDown").clicked();
            compare(testContext.lastAmBandwidthHz(), 6000);
            findChild(sheet, "radioAnalogBandwidthDown").clicked();
            compare(testContext.lastAmBandwidthHz(), 5000, "a second step goes on from the first");
            compare(testContext.amBandwidthCalls(), 3);
            verify(!findChild(sheet, "radioAnalogBandwidthDown").enabled, "5 kHz is the narrowest AM width");
        }

        function test_am_width_refused_request_expires_back_to_the_reading() {
            onAm(6000, 0);
            findChild(sheet, "radioAnalogBandwidthUp").clicked();
            compare(valueText(), "8 kHz");
            // The engine refused it: the reading never moves, and the request expires back to it.
            tryCompare(findChild(sheet, "radioAnalogBandwidthValue"), "text", "6 kHz (default)", 4000);
        }

        function test_am_width_skips_what_the_dsp_rate_cannot_filter() {
            // A 16 kHz DSP rate filters at most 13.2 kHz: from 10 kHz, 15 and 20 kHz are refused.
            onAm(10000, 10000, 13200);
            tryVerify(function () { return !findChild(sheet, "radioAnalogBandwidthUp").enabled });
            findChild(sheet, "radioAnalogBandwidthUp").clicked();
            compare(testContext.amBandwidthCalls(), 0, "a step the rate refuses was sent");
            verify(findChild(sheet, "radioAnalogBandwidthDown").enabled);
        }

        function test_am_width_back_to_the_default() {
            onAm(6000, 0);
            verify(!findChild(sheet, "radioAnalogBandwidthDefault").visible, "the default offered while in use");
            onAm(8000, 8000);
            tryVerify(function () { return findChild(sheet, "radioAnalogBandwidthDefault").visible });
            findChild(sheet, "radioAnalogBandwidthDefault").clicked();
            compare(testContext.lastAmBandwidthHz(), 0, "the default is sent as 0, not as 6000");
            compare(testContext.nfmBandwidthCalls(), 0);
        }

        function otherSection() {
            return findChild(sheet, "radioAnalogOtherSection");
        }

        function test_explicit_am_width_offered_under_nfm() {
            testContext.setMetric("analogBandwidthHz", 16000);
            testContext.setMetric("analogBandwidthReading", "16 kHz (default)");
            testContext.setMetric("decodeMode", commands.decodeModeForFlag("-fA"));
            tryVerify(function () { return findChild(sheet, "radioAnalogSection").visible });
            verify(!otherSection().visible, "an AM width showed with none set");
            testContext.setMetric("amBandwidthConfiguredHz", 20000);
            tryVerify(function () { return otherSection().visible }, 2000,
                      "an explicit AM width under NFM could not be narrowed");
            compare(findChild(sheet, "radioAnalogSectionTitle").text, "NFM channel width");
            compare(findChild(sheet, "radioAnalogOtherSectionTitle").text, "AM channel width");
            compare(findChild(sheet, "radioAnalogOtherBandwidthValue").text, "20 kHz");
            verify(findChild(sheet, "radioAnalogOtherBandwidthIdleNote").visible);
            verify(!findChild(sheet, "radioAnalogOtherBandwidthUp").enabled, "20 kHz is the widest AM width");
            findChild(sheet, "radioAnalogOtherBandwidthDown").clicked();
            compare(testContext.lastAmBandwidthHz(), 15000);
            compare(testContext.nfmBandwidthCalls(), 0, "the AM step went to the NFM command");
            compare(findChild(sheet, "radioAnalogOtherBandwidthValue").text, "15 kHz", "the request stands in");
            sheet.forgetRequests();
            findChild(sheet, "radioAnalogOtherBandwidthDefault").clicked();
            compare(testContext.lastAmBandwidthHz(), 0, "the default is sent as 0");
            sheet.forgetRequests();
            // The DSP rate bounds its steps as it bounds the section's.
            testContext.setMetric("amBandwidthConfiguredHz", 10000);
            testContext.setMetric("analogBandwidthMaxHz", 13200);
            tryVerify(function () { return !findChild(sheet, "radioAnalogOtherBandwidthUp").enabled });
            // Under a digital preset too, and never on PCM input.
            testContext.setMetric("decodeMode", 1);
            tryVerify(function () { return !findChild(sheet, "radioAnalogSection").visible });
            verify(otherSection().visible, "an explicit AM width under a digital mode could not be narrowed");
            testContext.setMetric("radioInput", false);
            tryVerify(function () { return !otherSection().visible });
        }

        function test_explicit_nfm_width_offered_under_am() {
            onAm(6000, 0);
            verify(!otherSection().visible, "an NFM width showed with none set");
            testContext.setMetric("nfmBandwidthConfiguredHz", 25000);
            tryVerify(function () { return otherSection().visible }, 2000,
                      "an explicit NFM width under AM could not be narrowed");
            compare(findChild(sheet, "radioAnalogSectionTitle").text, "AM channel width");
            compare(findChild(sheet, "radioAnalogOtherSectionTitle").text, "NFM channel width");
            compare(findChild(sheet, "radioAnalogOtherBandwidthValue").text, "25 kHz");
            findChild(sheet, "radioAnalogOtherBandwidthDown").clicked();
            compare(testContext.lastNfmBandwidthHz(), 20000);
            compare(testContext.amBandwidthCalls(), 0, "the NFM step went to the AM command");
        }

        function test_am_width_disabled_off_a_radio() {
            onAm(6000, 0);
            testContext.setMetric("radioInput", false);
            tryVerify(function () { return !findChild(sheet, "radioAnalogBandwidthUp").enabled });
            verify(!findChild(sheet, "radioAnalogBandwidthDown").enabled);
            verify(findChild(sheet, "radioAnalogBandwidthNote").visible, "no note says why the width is disabled");
            findChild(sheet, "radioAnalogBandwidthUp").clicked();
            compare(testContext.amBandwidthCalls(), 0);
        }
    }
}
