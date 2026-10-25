// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

Item {
    id: root
    width: 411
    height: 700
    // The readings under test live in the Monitor's "Signal & site" disclosure (#718), open here.
    Ui.MonitorScreen { id: monitor; anchors.fill: parent; detailsExpanded: true }
    TestCase {
        name: "MonitorSite"
        when: windowShown
        function item(name) {
            var found = findChild(monitor, name);
            verify(found !== null, name + " exists");
            return found;
        }
        function cleanup() {
            testContext.setMetric("siteLine", "");
            testContext.setMetric("siteConfirmed", false);
            testContext.setMetric("siteProtocol", "");
            testContext.setMetric("p25NacValid", false);
            testContext.setMetric("dmrColorCode", -1);
            testContext.setMetric("dmrSiteText", "");
            testContext.setMetric("dmrRestLsn", 0);
            testContext.setMetric("dmrXptFreeLcn", -1);
            testContext.setMetric("ccFreqHz", 0);
            testContext.setMetric("vcFreqHz", 0);
            testContext.setMetric("nxdnRan", -1);
            testContext.setMetric("nxdnSiteCode", 0);
            testContext.setMetric("nxdnSysCode", 0);
            testContext.setMetric("edacsSiteText", "");
            testContext.setMetric("edacsSystem", -1);
            testContext.setMetric("edacsArea", -1);
            testContext.setMetric("edacsCcLcn", -1);
            testContext.setMetric("dmrBrand", "");
            testContext.setMetric("dmrT3Syscode", -1);
            testContext.setMetric("siteAccessCode", -1);
            testContext.setMetric("tetraSiteMcc", -1);
            testContext.setMetric("tetraSiteMnc", -1);
            testContext.setMetric("tetraSiteLa", -1);
            testContext.setMetric("p25Phase2Seed", "");
            testContext.setMetric("p25Phase2SeedSet", false);
            testContext.setMetric("dmrRasDetected", false);
            testContext.setMetric("dmrRasNeedsRelaxed", false);
            testContext.setMetric("relaxedCrcConfigured", false);
            testContext.setMetric("relaxedCrcEffective", false);
            testContext.setMetric("relaxedCrcRowOverride", false);
            testContext.setMetric("optionsKnown", false);
            Ui.Theme.resetFontScale();
            root.width = 411;
            root.height = 700;
        }
        function test_cleanup_restores_platform_font_binding() {
            var previousFont = testContext.applicationFont();
            try {
                // Qt.application.font is CONSTANT: change the font before cleanup
                // so the restored binding evaluates the platform's current value.
                testContext.setApplicationFont(Qt.font({pixelSize: 24}));
                Ui.Theme.fontScale = 1.6;
                cleanup();
                compare(Ui.Theme.fontScale, 1.5);
            } finally {
                testContext.setApplicationFont(previousFont);
                Ui.Theme.resetFontScale();
            }
        }
        function test_site_row_and_sheet() {
            verify(!item("siteRow").visible);
            testContext.setMetric("siteProtocol", "P25");
            testContext.setMetric("siteLine", "P25 · NAC 293");
            testContext.setMetric("p25NacValid", true);
            testContext.setMetric("p25Nac", 659);
            testContext.setMetric("siteConfirmed", true);
            var row = item("siteRow");
            tryCompare(row, "visible", true);
            compare(row.opacity, 1);
            waitForRendering(row);
            mouseClick(row, row.width / 2, row.height / 2);
            tryCompare(item("siteSheet"), "visible", true);
            compare(item("siteNac").text, "293");
            verify(!item("siteWacn").visible);
            testContext.setMetric("siteConfirmed", false);
            tryVerify(function() { return row.opacity < 1; });
            verify(item("siteRetained").visible);
            testContext.setMetric("siteLine", "");
            tryCompare(row, "visible", false);
            tryCompare(item("siteSheet"), "visible", false);
        }
        // The Phase 2 seed is configuration, apart from the NAC the carrier sent: "(set)" when set by hand.
        function test_phase2_seed_row() {
            testContext.setMetric("siteProtocol", "P25");
            testContext.setMetric("siteLine", "P25 · NAC 2A1");
            testContext.setMetric("p25NacValid", true);
            testContext.setMetric("p25Nac", 0x2A1);
            item("siteSheet").visible = true;
            tryCompare(item("siteNac"), "visible", true);
            compare(item("siteNac").text, "2A1");
            verify(!item("sitePhase2Seed").visible);
            testContext.setMetric("p25Phase2Seed", "BEE00.348.293");
            testContext.setMetric("p25Phase2SeedSet", true);
            tryCompare(item("sitePhase2Seed"), "visible", true);
            compare(item("sitePhase2Seed").text, "BEE00.348.293 (set)");
            testContext.setMetric("p25Phase2SeedSet", false);
            tryCompare(item("sitePhase2Seed"), "text", "BEE00.348.293");
            compare(item("siteNac").text, "2A1");
            item("siteSheet").visible = false;
        }
        function test_protocol_grids_and_frequencies() {
            testContext.setMetric("siteLine", "DMR · CC 7");
            testContext.setMetric("siteProtocol", "DMR");
            testContext.setMetric("dmrColorCode", 7);
            testContext.setMetric("dmrSiteText", "<b>Net 12 Site 3</b> ");
            testContext.setMetric("dmrRestLsn", 4);
            testContext.setMetric("ccFreqHz", 851012500);
            testContext.setMetric("vcFreqHz", 852000000);
            item("siteSheet").visible = true;
            tryCompare(item("siteDmrColor"), "visible", true);
            compare(item("siteDmrColor").text, "7");
            compare(item("siteDmrText").text, "<b>Net 12 Site 3</b> ");
            compare(item("siteDmrText").textFormat, Text.PlainText);
            compare(item("siteCcFreq").text, "851.012500 MHz");
            compare(item("siteVcFreq").text, "852.000000 MHz");
            // A Hytera XPT site's free channel, 0 included, once received.
            verify(!item("siteDmrXptFree").visible);
            testContext.setMetric("dmrXptFreeLcn", 0);
            tryCompare(item("siteDmrXptFree"), "visible", true);
            compare(item("siteDmrXptFree").text, "0");
            testContext.setMetric("siteProtocol", "IDAS");
            testContext.setMetric("nxdnRan", -1);
            verify(!item("siteDmrColor").visible);
            verify(!item("siteRan").visible);
            testContext.setMetric("nxdnRan", 0);
            testContext.setMetric("nxdnSiteCode", 3);
            testContext.setMetric("nxdnSysCode", 12);
            tryCompare(item("siteRan"), "visible", true);
            compare(item("siteRan").text, "0");
            compare(item("siteNxdnSys").text, "12");
            // An SCCH site ID message names an IDAS site's code without its System ID.
            testContext.setMetric("nxdnSysCode", 0);
            tryCompare(item("siteNxdnSys"), "visible", false);
            verify(item("siteNxdnSite").visible);
            testContext.setMetric("siteProtocol", "EDACS");
            testContext.setMetric("edacsSiteText", "SITE 007 [07]");
            tryCompare(item("siteEdacs"), "visible", true);
            compare(item("siteEdacs").text, "SITE 007 [07]");
        }
        // Every protocol whose site row opens the sheet shows there what the carrier sent: dPMR's colour code, M17's
        // CAN, TETRA's network, colour code and location area, and the parts of a DMR or EDACS site with no site text.
        function test_access_tetra_and_part_rows() {
            testContext.setMetric("siteProtocol", "M17");
            testContext.setMetric("siteLine", "M17 · CAN 3");
            testContext.setMetric("siteAccessCode", 3);
            item("siteSheet").visible = true;
            tryCompare(item("siteAccess"), "visible", true);
            compare(item("siteAccess").text, "3");
            compare(item("siteAccess").parent.children[0].text, "CAN");
            testContext.setMetric("siteProtocol", "dPMR");
            testContext.setMetric("siteLine", "dPMR · CC 0");
            testContext.setMetric("siteAccessCode", 0);
            tryCompare(item("siteAccess"), "text", "0");
            compare(item("siteAccess").parent.children[0].text, "Color code");
            verify(item("siteAccess").visible);
            testContext.setMetric("siteAccessCode", -1);
            tryCompare(item("siteAccess"), "visible", false);

            testContext.setMetric("siteProtocol", "TETRA");
            testContext.setMetric("siteLine", "TETRA · MCC 262 · MNC 1001 · CC 25 · LA 4242");
            testContext.setMetric("tetraSiteMcc", 262);
            testContext.setMetric("tetraSiteMnc", 1001);
            testContext.setMetric("siteAccessCode", 25);
            testContext.setMetric("tetraSiteLa", 4242);
            tryCompare(item("siteTetraMcc"), "visible", true);
            verify(!item("siteAccess").visible);
            compare(item("siteTetraMcc").text, "262");
            compare(item("siteTetraMnc").text, "1001");
            compare(item("siteTetraColor").text, "25");
            compare(item("siteTetraLa").text, "4242");
            testContext.setMetric("tetraSiteLa", -1);
            tryCompare(item("siteTetraLa"), "visible", false);

            testContext.setMetric("siteProtocol", "DMR");
            testContext.setMetric("siteLine", "DMR · Hytera XPT · SYS 0A3F");
            testContext.setMetric("dmrBrand", "Hytera XPT");
            testContext.setMetric("dmrT3Syscode", 0x0A3F);
            tryCompare(item("siteDmrBrand"), "visible", true);
            verify(!item("siteTetraMcc").visible);
            compare(item("siteDmrBrand").text, "Hytera XPT");
            compare(item("siteDmrSyscode").text, "0A3F");

            testContext.setMetric("siteProtocol", "EDACS");
            testContext.setMetric("siteLine", "EDACS · SYS 3 · Area 0 · CC LCN 12");
            testContext.setMetric("edacsSystem", 3);
            testContext.setMetric("edacsArea", 0);
            testContext.setMetric("edacsCcLcn", 12);
            tryCompare(item("siteEdacsSystem"), "visible", true);
            verify(!item("siteEdacs").visible);
            compare(item("siteEdacsSystem").text, "3");
            compare(item("siteEdacsArea").text, "0");
            compare(item("siteEdacsCcLcn").text, "12");
            item("siteSheet").visible = false;
        }
        // DMR RAS (Restricted Access to System): the field and its hint read the app-control RAS view, in the
        // terminal's and the console notice's words with the app's name for -F, and the sheet turns Relaxed CRC on
        // through the same command and preference Settings uses.
        function rasHelp() {
            return "Relaxed CRC accepts control and data whose CRC check failed, and marks them. Turned on here, it stays on for later sessions until Settings turns it off.";
        }
        function rasNeedsRelaxed() {
            return "DMR RAS (Restricted Access to System) detected: control and data fail the standard CRC; Relaxed CRC decodes them.";
        }
        function test_dmr_ras_field_and_relaxed_crc() {
            testContext.useLifecycleHost(true);
            testContext.resetCommands();
            prefs.relaxedCrc = false;
            try {
                testContext.setMetric("siteProtocol", "DMR");
                testContext.setMetric("siteLine", "DMR · CC 1 · RAS");
                testContext.setMetric("siteConfirmed", true);
                testContext.setMetric("dmrColorCode", 1);
                testContext.setMetric("optionsKnown", true);
                item("siteSheet").visible = true;
                // No RAS: no field, no hint, no action.
                tryCompare(item("siteDmrColor"), "visible", true);
                verify(!item("siteDmrRas").visible);
                verify(!item("siteDmrRasNote").visible);
                verify(!item("siteRelaxedCrcButton").visible);

                // RAS while Relaxed CRC is off.
                testContext.setMetric("dmrRasDetected", true);
                testContext.setMetric("dmrRasNeedsRelaxed", true);
                tryCompare(item("siteDmrRas"), "visible", true);
                compare(item("siteDmrRas").text, "RAS");
                compare(item("siteDmrRas").parent.children[0].text, "Access");
                verify(item("siteDmrRasNote").visible);
                compare(item("siteDmrRasNote").text, rasNeedsRelaxed());
                var button = item("siteRelaxedCrcButton");
                verify(button.visible && button.enabled);
                compare(button.text, "Turn on Relaxed CRC");
                // What the action changes, beside it: the app-wide setting, in the Settings row's words.
                verify(item("siteRelaxedCrcHelp").visible);
                compare(item("siteRelaxedCrcHelp").text, rasHelp());
                button.forceActiveFocus(Qt.TabFocusReason);
                button.activate();
                compare(commands.relaxedCrcRequests(), [true]);
                compare(prefs.relaxedCrc, true);
                compare(button.enabled, false); // waits for the session to confirm
                verify(item("siteCloseButton").activeFocus, "focus moves to Close before the button disables");
                testContext.setMetric("relaxedCrcConfigured", true);
                testContext.setMetric("relaxedCrcEffective", true);
                testContext.setMetric("dmrRasNeedsRelaxed", false);
                tryCompare(button, "visible", false);
                verify(!item("siteRelaxedCrcHelp").visible);
                verify(item("siteCloseButton").activeFocus, "a keyboard user keeps a place in the sheet");
                compare(item("siteDmrRasNote").text, "Relaxed CRC is on, so this RAS system's control and data decode.");
                compare(item("siteDmrRas").text, "RAS");

                // Relaxed CRC configured on under a scan row with its own --strict-crc: the row decides for this
                // channel, the hint says so, and there is nothing left to turn on.
                testContext.setMetric("relaxedCrcRowOverride", true);
                testContext.setMetric("relaxedCrcEffective", false);
                testContext.setMetric("dmrRasNeedsRelaxed", true);
                tryCompare(item("siteDmrRasNote"), "text", rasNeedsRelaxed()
                    + " This channel sets strict CRC itself; Relaxed CRC applies from the next channel without its own.");
                verify(!item("siteRelaxedCrcButton").visible);
                verify(!item("siteRelaxedCrcHelp").visible);

                // Configured off under the same strict row: nothing says the configured value covers the next channel,
                // and turning it on still matters for the channels after this one.
                testContext.setMetric("relaxedCrcConfigured", false);
                tryCompare(item("siteDmrRasNote"), "text", rasNeedsRelaxed());
                verify(item("siteRelaxedCrcButton").visible && item("siteRelaxedCrcButton").enabled);
                compare(item("siteRelaxedCrcHelp").text, rasHelp()
                    + " This channel sets strict CRC itself, so it applies from the next channel.");
                testContext.resetCommands();
                item("siteRelaxedCrcButton").activate();
                compare(commands.relaxedCrcRequests(), [true]);
                compare(item("siteRelaxedCrcButton").enabled, false);
                // A request belongs to the sheet it was made in: closed and opened again, the action is live.
                item("siteSheet").visible = false;
                item("siteSheet").visible = true;
                verify(item("siteRelaxedCrcButton").enabled, "closing the sheet drops the pending request");
                // So does a session change.
                item("siteRelaxedCrcButton").activate();
                compare(item("siteRelaxedCrcButton").enabled, false);
                testContext.setLifecyclePhase(2);
                verify(item("siteRelaxedCrcButton").enabled, "a session change drops the pending request");
                testContext.setLifecyclePhase(0);

                // A refused command leaves the preference alone and says so.
                testContext.setMetric("relaxedCrcRowOverride", false);
                testContext.setMetric("relaxedCrcConfigured", false);
                prefs.relaxedCrc = false;
                commands.setRelaxedCrcAccepted(false);
                tryCompare(item("siteRelaxedCrcButton"), "visible", true);
                item("siteRelaxedCrcButton").activate();
                compare(prefs.relaxedCrc, false);
                compare(item("siteDmrRasNote").text, rasNeedsRelaxed());
                compare(item("siteRelaxedCrcError").text, "Could not change the running decoder's setting. Try again.");
                verify(item("siteRelaxedCrcError").visible);

                // Another protocol's sheet never shows the DMR field.
                testContext.setMetric("siteProtocol", "P25");
                testContext.setMetric("siteLine", "P25 · NAC 293");
                tryCompare(item("siteDmrRas"), "visible", false);
                verify(!item("siteDmrRasNote").visible);
                verify(!item("siteRelaxedCrcButton").visible);
                verify(!item("siteRelaxedCrcError").visible);
                item("siteSheet").visible = false;
            } finally {
                prefs.relaxedCrc = false;
                testContext.resetCommands();
                testContext.useLifecycleHost(false);
            }
        }
        function test_compact_site_reachable() {
            root.width = 320;
            root.height = 360;
            Ui.Theme.fontScale = 1.6;
            testContext.setMetric("siteLine", "P25 · WACN BEE00 · SYS 123 · NAC 293 · RFSS 2 · SITE 3");
            var row = item("siteRow");
            var body = item("recentCallsList");
            tryCompare(row, "visible", true);
            // The first of the readings, inside the one list that scrolls (#718): the
            // short screen scrolls it into reach, and it takes the tap there.
            var ancestor = row;
            while (ancestor && ancestor !== body.contentItem)
                ancestor = ancestor.parent;
            compare(ancestor, body.contentItem);
            compare(row.y, 0);
            verify(row.height >= 44);
            waitForRendering(monitor);
            body.contentY = Math.min(row.mapToItem(body.contentItem, 0, 0).y, body.originY + body.contentHeight - body.height);
            waitForRendering(row);
            verify(row.mapToItem(body, 0, 0).y >= -0.5 && row.mapToItem(body, 0, 20).y <= body.height);
            mouseClick(row, row.width / 2, Math.min(20, row.height / 2));
            tryCompare(item("siteSheet"), "visible", true);
            item("siteSheet").visible = false;
        }
    }
}
