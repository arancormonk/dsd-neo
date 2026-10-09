// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest

// Issue #574: Settings -> Listening's Screen and delay rows, which replaced the
// keep-awake switch. The rows only store the choice; the Android host applies it.
Item {
    width: 420
    height: 900

    Loader {
        id: screenLoader
        anchors.fill: parent
    }

    TestCase {
        name: "SettingsScreenPolicy"
        when: windowShown

        readonly property var modeNames: ["System default", "Always on", "Dim between calls", "Off between calls"]
        readonly property var delayNames: ["10 seconds", "30 seconds", "1 minute", "2 minutes", "5 minutes"]

        // The supported capability is CONSTANT on a real host, so it is set before
        // the screen loads.
        function openScreen(supported, brokered) {
            screenLoader.source = "";
            testContext.setScreenPolicySupported(supported);
            testContext.setScanTestUsb(brokered === true, false);
            screenLoader.source = uiDir + "/SettingsScreen.qml";
            verify(screenLoader.item !== null);
            compare(findChild(screenLoader.item, "settingsListeningHeader").text, "Listening");
        }

        function row(name) {
            var item = findChild(screenLoader.item, name);
            verify(item !== null, name);
            return item;
        }

        // Taps the sheet's real choice button, the way a user would.
        function choose(index) {
            var sheet = row("screenChoices");
            compare(sheet.visible, true);
            var button = findChild(sheet, "screenChoice" + index);
            verify(button !== null, "choice " + index);
            waitForRendering(screenLoader.item);
            mouseClick(button);
            compare(sheet.visible, false);
        }

        function init() {
            testContext.useLifecycleHost(true);
            prefs.screenMode = 0;
            prefs.screenDelaySec = 30;
        }

        function cleanup() {
            screenLoader.source = "";
            prefs.screenMode = 0;
            prefs.screenDelaySec = 30;
            testContext.setScreenPolicySupported(false);
            testContext.setScanTestUsb(false, false);
            testContext.useLifecycleHost(false);
        }

        function test_hidden_without_support() {
            prefs.screenMode = 3;
            openScreen(false);
            compare(row("screenModeRow").visible, false);
            compare(row("screenDelayRow").visible, false);
            // Nothing follows the background row on a desktop host.
            compare(row("backgroundListeningToggle").showDivider, false);
        }

        function test_divider_chain_data() {
            return [
                {tag: "unsupported", supported: false, brokered: false, mode: 3, background: false, attach: false, modeRow: false},
                {tag: "unsupported brokered", supported: false, brokered: true, mode: 3, background: true, attach: false, modeRow: false},
                {tag: "supported system", supported: true, brokered: false, mode: 0, background: true, attach: true, modeRow: false},
                {tag: "supported always on", supported: true, brokered: true, mode: 1, background: true, attach: true, modeRow: false},
                {tag: "supported dim", supported: true, brokered: true, mode: 2, background: true, attach: true, modeRow: true},
                {tag: "supported off", supported: true, brokered: false, mode: 3, background: true, attach: true, modeRow: true}
            ];
        }

        // Each visible row draws a divider only when a visible row follows it.
        function test_divider_chain(data) {
            prefs.screenMode = data.mode;
            openScreen(data.supported, data.brokered);
            compare(row("backgroundListeningToggle").showDivider, data.background);
            compare(row("autoStartOnAttachToggle").visible, data.brokered);
            compare(row("autoStartOnAttachToggle").showDivider, data.attach);
            compare(row("screenModeRow").showDivider, data.modeRow);
            compare(row("screenDelayRow").visible, data.supported && data.mode >= 2);
            compare(row("screenDelayRow").showDivider, false);
        }

        function test_choose_mode_and_delay() {
            openScreen(true);
            var mode = row("screenModeRow");
            var delay = row("screenDelayRow");
            compare(mode.visible, true);
            compare(mode.title, "Screen");
            compare(mode.subtitle, "System default · Follows your phone's screen timeout");
            compare(delay.visible, false, "System default has no delay to choose");

            mode.activate();
            var sheet = row("screenChoices");
            compare(sheet.accessibleName, "Screen");
            compare(sheet.choices, modeNames);
            choose(3);
            compare(prefs.screenMode, 3);
            compare(delay.visible, true);
            compare(delay.title, "Turn off after");
            compare(delay.subtitle, "30 seconds");
            compare(mode.subtitle, "Off between calls · While listening, dims after 30 seconds without audio, then turns off. Audio turns it back on if DSD-neo was on screen when it went off.");
            compare(mode.showDivider, true);

            delay.activate();
            compare(sheet.accessibleName, "Turn off after");
            compare(sheet.choices, delayNames);
            choose(3);
            compare(prefs.screenDelaySec, 120);
            compare(delay.subtitle, "2 minutes");

            mode.activate();
            choose(2);
            compare(prefs.screenMode, 2);
            compare(delay.title, "Dim after");
            compare(mode.subtitle, "Dim between calls · While listening, dims after 2 minutes without audio. Audio or a tap brightens it.");

            // Reopening Settings shows what was saved.
            openScreen(true);
            mode = row("screenModeRow");
            delay = row("screenDelayRow");
            compare(mode.subtitle, "Dim between calls · While listening, dims after 2 minutes without audio. Audio or a tap brightens it.");
            compare(delay.visible, true);
            compare(delay.title, "Dim after");
            compare(delay.subtitle, "2 minutes");

            mode.activate();
            choose(1);
            compare(prefs.screenMode, 1);
            compare(mode.subtitle, "Always on · Stays on while DSD-neo is open");
            compare(delay.visible, false);
            compare(prefs.screenDelaySec, 120, "the delay is kept for the next between-calls mode");
        }

        function test_every_delay_choice() {
            prefs.screenMode = 2;
            openScreen(true);
            var delay = row("screenDelayRow");
            var seconds = [10, 30, 60, 120, 300];
            compare(prefs.screenDelayChoices, seconds);
            for (var i = 0; i < seconds.length; ++i) {
                delay.activate();
                choose(i);
                compare(prefs.screenDelaySec, seconds[i]);
                compare(delay.subtitle, delayNames[i]);
            }
        }

        function test_cancel_keeps_the_choice() {
            prefs.screenMode = 3;
            prefs.screenDelaySec = 60;
            openScreen(true);
            row("screenModeRow").activate();
            var sheet = row("screenChoices");
            compare(sheet.visible, true);
            // The last button is Cancel; it carries no choice name.
            compare(findChild(sheet, "screenChoice" + modeNames.length), null);
            sheet.requestDismiss();
            compare(sheet.visible, false);
            compare(prefs.screenMode, 3);
            compare(prefs.screenDelaySec, 60);
            compare(row("screenDelayRow").subtitle, "1 minute");
        }
    }
}
