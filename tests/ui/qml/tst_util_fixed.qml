// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest
import "../../../src/ui/qt/qml/Util.js" as Util

// A signed reading (Util.fixed) never shows a negative zero: Number.toFixed keeps
// the sign of a value that rounds to zero, so -0.04 would read "-0.0" (issue #669).
TestCase {
    name: "UtilFixed"

    function test_fixed_data() {
        return [
            { tag: "rounds to zero, one place", value: -0.04, digits: 1, text: "0.0" },
            { tag: "rounds to zero, no places", value: -0.3, digits: 0, text: "0" },
            { tag: "negative zero", value: -0.0, digits: 1, text: "0.0" },
            { tag: "zero", value: 0, digits: 2, text: "0.00" },
            { tag: "keeps a real negative", value: -0.06, digits: 1, text: "-0.1" },
            { tag: "keeps a whole negative", value: -1234.4, digits: 0, text: "-1234" },
            { tag: "positive", value: 31.94, digits: 1, text: "31.9" },
            { tag: "string input", value: "-0.01", digits: 1, text: "0.0" },
            { tag: "not a number", value: NaN, digits: 1, text: "NaN" }
        ]
    }

    function test_fixed(data) {
        compare(Util.fixed(data.value, data.digits), data.text)
    }

    // The native rounding stays: the helper only drops the sign of an all-zero text.
    function test_matches_to_fixed() {
        var values = [0.15, 2.25, -2.25, -22.5, 99.95, -99.95, 11459.2]
        for (var i = 0; i < values.length; ++i) {
            compare(Util.fixed(values[i], 1), values[i].toFixed(1))
            compare(Util.fixed(values[i], 0), values[i].toFixed(0))
        }
    }
}
