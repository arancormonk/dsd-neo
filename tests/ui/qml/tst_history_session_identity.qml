// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest

Item {
    width: 420
    height: 900
    Loader { id: loader }

    TestCase {
        name: "HistorySessionIdentity"
        when: windowShown

        function init() {
            testContext.useLifecycleHost(true, true)
            testContext.setDelayedStop(false)
            while (savedSystems.count)
                savedSystems.remove(0)
            while (scanLists.count)
                scanLists.remove(0)
            savedSystems.add({name: "Saved site", sourceType: "rtltcp", host: "127.0.0.1", port: 1234, freqMhz: "851.5"})
            callHistory.sessionUid = "previous-session"
            loader.source = uiDir + "/Main.qml"
            verify(loader.item !== null)
        }

        function cleanup() {
            loader.source = ""
            testContext.useLifecycleHost(false)
            while (savedSystems.count)
                savedSystems.remove(0)
            while (scanLists.count)
                scanLists.remove(0)
            callHistory.sessionUid = "test-system"
            callHistory.sessionLabel = "Test Site"
        }

        function test_saved_to_explore_from_here() {
            loader.item.startSystem(0)
            testContext.setLifecyclePhase(2)
            compare(callHistory.sessionUid, savedSystems.get(0).uid)
            findChild(loader.item, "monitorScreen").openSpectrum()
            var spectrum = findChild(loader.item, "spectrumScreen")
            verify(spectrum !== null)
            spectrum.exploreFromHere()
            compare(callHistory.sessionUid, "")
            compare(callHistory.sessionLabel, "Exploring")
        }

        function test_scan_start_has_no_identity() {
            scanLists.add({name: "Scan", sourceType: "rtltcp", host: "127.0.0.1", port: 1234,
                entries: [{kind: "freq", name: "Simplex", protocol: "p25", freqMhz: "851.5", enabled: true}]})
            loader.item.startWithMap(scanLists.get(0), 0, true)
            compare(decoderHost.sessionState, 1)
            compare(callHistory.sessionUid, "")
        }

        function test_explore_start_has_no_identity() {
            loader.item.startWithMap(loader.item.exploreSystem("rtltcp", "127.0.0.1", 1234, "851.5"), -1, false)
            compare(decoderHost.sessionState, 1)
            compare(callHistory.sessionUid, "")
        }

        function test_invalid_start_preserves_identity() {
            var sys = savedSystems.get(0)
            sys.hangtime = "abc"
            loader.item.startWithMap(sys, 0, false)
            compare(decoderHost.sessionState, 0)
            compare(callHistory.sessionUid, "previous-session")
            compare(loader.item.startError, "Enter hang time in seconds from 0 to 30.")
        }
    }
}
