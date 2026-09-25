// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest

// Issue #524: the Radio sheet's AM decode chip. On a radio input it selects the
// AM preset (-fM) and reads back as selected once the engine reports that mode.
// On audio that arrives already demodulated the engine refuses AM, so the chip is
// not offered there, and a note says why rather than leaving a dead control.
Item {
    width: 420
    height: 900

    Loader {
        id: loader
        anchors.fill: parent
        source: uiDir + "/RadioSheet.qml"
    }

    TestCase {
        name: "RadioAm"
        when: windowShown

        readonly property var sheet: loader.item

        function init() {
            verify(sheet !== null, "RadioSheet must load");
            testContext.setHostRunning(true);
            testContext.resetCommands();
            sheet.open();
        }

        function cleanup() {
            testContext.setMetric("radioInput", true);
            testContext.setMetric("decodeMode", 1);
            if (sheet) {
                sheet.forgetRequests();
                sheet.visible = false;
            }
            testContext.setHostRunning(false);
        }

        function test_am_chip_selects_the_am_preset() {
            var chip = findChild(sheet, "radioDecode_AM");
            verify(chip !== null, "the Radio sheet offers no AM chip");
            verify(chip.enabled, "the AM chip is disabled on a radio input");
            verify(!findChild(sheet, "radioDecodeIqNote").visible, "the PCM note shows on a radio input");
            verify(!chip.selected);
            chip.clicked();
            compare(testContext.lastDecodeMode(), commands.decodeModeForFlag("-fM"));
            verify(testContext.lastDecodeMode() > 0, "-fM names no preset");
            testContext.setMetric("decodeMode", commands.decodeModeForFlag("-fM"));
            tryVerify(function () { return chip.selected });
        }

        function test_am_chip_is_not_offered_on_pcm_input() {
            testContext.setMetric("radioInput", false);
            tryVerify(function () { return metrics.radioInput === false });
            var chip = findChild(sheet, "radioDecode_AM");
            verify(chip !== null);
            verify(!chip.enabled, "the AM chip is offered on PCM input");
            verify(findChild(sheet, "radioDecodeIqNote").visible, "no note says why AM is unavailable");
            verify(findChild(sheet, "radioDecode_DMR").enabled, "the digital chips must stay available");
        }
    }
}
