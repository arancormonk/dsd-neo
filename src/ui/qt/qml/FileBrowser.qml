// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

pragma Singleton
import QtQuick

// Issue #830: the one browse request DSD-neo's file browser serves, app-wide.
// A DocumentPicker that browses opens a request and keeps its token; the
// browser screen shows whatever request is live and ends it with exactly one of
// finish(), reject() or otherApp(), each answered by its signal with the token,
// so only the picker that holds that token acts on it. dismiss() ends it
// silently, for a picker that closes or goes away. Every ending clears
// `request` before its signal, so a handler already sees the browser gone, and
// a token that is not the live one is ignored: a late or repeated ending never
// reaches a newer request.
QtObject {
    id: browser

    // null, or {token, kind ("csv", "backup" or "replay"), title, allowOtherApp}.
    readonly property var request: d.request

    signal picked(int token, string reference, string name)
    signal rejected(int token)
    signal otherAppRequested(int token)

    // A new request for @p kind; returns its token (> 0). A live request is
    // superseded: the new one replaces it first, then its owner hears
    // rejected(), so that owner never sees its own request still showing.
    function open(kind, title, allowOtherApp) {
        var previous = d.request;
        var token = d.lastToken >= 2147483647 ? 1 : d.lastToken + 1;
        d.lastToken = token;
        d.request = {
            token: token,
            kind: kind,
            title: title,
            allowOtherApp: allowOtherApp === true
        };
        // Its owner may open again while it is told, so the token returned is
        // this request's, held before the signal.
        if (previous)
            rejected(previous.token);
        return token;
    }
    // The user picked @p reference, shown as @p name.
    function finish(token, reference, name) {
        if (d.take(token))
            picked(token, reference, name);
    }
    // The user backed out of the browser.
    function reject(token) {
        if (d.take(token))
            rejected(token);
    }
    // The user asked for another app's picker instead.
    function otherApp(token) {
        if (d.take(token))
            otherAppRequested(token);
    }
    // Ends the request without a signal.
    function dismiss(token) {
        d.take(token);
    }

    readonly property QtObject internals: QtObject {
        id: d
        property var request: null
        property int lastToken: 0

        // True when @p token is the live request's, which then ends.
        function take(token) {
            if (!request || !token || request.token !== token)
                return false;
            request = null;
            return true;
        }
    }
}
