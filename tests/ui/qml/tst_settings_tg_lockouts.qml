// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest

Item {
    width: 420
    height: 900
    Loader { id: screenLoader; anchors.fill: parent }

    TestCase {
        name: "SettingsTgLockouts"
        when: windowShown

        function openScreen() {
            screenLoader.source = uiDir + "/SettingsScreen.qml";
            verify(screenLoader.item !== null);
            compare(findChild(screenLoader.item, "settingsListeningHeader").text, "Listening");
            return findChild(screenLoader.item, "persistTgLockoutsToggle");
        }

        function init() {
            testContext.useLifecycleHost(true);
            testContext.resetCommands();
            prefs.persistTgLockouts = true;
            testContext.setMetric("persistTgLockouts", true);
            testContext.setMetric("optionsKnown", true);
            testContext.setMetric("temporaryTgAvoidCount", 0);
            testContext.setMetric("callSkipCount", 0);
            testContext.setMetric("tgPolicyContext", "9007199254740993");
        }

        function cleanup() {
            screenLoader.source = "";
            prefs.persistTgLockouts = true;
            testContext.useLifecycleHost(false);
            testContext.resetCommands();
            testContext.setMetric("persistTgLockouts", true);
            testContext.setMetric("optionsKnown", false);
            testContext.setMetric("temporaryTgAvoidCount", 0);
            testContext.setMetric("callSkipCount", 0);
            testContext.setMetric("tgPolicyContext", "0");
        }

        function test_avoid_copy() {
            var toggle = openScreen();
            compare(toggle.title, "Save avoided talkgroups");
            compare(toggle.subtitle, "Applies to Avoid TG immediately, saving it to the talkgroup list. Off keeps avoids until stop or list reload, and the button reads Avoid TG. Skip is unaffected. Talkgroup-list edits still save.");
        }

        function test_clear_call_skip_without_avoids() {
            testContext.setLifecyclePhase(2);
            testContext.setMetric("callSkipCount", 1);
            openScreen();
            var clear = findChild(screenLoader.item, "clearTemporaryTgAvoidsButton");
            verify(clear.visible && clear.enabled);
            compare(clear.text, "Clear temporary avoids and call skips — current list (1)");
            clear.activate();
            compare(commands.avoidClearRequests(), ["9007199254740993"]);
            testContext.setMetric("temporaryTgAvoidCount", 2);
            compare(clear.text, "Clear temporary avoids and call skips — current list (3)");
            testContext.setMetric("temporaryTgAvoidCount", 0);
            testContext.setMetric("callSkipCount", 0);
            tryCompare(clear, "visible", false);
        }

        function test_stopped_preference() {
            var toggle = openScreen();
            toggle.activate();
            compare(prefs.persistTgLockouts, false);
            compare(toggle.checked, false);
            compare(commands.lockoutRequests().length, 0);
            screenLoader.source = "";
            toggle = openScreen();
            compare(toggle.checked, false);
        }

        function test_running_acknowledgement_and_reattach() {
            testContext.setLifecyclePhase(2);
            var toggle = openScreen();
            toggle.activate();
            compare(commands.lockoutRequests(), [false]);
            compare(prefs.persistTgLockouts, false);
            compare(toggle.checked, true); // Engine snapshot remains authoritative.
            compare(toggle.enabled, false);
            testContext.setMetric("persistTgLockouts", false);
            tryCompare(toggle, "checked", false);
            tryCompare(toggle, "enabled", true);
            prefs.persistTgLockouts = true; // A stored default must not override the running service.
            screenLoader.source = "";
            toggle = openScreen();
            compare(toggle.checked, false);
            compare(commands.lockoutRequests().length, 1);
        }

        function test_rejection_and_transition() {
            testContext.setLifecyclePhase(2);
            var toggle = openScreen();
            commands.setLockoutAccepted(false);
            toggle.activate();
            compare(toggle.checked, true);
            compare(prefs.persistTgLockouts, true);
            verify(findChild(screenLoader.item, "tgLockoutError").text.length > 0);
            testContext.setLifecyclePhase(3);
            compare(toggle.enabled, false);
            toggle.activate();
            compare(commands.lockoutRequests().length, 1);
            testContext.setLifecyclePhase(1);
            compare(toggle.enabled, false);
        }

        function test_clear_captures_current_list() {
            testContext.setLifecyclePhase(2);
            testContext.setMetric("temporaryTgAvoidCount", 3);
            openScreen();
            var clear = findChild(screenLoader.item, "clearTemporaryTgAvoidsButton");
            verify(clear.visible);
            clear.activate();
            compare(commands.avoidClearRequests(), ["9007199254740993"]);
            testContext.setMetric("temporaryTgAvoidCount", 0);
            testContext.setMetric("callSkipCount", 0);
            tryCompare(clear, "visible", false);
        }
    }
}
