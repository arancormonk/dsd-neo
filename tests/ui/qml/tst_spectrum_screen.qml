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
            testContext.setMetric("trunkingEnabled",false)
            testContext.setMetric("scannerMode",false)
            testContext.setMetric("syncedHere",false)
            // Most cases are about what exploring can do; the screen defaults to
            // the other intent, where none of it is available.
            screenLoader.item.exploring = true
            screenLoader.item.hint = ""
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

        // The other reason, and the one this screen decides for itself: a session
        // started from a saved system is a session about that system. Nothing owns
        // the tuner and the engine would accept the tune — the refusal is the
        // product's, so nothing but this binding enforces it.
        function test_03c_a_saved_system_session_is_view_only_with_nothing_holding_the_tuner() {
            var screen = screenLoader.item
            screen.exploring = false
            verify(screen.viewOnly, "a saved-system session offered tap-to-tune")
            verify(!screen.tunerHeld, "the fixture was not left with a free tuner")

            mouseClick(tc.area, tc.xOf(testContext.spectrumPeakHz()), tc.area.height * 0.75)
            tc.wait(50)
            compare(testContext.manualTuneCalls(), 0)

            // And the controls that only make sense while exploring are not there.
            var tuning = findChild(screen, "spectrumTuning")
            verify(tuning !== null, "the tuning row is missing entirely")
            verify(!tuning.visible, "a saved-system session showed the tuning controls")

            screen.exploring = true
            verify(!screen.viewOnly)
            tryVerify(function () { return tuning.visible })
        }

        // The way out of view-only. Nothing is holding the tuner here, so there is
        // nothing to warn about and the ask goes straight through.
        function test_03d_explore_from_here_asks_for_the_tuner() {
            var screen = screenLoader.item
            screen.exploring = false
            var button = findChild(screen, "spectrumExploreFromHere")
            verify(button !== null, "the explore-from-here button is missing")
            tryVerify(function () { return button.visible })

            var asked = 0
            screen.exploreFromHere.connect(function () { asked++ })
            button.clicked()

            compare(testContext.releaseTunerCalls(), 0, "nothing held the tuner; nothing to release")
            compare(asked, 1)
            screen.exploring = true
        }

        // With a controller holding the tuner, taking it costs something the user
        // did not ask for — following calls across channels stops — so it is named
        // and confirmed rather than done on the tap.
        function test_03e_taking_the_tuner_from_a_controller_is_confirmed_first() {
            var screen = screenLoader.item
            screen.exploring = false
            testContext.setMetric("tunerControlled",true)
            testContext.setMetric("trunkingEnabled",true)
            tryVerify(function () { return screen.tunerHeld })

            var asked = 0
            screen.exploreFromHere.connect(function () { asked++ })

            var button = findChild(screen, "spectrumExploreFromHere")
            button.clicked()
            var sheet = findChild(screen, "spectrumExploreConfirm")
            verify(sheet !== null, "the confirm sheet is missing")
            tryVerify(function () { return sheet.visible }, 2000, "the tap did not ask first")
            compare(asked, 0, "the tuner was taken before the user confirmed")

            findChild(screen, "spectrumExploreConfirmAccept").clicked()
            tryVerify(function () { return !sheet.visible })
            compare(asked, 1)

            testContext.setMetric("tunerControlled",false)
            testContext.setMetric("trunkingEnabled",false)
            screen.exploring = true
        }

        // The way *into* view-only, and the inverse of the two cases above: having
        // found a control channel by hand, hand the tuner to trunking and let it
        // follow the system from there. Offered only where trunking has something
        // to follow — a lock on M17 is still a lock, and a button that does
        // nothing teaches the user that the app is broken.
        function test_03f_follow_this_system_hands_the_tuner_to_trunking() {
            var screen = screenLoader.item
            var button = findChild(screen, "spectrumFollowSystem")
            verify(button !== null, "the follow-this-system button is missing")

            // Exploring, but parked on noise: nothing to follow yet.
            screen.exploring = true
            testContext.setMetric("trunkableSync",false)
            tryVerify(function () { return !button.visible },
                      2000, "trunking was offered with nothing to follow")

            // A lock on a trunked protocol is what makes the offer real.
            testContext.setMetric("trunkableSync",true)
            tryVerify(function () { return button.visible },
                      2000, "a control channel did not bring up the offer")

            button.clicked()
            compare(testContext.setTrunkingCalls(), 1)
            compare(testContext.lastSetTrunking(), true, "the button asked to stop trunking")

            // And it is gone the moment there is nothing left to ask for: on a
            // saved system the tuner was never the user's to give, and once
            // trunking holds it the escape hatch is the only thing left to offer.
            screen.exploring = false
            tryVerify(function () { return !button.visible },
                      2000, "a saved-system session offered to start trunking")

            screen.exploring = true
            testContext.setMetric("tunerControlled",true)
            tryVerify(function () { return !button.visible },
                      2000, "trunking was offered while something already held the tuner")

            testContext.setMetric("tunerControlled",false)
            testContext.setMetric("trunkableSync",false)
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

        // Tapping only reaches what is already on screen. Getting anywhere else
        // means walking the band, and a step that did not overlap would hide a
        // signal sitting on the seam from both screens it straddles.
        function test_09_a_step_moves_one_overlapping_screenful() {
            var screen = screenLoader.item
            var from = spectrum.centerFreqHz

            screen.stepBy(1)
            compare(testContext.manualTuneCalls(), 1)
            var up = testContext.lastManualTuneHz()
            verify(up > from, "stepping up went down")
            verify(Math.abs((up - from) - (spectrum.spanHz * 0.9)) <= 1,
                   "stepped " + (up - from) + " Hz, not one overlapping span")

            testContext.resetCommands()
            screen.stepBy(-1)
            compare(testContext.manualTuneCalls(), 1)
            verify(Math.abs((from - testContext.lastManualTuneHz()) - (spectrum.spanHz * 0.9)) <= 1)
        }

        // Walking off the top of a band leads into spectrum this app decodes
        // nothing in, so the walk comes back round instead. The receiver cannot be
        // driven to a band edge from here, which is exactly why the arithmetic is
        // its own function.
        function test_10_stepping_wraps_inside_the_band() {
            var screen = screenLoader.item
            var band = screen.band
            verify(band !== null, "the fixture centre is outside every known band")
            var step = spectrum.spanHz * 0.9

            // Mid-band, a step is just a step.
            var mid = (band.low + band.high) / 2
            verify(Math.abs(screen.nextStepHz(mid, 1) - (mid + step)) <= 1)
            verify(Math.abs(screen.nextStepHz(mid, -1) - (mid - step)) <= 1)

            // At the top, it comes back to the bottom — and lands inside the band
            // rather than exactly on its edge, so the first screenful is spectrum.
            var top = screen.nextStepHz(band.high - (step / 2), 1)
            verify(top < band.high, "stepped past the top of the band to " + top)
            verify(top > band.low, "wrapped onto the very edge of the band")
            verify(top < mid, "the wrap did not come back round")

            var bottom = screen.nextStepHz(band.low + (step / 2), -1)
            verify(bottom > band.low, "stepped below the bottom of the band to " + bottom)
            verify(bottom < band.high)
            verify(bottom > mid, "the downward wrap did not come back round")

            // Off-band — 250 MHz is in none of them — there is nothing to wrap
            // against, and a walk that snapped into some other band would move the
            // radio somewhere it was never pointed.
            var away = 250.0e6
            verify(Math.abs(screen.nextStepHz(away, 1) - (away + step)) <= 1)
            verify(Math.abs(screen.nextStepHz(away, -1) - (away - step)) <= 1)
        }

        // The sweep is stepping repeated until something is found; the finding is
        // the whole point, so it has to stop on it rather than walk past.
        function test_11_a_sweep_steps_until_the_decoder_finds_something() {
            var screen = screenLoader.item
            screen.startSweep()
            verify(screen.sweeping, "the sweep did not start")

            screen.sweepTick()
            compare(testContext.manualTuneCalls(), 1, "a dwell with nothing found did not move on")
            verify(screen.sweeping)

            testContext.setMetric("syncedHere",true)
            tryVerify(function () { return metrics.syncedHere })
            screen.sweepTick()
            verify(!screen.sweeping, "the sweep walked past a signal")
            compare(testContext.manualTuneCalls(), 1, "the sweep moved off what it found")
            verify(screen.hint.length > 0, "the sweep stopped without saying why")

            testContext.setMetric("syncedHere",false)
        }

        // Touching the spectrum is taking over. A sweep still running underneath
        // would move the radio off whatever was just chosen, seconds later.
        function test_12_touching_the_spectrum_stops_a_sweep() {
            var screen = screenLoader.item
            screen.startSweep()
            verify(screen.sweeping)

            mouseClick(tc.area, tc.xOf(testContext.spectrumPeakHz()), tc.area.height * 0.75)
            tryVerify(function () { return !screen.sweeping }, 2000, "the sweep survived a tap")

            // As does losing the right to tune at all.
            screen.startSweep()
            verify(screen.sweeping)
            testContext.setMetric("tunerControlled",true)
            tryVerify(function () { return !screen.sweeping }, 2000,
                      "the sweep survived the tuner being taken")
            testContext.setMetric("tunerControlled",false)
        }

        // Hopping to the next carrier, and — the part that is easy to get wrong —
        // doing nothing at all when there is not one.
        function test_13_the_signal_hop_lands_on_a_carrier_or_says_it_cannot() {
            var screen = screenLoader.item
            var peak = testContext.spectrumPeakHz()
            var direction = peak > spectrum.centerFreqHz ? 1 : -1

            screen.hopToSignal(direction)
            compare(testContext.manualTuneCalls(), 1, "the hop asked for nothing")
            verify(Math.abs(testContext.lastManualTuneHz() - peak) <= 4000,
                   "hopped to " + testContext.lastManualTuneHz() + " but the peak is at " + peak)

            // The other way there is only noise, and a hop that fell back to the
            // current centre would retune the radio to where it already is.
            testContext.resetCommands()
            screen.hopToSignal(-direction)
            compare(testContext.manualTuneCalls(), 0, "an empty band still moved the radio")
            verify(screen.hint.length > 0, "an empty band said nothing")
        }

        // Typing a frequency is how someone leaves the neighbourhood entirely.
        function test_14_go_to_tunes_to_a_typed_frequency() {
            var screen = screenLoader.item
            var sheet = findChild(screen, "spectrumGoToSheet")
            verify(sheet !== null, "the go-to sheet is missing")

            sheet.open(spectrum.centerFreqHz)
            verify(sheet.visible)

            findChild(screen, "spectrumGoToField").text = "154.0000"
            findChild(screen, "spectrumGoToConfirm").clicked()

            verify(!sheet.visible, "the sheet stayed open after tuning")
            compare(testContext.manualTuneCalls(), 1)
            compare(testContext.lastManualTuneHz(), 154000000)
        }

        // The engine reports every step of a sweep with a three-second message; at
        // this cadence the line would never clear and would read as a fault.
        function test_15_the_engine_message_is_hidden_while_sweeping() {
            var screen = screenLoader.item
            var toast = findChild(screen, "spectrumToast")
            testContext.setMetric("uiMessage", "Applied: tuned -> 851000000 Hz")
            tryVerify(function () { return toast.visible })

            screen.startSweep()
            tryVerify(function () { return !toast.visible }, 2000,
                      "the per-step message stayed up through the sweep")

            screen.stopSweep()
            testContext.setMetric("uiMessage", "")
        }

        // A waterfall shows where energy is and says nothing about whether any of
        // it is being decoded. On a band being swept that is the only question,
        // and "which protocol" is the answer to why a real signal is silent.
        function test_16_the_strip_says_whether_the_decoder_is_locked() {
            var screen = screenLoader.item
            var label = findChild(screen, "spectrumSyncLabel")
            verify(label !== null, "the sync label is missing")

            testContext.setMetric("syncedHere",false)
            tryVerify(function () { return label.text === "NO SYNC" },
                      2000, "an unlocked decoder did not say so")

            testContext.setMetric("syncLabel","DMR")
            testContext.setMetric("syncedHere",true)
            tryVerify(function () { return label.text === "DMR" },
                      2000, "a locked decoder did not name what it locked to")

            testContext.setMetric("syncedHere",false)
            testContext.setMetric("syncLabel","")
        }

        // A call in progress is the payoff, and it belongs next to the lock that
        // produced it rather than on a screen the user has to leave this one for.
        function test_17_the_strip_shows_a_call_in_progress() {
            var screen = screenLoader.item
            var call = findChild(screen, "spectrumCallLabel")
            verify(call !== null, "the call label is missing")
            verify(!call.visible, "an idle screen showed a call")

            testContext.setMetric("slot1TgText","1201")
            testContext.setMetric("slot1CallState",2)
            tryVerify(function () { return call.visible && call.text === "1201" },
                      2000, "an active call did not reach the strip")

            // A control channel opens call epochs whose target has not decoded
            // yet. Rendering that as talkgroup 0 would name a transmission
            // nobody is making.
            testContext.setMetric("slot1TgText","0")
            tryVerify(function () { return !call.visible }, 2000,
                      "an unidentified call was shown as talkgroup 0")

            testContext.setMetric("slot1CallState",0)
            testContext.setMetric("slot1TgText","")
            tryVerify(function () { return !call.visible })
        }

        // A standing frequency offset is the symptom of a PPM correction that does
        // not match this dongle — the one setting nobody can verify when they type
        // it. Shown only once something is locked, because before that it is noise.
        function test_17b_the_strip_shows_a_frequency_offset_worth_acting_on() {
            var screen = screenLoader.item
            var cfo = findChild(screen, "spectrumCfoLabel")
            verify(cfo !== null, "the offset label is missing")

            testContext.setMetric("syncedHere",false)
            testContext.setMetric("cfoHz",900.0)
            tryVerify(function () { return !cfo.visible }, 2000,
                      "an offset was shown with nothing locked")

            testContext.setMetric("syncedHere",true)
            tryVerify(function () { return cfo.visible && cfo.text === "900 Hz" }, 2000,
                      "a large offset was not shown")

            // A few Hz is normal and is not worth a reading.
            testContext.setMetric("cfoHz",4.0)
            tryVerify(function () { return !cfo.visible }, 2000,
                      "a negligible offset was still reported")

            testContext.setMetric("cfoHz",0.0)
            testContext.setMetric("syncedHere",false)
        }

        // Gain, squelch, modulation and decode are what decide whether a signal
        // on the waterfall becomes audio. Reaching them used to mean stopping the
        // session, so the panel is available even where tuning is not.
        function test_18_the_radio_panel_opens_in_both_intents() {
            var screen = screenLoader.item
            var button = findChild(screen, "spectrumRadioButton")
            var panel = findChild(screen, "radioSheet")
            verify(button !== null, "the radio button is missing")
            verify(panel !== null, "the radio panel is missing")

            screen.exploring = false
            tryVerify(function () { return screen.viewOnly })
            verify(button.visible, "a view-only session hid the radio settings")

            screen.exploring = true
            verify(button.visible)
            verify(!panel.visible)
            panel.open()
            verify(panel.visible)
            findChild(screen, "radioSheetDone").clicked()
            verify(!panel.visible)
        }

        // Each control asks the engine for an absolute value. It derives that value
        // from what the engine reports, except while one of its own requests is
        // still outstanding — which it must, or taps inside one poll are lost.
        function test_19_the_radio_panel_changes_the_radio() {
            var screen = screenLoader.item
            var panel = findChild(screen, "radioSheet")
            panel.open()

            testContext.setMetric("tunerGainDb",30)
            tryVerify(function () { return metrics.tunerGainDb === 30 })
            findChild(screen, "radioGainUp").clicked()
            compare(testContext.lastGainDb(), 31)

            // Taps land faster than the 250 ms mirror is republished, and the
            // command is a coalescible setter: stepping from the reading would have
            // every tap inside one poll ask for the same value, and the queue would
            // merge them into one. Five taps have to be five steps.
            for (var i = 0; i < 4; i++)
                findChild(screen, "radioGainUp").clicked()
            compare(testContext.lastGainDb(), 35)
            compare(findChild(screen, "radioGainValue").text, "35 dB")

            // And down from what was asked for, not from the stale reading.
            findChild(screen, "radioGainDown").clicked()
            compare(testContext.lastGainDb(), 34)

            // The request only stands in for the reading while it is outstanding: a
            // command can be refused, and the panel must not go on showing a gain
            // the radio never took.
            panel.forgetRequests()
            compare(findChild(screen, "radioGainValue").text, "30 dB")

            // Clamped to what the tuner actually offers, so the panel cannot ask
            // for a gain the backend will only refuse — and a step that changes no
            // setting is not sent at all, because every accepted gain command
            // restarts the dongle and drops the audio and the spectrum with it.
            testContext.setMetric("tunerGainDb",49)
            tryVerify(function () { return metrics.tunerGainDb === 49 })
            panel.forgetRequests()
            var atCeiling = testContext.gainCalls()
            findChild(screen, "radioGainUp").clicked()
            compare(testContext.gainCalls(), atCeiling,
                    "a step at the ceiling restarted the dongle to change nothing")
            testContext.setMetric("tunerGainDb",0)
            tryVerify(function () { return metrics.tunerGainDb === 0 })
            panel.forgetRequests()
            var atFloor = testContext.gainCalls()
            findChild(screen, "radioGainDown").clicked()
            compare(testContext.gainCalls(), atFloor,
                    "a step at the floor restarted the dongle to change nothing")

            findChild(screen, "radioSquelchUp").clicked()
            verify(Math.abs(testContext.lastSquelchDb() - (-115)) < 0.001)

            // PPM is chosen once, by someone with no way to tell if it was right.
            findChild(screen, "radioPpmUp").clicked()
            compare(testContext.lastPpm(), 1)
            findChild(screen, "radioPpmDown").clicked()
            compare(testContext.lastPpm(), 0)
            panel.forgetRequests()
            findChild(screen, "radioPpmDown").clicked()
            compare(testContext.lastPpm(), -1)

            // QPSK is the simulcast answer, and picking it must ask for that state
            // rather than for "the other one".
            findChild(screen, "radioModulation").selected(1)
            compare(testContext.lastModulation(), 1)
            findChild(screen, "radioModulation").selected(0)
            compare(testContext.lastModulation(), 0)

            // GFSK is a state the session arrives in on its own — the DMR and
            // EDACS/ProVoice presets select it — so it has to be offered as a
            // choice too, or the first press above is a one-way door out of it.
            compare(findChild(screen, "radioModulation").model.length, 3)
            findChild(screen, "radioModulation").selected(2)
            compare(testContext.lastModulation(), 2)

            // And it has to read back as itself rather than as C4FM, or the
            // control claims a modulation the decoder is not on.
            testContext.setMetric("modulation", 2)
            compare(findChild(screen, "radioModulation").currentIndex, 2)
            testContext.setMetric("modulation", 0)

            // Every one of those presses has to leave the panel open. The scrim
            // dismisses on a tap, and a handler on the panel cannot stop that —
            // handlers never take exclusive grabs — so the scrim has to decide by
            // where the tap landed, or the panel closes on its own first button.
            verify(panel.visible, "the panel closed while its controls were used")
            verify(panel.hitsPanel(root.width / 2, root.height / 2),
                   "the panel does not claim its own centre")
            verify(!panel.hitsPanel(root.width / 2, 4),
                   "the panel claimed the scrim above it")

            panel.visible = false
            testContext.setMetric("tunerGainDb",30)
        }

        // The chips must send the preset the flag means, and the modulation chip
        // must not appear among them at all — it changes nothing here.
        function test_20_the_decode_chips_send_a_preset() {
            var screen = screenLoader.item
            var panel = findChild(screen, "radioSheet")
            panel.open()

            verify(findChild(screen, "radioDecode_P25 LSM") === null,
                   "the simulcast modulation chip appeared as a decode choice")

            var dmr = findChild(screen, "radioDecode_DMR")
            verify(dmr !== null, "the DMR chip is missing")
            dmr.clicked()
            // DSDCFG_MODE_DMR. The mapping behind this is the production one, so
            // the number is the enum's own — if it ever renumbers, this fails and
            // someone has to look at what else reads it.
            compare(testContext.lastDecodeMode(), 4)

            // Selection reads the engine, not the tap: the fixture reports auto,
            // so Auto is the chip that shows as chosen.
            var auto = findChild(screen, "radioDecode_Auto")
            verify(auto !== null, "the auto chip is missing")
            verify(auto.selected, "the panel did not show the engine's own mode")
            verify(!dmr.selected, "a tapped chip showed as chosen before the engine agreed")

            panel.visible = false
        }

        // TapHandlers never take exclusive grabs, so a tap on a sheet also lands
        // on the spectrum behind it. Left alone, every press of a button on the
        // radio panel also retunes the receiver — the panel closes and the radio
        // has moved somewhere nobody asked for.
        function test_21_a_tap_on_a_sheet_never_reaches_the_spectrum() {
            var screen = screenLoader.item
            var sheets = [findChild(screen, "radioSheet"),
                          findChild(screen, "spectrumGoToSheet"),
                          findChild(screen, "spectrumExploreConfirm")]

            for (var i = 0; i < sheets.length; i++) {
                verify(sheets[i] !== null, "a sheet is missing")
                testContext.resetCommands()
                sheets[i].visible = true
                verify(screen.sheetOpen, "an open sheet did not shield the spectrum")

                // Straight through the middle of the panel, where its own controls are.
                mouseClick(tc.area, tc.xOf(testContext.spectrumPeakHz()), tc.area.height * 0.5)
                tc.wait(50)
                compare(testContext.manualTuneCalls(), 0,
                        "a tap on a sheet retuned the receiver behind it")

                sheets[i].visible = false
            }
            verify(!screen.sheetOpen)
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

        // Where the receiver is, on a picture whose whole point is showing where
        // signals are. The frame's center bin IS the tuned frequency, so at 1x
        // this sits mid-screen and only moves once the view is panned or zoomed.
        function test_22_the_marker_sits_where_the_receiver_is() {
            var marker = findChild(screenLoader.item, "spectrumTunerMarker")
            verify(marker !== null, "the tuner marker is missing")
            verify(marker.onScreen, "the marker is off screen at rest")
            verify(Math.abs(marker.xFraction - 0.5) < 0.01,
                   "at rest the marker is at " + marker.xFraction + ", not mid-screen")

            // Pan the viewport and the marker must travel with the spectrum.
            spectrum.zoom = 4.0
            spectrum.viewOffsetHz = spectrum.spanHz * 0.1
            var want = (spectrum.centerFreqHz - spectrum.viewLowHz) / spectrum.viewSpanHz
            verify(Math.abs(marker.xFraction - want) < 0.001,
                   "panned, the marker is at " + marker.xFraction + " not " + want)
            spectrum.resetView()
        }

        // The column is the channel the demodulator is filtering, so it has to be
        // that wide against the view — not a fixed number of pixels that would lie
        // at every zoom but one.
        function test_23_the_channel_column_is_as_wide_as_the_channel() {
            var marker = findChild(screenLoader.item, "spectrumTunerMarker")
            testContext.setMetric("channelBandwidthHz", 12500)
            var want = 12500 / spectrum.viewSpanHz
            verify(Math.abs(marker.bandWidthFraction - want) < 1e-6,
                   "column is " + marker.bandWidthFraction + " of the view, want " + want)

            // Zoomed 4x the same channel covers four times the screen.
            spectrum.zoom = 4.0
            want = 12500 / spectrum.viewSpanHz
            verify(Math.abs(marker.bandWidthFraction - want) < 1e-6, "the column ignored the zoom")
            spectrum.resetView()
        }

        // Before the demodulator reports a profile there is no honest width to
        // draw. The line still says where the receiver is; the column says nothing
        // rather than claiming zero width.
        function test_24_an_unknown_channel_width_draws_no_column() {
            var marker = findChild(screenLoader.item, "spectrumTunerMarker")
            testContext.setMetric("channelBandwidthHz", 0)
            compare(marker.bandWidthFraction, 0)
            verify(marker.onScreen, "the marker vanished with the column")
            testContext.setMetric("channelBandwidthHz", 12500)
        }

        // Zoomed in and panned to the far edge, the receiver is genuinely not on
        // screen. Drawing the marker clamped to the edge would put it on a carrier
        // it is not tuned to, so it hides and an edge hint points the way back.
        function test_25_the_marker_hides_when_the_receiver_is_off_screen() {
            var marker = findChild(screenLoader.item, "spectrumTunerMarker")
            spectrum.zoom = 8.0
            spectrum.viewOffsetHz = spectrum.spanHz
            verify(!marker.onScreen, "the marker claims to be on screen at the far edge")
            var hint = findChild(screenLoader.item, "spectrumTunerEdgeHint")
            verify(hint !== null && hint.visible, "nothing points back toward the receiver")
            spectrum.resetView()
            verify(marker.onScreen, "the marker did not come back")
        }

        // Locked to a saved system, or with trunking holding the tuner, is
        // exactly when "what am I on?" matters most — so the marker belongs to
        // the picture, not to the tuning controls that disappear in view-only.
        function test_25b_the_marker_survives_view_only() {
            var marker = findChild(screenLoader.item, "spectrumTunerMarker")
            testContext.setMetric("tunerControlled", true)
            verify(screenLoader.item.viewOnly, "the screen did not go view-only")
            verify(marker.visible && marker.onScreen, "the marker vanished in view-only")
            testContext.setMetric("tunerControlled", false)
        }
    }
}
