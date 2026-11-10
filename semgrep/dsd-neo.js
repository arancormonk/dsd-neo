// SPDX-License-Identifier: GPL-3.0-or-later
// Intentional rule fixtures; run with semgrep --test, never load.

function clockReads() {
    // ruleid: dsd-neo.no-direct-clock-read-js
    var nowMs = Date.now();
    // ruleid: dsd-neo.no-direct-clock-read-js
    var spaced = Date . now ();
    // ruleid: dsd-neo.no-direct-clock-read-js
    var current = new Date();
    // ruleid: dsd-neo.no-direct-clock-read-js
    var bare = new Date;
    // ruleid: dsd-neo.no-direct-clock-read-js
    var text = Date();
    // ruleid: dsd-neo.no-direct-clock-read-js
    var label = Qt.formatDateTime(new Date(), "hh:mm");
    return [nowMs, spaced, current, bare, text, label];
}

function namedDomainReads(row, metrics, savedSystems, myDate) {
    // ok: dsd-neo.no-direct-clock-read-js
    var age = metrics.decodeNowMs - row.when * 1000;
    // ok: dsd-neo.no-direct-clock-read-js
    var heard = savedSystems.realtimeNowMs();
    // Converting a stored stamp, and other names that contain Date or now, are not clock reads.
    // ok: dsd-neo.no-direct-clock-read-js
    var stamped = new Date(row.when * 1000);
    // ok: dsd-neo.no-direct-clock-read-js
    var day = Qt.formatDate(new Date(row.importedAt * 1000), "MMM d");
    // ok: dsd-neo.no-direct-clock-read-js
    var parsed = Date.parse("2026-01-01T00:00:00Z");
    // ok: dsd-neo.no-direct-clock-read-js
    var utc = Date.UTC(2026, 0, 1);
    // ok: dsd-neo.no-direct-clock-read-js
    var custom = myDate.now();
    // ok: dsd-neo.no-direct-clock-read-js
    var refreshed = row.lastDate();
    return [age, heard, stamped, day, parsed, utc, custom, refreshed];
}

function textMatchLimit() {
    // The rule is a text match (Semgrep has no QML parser), so a comment that spells the call is flagged too.
    // ruleid: dsd-neo.no-direct-clock-read-js
    // Date.now()
    return 0;
}

// Issue #831: QML text for the tap-handler rules, which read .qml as text.
var tapHandlerFixtures = `
Item {
    // ruleid: dsd-neo.qml-bare-tap-handler
    TapHandler {
        onTapped: act()
    }
    // ruleid: dsd-neo.qml-bare-tap-handler
    QtQuick.TapHandler{ onAllowedTap: act() }
    // ok: dsd-neo.qml-bare-tap-handler
    GatedTapHandler {
        enabled: control.enabled
        onAllowedTap: function (eventPoint) {
            if (eventPoint.position.x > 0) {
                act();
            }
        }
        // ruleid: dsd-neo.qml-gated-tap-ungated-signal
        onLongPressed: manage()
    }
    GatedTapHandler {
        // ruleid: dsd-neo.qml-gated-tap-ungated-signal
        onTapped: act()
    }
    GatedTapHandler {
        // ok: dsd-neo.qml-gated-tap-ungated-signal
        onAllowedTap: act()
        // ok: dsd-neo.qml-gated-tap-ungated-signal
        onAllowedLongPress: manage()
    }
    DisclosureRow {
        // A row's own tapped signal is not a handler's.
        // ok: dsd-neo.qml-gated-tap-ungated-signal
        onTapped: open()
    }
}
`;
