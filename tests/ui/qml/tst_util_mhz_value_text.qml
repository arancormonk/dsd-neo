// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest
import "../../../src/ui/qt/qml/Util.js" as Util

// A frequency written to be tuned or saved (Util.mhzValueText) keeps the Hz that
// Util.mhzText rounds away for display. A whole multiple of 10 Hz keeps the
// display spelling, so the values saved before read the same.
TestCase {
    name: "UtilMhzValueText"

    function test_value_text_data() {
        return [
            { tag: "4-decimal channel", hz: 851012500, text: "851.0125" },
            { tag: "5-decimal channel", hz: 769768750, text: "769.76875" },
            { tag: "whole MHz", hz: 460000000, text: "460.0000" },
            { tag: "3.125 kHz raster", hz: 851015625, text: "851.015625" },
            { tag: "5 Hz below a tie", hz: 155000005, text: "155.000005" },
            { tag: "1 Hz", hz: 851012501, text: "851.012501" },
            { tag: "a fraction of a Hz rounds", hz: 851015624.6, text: "851.015625" },
            { tag: "zero", hz: 0, text: "0.0000" }
        ]
    }

    function test_value_text(data) {
        compare(Util.mhzValueText(data.hz), data.text)
    }

    // The display keeps its 10 Hz rounding.
    function test_display_is_unchanged() {
        compare(Util.mhzText(851015625), "851.01563")
        compare(Util.fmtMhz(851012500), "851.0125 MHz")
    }
}
