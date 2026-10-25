// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// #647 D1a: the Monitor's LIVE pill reads LOST the moment the host reports the
// session's receiver lost or failed, while the engine is still winding down
// (running stays true through its cleanup). For a USB receiver session on a
// host that brokers it, the pill opens a sheet: the receiver and its status
// while healthy; what happened and Stop now once it is lost.
Item {
    id: root

    width: 411
    height: 914

    Component {
        id: monitorComponent

        Ui.MonitorScreen {
            anchors.fill: parent
        }
    }

    TestCase {
        id: tc

        name: "MonitorReceiver"
        when: windowShown

        property var monitor: null
        property int stopsBefore: 0

        function item(name) {
            var found = findChild(tc.monitor, name);
            verify(found !== null, name + " is missing");
            return found;
        }
        function pill() {
            return item("monitorLiveStatus");
        }
        function sheet() {
            return item("monitorReceiverSheet");
        }
        function stops() {
            return decoderHost.stopCalls() - tc.stopsBefore;
        }
        function settle() {
            waitForRendering(tc.monitor);
            wait(20);
        }
        // A running session on @p source ("usb", "airspy", "rtltcp", ...), on a
        // host that brokers USB access when @p brokered (Android) or not (desktop).
        function startSession(source, brokered) {
            testContext.setUsbBrokered(brokered);
            testContext.useLifecycleHost(true, false, true);
            verify(decoderHost.start([]), "the fixture host starts");
            compare(decoderHost.running, true);
            compare(decoderHost.sessionState, 2);
            tc.monitor = createTemporaryObject(monitorComponent, root, {
                system: {name: "County P25", sourceType: source}
            });
            verify(tc.monitor !== null);
            tc.stopsBefore = decoderHost.stopCalls();
            settle();
        }
        // As the Android host reports a receiver ending: the health first, then
        // Stopping published at once and held, while the engine still runs.
        function loseReceiver(state, source) {
            testContext.setReceiverHealth(state, source);
            testContext.setLifecyclePhase(3);
            compare(decoderHost.running, true, "the engine is still cleaning up");
        }
        function tap(target) {
            mouseClick(target, target.width / 2, target.height / 2);
        }

        // Pristine: any warning fails the case.
        function init() {
            failOnWarning(/.*/);
        }
        function cleanup() {
            if (tc.monitor)
                findChild(tc.monitor, "monitorReceiverSheet").visible = false;
            testContext.setReceiverHealth("ok", "");
            testContext.setDongleStatus(false, 0, "");
            testContext.setLocalDeviceRecord("usb", false, "");
            testContext.setUsbBrokered(false);
            testContext.useLifecycleHost(false);
            Ui.Theme.resetFontScale();
            tc.monitor = null;
        }

        // LOST shows as soon as the host says so, not when the engine stops.
        function test_lost_while_running_data() {
            return [{tag: "rtl unplugged", source: "usb", state: "lost", name: "RTL-SDR lost"},
                    {tag: "rtl stopped sending", source: "usb", state: "failed", name: "RTL-SDR lost"},
                    {tag: "airspy unplugged", source: "airspy", state: "lost", name: "Airspy lost"}];
        }
        function test_lost_while_running(data) {
            startSession(data.source, true);
            var label = item("monitorLiveLabel");
            var dot = item("monitorLiveDot");
            compare(label.text, "LIVE");
            compare(dot.color, Ui.Theme.cyan);
            loseReceiver(data.state, data.source);
            tryCompare(label, "text", "LOST");
            compare(dot.color, Ui.Theme.alert);
            compare(pill().Accessible.name, data.name);
            compare(pill().Accessible.role, Accessible.Button);
        }

        // The lost sheet says what happened and that the session is finishing;
        // Stop now is the user's Stop, while the Stop button waits on the cleanup.
        function test_lost_sheet_stops_now() {
            startSession("usb", true);
            loseReceiver("lost", "usb");
            settle();
            verify(!item("stopListeningButton").enabled, "Stop waits while the session winds down");
            tap(pill());
            tryCompare(sheet(), "visible", true);
            compare(sheet().title, "RTL-SDR was unplugged");
            verify(sheet().message.indexOf("finishing") >= 0, sheet().message);
            compare(sheet().confirmText, "Stop now");
            verify(sheet().showCancel);
            // Cancel keeps the session finishing on its own.
            findChild(sheet(), "confirmCancelButton").activate();
            tryCompare(sheet(), "visible", false);
            compare(stops(), 0);
            tap(pill());
            tryCompare(sheet(), "visible", true);
            findChild(sheet(), "confirmActionButton").activate();
            tryCompare(sheet(), "visible", false);
            compare(stops(), 1, "Stop now stops once");
        }

        // A stream failure is worded as one.
        function test_failed_sheet_names_the_cause() {
            startSession("usb", true);
            loseReceiver("failed", "usb");
            tap(pill());
            tryCompare(sheet(), "visible", true);
            compare(sheet().title, "RTL-SDR stopped sending samples");
            compare(sheet().confirmText, "Stop now");
        }

        // Healthy: the receiver's name and status, and nothing to stop.
        function test_healthy_sheet_data() {
            return [{tag: "rtl", source: "usb", title: "RTL-SDR connected", text: "Ready: RTL-SDR Blog V4"},
                    {tag: "airspy", source: "airspy", title: "Airspy connected", text: "Ready: Airspy R2"}];
        }
        function test_healthy_sheet(data) {
            testContext.setDongleStatus(true, 0, data.text);
            testContext.setLocalDeviceRecord(data.source, false, data.source);
            startSession(data.source, true);
            compare(item("monitorLiveLabel").text, "LIVE");
            tap(pill());
            tryCompare(sheet(), "visible", true);
            compare(sheet().title, data.title);
            compare(sheet().message, data.text);
            compare(sheet().confirmText, "OK");
            verify(!sheet().showCancel);
            verify(!findChild(sheet(), "confirmCancelButton").visible);
            findChild(sheet(), "confirmActionButton").activate();
            tryCompare(sheet(), "visible", false);
            compare(stops(), 0, "OK stops nothing");
        }

        // One guarded path for touch, Return, Enter, Space and the accessibility
        // press; none of them reach the pill behind its own sheet.
        function test_activation_paths_data() {
            return [{tag: "return", key: Qt.Key_Return}, {tag: "enter", key: Qt.Key_Enter},
                    {tag: "space", key: Qt.Key_Space}, {tag: "accessibility", key: 0}];
        }
        function test_activation_paths(data) {
            startSession("usb", true);
            loseReceiver("lost", "usb");
            var p = pill();
            verify(p.activeFocusOnTab, "a tab stop");
            p.forceActiveFocus();
            if (data.key)
                keyClick(data.key);
            else
                p.Accessible.pressAction();
            tryCompare(sheet(), "visible", true);
            verify(p.Accessible.ignored, "the pill is behind its sheet");
            p.Accessible.pressAction();
            compare(stops(), 0);
            sheet().cancel();
            tryCompare(sheet(), "visible", false);
        }

        // A session without a local USB receiver keeps the pill an indicator, as
        // the desktop does for every session.
        function test_pill_inert_data() {
            return [{tag: "rtl_tcp", source: "rtltcp", brokered: true},
                    {tag: "tcp audio", source: "tcp", brokered: true},
                    {tag: "udp audio", source: "udp", brokered: true},
                    {tag: "file", source: "file", brokered: true},
                    {tag: "desktop usb", source: "usb", brokered: false}];
        }
        function test_pill_inert(data) {
            startSession(data.source, data.brokered);
            var p = pill();
            compare(p.Accessible.role, Accessible.StaticText);
            verify(!p.activeFocusOnTab, "no tab stop");
            tap(p);
            p.Accessible.pressAction();
            p.forceActiveFocus();
            keyClick(Qt.Key_Return);
            settle();
            verify(!sheet().visible, "the pill opened a sheet");
            compare(item("monitorLiveLabel").text, data.source === "file" ? "PLAYING" : "LIVE");
        }

        // The sheet answers for the state it was opened in: a receiver lost while
        // the healthy sheet is up closes it, so OK never turns into Stop now under
        // a finger. The pill then says LOST.
        function test_sheet_closes_when_the_receiver_is_lost() {
            testContext.setDongleStatus(true, 0, "Ready: RTL-SDR Blog V4");
            startSession("usb", true);
            tap(pill());
            tryCompare(sheet(), "visible", true);
            compare(sheet().confirmText, "OK");
            loseReceiver("lost", "usb");
            tryCompare(sheet(), "visible", false);
            compare(stops(), 0);
            compare(item("monitorLiveLabel").text, "LOST");
        }

        // After an Activity recreation the session has no saved-system map; a
        // lost receiver still names itself and offers Stop now.
        function test_lost_without_a_system() {
            startSession("usb", true);
            tc.monitor.system = null;
            loseReceiver("lost", "airspy");
            tryCompare(item("monitorLiveLabel"), "text", "LOST");
            compare(pill().Accessible.name, "Airspy lost");
            tap(pill());
            tryCompare(sheet(), "visible", true);
            compare(sheet().confirmText, "Stop now");
        }

        // Leaving the Monitor closes the sheet with the others.
        function test_hidden_monitor_closes_the_sheet() {
            startSession("usb", true);
            loseReceiver("lost", "usb");
            tap(pill());
            tryCompare(sheet(), "visible", true);
            tc.monitor.visible = false;
            verify(!sheet().visible);
        }
    }
}
