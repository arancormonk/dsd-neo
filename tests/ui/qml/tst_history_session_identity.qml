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
            // A follow call queued as the monitor closed used to run in its torn-down context.
            failOnWarning(/FollowLatest\.qml|evaluate a function in an invalid context/)
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
            testContext.resetCommands()
            setOwner(false, false, 0, false, false)
            testContext.setMetric("optionsKnown", false)
        }

        // Issue #671: what the decoder reports about the tuner, published as one snapshot: Main reads the hold and the
        // owner together when the metrics notify.
        function setOwner(controlled, trunking, scanKind, running, operatorTuned) {
            testContext.setMetrics({operatorTuned: operatorTuned, trunkingEnabled: trunking, scanKind: scanKind,
                                    scanRotationActive: running, scanPaused: scanKind !== 0 && !running,
                                    scanResumable: scanKind !== 0 && !running, tunerControlled: controlled})
        }
        function monitorTitle() {
            return findChild(findChild(loader.item, "monitorScreen"), "monitorHeaderTitle").text
        }
        function exploreFromSpectrum() {
            findChild(loader.item, "monitorScreen").openSpectrum()
            var spectrum = findChild(loader.item, "spectrumScreen")
            verify(spectrum !== null)
            spectrum.exploreFromHere()
        }

        // Nothing was steering a conventional saved system, so the decoder has no
        // hold to report: the session detaches from the system at the tap, but
        // history is not relabelled. The call the release ends, and the calls
        // heard before the first tune, are still the system's; that tune stamps
        // the ones after it Exploring (the rows' half is UI_QT_CALL_HISTORY_MODEL's
        // test_tuner_owner_stamps_set_row_identity()).
        function test_saved_to_explore_from_here() {
            loader.item.startSystem(0)
            testContext.setLifecyclePhase(2)
            testContext.setMetric("optionsKnown", true)
            var uid = savedSystems.get(0).uid
            compare(callHistory.sessionUid, uid)
            exploreFromSpectrum()
            compare(testContext.releaseTunerCalls(), 1)
            compare(callHistory.sessionUid, uid, "history was relabelled at queue time")
            compare(callHistory.sessionLabel, "Saved site", "history was relabelled at queue time")
            compare(monitorTitle(), "Exploring")
            verify(loader.item.exploring)
            compare(loader.item.sessionRow, -1)
            // The first tap lands: the decoder reports the operator's tuner. The session stays Explore's.
            setOwner(false, false, 0, false, true)
            compare(monitorTitle(), "Exploring")
            verify(loader.item.exploring)
            compare(callHistory.sessionLabel, "Saved site")
        }

        // An Explore-started session: its first tune hands the operator the tuner
        // in the decoder's eyes, which changes nothing a reader can see.
        function test_the_first_tune_of_an_explore_session() {
            loader.item.startWithMap(loader.item.exploreSystem("rtltcp", "127.0.0.1", 1234, "851.5"), -1, false)
            testContext.setLifecyclePhase(2)
            testContext.setMetric("optionsKnown", true)
            compare(monitorTitle(), "Exploring")
            setOwner(false, false, 0, false, true)
            compare(monitorTitle(), "Exploring")
            verify(loader.item.exploring)
            verify(loader.item.exploreRemembered, "an Explore session stopped remembering where it went")
            compare(callHistory.sessionUid, "")
        }

        // B17: with an owner, the release ends the call on air, and that call began
        // under the saved system. Nothing changes identity when the release is
        // queued; the monitor names the session Explore once the decoder reports
        // the operator holding the tuner, and history is left to the calls' own
        // stamps, so the label the release's own call lands under is the system's.
        function test_explore_with_an_owner_waits_for_the_decoder() {
            loader.item.startSystem(0)
            testContext.setLifecyclePhase(2)
            testContext.setMetric("optionsKnown", true)
            setOwner(true, true, 0, false, false)
            var uid = savedSystems.get(0).uid
            exploreFromSpectrum()
            compare(testContext.releaseTunerCalls(), 1)
            compare(callHistory.sessionLabel, "Saved site", "history was relabelled before the release ran")
            compare(callHistory.sessionUid, uid)
            compare(monitorTitle(), "Saved site")
            // The release lands.
            setOwner(false, false, 0, false, true)
            tryCompare(findChild(findChild(loader.item, "monitorScreen"), "monitorHeaderTitle"), "text", "Exploring")
            compare(loader.item.sessionSavedSystemUid, "", "the session still writes back to the saved system")
            compare(loader.item.sessionRow, -1)
            compare(callHistory.sessionLabel, "Saved site", "the calls' own stamps file them, not the label")
            compare(callHistory.sessionUid, uid)
        }

        // A release the queue turns away asked for nothing: the session stays as it was.
        function test_refused_explore_changes_nothing() {
            loader.item.startSystem(0)
            testContext.setLifecyclePhase(2)
            testContext.setMetric("optionsKnown", true)
            testContext.setReleaseTunerAccepted(false)
            exploreFromSpectrum()
            compare(testContext.releaseTunerCalls(), 1)
            verify(!loader.item.exploring, "a refused release started exploring")
            compare(callHistory.sessionLabel, "Saved site")
            compare(callHistory.sessionUid, savedSystems.get(0).uid)
            compare(monitorTitle(), "Saved site")
        }

        // A scan list's Go to (or a frequency typed at the terminal or the API)
        // pauses the scan: the monitor names the session Explore while the
        // operator holds the tuner, the list waits for Resume scan, and what the
        // operator explored meanwhile is not remembered as Explore's. Resumed,
        // the session is the scan list again. A pause the scan never comes back
        // from (paused, then stopped) keeps the Explore name.
        function addScanList() {
            scanLists.add({name: "Scan", sourceType: "rtltcp", host: "127.0.0.1", port: 1234,
                entries: [{kind: "freq", name: "Simplex", protocol: "p25", freqMhz: "851.5", enabled: true}]})
        }
        function test_a_paused_scan_list_and_its_resume() {
            addScanList()
            loader.item.startWithMap(scanLists.get(0), 0, true)
            testContext.setLifecyclePhase(2)
            testContext.setMetric("optionsKnown", true)
            setOwner(true, false, 2, true, false)
            compare(monitorTitle(), "Scan")
            // Paused (here from outside this screen): the receiver is the operator's to tune by hand, the list waits.
            setOwner(false, false, 2, false, true)
            tryCompare(findChild(findChild(loader.item, "monitorScreen"), "monitorHeaderTitle"), "text", "Exploring")
            verify(loader.item.exploring, "a paused scan list stayed view-only")
            verify(!loader.item.exploreRemembered, "a paused scan list's frequency would be remembered as Explore's")
            verify(loader.item.sessionScanList, "the pause dropped the list")
            compare(loader.item.pausedScanSystem.name, "Scan")
            compare(callHistory.sessionLabel, "Scan")
            compare(callHistory.sessionUid, "")
            // Resume scan lands.
            setOwner(true, false, 2, true, false)
            tryCompare(findChild(findChild(loader.item, "monitorScreen"), "monitorHeaderTitle"), "text", "Scan")
            verify(!loader.item.exploring, "the resumed scan list stayed free to tap-tune")
            compare(loader.item.pausedScanSystem, null)
            // Paused, then stopped (an input switch, say): no resume comes, the name and the hand tuning stay.
            setOwner(false, false, 2, false, true)
            tryCompare(findChild(findChild(loader.item, "monitorScreen"), "monitorHeaderTitle"), "text", "Exploring")
            setOwner(false, false, 0, false, true)
            wait(20)
            compare(monitorTitle(), "Exploring")
            verify(loader.item.exploring, "a stopped pause went back to view-only")
        }

        // A call heard before an Explore that released trunking keeps the saved
        // system's UID, but the receiver has left that system: its details offer
        // no hold until a new session.
        function test_no_history_hold_once_the_operator_holds_the_tuner() {
            loader.item.startSystem(0)
            testContext.setLifecyclePhase(2)
            testContext.setMetric("optionsKnown", true)
            setOwner(true, true, 0, false, false)
            var uid = savedSystems.get(0).uid
            var detail = findChild(findChild(loader.item, "monitorScreen"), "monitorHistoryDetail")
            verify(detail !== null, "the monitor's details sheet is missing")
            detail.open({name: "Dispatch", tg: 1001, systemUid: uid})
            var button = findChild(detail, "historyHoldButton")
            verify(button.enabled, "the fixture's own system row offers no hold")
            detail.visible = false
            exploreFromSpectrum()
            setOwner(false, false, 0, false, true)
            // Back on the monitor: the spectrum layer disables everything under it.
            loader.item.spectrumOpen = false
            verify(findChild(loader.item, "monitorScreen").enabled, "the monitor is still under another layer")
            compare(callHistory.sessionUid, uid, "the session keeps its UID for the calls heard before the Explore")
            detail.open({name: "Dispatch", tg: 1001, systemUid: uid})
            verify(!button.enabled, "a pre-Explore row offered a hold while the operator explores")
            compare(findChild(detail, "historyHoldReason").text, "Hold needs a single saved system")
            detail.visible = false
        }

        // B18: an Explore taken while the scan list is still starting (no options
        // yet, so nobody knows what owns the tuner) does not change what was
        // started: the post-initialization edge records the scan list, with its
        // UID, so "resume the last scan list" can find it.
        function test_explore_before_the_initialized_edge_records_what_was_started() {
            addScanList()
            var uid = scanLists.get(0).uid
            loader.item.startWithMap(scanLists.get(0), 0, true)
            compare(decoderHost.sessionState, 1)
            exploreFromSpectrum()
            testContext.emitSessionInitialized()
            compare(prefs.lastStartedKind, "scan")
            compare(prefs.lastStartedUid, uid)
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

        // The key sheet's "Use for <system>" names only a standalone saved
        // system: Monitor passes its UID, and a scan list or Explore passes none.
        function keySheet() {
            return findChild(findChild(loader.item, "monitorScreen"), "keySheet")
        }
        function test_key_sheet_names_the_saved_system() {
            var uid = savedSystems.get(0).uid
            loader.item.startSystem(0)
            compare(findChild(loader.item, "monitorScreen").sessionSystemUid, uid)
            compare(keySheet().systemUid, uid)
            compare(keySheet().systemName, "Saved site")
            // A profile written for future starts reaches the session's copy of the system.
            verify(savedSystems.update(0, {decryptionProfileUid: "future-profile"}))
            keySheet().systemProfileSaved(uid)
            compare(loader.item.sessionSystem.decryptionProfileUid, "future-profile")
        }
        function test_key_sheet_has_no_system_for_a_scan_list() {
            scanLists.add({name: "Scan", sourceType: "rtltcp", host: "127.0.0.1", port: 1234,
                entries: [{kind: "freq", name: "Simplex", protocol: "p25", freqMhz: "851.5", enabled: true}]})
            loader.item.startWithMap(scanLists.get(0), 0, true)
            compare(decoderHost.sessionState, 1)
            compare(keySheet().systemUid, "")
        }
        function test_key_sheet_has_no_system_when_exploring() {
            loader.item.startWithMap(loader.item.exploreSystem("rtltcp", "127.0.0.1", 1234, "851.5"), -1, false)
            compare(decoderHost.sessionState, 1)
            compare(keySheet().systemUid, "")
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
