// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest

// Saving a Signal search detection. Main.qml hands openForFound() the map
// SignalSearchModel::systemForHit() builds from the detection's immutable
// snapshot, and the saved system has to keep the receiver the detection was
// made with: an RTL's PPM correction and bias tee, an Airspy's own serial,
// rate, gains and bias tee. A draft that reset them to the app defaults saved
// another receiver configuration, which could pick a different Airspy, change
// its gain or cut an LNA's power. The Spectrum's save passes no detection: its
// session ran on the app defaults, and the saved system follows them.
Item {
    id: root

    width: 420
    height: 900

    Loader {
        id: wizardLoader

        anchors.fill: parent
        source: uiDir + "/WizardScreen.qml"
    }

    TestCase {
        id: tc

        name: "WizardSearchSave"
        when: windowShown

        readonly property var wizard: wizardLoader.item
        // savedSystems is the production model on shared test storage: a row a
        // case adds has to go again whatever happens (see tst_wizard_trunking).
        property int savedRow: -1

        function init() {
            verify(tc.wizard !== null, "WizardScreen.qml failed to load")
            tc.wizard.openForAdd(false)
        }

        function cleanup() {
            if (tc.savedRow >= 0) {
                savedSystems.remove(tc.savedRow)
                tc.savedRow = -1
            }
        }

        // The shape systemForHit() returns for a detection on each source.
        function detection(sourceType, receiver) {
            return Object.assign({
                                     "sourceType": sourceType, "host": sourceType === "rtltcp" ? "127.0.0.2" : "",
                                     "port": sourceType === "rtltcp" ? 1235 : 0, "freqMhz": "463.552100",
                                     "decodeFlag": "-fA", "gainDb": 0, "bandwidthKhz": 48, "trunking": false,
                                     "extraArgs": "--nfm-bandwidth-hz 12500", "searchUnresolved": true
                                 }, receiver)
        }

        function save(found, freqMhz, withDetection) {
            var before = savedSystems.count
            tc.wizard.openForFound(found, freqMhz, withDetection ? found : undefined)
            tc.wizard.nameText = "Found"
            tc.wizard.commit()
            if (savedSystems.count > before)
                tc.savedRow = before
            compare(savedSystems.count, before + 1, "commit() saved nothing")
            return savedSystems.get(before)
        }

        function test_rtl_detection_keeps_ppm_and_bias_tee_data() {
            return [
                {tag: "usb-bias-on", source: "usb", ppm: "-3", biasTee: 1},
                {tag: "rtltcp-bias-off", source: "rtltcp", ppm: "12", biasTee: 0},
                {tag: "usb-zero-ppm", source: "usb", ppm: "0", biasTee: 1}
            ]
        }

        function test_rtl_detection_keeps_ppm_and_bias_tee(data) {
            var found = detection(data.source, {"ppm": data.ppm, "biasTee": data.biasTee})
            var saved = save(found, found.freqMhz, true)
            compare(saved.sourceType, data.source)
            compare(saved.ppm, data.ppm, "the detection's PPM correction was replaced by the app default")
            // Explicit either way: an Off must not follow a later app-wide On
            // into feeding this antenna, nor an On a later Off out of it.
            compare(saved.biasTee, data.biasTee, "the detection's bias tee was replaced by the app default")
            compare(Object.keys(saved.airspy).length, 0, "an RTL detection must not gain Airspy settings")
        }

        function test_airspy_detection_keeps_its_native_settings() {
            var native = {
                "serial": "A1B2C3D4E5F60718", "sample_rate": 3000000, "gain_mode": "manual",
                "sensitivity_gain": 10, "linearity_gain": 10, "lna_gain": 12, "mixer_gain": 9,
                "vga_gain": 7, "lna_agc": 0, "mixer_agc": 1, "bias_tee": 1
            }
            var found = detection("airspy", {"airspy": native})
            var saved = save(found, found.freqMhz, true)
            compare(saved.sourceType, "airspy")
            for (var key in native)
                compare(saved.airspy[key], native[key], "Airspy " + key + " was not saved as detected")
            // The Airspy carries its own tee and takes no PPM: the RTL fields
            // stay unset rather than pinning values that do not describe it.
            compare(saved.ppm, "")
            compare(saved.biasTee, -1)
        }

        // systemForHit() hands over the detection's gain as the whole-dB
        // override the saved session starts with: 0 is the tuner's AGC and a
        // manual gain is at least 1 dB (a manual 0 dB detection arrives as 1).
        // An Airspy's gains live in its native map, so it carries none (-1),
        // and the draft shows no override rather than "-1".
        function test_detection_gain_is_saved_as_handed_over_data() {
            return [
                {tag: "usb-agc", source: "usb", gainDb: 0, field: "0"},
                {tag: "rtltcp-manual-lowest", source: "rtltcp", gainDb: 1, field: "1"},
                {tag: "usb-manual", source: "usb", gainDb: 34, field: "34"},
                {tag: "airspy", source: "airspy", gainDb: -1, field: ""}
            ]
        }

        function test_detection_gain_is_saved_as_handed_over(data) {
            var found = detection(data.source, {"gainDb": data.gainDb})
            tc.wizard.openForFound(found, found.freqMhz, found)
            compare(tc.wizard.gainText, data.field)
            var saved = save(found, found.freqMhz, true)
            compare(saved.gainDb, data.gainDb, "the saved system must start with the detection's gain")
        }

        function test_the_draft_opens_clean() {
            tc.wizard.openForFound(detection("usb", {"ppm": "-3", "biasTee": 1}), "463.552100",
                                   detection("usb", {"ppm": "-3", "biasTee": 1}))
            compare(tc.wizard.ppmText, "-3")
            compare(tc.wizard.biasTee, 1)
            compare(tc.wizard.initialDraft, tc.wizard.draftFingerprint(),
                    "the detection's own settings must not read as unsaved edits")
        }

        function test_explore_save_follows_the_app_defaults() {
            // A session map carrying receiver overrides must not leak them into
            // a found system: only a detection's snapshot speaks for them.
            var session = {
                "sourceType": "usb", "ppm": "7", "biasTee": 1, "airspy": {"lna_gain": 9}
            }
            var saved = save(session, "851.375", false)
            compare(saved.ppm, "")
            compare(saved.biasTee, -1)
            compare(Object.keys(saved.airspy).length, 0)
        }
    }
}
