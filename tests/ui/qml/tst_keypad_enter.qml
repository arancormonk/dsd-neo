// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// Issue #646 (B6): a keyboard's keypad Enter presses the focused control, as
// Return and Space do. OutlineButton, IconButton and PinButton took it; the
// shared controls below (a Listen tab's Add and Play among them) and the
// screens' own buttons that take Return did not, so keypad Enter chose a
// segment's neighbour on one row and did nothing on the next. Each goes
// through the control's one guarded activation, so a disabled control or one
// under an open sheet still ignores every key.
Item {
    id: root

    width: 420
    height: 900

    // Stands in for an open sheet: the top modal, which none of the controls
    // below is inside, so Navigation.allows() refuses them all. The field is
    // where the sheet takes the focus.
    Item {
        id: blocker

        y: 860
        width: 100
        height: 30

        TextInput {
            id: sheetField

            width: parent.width
            height: parent.height
            activeFocusOnTab: true
        }
    }

    Column {
        id: controls

        width: 360

        Ui.SegmentedControl {
            id: segmented

            width: parent.width
            model: ["One", "Two", "Three"]
        }
        Ui.BottomNav {
            id: nav

            width: parent.width
        }
        Ui.DashedActionButton {
            id: dashed

            width: parent.width
            text: "Add a system"
        }
        Ui.PlayCircle {
            id: play
        }
        Ui.CallRow {
            id: callRow

            width: parent.width
            interactive: true
            name: "Fire dispatch"
        }
        Ui.DisclosureRow {
            id: disclosure

            title: "Imports"
        }
        Ui.PlexSwitch {
            id: plexSwitch

            accessibleName: "Background listening"
        }
        Ui.ToggleRow {
            id: toggleRow

            title: "Background listening"
        }
        Ui.IconButton {
            id: iconButton
        }
        Ui.DecodeChip {
            id: decodeChip

            text: "P25"
        }
        Ui.FilterPill {
            id: filterPill

            text: "All"
        }
        Ui.BandRail {
            id: bandRail

            width: parent.width
            tunedHz: 851.0125e6
        }
        Ui.PlexTextField {
            id: textField

            width: parent.width
            label: "Name"
        }
    }

    SignalSpy {
        id: spy
    }

    Loader {
        id: screenLoader

        width: root.width
        height: root.height
        active: false
    }

    TestCase {
        id: tc

        name: "KeypadEnter"
        when: windowShown

        // A control that keeps the focus as a sheet opens over it holds its Tab
        // stop until the focus leaves; Qt warns when a binding asks to drop the
        // stop of the item holding the focus.
        function init() {
            failOnWarning(/activeFocusOnTab/);
        }
        function cleanup() {
            Ui.Navigation.removeModal(blocker);
            for (var control of [segmented, nav, dashed, play, callRow, disclosure, plexSwitch, toggleRow, iconButton, decodeChip, filterPill, bandRail, textField]) {
                control.enabled = true;
                control.visible = true;
            }
            disclosure.tapEnabled = true;
            segmented.currentIndex = 0;
            nav.currentIndex = 0;
            spy.clear();
            // Name and target change together: a spy holding the last case's
            // name looks it up on the next target and logs that it is missing.
            spy.target = null;
            spy.signalName = "";
            screenLoader.active = false;
            screenLoader.source = "";
            // After the screen is gone, so a case that failed half way leaks
            // no host, playback, recordings or history into the next one.
            testContext.resetHostPlayback();
            recordings.reset();
            callHistory.clearAll();
            testContext.useLifecycleHost(false);
            testContext.setRadioReference("busy", false);
            testContext.setRadioReference("sites", []);
            testContext.setRadioReference("systemDetails", {});
            testContext.setRadioReference("countries", []);
            testContext.setRadioReference("credentialsReady", false);
            root.width = 420;
            testContext.setMetric("radioInput", false);
            testContext.setHostRunning(false);
            testContext.resetCommands();
        }

        // The delegate a segmented control or the bottom bar draws for `index`.
        function choiceOf(control, index) {
            var found = null;
            (function walk(node) {
                for (var child of node.children) {
                    if (found)
                        return;
                    if (child.index === index && child.modelData !== undefined)
                        found = child;
                    else
                        walk(child);
                }
            })(control);
            return found;
        }

        // Stands in for a sheet opening over the focused control, which keeps
        // the focus (and so its keys) until the sheet takes it.
        function cover() {
            Ui.Navigation.addModal(blocker);
        }

        // A key acts on its first press only: the repeats of a held Enter,
        // keypad Enter, Return or Space press nothing (a sheet's choice that
        // hands the keyboard back while the key is down opened the sheet again)
        // and go no further than the control.
        function heldKeysActOnce(tag, target) {
            target.forceActiveFocus();
            for (var key of [[Qt.Key_Enter, Qt.KeypadModifier], [Qt.Key_Enter, Qt.NoModifier], [Qt.Key_Return, Qt.NoModifier], [Qt.Key_Space, Qt.NoModifier]]) {
                for (var i = 0; i < 2; ++i)
                    verify(testContext.sendKey(target, key[0], key[1], true, true), tag + ": a repeat of " + key[0] + " is taken");
                keyRelease(key[0], key[1]);
            }
        }

        // Enter, Return and Space each press the focused control once; under a
        // sheet or disabled, none of them does anything.
        function pressAndBlock(tag, target, control) {
            target.forceActiveFocus();
            verify(target.activeFocus, tag + " takes focus");
            keyClick(Qt.Key_Enter, Qt.KeypadModifier);
            compare(spy.count, 1, tag + ": keypad Enter presses it");
            keyClick(Qt.Key_Return);
            compare(spy.count, 2, tag + ": Return presses it");
            keyClick(Qt.Key_Space);
            compare(spy.count, 3, tag + ": Space presses it");
            heldKeysActOnce(tag, target);
            compare(spy.count, 3, tag + ": a held key's repeats press nothing");

            cover();
            verify(target.activeFocus, tag + " keeps its focus under the sheet");
            keyClick(Qt.Key_Enter, Qt.KeypadModifier);
            keyClick(Qt.Key_Return);
            keyClick(Qt.Key_Space);
            compare(spy.count, 3, tag + ": a covered control ignores every key");
            Ui.Navigation.removeModal(blocker);

            control.enabled = false;
            keyClick(Qt.Key_Enter, Qt.KeypadModifier);
            keyClick(Qt.Key_Return);
            target.Accessible.pressAction();
            compare(spy.count, 3, tag + ": a disabled control ignores every key");
            control.enabled = true;
        }

        function test_shared_controls_take_keypad_enter_data() {
            return [
                {
                    tag: "SegmentedControl",
                    control: segmented,
                    choice: 1,
                    signalName: "selected"
                },
                {
                    tag: "BottomNav",
                    control: nav,
                    choice: 1,
                    signalName: "selected"
                },
                {
                    tag: "DashedActionButton",
                    control: dashed,
                    signalName: "clicked"
                },
                {
                    tag: "PlayCircle",
                    control: play,
                    signalName: "clicked"
                },
                {
                    tag: "CallRow",
                    control: callRow,
                    signalName: "activated"
                },
                {
                    tag: "DisclosureRow",
                    control: disclosure,
                    signalName: "tapped"
                },
                {
                    tag: "PlexSwitch",
                    control: plexSwitch,
                    signalName: "toggled"
                },
                {
                    tag: "ToggleRow",
                    control: toggleRow,
                    signalName: "toggled"
                }
            ];
        }
        function test_shared_controls_take_keypad_enter(data) {
            var target = data.choice !== undefined ? choiceOf(data.control, data.choice) : data.control;
            verify(target !== null, data.tag + " has its choice");
            spy.signalName = data.signalName;
            spy.target = data.control;
            pressAndBlock(data.tag, target, data.control);
            if (data.choice !== undefined) {
                for (var args of spy.signalArguments)
                    compare(args[0], data.choice, data.tag + " chose the focused one");
            }
        }

        // A control covered by a sheet while it has the focus keeps its Tab stop
        // only while it keeps the focus: once the sheet takes the focus, Tab
        // cannot land back on the control under it. Shown again, it is a stop.
        function test_a_covered_control_is_no_tab_stop_once_focus_leaves_data() {
            return [
                {
                    tag: "SegmentedControl",
                    control: segmented,
                    choice: 1
                },
                {
                    tag: "BottomNav",
                    control: nav,
                    choice: 1
                },
                {
                    tag: "DashedActionButton",
                    control: dashed
                },
                {
                    tag: "PlayCircle",
                    control: play
                },
                {
                    tag: "CallRow",
                    control: callRow
                },
                {
                    tag: "DisclosureRow",
                    control: disclosure
                },
                {
                    tag: "PlexSwitch",
                    control: plexSwitch
                },
                {
                    tag: "ToggleRow",
                    control: toggleRow
                },
                {
                    tag: "IconButton",
                    control: iconButton
                },
                {
                    tag: "DecodeChip",
                    control: decodeChip
                },
                {
                    tag: "FilterPill",
                    control: filterPill
                },
                {
                    tag: "BandRail",
                    control: bandRail
                },
                {
                    tag: "PlexTextField",
                    control: textField.input
                }
            ];
        }
        function holdsTabStopOnlyWhileFocused(tag, target) {
            verify(target !== null, tag + " exists");
            verify(target.activeFocusOnTab, tag + " is a Tab stop");
            target.forceActiveFocus();
            verify(target.activeFocus, tag + " takes focus");
            cover();
            verify(target.activeFocusOnTab, tag + " holds its stop while it has the focus");
            sheetField.forceActiveFocus();
            verify(sheetField.activeFocus, "the sheet takes the focus");
            verify(!target.activeFocusOnTab, tag + " is no Tab stop under the sheet");
            Ui.Navigation.removeModal(blocker);
            verify(target.activeFocusOnTab, tag + " is a Tab stop again");
        }
        function test_a_covered_control_is_no_tab_stop_once_focus_leaves(data) {
            holdsTabStopOnlyWhileFocused(data.tag, data.choice !== undefined ? choiceOf(data.control, data.choice) : data.control);
        }

        // The same hold on the screens' own stops.
        function test_a_covered_screen_stop_is_no_tab_stop_once_focus_leaves_data() {
            return [
                {
                    tag: "Listen device status pill",
                    file: "HomeScreen.qml",
                    brokered: true,
                    reach: function (screen) {
                        return findChild(screen, "dongleStatusPill");
                    }
                },
                {
                    tag: "scan list Advanced header",
                    file: "ScanListScreen.qml",
                    reach: function (screen) {
                        screen.openFor(-1);
                        return findChild(screen, "scanAdvancedToggle");
                    }
                },
                {
                    tag: "wizard Advanced header",
                    file: "WizardScreen.qml",
                    reach: function (screen) {
                        screen.openForAdd(false);
                        // The Advanced panel is on the second step.
                        screen.step = 1;
                        return findChild(screen, "wizardAdvancedHeader");
                    }
                },
                {
                    tag: "call recording seek bar",
                    file: "HistoryDetailSheet.qml",
                    reach: function (sheet) {
                        var path = "/rec/County (12345678)/Keypad.wav";
                        var uid = callHistory.pushWithRecordings([{
                                id: "k-1",
                                path: path,
                                fileName: "Keypad.wav",
                                folderUid: "u1",
                                start: 1700000000,
                                durationMs: 10000,
                                rate: 8000,
                                bytes: 96000,
                                failed: false,
                                available: true
                            }]);
                        sheet.open({
                            uid: uid,
                            name: "Dispatch",
                            tg: 1001,
                            when: 1700000000,
                            systemUid: "test-system"
                        });
                        findChild(sheet, "recordingPlayButton").activate();
                        testContext.publishPlayback(path, 2, 1000, 10000, "");
                        return findChild(sheet, "recordingSeekBar");
                    }
                }
            ];
        }
        function test_a_covered_screen_stop_is_no_tab_stop_once_focus_leaves(data) {
            // The Listen pill shows only where the host brokers USB; that is
            // fixed before the host is published (see tst_home_dongle_status).
            if (data.brokered) {
                testContext.setDongleStatus(false, 0, "");
                testContext.useLifecycleHost(true);
            }
            testContext.resetHostPlayback();
            recordings.reset();
            callHistory.clearAll();
            var screen = load(data.file);
            holdsTabStopOnlyWhileFocused(data.tag, data.reach(screen));
        }

        // DisclosureRow's own gate (a row that goes nowhere right now).
        function test_a_disclosure_that_goes_nowhere_ignores_keypad_enter() {
            spy.signalName = "tapped";
            spy.target = disclosure;
            disclosure.forceActiveFocus();
            disclosure.tapEnabled = false;
            keyClick(Qt.Key_Enter, Qt.KeypadModifier);
            compare(spy.count, 0);
            disclosure.tapEnabled = true;
            keyClick(Qt.Key_Enter, Qt.KeypadModifier);
            compare(spy.count, 1);
        }

        // A hidden Add or Play (a Listen tab not shown) leaves the tree, and a
        // late press reaching it (an accessibility press from a node Android
        // still holds, or a key while it keeps the focus) does nothing.
        function test_a_hidden_add_or_play_ignores_every_press_data() {
            return [
                {
                    tag: "DashedActionButton",
                    control: dashed,
                    setsFocusable: true
                },
                {
                    tag: "PlayCircle",
                    control: play
                }
            ];
        }
        function test_a_hidden_add_or_play_ignores_every_press(data) {
            var control = data.control;
            spy.signalName = "clicked";
            spy.target = control;
            control.forceActiveFocus();
            control.visible = false;
            verify(control.Accessible.ignored, data.tag + " leaves the tree while hidden");
            if (data.setsFocusable)
                verify(!control.Accessible.focusable, data.tag + " is no swipe stop while hidden");
            control.Accessible.pressAction();
            keyClick(Qt.Key_Enter, Qt.KeypadModifier);
            keyClick(Qt.Key_Return);
            keyClick(Qt.Key_Space);
            compare(spy.count, 0, data.tag + ": a hidden control ignores every press");
            control.visible = true;
            verify(!control.Accessible.ignored);
            control.Accessible.pressAction();
            compare(spy.count, 1, data.tag + " acts again once shown");
        }

        function load(file) {
            screenLoader.source = uiDir + "/" + file;
            screenLoader.active = true;
            tryCompare(screenLoader, "status", Loader.Ready);
            return screenLoader.item;
        }
        function item(parent, name) {
            var found = findChild(parent, name);
            verify(found !== null, name + " exists");
            return found;
        }
        // Each screen button that takes Return: under a sheet no key presses it,
        // nor does a held key's repeat, then keypad Enter does, once. `effect` reads how many times it has
        // acted. Covered first, since a press can close what holds the button.
        function enterActsOnce(tag, target, effect) {
            target.forceActiveFocus();
            verify(target.activeFocus, tag + " takes focus");
            var before = effect();
            cover();
            verify(target.activeFocus, tag + " keeps its focus under the sheet");
            keyClick(Qt.Key_Enter, Qt.KeypadModifier);
            keyClick(Qt.Key_Return);
            compare(effect(), before, tag + ": under a sheet no key presses it");
            Ui.Navigation.removeModal(blocker);
            // Before it acts, as acting can take it away.
            heldKeysActOnce(tag, target);
            compare(effect(), before, tag + ": a held key's repeats press nothing");
            keyClick(Qt.Key_Enter, Qt.KeypadModifier);
            compare(effect(), before + 1, tag + ": keypad Enter presses it");
        }

        function test_spectrum_buttons_take_keypad_enter() {
            var screen = load("SpectrumScreen.qml");
            screen.exploring = true;
            tryVerify(function () {
                return spectrum.hasData;
            }, 5000, "no spectrum frame arrived");
            spectrum.resetView();
            var tunes = function () {
                return testContext.manualTuneCalls();
            };
            enterActsOnce("Tune down", item(screen, "spectrumStepDown"), tunes);
            enterActsOnce("Tune up", item(screen, "spectrumStepUp"), tunes);
            var toward = testContext.spectrumPeakHz() > spectrum.centerFreqHz ? "spectrumSignalUp" : "spectrumSignalDown";
            enterActsOnce("Signal hop", item(screen, toward), tunes);
            var sweeps = 0;
            var sweepChanged = function () {
                ++sweeps;
            };
            screen.sweepingChanged.connect(sweepChanged);
            enterActsOnce("Sweep", item(screen, "spectrumSweepToggle"), function () {
                return sweeps;
            });
            screen.sweepingChanged.disconnect(sweepChanged);
            screen.stopSweep();
        }

        function test_monitor_buttons_take_keypad_enter() {
            root.width = 800;
            testContext.setHostRunning(true);
            testContext.setMetric("radioInput", true);
            var monitor = load("MonitorScreen.qml");
            var spectrumButton = item(monitor, "openSpectrumButton");
            tryVerify(function () {
                return spectrumButton.visible;
            }, 2000, "the Spectrum button shows on a wide screen");
            spy.signalName = "openSpectrum";
            spy.target = monitor;
            enterActsOnce("Spectrum", spectrumButton, function () {
                return spy.count;
            });
            // Hidden while it has the focus (the input stops being a radio, or
            // the header narrows): no key and no late press opens the Spectrum.
            spectrumButton.forceActiveFocus();
            testContext.setMetric("radioInput", false);
            verify(!spectrumButton.visible, "the Spectrum button hides off a radio");
            var shown = spy.count;
            keyClick(Qt.Key_Enter, Qt.KeypadModifier);
            keyClick(Qt.Key_Return);
            keyClick(Qt.Key_Space);
            compare(spy.count, shown, "a hidden Spectrum button ignores its keys");
            spectrumButton.Accessible.pressAction();
            compare(spy.count, shown, "a hidden Spectrum button ignores a late accessibility press");
            verify(spectrumButton.Accessible.ignored, "a hidden Spectrum button leaves the tree");
            testContext.setMetric("radioInput", true);
            var keySheet = item(monitor, "keySheet");
            var opens = 0;
            enterActsOnce("Decryption", item(monitor, "openKeySheetButton"), function () {
                if (keySheet.visible) {
                    ++opens;
                    keySheet.visible = false;
                }
                return opens;
            });
        }

        function test_radioreference_rows_take_keypad_enter() {
            var screen = load("RadioReferenceScreen.qml");
            screen.visible = true;
            testContext.setRadioReference("countries", [
                {
                    name: "Canada",
                    coid: 2
                },
                {
                    name: "United States",
                    coid: 1
                }
            ]);
            var browse = item(screen, "radioReferenceCountrySheet");
            browse.visible = true;
            spy.signalName = "chosen";
            spy.target = browse;
            enterActsOnce("Browse row", item(browse, "browseRow0"), function () {
                return spy.count;
            });
            // A chosen country opens the next level; close every level.
            for (var level of ["radioReferenceCountrySheet", "radioReferenceStateSheet", "radioReferenceCountySheet"])
                item(screen, level).visible = false;
            compare(Ui.Navigation.topModal, null, "no browse sheet is left open");

            testContext.setRadioReference("credentialsReady", true);
            testContext.setRadioReference("systemDetails", {
                sid: 6673,
                name: "Test System",
                typeDescr: "Project 25",
                flavorDescr: "Phase II"
            });
            testContext.setRadioReference("sites", [
                {
                    siteNumber: 1,
                    descr: "Alpha",
                    freqCount: 11,
                    controlFreqMhz: "851.0125",
                    simulcast: false,
                    freqMhz: "851.0125",
                    colorCode: ""
                },
                {
                    siteNumber: 2,
                    descr: "Bravo",
                    freqCount: 13,
                    controlFreqMhz: "852.5",
                    simulcast: false,
                    freqMhz: "852.5",
                    colorCode: ""
                }
            ]);
            tryVerify(function () {
                return screen.systemLoaded;
            });
            var siteRow = null;
            (function walk(node) {
                for (var child of node.children) {
                    // A site not chosen yet: choosing the chosen one again changes nothing.
                    if (!siteRow && child.chosen === false && child.modelData !== undefined && child.modelData.site !== undefined)
                        siteRow = child;
                    else
                        walk(child);
                }
            })(screen);
            verify(siteRow !== null, "a site row not chosen exists");
            var toggles = 0;
            var toggled = function () {
                ++toggles;
            };
            screen.selectedSitesChanged.connect(toggled);
            enterActsOnce("Site row", siteRow, function () {
                return toggles;
            });
            screen.selectedSitesChanged.disconnect(toggled);

            // Leaving with a site chosen asks first: the question is the press.
            verify(screen.selectedSites.length > 0);
            var question = item(screen, "radioReferenceLeaveDiscard");
            var asked = 0;
            enterActsOnce("All systems row", item(screen, "radioReferenceBackToResults"), function () {
                if (question.visible) {
                    ++asked;
                    question.visible = false;
                }
                return asked;
            });
        }

        // A RadioReference row that has the focus when a lookup starts keeps
        // its Tab stop until the focus leaves (init's failOnWarning catches
        // the warning a dropped stop of the focused item gives). The lookup's
        // overlay usually takes the focus first, but Qt does not promise that
        // order, so it is held closed here: the row's gate then changes while
        // the row still has the focus.
        function holdsTabStopWhileBusy(tag, target, screen) {
            item(screen, "radioReferenceBusyOverlay").visible = false;
            verify(target.activeFocusOnTab, tag + " is a Tab stop");
            target.forceActiveFocus();
            verify(target.activeFocus, tag + " takes focus");
            testContext.setRadioReference("busy", true);
            verify(target.activeFocus, tag + " keeps the focus");
            verify(target.activeFocusOnTab, tag + " holds its stop while it has the focus");
            sheetField.forceActiveFocus();
            verify(sheetField.activeFocus, "the focus leaves");
            verify(!target.activeFocusOnTab, tag + " is no Tab stop during a lookup");
            testContext.setRadioReference("busy", false);
            verify(target.activeFocusOnTab, tag + " is a Tab stop again");
        }
        function test_radioreference_rows_hold_their_tab_stop_while_busy() {
            var screen = load("RadioReferenceScreen.qml");
            screen.visible = true;
            testContext.setRadioReference("countries", [{name: "Canada", coid: 2}]);
            var browse = item(screen, "radioReferenceCountrySheet");
            browse.visible = true;
            holdsTabStopWhileBusy("Browse row", item(browse, "browseRow0"), screen);
            browse.visible = false;

            testContext.setRadioReference("credentialsReady", true);
            testContext.setRadioReference("systemDetails", {sid: 6673, name: "Test System", typeDescr: "Project 25",
                flavorDescr: "Phase II"});
            testContext.setRadioReference("sites", [{siteNumber: 1, descr: "Alpha", freqCount: 11,
                controlFreqMhz: "851.0125", simulcast: false, freqMhz: "851.0125", colorCode: ""}]);
            tryVerify(function () {
                return screen.systemLoaded;
            });
            var siteRow = null;
            (function walk(node) {
                for (var child of node.children) {
                    if (!siteRow && child.modelData !== undefined && child.modelData.site !== undefined)
                        siteRow = child;
                    else
                        walk(child);
                }
            })(screen);
            verify(siteRow !== null, "a site row exists");
            holdsTabStopWhileBusy("Site row", siteRow, screen);
        }

        function test_wizard_file_choice_takes_keypad_enter() {
            var path = testContext.writeFixtureCsv("keypad-groups.csv", "id,mode,name\n1001,A,Dispatch\n");
            var imported = testContext.importNow(path, "group");
            verify(imported.ok);
            var wizard = load("WizardScreen.qml");
            wizard.openForAdd(false);
            wizard.pickerTarget = "group";
            var sheet = item(wizard, "wizardCsvSheet");
            var chosen = 0;
            var choose = function () {
                sheet.visible = true;
                var choice = null;
                (function walk(node) {
                    for (var child of node.children) {
                        if (!choice && child.modelData !== undefined && child.modelData.path === imported.path && child.choose !== undefined)
                            choice = child;
                        else
                            walk(child);
                    }
                })(sheet);
                return choice;
            };
            var choice = choose();
            verify(choice !== null, "the imported file is offered");
            enterActsOnce("File choice", choice, function () {
                if (wizard.groupCsvPath === imported.path) {
                    ++chosen;
                    wizard.groupCsvPath = "";
                }
                return chosen;
            });
            sheet.visible = false;
            importedFiles.remove(importedFiles.rowForPath(imported.path));
        }
    }
}
