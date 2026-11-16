// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
//
// #786: GPS roaming's line in the Monitor header. State first and the feature
// name last, so eliding drops the name; where even that is too wide beside the
// header actions, the state alone. Its place is kept for the whole saved
// session, so the header never changes height. Made-up sites Hill (lat 10.0)
// and Lake (10.5) of one system on one rtl_tcp receiver.
import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

Item {
    id: root
    width: 640
    height: 800

    Ui.MonitorScreen {
        id: monitor
        anchors.fill: parent
    }

    TestCase {
        id: tc
        name: "MonitorRoaming"
        when: windowShown

        property double ns: 1
        property var uids: []

        function item(name) {
            var found = findChild(monitor, name);
            verify(found !== null, name);
            return found;
        }

        function init() {
            testContext.setLifecycleLocationSupported(true);
            testContext.useLifecycleHost(true);
            testContext.setRoamingNow(1000000);
            testContext.setMetric("radioInput", true);
            prefs.metricUnits = true;
            prefs.extraArgs = "";
            prefs.roamingEnabled = true;
            while (savedSystems.count)
                savedSystems.remove(0);
            var names = ["Hill", "Lake"];
            var ids = [];
            for (var i = 0; i < 2; ++i) {
                savedSystems.add({name: "Valley — " + names[i], siteName: names[i], rrSid: 7, rrSiteId: 70 + i,
                    trunking: true, sourceType: "rtltcp", host: "127.0.0.1", port: 1234, decodeFlag: "-fp",
                    freqMhz: (851.0125 + i).toFixed(4), siteLat: 10.0 + 0.5 * i, siteLon: 20.0, hasSitePos: true});
                ids.push(savedSystems.get(i).uid);
            }
            uids = ids;
            monitor.system = savedSystems.get(0);
            monitor.sessionSystemUid = uids[0];
            monitor.sitesAvailable = true;
            root.width = 640;
            Ui.Theme.fontScale = 1;
        }

        function cleanup() {
            prefs.roamingEnabled = false;
            monitor.system = null;
            monitor.sessionSystemUid = "";
            monitor.sitesAvailable = false;
            testContext.setMetric("radioInput", false);
            testContext.setLifecyclePhase(0);
            testContext.useLifecycleHost(false);
            testContext.setLifecycleLocationSupported(false);
            Ui.Theme.resetFontScale();
            root.width = 640;
            while (savedSystems.count)
                savedSystems.remove(0);
        }

        // Hill running as a saved session; past its initialization edge unless @p starting.
        function runHill(starting, busy) {
            testContext.setLifecyclePhase(1);
            siteRoaming.noteSessionStart("saved", uids[0], 0);
            testContext.setLifecyclePhase(2);
            siteRoaming.facts = {savedUid: uids[0], trunking: true, radioInput: true, replayInput: false, scanKind: 0,
                operatorTuned: false, heldTg: 0, callBusy: busy === true, optionsKnown: true};
            if (!starting)
                siteRoaming.noteSessionInitialized();
        }
        function fix(lat, after) {
            testContext.setRoamingNow(testContext.roamingNow() + (after || 0));
            ns += 1000;
            testContext.emitLocationStreamUpdate({generation: 1, state: "ok", hasFix: true, provider: "gps",
                measurementNs: ns, lat: lat, lon: 20.0, accuracyM: 10, fixAtMs: testContext.roamingNow()});
        }
        // Three fixes near Lake over 20 s, during a call: the switch waits for the call to end.
        function waitForCall() {
            runHill(false, true);
            fix(10.49, 1000);
            fix(10.49, 10000);
            fix(10.49, 10000);
            compare(siteRoaming.statusText, "Waiting for this call to end before switching to Lake (up to 90 seconds)");
        }

        // The line is there, blank, from the session's start, so the state arriving never moves the header.
        function test_slot_reserved_for_the_session() {
            var line = item("roamingHeaderLine");
            var header = item("monitorHeader");
            runHill(true);
            verify(line.visible, "kept for the whole saved session");
            compare(line.text, "", "blank while roaming idles (starting)");
            verify(line.Accessible.ignored, "a blank line is not read");
            waitForRendering(monitor);
            var height = header.height;
            siteRoaming.noteSessionInitialized();
            compare(line.text, "PAUSED: NO FIX · GPS ROAMING");
            fix(10.01, 1000);
            compare(line.text, "STAYING · GPS ROAMING");
            compare(line.Accessible.name, "Staying on this site until another is clearly closer", "read as the sentence");
            waitForRendering(monitor);
            compare(header.height, height, "the header kept its height");
            prefs.roamingEnabled = false;
            verify(!line.visible, "roaming off: no line");
            prefs.roamingEnabled = true;
            monitor.sessionSystemUid = "";
            verify(!line.visible, "not a saved session: no line");
        }

        // A change of state is read out once; a fix that only moves the distance is not.
        function test_state_changes_are_read_out() {
            var speech = item("roamingHeaderSpeech");
            var heard = speech.announcements;
            runHill(false);
            fix(10.01, 1000);
            tryCompare(speech, "announcements", heard + 1);
            compare(speech.announced, "Staying on this site until another is clearly closer", "the state it settled on");
            heard = speech.announcements;
            fix(10.02, 1000);
            wait(0);
            compare(speech.announcements, heard, "a distance alone is not read out");
            fix(10.49, 1000);
            tryCompare(speech, "announcements", heard + 1);
            compare(speech.announced, "Lake is closer. Checking before switching");
        }

        // With the header actions beside it, the line keeps the whole state where it fits, else the state alone,
        // never cut: checked on the rendered text at a phone's width and large font scales.
        function test_state_never_elided_data() {
            return [{tag: "wide", width: 640, scale: 1, full: true},
                    {tag: "phone", width: 360, scale: 1, full: true},
                    {tag: "phone 1.3", width: 360, scale: 1.3, full: false},
                    {tag: "phone 2.0", width: 360, scale: 2.0, full: false},
                    {tag: "landscape 1.3", width: 800, scale: 1.3, full: true},
                    {tag: "landscape 2.0", width: 800, scale: 2.0, full: false}];
        }
        function test_state_never_elided(data) {
            root.width = data.width;
            Ui.Theme.fontScale = data.scale;
            waitForCall();
            waitForRendering(monitor);
            var line = item("roamingHeaderLine");
            verify(line.visible && line.width > 0);
            compare(line.text, data.full ? "WAITING FOR CALL END · GPS ROAMING" : "WAITING");
            verify(!line.truncated, "the state is not cut: " + line.text);
            verify(line.paintedWidth <= line.width + 0.5, "it fits its line");
            compare(line.Accessible.name, "Waiting for this call to end before switching to Lake (up to 90 seconds)");
            // Beside the actions, inside the screen.
            var at = line.mapToItem(monitor, 0, 0);
            verify(at.x >= -0.5 && at.x + line.paintedWidth <= monitor.width + 0.5, "inside the screen");
            var actions = item("sessionOptionsButton");
            var actionsAt = actions.mapToItem(monitor, 0, 0);
            var beside = actionsAt.y + actions.height <= at.y + 0.5 || actionsAt.y >= at.y + line.height - 0.5;
            verify(beside || at.x + line.paintedWidth <= actionsAt.x + 0.5, "clear of the header actions");
        }
    }
}
