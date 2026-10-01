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
