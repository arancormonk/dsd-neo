// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest

// Issue #525: the Radio sheet's NFM channel width. Under the analog preset it
// shows the width in force as the engine spells it for every frontend -- the
// width the front end reports, "DSP-limited" when the DSP rate bounds it,
// "default" when none is configured -- and its stepper edits the configured
// width through the common channel plans, skipping the ones the running DSP rate
// cannot filter. An explicit width can go back to the unset default, and stays
// editable on a radio under another preset. On PCM input the width cannot act, so
// the stepper is disabled with the reason beside it. The NFM chip selects the
// analog preset. Issue #526: an nfm scan row on air runs the width on any
// session, so the sheet shows it under a digital preset too; a row that sets its
// own width is read first and badged, and the stepper edits the default beneath.
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
            testContext.setMetric("analogBandwidthMaxHz", 0);
            testContext.setMetric("analogBandwidthReading", "");
            testContext.setMetric("analogBandwidthRowActive", false);
            testContext.setMetric("analogBandwidthRowOverride", false);
            testContext.setMetric("radioInput", true);
            if (sheet) {
                sheet.forgetRequests();
                sheet.visible = false;
            }
            testContext.setHostRunning(false);
        }

        // What the engine publishes for an analog session: the width in force, the
        // DSP-limited flag, the configured width (0 = default), with a stream
        // running the widest width its DSP rate filters (0 or left out = not known),
        // and the reading app_control's analog width view spells from those.
        function analogSession(widthHz, limited, configuredHz, maxHz) {
            testContext.setMetric("analogBandwidthHz", widthHz);
            testContext.setMetric("analogBandwidthDspLimited", limited);
            testContext.setMetric("analogBandwidthConfiguredHz", configuredHz);
            testContext.setMetric("analogBandwidthMaxHz", maxHz === undefined ? 0 : maxHz);
            var reading = (widthHz / 1000) + " kHz";
            if (limited)
                reading += " (DSP-limited)";
            else if (configuredHz === 0)
                reading += " (default)";
            testContext.setMetric("analogBandwidthReading", reading);
            testContext.setMetric("decodeMode", analogMode);
            tryVerify(function () { return metrics.decodeMode === analogMode });
        }

        // What the engine publishes while an nfm scan row is on air: the width in
        // force (the row's own when @p rowHz is above 0), the configured width
        // the stepper edits, and the reading app_control spells from them.
        function nfmRowOnAir(widthHz, configuredHz, rowHz, reading) {
            testContext.setMetric("analogBandwidthHz", widthHz);
            testContext.setMetric("analogBandwidthDspLimited", false);
            testContext.setMetric("analogBandwidthConfiguredHz", configuredHz);
            testContext.setMetric("analogBandwidthMaxHz", 42000);
            testContext.setMetric("analogBandwidthReading", reading);
            testContext.setMetric("analogBandwidthRowOverride", rowHz > 0);
            testContext.setMetric("analogBandwidthRowActive", true);
            tryVerify(function () { return metrics.analogBandwidthRowActive === true });
        }

        function test_hidden_outside_the_analog_preset() {
            verify(analogMode >= 0, "-fA maps to a decode preset");
            verify(!findChild(sheet, "radioAnalogSection").visible, "the analog section showed on a digital preset");
        }

        // An explicit width stays editable under another preset on a radio: a
        // switch to NFM is held to it, and where the device or the capture forces
        // a DSP rate that cannot filter it, the refusal says to narrow it first.
        function test_explicit_width_editable_outside_the_analog_preset() {
            testContext.setMetric("analogBandwidthConfiguredHz", 25000);
            tryVerify(function () { return metrics.analogBandwidthConfiguredHz === 25000 });
            verify(metrics.decodeMode !== analogMode);
            verify(findChild(sheet, "radioAnalogSection").visible, "an explicit width outside NFM could not be narrowed");
            // No width is in force outside the preset: the setting stands in.
            compare(findChild(sheet, "radioAnalogBandwidthValue").text, "25 kHz");
            verify(findChild(sheet, "radioAnalogBandwidthIdleNote").visible, "the setting's purpose was not shown");
            findChild(sheet, "radioAnalogBandwidthDown").clicked();
            compare(testContext.lastNfmBandwidthHz(), 20000);
            sheet.forgetRequests();
            var reset = findChild(sheet, "radioAnalogBandwidthDefault");
            verify(reset.visible && reset.enabled);
            reset.clicked();
            compare(testContext.lastNfmBandwidthHz(), 0);
            sheet.forgetRequests();
            // Under the preset the idle note gives way to the reading.
            analogSession(20000, false, 20000);
            verify(!findChild(sheet, "radioAnalogBandwidthIdleNote").visible);
            testContext.setMetric("decodeMode", 1);
            tryVerify(function () { return metrics.decodeMode === 1 });
            // On PCM input the width filters nothing, so it is not offered there.
            testContext.setMetric("radioInput", false);
            tryVerify(function () { return metrics.radioInput === false });
            verify(!findChild(sheet, "radioAnalogSection").visible, "the width was offered on PCM input outside NFM");
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
            // The legacy default below a 20 kHz DSP rate runs no channel filter:
            // the 12 kHz rate itself bounds the channel.
            analogSession(12000, true, 0, 9600);
            compare(findChild(sheet, "radioAnalogBandwidthValue").text, "12 kHz (DSP-limited)");
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
            // The default at a 48 kHz DSP rate: the steps start at 16 kHz.
            analogSession(16000, false, 0, 42000);
            findChild(sheet, "radioAnalogBandwidthUp").clicked();
            compare(testContext.lastNfmBandwidthHz(), 20000);
        }

        // The default below a 20 kHz DSP rate reads the width the rate leaves, and
        // the steps start from there and skip what the rate cannot filter, so no
        // click asks for a width the engine would refuse.
        function test_stepper_steps_within_the_dsp_rate() {
            // 12 kHz DSP rate: nothing wider than 9.6 kHz fits.
            analogSession(12000, true, 0, 9600);
            verify(!findChild(sheet, "radioAnalogBandwidthUp").enabled, "a width the 12 kHz rate cannot filter was offered");
            findChild(sheet, "radioAnalogBandwidthDown").clicked();
            compare(testContext.lastNfmBandwidthHz(), 8000);
            sheet.forgetRequests();
            // 16 kHz DSP rate: 12.5 kHz fits, 20 kHz does not.
            analogSession(16000, true, 0, 13200);
            verify(!findChild(sheet, "radioAnalogBandwidthUp").enabled, "a width the 16 kHz rate cannot filter was offered");
            findChild(sheet, "radioAnalogBandwidthDown").clicked();
            compare(testContext.lastNfmBandwidthHz(), 12500);
            sheet.forgetRequests();
            // An explicit width at a 24 kHz rate: 20 kHz fits, 25 kHz does not.
            analogSession(20000, false, 20000, 20400);
            verify(!findChild(sheet, "radioAnalogBandwidthUp").enabled, "25 kHz was offered at a 24 kHz DSP rate");
            verify(findChild(sheet, "radioAnalogBandwidthDown").enabled);
        }

        // An explicit width can go back to the unset default, which is not the
        // same as 16 kHz: the control sends 0.
        function test_default_returns_to_the_unset_default() {
            analogSession(16000, false, 0);
            verify(!findChild(sheet, "radioAnalogBandwidthDefault").visible, "the default offered itself");
            analogSession(12500, false, 12500);
            var reset = findChild(sheet, "radioAnalogBandwidthDefault");
            verify(reset.visible, "an explicit width offered no way back to the default");
            verify(reset.enabled);
            reset.clicked();
            compare(testContext.lastNfmBandwidthHz(), 0);
            // The request stands in as the setting it is, spelled as every frontend spells it.
            compare(findChild(sheet, "radioAnalogBandwidthValue").text, "default");
            // The request stands in until the engine answers; the default hides the control.
            verify(!reset.visible);
            sheet.forgetRequests();
            // On PCM input it cannot act.
            testContext.setMetric("radioInput", false);
            tryVerify(function () { return metrics.radioInput === false });
            verify(!findChild(sheet, "radioAnalogBandwidthDefault").enabled);
            var calls = testContext.nfmBandwidthCalls();
            sheet.resetAnalogWidth();
            compare(testContext.nfmBandwidthCalls(), calls);
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

        // A digital session scanning an nfm row: the row runs the configured
        // width, so the sheet reads it as the terminal does, not as an idle
        // setting, and the stepper edits it.
        function test_nfm_row_on_a_digital_session_shows_the_width_in_force() {
            verify(metrics.decodeMode !== analogMode);
            nfmRowOnAir(16000, 0, 0, "16 kHz (default)");
            verify(findChild(sheet, "radioAnalogSection").visible, "an nfm row's width was hidden on a digital preset");
            compare(findChild(sheet, "radioAnalogBandwidthValue").text, "16 kHz (default)");
            verify(!findChild(sheet, "radioAnalogBandwidthIdleNote").visible, "the width in force read as idle");
            verify(!findChild(sheet, "radioAnalogBandwidthRowNote").visible, "no row width, no badge");
            findChild(sheet, "radioAnalogBandwidthDown").clicked();
            compare(testContext.lastNfmBandwidthHz(), 12500);
            sheet.forgetRequests();
            // An explicit configured width reads as the width in force, not the setting.
            nfmRowOnAir(20000, 20000, 0, "20 kHz");
            compare(findChild(sheet, "radioAnalogBandwidthValue").text, "20 kHz");
            verify(!findChild(sheet, "radioAnalogBandwidthIdleNote").visible);
            // Once the row leaves, the digital session shows the setting again.
            testContext.setMetric("analogBandwidthRowActive", false);
            testContext.setMetric("analogBandwidthReading", "");
            tryVerify(function () { return metrics.analogBandwidthRowActive === false });
            verify(findChild(sheet, "radioAnalogBandwidthIdleNote").visible);
        }

        // A row that sets its own width owns the reading; the badge says so and
        // names the configured default the stepper changes, which the row's leave
        // returns to. The steps start from that default, never from the row's.
        function test_row_width_reads_first_and_the_stepper_edits_the_default() {
            nfmRowOnAir(12500, 20000, 12500, "12.5 kHz (row; default 20 kHz)");
            compare(findChild(sheet, "radioAnalogBandwidthValue").text, "12.5 kHz");
            verify(findChild(sheet, "radioAnalogBandwidthRowNote").visible);
            verify(findChild(sheet, "radioAnalogBandwidthRowBadge").visible);
            compare(findChild(sheet, "radioAnalogBandwidthRowDefault").text, "default 20 kHz");
            // Read aloud as the terminal prints it.
            compare(findChild(sheet, "radioAnalogBandwidthValue").Accessible.name, "12.5 kHz (row; default 20 kHz)");
            findChild(sheet, "radioAnalogBandwidthUp").clicked();
            compare(testContext.lastNfmBandwidthHz(), 25000);
            // The row still owns the reading; only the named default moved.
            compare(findChild(sheet, "radioAnalogBandwidthValue").text, "12.5 kHz");
            compare(findChild(sheet, "radioAnalogBandwidthRowDefault").text, "default 25 kHz");
            sheet.forgetRequests();
            // The unset default under a row width: named as the width it gives, and
            // stepped from it, not from the row's 12.5 kHz.
            nfmRowOnAir(12500, 0, 12500, "12.5 kHz (row; default 16 kHz)");
            compare(findChild(sheet, "radioAnalogBandwidthRowDefault").text, "default 16 kHz");
            findChild(sheet, "radioAnalogBandwidthDown").clicked();
            compare(testContext.lastNfmBandwidthHz(), 12500);
            sheet.forgetRequests();
            // The same on the analog preset.
            analogSession(12500, false, 0, 42000);
            nfmRowOnAir(12500, 0, 12500, "12.5 kHz (row; default 16 kHz)");
            findChild(sheet, "radioAnalogBandwidthUp").clicked();
            compare(testContext.lastNfmBandwidthHz(), 20000);
            sheet.forgetRequests();
            // On PCM input the width filters nothing, row or not.
            testContext.setMetric("analogBandwidthHz", 0);
            testContext.setMetric("analogBandwidthReading", "not used on PCM input");
            testContext.setMetric("radioInput", false);
            tryVerify(function () { return metrics.radioInput === false });
            compare(findChild(sheet, "radioAnalogBandwidthValue").text, "not used on PCM input");
            verify(!findChild(sheet, "radioAnalogBandwidthRowNote").visible);
        }

        function test_pcm_input_disables_the_stepper_and_says_why() {
            analogSession(16000, false, 0);
            // On PCM input no width is in force: the engine publishes none, and the
            // reading says the width does not apply.
            testContext.setMetric("analogBandwidthHz", 0);
            testContext.setMetric("analogBandwidthReading", "not used on PCM input");
            testContext.setMetric("radioInput", false);
            tryVerify(function () { return metrics.radioInput === false });
            compare(findChild(sheet, "radioAnalogBandwidthValue").text, "not used on PCM input");
            verify(!findChild(sheet, "radioAnalogBandwidthUp").enabled);
            verify(!findChild(sheet, "radioAnalogBandwidthDown").enabled);
            verify(findChild(sheet, "radioAnalogBandwidthNote").visible, "no reason shown for the disabled control");
            var calls = testContext.nfmBandwidthCalls();
            sheet.stepAnalogWidth(1);
            compare(testContext.nfmBandwidthCalls(), calls);
        }
    }
}
