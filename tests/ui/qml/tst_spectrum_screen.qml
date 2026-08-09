// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest

// Tap-to-tune is the one thing on this screen that moves hardware, and the two
// ways it can be wrong are both invisible to a C++ model test: a tap could map
// to the wrong frequency, or the trunking gate could be bound to something that
// never goes false. Both live in binding expressions and handler wiring.
Item {
    id: root

    width: 420
    height: 900

    Loader {
        id: screenLoader

        anchors.fill: parent
        source: uiDir + "/SpectrumScreen.qml"
    }

    TestCase {
        id: tc

        name: "SpectrumScreenTapToTune"
        when: windowShown

        property var area: null

        function initTestCase() {
            verify(screenLoader.item !== null, "SpectrumScreen.qml failed to load")
            tc.area = findChild(screenLoader.item, "spectrumTapArea")
            verify(tc.area !== null, "the gesture area is missing")
        }

        function init() {
            testContext.resetCommands()
            testContext.setMetric("tunerControlled",false)
            // The screen switches production on for itself; wait for the first frame.
            tryVerify(function () { return spectrum.hasData }, 5000,
                      "no spectrum frame arrived")
            spectrum.resetView()
        }

        // x of a frequency in the current view, in the gesture area's coordinates.
        function xOf(hz) {
            return ((hz - spectrum.viewLowHz) / spectrum.viewSpanHz) * tc.area.width
        }

        function test_01_the_screen_reads_the_tuned_frequency() {
            var readout = findChild(screenLoader.item, "spectrumCenterReadout")
            verify(readout !== null, "the frequency readout is missing")
            compare(readout.text, (spectrum.centerFreqHz / 1.0e6).toFixed(4) + " MHz")
        }

        // A fingertip is far wider than a channel, so a tap that lands near a
        // signal has to mean that signal. 18 kHz off is well inside the 25 kHz
        // snap window at 1x, and far enough out that a tap taken literally would
        // land a dozen bins away.
        function test_02_a_tap_beside_a_signal_snaps_onto_it() {
            var peak = testContext.spectrumPeakHz()
            mouseClick(tc.area, tc.xOf(peak - 18000), tc.area.height * 0.75)

            tryVerify(function () { return testContext.manualTuneCalls() === 1 }, 5000,
                      "the tap did not ask for a tune")
            var asked = testContext.lastManualTuneHz()
            // One bin of slack (1.5 kHz at this span) and no more.
            verify(Math.abs(asked - peak) <= 1500,
                   "tuned to " + asked + " but the peak is at " + peak)
        }

        // ...and the snap is a window, not a magnet. A tap on empty spectrum
        // must tune where the finger went down, or a deliberate move to a quiet
        // channel would be dragged back to whatever was loudest nearby.
        function test_02b_a_tap_far_from_a_signal_stays_where_it_landed() {
            var peak = testContext.spectrumPeakHz()
            var quiet = peak - 200000
            mouseClick(tc.area, tc.xOf(quiet), tc.area.height * 0.75)

            tryVerify(function () { return testContext.manualTuneCalls() === 1 }, 5000,
                      "the tap did not ask for a tune")
            var asked = testContext.lastManualTuneHz()
            verify(Math.abs(asked - quiet) <= 4000,
                   "tuned to " + asked + " but the tap was at " + quiet)
        }

        // The engine refuses the tune anyway, but a control that silently does
        // nothing is worse than one that is visibly unavailable. True under
        // trunking and under conventional scanner mode alike — both own the
        // tuner, and the screen is told only that something does.
        function test_03_trunking_makes_the_screen_view_only() {
            testContext.setMetric("tunerControlled",true)
            var pill = findChild(screenLoader.item, "spectrumStatusPill")
            verify(pill !== null, "the status pill is missing")
            tryVerify(function () { return screenLoader.item.viewOnly })

            mouseClick(tc.area, tc.xOf(testContext.spectrumPeakHz()), tc.area.height * 0.75)
            // Asserting an absence, so give the handler the passes it would need.
            tc.wait(50)
            compare(testContext.manualTuneCalls(), 0)

            testContext.setMetric("tunerControlled",false)
            tryVerify(function () { return !screenLoader.item.viewOnly })
        }

        // The monitor's copy of this line is underneath this layer, so a refused
        // tune would otherwise be silent here.
        function test_03b_the_engine_answer_is_visible_on_this_screen() {
            var toast = findChild(screenLoader.item, "spectrumToast")
            verify(toast !== null, "the engine-message line is missing")

            testContext.setMetric("uiMessage", "")
            tryVerify(function () { return !toast.visible })

            testContext.setMetric("uiMessage", "Trunking active: tap-to-tune disabled")
            tryVerify(function () { return toast.visible })

            testContext.setMetric("uiMessage", "")
            tryVerify(function () { return !toast.visible })
        }

        // Panning inside the span is free; only a pan that ran out of capture
        // bandwidth asks the hardware to move, and only once, on release.
        function test_04_panning_inside_the_span_never_retunes() {
            spectrum.zoom = 4.0
            spectrum.viewOffsetHz = 100000
            compare(spectrum.edgeOvershootHz, 0)
            tc.wait(50)
            compare(testContext.manualTuneCalls(), 0)
        }

        function test_05_panning_past_the_edge_reports_the_overshoot() {
            spectrum.zoom = 1.0
            spectrum.viewOffsetHz = 400000
            // Fully zoomed out there is nowhere to go, so all of it is overshoot.
            verify(spectrum.edgeOvershootHz > 0)
            spectrum.clearOvershoot()
            compare(spectrum.edgeOvershootHz, 0)
        }

        // A pinch that moves the signal out from under the fingers reads as
        // broken, so the anchor frequency has to survive the zoom.
        function test_07_a_pinch_zooms_around_its_anchor() {
            var screen = screenLoader.item
            spectrum.resetView()
            screen.pinchStartZoom = spectrum.zoom
            screen.pinchAnchorX = 0.25
            var anchorHz = spectrum.viewLowHz + (0.25 * spectrum.viewSpanHz)

            screen.applyPinch(4.0)

            compare(spectrum.zoom, 4.0)
            var afterHz = spectrum.viewLowHz + (0.25 * spectrum.viewSpanHz)
            verify(Math.abs(afterHz - anchorHz) < 1.0,
                   "the anchor moved from " + anchorHz + " to " + afterHz)

            // And the model owns the limit, whatever the fingers ask for.
            screen.applyPinch(100.0)
            compare(spectrum.zoom, 8.0)
            screen.applyPinch(0.001)
            compare(spectrum.zoom, 1.0)
            spectrum.resetView()
        }

        // A drag is free while it stays inside the capture span, and asks for
        // exactly one retune when it runs off the edge — on release, not per frame.
        function test_08_a_drag_retunes_once_and_only_off_the_edge() {
            var screen = screenLoader.item
            spectrum.zoom = 4.0
            screen.panStartOffsetHz = spectrum.viewOffsetHz

            // Well inside the span: many drag frames, no retune.
            for (var i = 1; i <= 5; i++)
                screen.applyPan(-i * 4, 400)
            verify(spectrum.edgeOvershootHz === 0, "an interior drag reported overshoot")
            screen.endPan(false)
            compare(testContext.manualTuneCalls(), 0)

            // Off the edge: still no retune until the finger lifts.
            screen.panStartOffsetHz = spectrum.viewOffsetHz
            for (var j = 1; j <= 5; j++)
                screen.applyPan(-j * 400, 400)
            verify(spectrum.edgeOvershootHz > 0, "a drag off the edge reported no overshoot")
            compare(testContext.manualTuneCalls(), 0)

            screen.endPan(false)
            compare(testContext.manualTuneCalls(), 1)
            compare(spectrum.edgeOvershootHz, 0)
            spectrum.resetView()
        }

        // The retune has to land where the finger was heading. Zoomed in, the
        // viewport is already carried toward one end of the span before it runs
        // out of capture, so tuning to the overshoot alone would land short and
        // snap the view backwards, away from the band being dragged toward.
        function test_08b_an_edge_drag_tunes_to_where_the_view_was_asking_to_be() {
            var screen = screenLoader.item
            spectrum.resetView()
            spectrum.zoom = 4.0
            screen.panStartOffsetHz = spectrum.viewOffsetHz

            screen.applyPan(-2000, 400)
            var granted = spectrum.viewOffsetHz
            var overshoot = spectrum.edgeOvershootHz
            verify(granted > 0, "the drag was fully absorbed by the overshoot")
            verify(overshoot > 0, "the drag never reached the edge")

            screen.endPan(false)
            compare(testContext.manualTuneCalls(), 1)
            var want = spectrum.centerFreqHz + granted + overshoot
            verify(Math.abs(testContext.lastManualTuneHz() - want) <= 1,
                   "tuned to " + testContext.lastManualTuneHz() + " but the view was asking for " + want)
            spectrum.resetView()
        }

        // A pinch takes the drag handler's grab, which deactivates it exactly as
        // a release does. The drift before the second finger landed is not a
        // request to move the receiver — and at 1x every drift is overshoot, so
        // acting on it would retune on the way into every zoom.
        function test_08c_a_pinch_stealing_the_drag_never_retunes() {
            var screen = screenLoader.item
            spectrum.resetView()
            screen.panStartOffsetHz = spectrum.viewOffsetHz

            // A few pixels of drift at 1x, where there is nowhere to pan.
            screen.applyPan(-20, 400)
            verify(spectrum.edgeOvershootHz !== 0, "a 1x drag reported no overshoot")

            screen.endPan(true)
            compare(testContext.manualTuneCalls(), 0)
            compare(spectrum.edgeOvershootHz, 0)
            spectrum.resetView()
        }

        // The axis has to follow the viewport, not freeze on the first frame.
        function test_06_the_axis_labels_follow_the_zoom() {
            spectrum.resetView()
            var wide = spectrum.axisTicks(5)
            verify(wide.length > 0, "no axis labels at 1x")

            spectrum.zoom = 8.0
            var tight = spectrum.axisTicks(5)
            verify(tight.length > 0, "no axis labels at 8x")
            // A narrower window means finer steps between labels.
            verify(tight[0].freqHz !== wide[0].freqHz || tight.length !== wide.length,
                   "the axis did not change with the zoom")
            spectrum.resetView()
        }
    }
}
