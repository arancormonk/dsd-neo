// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// One reading of the brokered USB receiver (#647): which receiver it is, what
// state it is in and what a tap would do. Listen, Onboarding and the Monitor all
// read it, so the same receiver is never named or worded two ways. Meaningful
// only where the host brokers USB access (Android); the desktop opens its
// receiver itself and shows none of this.
QtObject {
    id: status

    property var host: null
    // The receiver a request named. Main overrides it with the one a start is
    // waiting on, which the host's record only names once the request lands.
    property string requestedSource: host && host.localDeviceRequestedSource ? host.localDeviceRequestedSource : ""

    readonly property var health: host && host.receiverHealth ? host.receiverHealth : ({state: "ok", source: ""})
    readonly property bool ready: !!host && host.localDeviceReady === true
    readonly property string attached: host && host.attachedDeviceSource ? host.attachedDeviceSource : ""
    // DecoderHost::LocalDeviceFailureKind: 1 busy, 2 open failed, 3 detached or
    // not found, 4 no permission.
    readonly property int failureKind: host && host.localDeviceFailureKind ? host.localDeviceFailureKind : 0
    readonly property string hostText: host && host.localDeviceStatus ? host.localDeviceStatus : ""

    function local(source) {
        return source === "usb" || source === "airspy";
    }

    // The connected receiver; else the one whose session just ended, so a
    // receiver that vanished without a request is still named; else the
    // requested one; else the only kind on the bus. Empty with both kinds and
    // no request, or with nothing attached.
    readonly property string kind: ready && local(host.localDeviceSource) ? host.localDeviceSource
        : health.state !== "ok" && local(health.source) ? health.source
        : local(requestedSource) ? requestedSource
        : local(attached) ? attached : ""
    readonly property string name: kind === "usb" ? "RTL-SDR" : kind === "airspy" ? "Airspy" : qsTr("receiver")
    // The name at the start of a sentence.
    readonly property string leadName: kind.length ? name : qsTr("Receiver")
    readonly property bool onBus: kind.length ? attached === kind || attached === "both" : attached.length > 0

    // First match wins: a receiver the last session lost outranks everything
    // the record says since; a failed request outranks a connection that
    // looks ready (a claim can fail on an open descriptor).
    readonly property string state: {
        if (health.state !== "ok" && kind.length && health.source === kind)
            return health.state === "lost" ? "lost" : "fault";
        if (failureKind === 1 || failureKind === 2 || failureKind === 4)
            return "failed";
        if (ready)
            return "ready";
        if (host && host.localDeviceRequestPending)
            return "pending";
        if (failureKind === 3 || (kind.length && !onBus))
            return "missing";
        return onBus ? "available" : "absent";
    }
    readonly property bool alerting: state === "lost" || state === "fault" || state === "failed"

    readonly property string pillText: {
        var upper = name.toUpperCase();
        switch (state) {
        case "lost":
        case "fault":
            return qsTr("%1 LOST").arg(upper);
        case "failed":
            return failureKind === 4 ? qsTr("%1 NO ACCESS").arg(upper) : qsTr("%1 ERROR").arg(upper);
        case "ready":
            return qsTr("%1 READY").arg(upper);
        case "pending":
            return qsTr("%1 WAITING").arg(upper);
        case "available":
            return qsTr("CONNECT %1").arg(upper);
        default:
            return qsTr("NO %1").arg(upper);
        }
    }
    readonly property color dotColor: state === "ready" ? Theme.cyan : alerting ? Theme.alert : Theme.textSubdued

    readonly property string accessibleName: {
        switch (state) {
        case "lost":
        case "fault":
            return qsTr("%1 lost").arg(leadName);
        case "failed":
            return failureKind === 4 ? qsTr("%1 has no access").arg(leadName) : qsTr("%1 error").arg(leadName);
        case "ready":
            return qsTr("%1 ready").arg(leadName);
        case "pending":
            return qsTr("%1 waiting for permission").arg(leadName);
        case "available":
            return qsTr("Connect %1").arg(name);
        default:
            return qsTr("No %1").arg(name);
        }
    }

    // A short title for the receiver card (Onboarding).
    readonly property string headline: {
        switch (state) {
        case "lost":
            return qsTr("%1 was unplugged").arg(leadName);
        case "fault":
            return qsTr("%1 stopped sending samples").arg(leadName);
        case "failed":
            return qsTr("%1 needs attention").arg(leadName);
        case "ready":
            return qsTr("%1 connected").arg(leadName);
        case "pending":
            return qsTr("%1 is waiting for permission").arg(leadName);
        case "missing":
            return qsTr("No %1 detected").arg(name);
        case "available":
            return kind.length ? qsTr("%1 detected").arg(name) : attached === "both" ? qsTr("RTL-SDR and Airspy detected") : qsTr("Receiver detected");
        default:
            return qsTr("No receiver detected");
        }
    }

    // What happened and what to do. The record's own text wins where it has
    // one, since it names the device and the cause; a session's ending is told
    // here, because the record only knows the device went away.
    readonly property string detailText: {
        switch (state) {
        case "lost":
            return qsTr("%1 was unplugged while listening. Plug it back in, then tap Retry.").arg(leadName);
        case "fault":
            // #782: an RTL-SDR's next open resets it; nothing says why it stopped.
            return (kind === "usb" ? qsTr("%1 stopped sending samples while listening. Tap Retry, then start listening again to reset it.") : qsTr("%1 stopped sending samples while listening. Tap Retry, then start listening again.")).arg(leadName);
        case "failed":
            return hostText || (failureKind === 4 ? qsTr("Android has not given access to %1. Tap Retry to ask again.").arg(name) : qsTr("Android could not open %1. Tap Retry to try again.").arg(name));
        case "ready":
            return hostText || qsTr("%1 is ready.").arg(leadName);
        case "pending":
            return hostText || qsTr("Answer Android's prompt to let DSD-neo use %1.").arg(name);
        case "missing":
            return hostText || qsTr("Plug in the %1, then tap Retry.").arg(name);
        case "available":
            return hostText || (kind.length ? qsTr("%1 is plugged in. Tap Connect to use it.").arg(leadName) : qsTr("A receiver is plugged in. Tap Connect to use it."));
        default:
            return hostText || qsTr("Plug in an RTL-SDR or Airspy, then tap Connect.");
        }
    }

    // Retry after something went wrong, Connect otherwise; nothing while the
    // receiver is ready or Android's prompt is up (Connect returns once the
    // request expires). The action asks for access only; it starts nothing.
    readonly property string actionText: state === "ready" || state === "pending" ? "" : state === "available" || state === "absent" ? qsTr("Connect") : qsTr("Retry")
    readonly property bool canAct: actionText.length > 0 && !!host && !host.sessionActive && !host.transitioning
}
