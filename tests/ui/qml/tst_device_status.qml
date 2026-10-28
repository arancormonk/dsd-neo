// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// #647 WP5: one reading of the brokered receiver for Listen, Onboarding and the
// Monitor. Driven through a plain host object so every combination of the
// record, the bus and the last session's health can be set directly.
Item {
    id: root

    QtObject {
        id: fakeHost
        property bool localDeviceBrokered: true
        property bool localDeviceReady: false
        property string localDeviceSource: ""
        property string localDeviceSerial: ""
        property string localDeviceStatus: ""
        property int localDeviceFailureKind: 0
        property string attachedDeviceSource: ""
        property bool localDeviceRequestPending: false
        property string localDeviceRequestedSource: ""
        property var receiverHealth: ({state: "ok", source: "", session: 0})
        property bool sessionActive: false
        property bool transitioning: false
    }

    Ui.DeviceStatus {
        id: status
        host: fakeHost
    }

    TestCase {
        name: "DeviceStatus"

        function reset() {
            fakeHost.localDeviceReady = false;
            fakeHost.localDeviceSource = "";
            fakeHost.localDeviceSerial = "";
            fakeHost.localDeviceStatus = "";
            fakeHost.localDeviceFailureKind = 0;
            fakeHost.attachedDeviceSource = "";
            fakeHost.localDeviceRequestPending = false;
            fakeHost.localDeviceRequestedSource = "";
            fakeHost.receiverHealth = {state: "ok", source: "", session: 0};
            fakeHost.sessionActive = false;
            fakeHost.transitioning = false;
            status.requestedSource = Qt.binding(function () {
                return fakeHost.localDeviceRequestedSource;
            });
        }
        function init() {
            reset();
        }
        function cleanup() {
            reset();
        }
        function apply(data) {
            fakeHost.localDeviceReady = data.ready === true;
            fakeHost.localDeviceSource = data.source || "";
            fakeHost.localDeviceFailureKind = data.failure || 0;
            fakeHost.localDeviceStatus = data.text || "";
            fakeHost.attachedDeviceSource = data.attached || "";
            fakeHost.localDeviceRequestPending = data.pending === true;
            fakeHost.localDeviceRequestedSource = data.requested || "";
            fakeHost.receiverHealth = data.health ? {state: data.health, source: data.healthSource, session: 7} : {state: "ok", source: "", session: 0};
        }

        function test_state_data() {
            return [
                {tag: "nothing attached", kind: "", name: "receiver", state: "absent", pill: "NO RECEIVER", action: "Connect", dot: "subdued", a11y: "No receiver"},
                {tag: "RTL-SDR on the bus", attached: "usb", kind: "usb", name: "RTL-SDR", state: "available", pill: "CONNECT RTL-SDR", action: "Connect", dot: "subdued", a11y: "Connect RTL-SDR"},
                {tag: "Airspy on the bus", attached: "airspy", kind: "airspy", name: "Airspy", state: "available", pill: "CONNECT AIRSPY", action: "Connect", dot: "subdued", a11y: "Connect Airspy"},
                {tag: "both, no request", attached: "both", kind: "", name: "receiver", state: "available", pill: "CONNECT RECEIVER", action: "Connect", dot: "subdued", a11y: "Connect receiver"},
                {tag: "both, Airspy requested", attached: "both", requested: "airspy", kind: "airspy", name: "Airspy", state: "available", pill: "CONNECT AIRSPY", action: "Connect", dot: "subdued", a11y: "Connect Airspy"},
                {tag: "RTL-SDR ready", ready: true, source: "usb", attached: "usb", text: "Ready: RTL-SDR", kind: "usb", name: "RTL-SDR", state: "ready", pill: "RTL-SDR READY", action: "", dot: "ready", a11y: "RTL-SDR ready"},
                {tag: "Airspy ready", ready: true, source: "airspy", attached: "both", requested: "usb", kind: "airspy", name: "Airspy", state: "ready", pill: "AIRSPY READY", action: "", dot: "ready", a11y: "Airspy ready"},
                {tag: "RTL-SDR waiting", attached: "usb", requested: "usb", pending: true, text: "Requesting permission for RTL-SDR", kind: "usb", name: "RTL-SDR", state: "pending", pill: "RTL-SDR WAITING", action: "", dot: "subdued", a11y: "RTL-SDR waiting for permission"},
                {tag: "Airspy waiting", attached: "airspy", requested: "airspy", pending: true, kind: "airspy", name: "Airspy", state: "pending", pill: "AIRSPY WAITING", action: "", dot: "subdued", a11y: "Airspy waiting for permission"},
                {tag: "Airspy requested, RTL-SDR attached", attached: "usb", requested: "airspy", kind: "airspy", name: "Airspy", state: "missing", pill: "NO AIRSPY", action: "Retry", dot: "subdued", a11y: "No Airspy"},
                {tag: "requested RTL-SDR detached", failure: 3, requested: "usb", text: "Device detached: RTL-SDR", kind: "usb", name: "RTL-SDR", state: "missing", pill: "NO RTL-SDR", action: "Retry", dot: "subdued", a11y: "No RTL-SDR"},
                {tag: "Airspy serial not found", failure: 3, attached: "airspy", requested: "airspy", text: "Requested Airspy serial not found", kind: "airspy", name: "Airspy", state: "missing", pill: "NO AIRSPY", action: "Retry", dot: "subdued", a11y: "No Airspy"},
                {tag: "permission denied", failure: 4, attached: "usb", requested: "usb", text: "USB permission denied", kind: "usb", name: "RTL-SDR", state: "failed", pill: "RTL-SDR NO ACCESS", action: "Retry", dot: "alert", a11y: "RTL-SDR has no access"},
                {tag: "Airspy open failed", failure: 2, attached: "airspy", requested: "airspy", text: "Android could not open Airspy (open_failed).", kind: "airspy", name: "Airspy", state: "failed", pill: "AIRSPY ERROR", action: "Retry", dot: "alert", a11y: "Airspy error"},
                {tag: "busy claim while ready", failure: 1, ready: true, source: "usb", attached: "usb", requested: "usb", text: "Android could not claim RTL-SDR.", kind: "usb", name: "RTL-SDR", state: "failed", pill: "RTL-SDR ERROR", action: "Retry", dot: "alert", a11y: "RTL-SDR error"},
                {tag: "RTL-SDR unplugged", health: "lost", healthSource: "usb", requested: "usb", text: "Device detached: RTL-SDR", kind: "usb", name: "RTL-SDR", state: "lost", pill: "RTL-SDR LOST", action: "Retry", dot: "alert", a11y: "RTL-SDR lost"},
                {tag: "Airspy unplugged, no request", health: "lost", healthSource: "airspy", kind: "airspy", name: "Airspy", state: "lost", pill: "AIRSPY LOST", action: "Retry", dot: "alert", a11y: "Airspy lost"},
                {tag: "RTL-SDR stopped sending", health: "failed", healthSource: "usb", attached: "usb", requested: "usb", kind: "usb", name: "RTL-SDR", state: "fault", pill: "RTL-SDR LOST", action: "Retry", dot: "alert", a11y: "RTL-SDR lost"},
                {tag: "another receiver's ending", health: "lost", healthSource: "airspy", ready: true, source: "usb", attached: "usb", kind: "usb", name: "RTL-SDR", state: "ready", pill: "RTL-SDR READY", action: "", dot: "ready", a11y: "RTL-SDR ready"}
            ];
        }
        function test_state(data) {
            apply(data);
            compare(status.kind, data.kind);
            compare(status.name, data.name);
            compare(status.state, data.state);
            compare(status.pillText, data.pill);
            compare(status.actionText, data.action);
            compare(status.canAct, data.action.length > 0);
            compare(status.accessibleName, data.a11y);
            var dot = data.dot === "ready" ? Ui.Theme.cyan : data.dot === "alert" ? Ui.Theme.alert : Ui.Theme.textSubdued;
            verify(Qt.colorEqual(status.dotColor, dot), "dot " + status.dotColor + " for " + data.dot);
            verify(status.detailText.length > 0, "every state explains itself");
            verify(status.headline.length > 0, "every state has a headline");
            // The record's own words win where it has them: they name the device and the cause.
            if (data.text && data.state !== "lost" && data.state !== "fault")
                compare(status.detailText, data.text);
        }

        function test_no_action_during_a_session() {
            fakeHost.attachedDeviceSource = "usb";
            compare(status.canAct, true);
            fakeHost.sessionActive = true;
            compare(status.actionText, "Connect", "the action stays named");
            compare(status.canAct, false);
            fakeHost.sessionActive = false;
            fakeHost.transitioning = true;
            compare(status.canAct, false);
        }

        function test_pending_action_returns_when_the_request_expires() {
            apply({attached: "usb", requested: "usb", pending: true});
            compare(status.canAct, false);
            fakeHost.localDeviceRequestPending = false;
            compare(status.state, "available");
            compare(status.actionText, "Connect");
            compare(status.canAct, true);
        }

        function test_requested_source_override() {
            // Main names the receiver a start is waiting on before the host has
            // recorded the request.
            fakeHost.attachedDeviceSource = "both";
            compare(status.kind, "");
            status.requestedSource = "airspy";
            compare(status.kind, "airspy");
            compare(status.pillText, "CONNECT AIRSPY");
        }

        function test_lost_detail_names_the_cause() {
            apply({health: "lost", healthSource: "usb", requested: "usb", text: "Device detached: RTL-SDR"});
            verify(status.detailText.indexOf("unplugged") >= 0, status.detailText);
            apply({health: "failed", healthSource: "airspy", requested: "airspy"});
            verify(status.detailText.indexOf("stopped sending samples") >= 0, status.detailText);
            verify(status.detailText.indexOf("Airspy") >= 0, status.detailText);
        }

        function test_stream_failure_detail_never_blames_the_cable() {
            // #782: an RTL-SDR's next open resets it; an Airspy's does not.
            apply({health: "failed", healthSource: "usb", attached: "usb", requested: "usb"});
            compare(status.state, "fault");
            compare(status.detailText, "RTL-SDR stopped sending samples while listening. Tap Retry, then start listening again to reset it.");
            apply({health: "failed", healthSource: "airspy", attached: "airspy", requested: "airspy"});
            compare(status.detailText, "Airspy stopped sending samples while listening. Tap Retry, then start listening again.");
        }
    }
}
