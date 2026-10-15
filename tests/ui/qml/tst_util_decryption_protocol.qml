// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest
import "../../../src/ui/qt/qml/Util.js" as Util

// The decryption editors and the profile selector offer what the protocol a saved
// system's decode flag names can use (Util.decryptionProtocol). -fp is ProVoice,
// not dPMR: no profile protocol describes it, so it reads as mixed. dPMR (-fm)
// reads as mixed too, as session_args.cpp does: an NXDN profile's scrambler
// also serves it. UI_QT_DECRYPTION_PROFILES checks the two mappings agree.
TestCase {
    name: "UtilDecryptionProtocol"

    function test_protocol_data() {
        return [
            { tag: "dPMR", flags: "-fm", protocol: "mixed" },
            { tag: "ProVoice", flags: "-fp", protocol: "mixed" },
            { tag: "P25", flags: "-ft", protocol: "p25" },
            { tag: "P25 Simulcast", flags: "-mq", protocol: "p25" },
            { tag: "P25 imported", flags: "-ft -^", protocol: "p25" },
            { tag: "DMR", flags: "-fs", protocol: "dmr" },
            { tag: "DMR scan", flags: "-fs -Y", protocol: "dmr" },
            { tag: "NXDN48", flags: "-fi", protocol: "nxdn" },
            { tag: "NXDN96", flags: "-fn", protocol: "nxdn" },
            { tag: "M17", flags: "-fz", protocol: "m17" },
            { tag: "D-STAR", flags: "-fd", protocol: "dstar" },
            { tag: "YSF", flags: "-fy", protocol: "ysf" },
            { tag: "AM", flags: "-fM", protocol: "mixed" },
            { tag: "Auto", flags: "", protocol: "mixed" }
        ]
    }

    function test_protocol(data) {
        compare(Util.decryptionProtocol(data.flags), data.protocol)
    }
}
