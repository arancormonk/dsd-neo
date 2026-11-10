// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest
import "../../../src/ui/qt/qml/Util.js" as Util

// Issue #830: a picked file named from its reference alone (the wizard's
// replay pick, a CSV pick with no name) reads the reference's last segment,
// decoded once, and a reference whose escape is malformed as it came.
TestCase {
    name: "UtilReferenceName"

    function test_reference_name_data() {
        return [
            { tag: "provider document", reference: "content://provider/document/primary%3ADownload%2Fmap.csv", name: "map.csv" },
            { tag: "file reference", reference: "file:///storage/emulated/0/Download/a%20b%231%25.csv", name: "a b#1%.csv" },
            { tag: "decoded once", reference: "file:///sdcard/map%2520v2.csv", name: "map%20v2.csv" },
            { tag: "malformed escape", reference: "file:///sdcard/100%.wav", name: "100%.wav" },
            { tag: "plain path", reference: "/data/replays/cap.wav", name: "cap.wav" },
            { tag: "no folder", reference: "cap.wav", name: "cap.wav" }
        ]
    }

    function test_reference_name(data) {
        compare(Util.referenceName(data.reference), data.name)
    }
}
