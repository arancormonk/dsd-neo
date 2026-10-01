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
            callHistory.clearAll()
            monitorView.historySession = 0
            talkgroups.historySession = 0
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

        // The monitor's recent calls and the heard talkgroups show the calls the history logs in
        // the running session, not calls stamped after a clock reading taken at the start. A
        // replay's calls carry the capture's time, years before the decode clock the start reads:
        // the engine moves that clock onto the capture only once the replay opens.
        function test_session_views_show_replayed_calls() {
            callHistory.clearAll()
            verify(testContext.clearTalkgroups())
            // The previous session's last call, stamped a minute ago.
            callHistory.pushAt(Math.floor(Date.now() / 1000) - 60)
            loader.item.startSystem(0)
            compare(decoderHost.sessionState, 1)
            // A replay of a 2010 capture.
            callHistory.pushAt(1262304000)
            callHistory.pushAt(1262304100)
            compare(monitorView.count, 2, "the session's replayed calls, and only those, are on the monitor")
            verify(testContext.clearTalkgroups())
            compare(talkgroups.count, 2, "and in the heard talkgroups")
            compare(monitorView.historySession, callHistory.session)
            compare(talkgroups.historySession, callHistory.session)
        }

        // A UI relaunched while the service decodes rejoins the session the history kept: its
        // views show the calls its predecessor logged in that session, whatever their stamps, and
        // none from before it.
        function test_reattach_shows_the_running_sessions_calls() {
            callHistory.clearAll()
            verify(testContext.clearTalkgroups())
            callHistory.pushAt(Math.floor(Date.now() / 1000) - 60)
            // The previous UI's start, and a replayed call it logged.
            callHistory.beginSession()
            callHistory.pushAt(1262304000)
            // The relaunched UI finds the session running.
            testContext.setLifecyclePhase(2)
            compare(monitorView.count, 1, "the running session's call, and only that, is on the reattached monitor")
            verify(testContext.clearTalkgroups())
            compare(talkgroups.count, 1, "and in the heard talkgroups")
            compare(monitorView.historySession, callHistory.session)
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
