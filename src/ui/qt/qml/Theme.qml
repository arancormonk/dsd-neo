// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

pragma Singleton
import QtQuick

// The "Instrument" token set: the launcher icon's cyan→magenta on near-black, with
// an exact light counterpart whose accents are the same hues darkened for contrast
// on white. Every color and size in the UI resolves through here — screens never
// hard-code a hex value, so the appearance switch is one property flip.
QtObject {
    // 0 = follow the OS, 1 = light, 2 = dark; bound to the persisted preference.
    readonly property int appearance: (typeof prefs !== "undefined" && prefs) ? prefs.appearance : 0
    // Qt.ColorScheme: 0 unknown, 1 light, 2 dark. Unknown lands on dark — the brand
    // surface — rather than on a guess at the OS's intent.
    readonly property bool dark: appearance === 2 || (appearance === 0 && Application.styleHints.colorScheme !== 1)

    readonly property string sans: (typeof sansFontFamily !== "undefined") ? sansFontFamily : "sans-serif"
    readonly property string mono: (typeof monoFontFamily !== "undefined") ? monoFontFamily : "monospace"

    // The fixed pixel type sizes otherwise ignore the platform's larger text
    // preference. Kept writable so compact-height tests can exercise that layout.
    property real fontScale: systemFontScale()
    function systemFontScale() {
        var revision = fontRevision;
        if (typeof decoderHost !== "undefined" && decoderHost && decoderHost.usesPlatformFontScaling === true)
            return decoderHost.fontPixelSize(16) / 16;
        return Math.min(1.6, Math.max(1, Qt.application.font.pixelSize / 16));
    }
    function keyboardTop(item) {
        var host = typeof decoderHost !== "undefined" ? decoderHost : null;
        if (host && typeof host.keyboardTop === "number" && host.keyboardTop >= 0)
            return Math.min(item.height, item.mapFromItem(null, 0, host.keyboardTop).y);
        var keyboard = Qt.inputMethod.keyboardRectangle;
        return Qt.inputMethod.visible && keyboard.height > 0 ? Math.min(item.height, item.mapFromItem(null, 0, keyboard.y).y) : item.height;
    }
    readonly property int fontRevision: (typeof decoderHost !== "undefined" && decoderHost && decoderHost.fontRevision !== undefined) ? decoderHost.fontRevision : 0
    function fontSize(pixels) {
        var revision = fontRevision;
        if (typeof decoderHost !== "undefined" && decoderHost && decoderHost.usesPlatformFontScaling === true)
            return decoderHost.fontPixelSize(pixels);
        return pixels * fontScale;
    }
    function revealFocus(scroll, content, focus) {
        if (!scroll || !content || !focus)
            return;
        var ancestor = focus;
        while (ancestor && ancestor !== content)
            ancestor = ancestor.parent;
        if (ancestor !== content)
            return;
        var target = focus.parent && focus.parent.input === focus ? focus.parent : focus;
        if (target.height > scroll.height)
            target = focus;
        var point = target.mapToItem(content, 0, 0);
        var next = scroll.contentY;
        if (point.y < next)
            next = point.y;
        else if (point.y + target.height > next + scroll.height)
            next = point.y + target.height - scroll.height;
        scroll.contentY = Math.max(0, Math.min(next, scroll.contentHeight - scroll.height));
    }
    function resetFontScale() {
        fontScale = Qt.binding(function () {
            return systemFontScale();
        });
    }

    // Surfaces
    readonly property color bg: dark ? "#0E1116" : "#F2F4F8"
    readonly property color panel: dark ? "#12161E" : "#FFFFFF"
    readonly property color panelBorder: dark ? "#232B3A" : "#E0E4EE"
    readonly property color divider: dark ? "#1A212E" : "#EEF1F6"
    readonly property color controlBorder: dark ? "#233042" : "#D6DBE6"

    // Text
    readonly property color textPrimary: dark ? "#E6EAF2" : "#10141C"
    readonly property color textSecondary: dark ? "#9AA5BD" : "#5B6478"
    readonly property color textSubdued: dark ? "#9AA5BD" : "#5B6478"
    readonly property color buttonSecondaryText: dark ? "#C6CFE0" : "#38415A"

    // Accents — icon hues, darkened in light mode so they hold contrast on white.
    readonly property color cyan: dark ? "#22DCF5" : "#006B91"
    readonly property color magenta: dark ? "#EC1FDC" : "#B306A6"

    readonly property color alert: dark ? "#FFAC88" : "#9C3600"
    readonly property color alertBorder: Qt.alpha(alert, dark ? 0.55 : 0.45)

    // Derived fills
    readonly property color toggleOnTrack: Qt.alpha(cyan, dark ? 0.25 : 0.22)
    readonly property color toggleOffTrack: dark ? "#233042" : "#D6DBE6"
    readonly property color toggleKnobOff: dark ? "#6B7690" : "#FFFFFF"
    readonly property color chipSelectedFill: Qt.alpha(cyan, dark ? 0.10 : 0.08)
    readonly property color encBorder: Qt.alpha(magenta, dark ? 0.40 : 0.35)

    // Radii
    readonly property int radiusPanel: 12
    readonly property int radiusButton: 8
    readonly property int radiusChip: 8

    // Spacing
    readonly property int screenPadding: 18
    readonly property int cardPadding: 16
    readonly property int gap: 13
    readonly property int minimumTouchSize: 48
    readonly property int formWidth: 640

    // One call row. Also the slack the call lists allow around the top before
    // they stop treating the reader as parked on the latest call.
    readonly property int rowHeight: 58
}
