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
    // below is inside, so Navigation.allows() refuses them all.
    Item {
        id: blocker
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

        function cleanup() {
            Ui.Navigation.removeModal(blocker);
            for (var control of [segmented, nav, dashed, play, callRow, disclosure, plexSwitch, toggleRow])
                control.enabled = true;
            disclosure.tapEnabled = true;
            segmented.currentIndex = 0;
            nav.currentIndex = 0;
            spy.clear();
            spy.target = null;
            screenLoader.active = false;
            screenLoader.source = "";
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

        // Stands in for a sheet opening over the focused control. A control
        // whose Tab stop follows the navigation gate alone asks to drop it
        // here, which Qt refuses (and says so) for the item holding the
        // focus; the focus stays, so its keys still reach it.
        function cover(target, dropsTabStop) {
            if (dropsTabStop)
                ignoreWarning("QQuickItem: Cannot set activeFocusOnTab to false once item is the active focus item.");
            Ui.Navigation.addModal(blocker);
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

            cover(target, true);
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
            spy.target = data.control;
            spy.signalName = data.signalName;
            pressAndBlock(data.tag, target, data.control);
            if (data.choice !== undefined) {
                for (var args of spy.signalArguments)
                    compare(args[0], data.choice, data.tag + " chose the focused one");
            }
        }

        // DisclosureRow's own gate (a row that goes nowhere right now).
        function test_a_disclosure_that_goes_nowhere_ignores_keypad_enter() {
            spy.target = disclosure;
            spy.signalName = "tapped";
            disclosure.forceActiveFocus();
            ignoreWarning("QQuickItem: Cannot set activeFocusOnTab to false once item is the active focus item.");
            disclosure.tapEnabled = false;
            keyClick(Qt.Key_Enter, Qt.KeypadModifier);
            compare(spy.count, 0);
            disclosure.tapEnabled = true;
            keyClick(Qt.Key_Enter, Qt.KeypadModifier);
            compare(spy.count, 1);
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
        // then keypad Enter does, once. `effect` reads how many times it has
        // acted. Covered first, since a press can close what holds the button.
        function enterActsOnce(tag, target, effect, dropsTabStop) {
            target.forceActiveFocus();
            verify(target.activeFocus, tag + " takes focus");
            var before = effect();
            cover(target, dropsTabStop !== false);
            verify(target.activeFocus, tag + " keeps its focus under the sheet");
            keyClick(Qt.Key_Enter, Qt.KeypadModifier);
            keyClick(Qt.Key_Return);
            compare(effect(), before, tag + ": under a sheet no key presses it");
            Ui.Navigation.removeModal(blocker);
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
            spy.target = monitor;
            spy.signalName = "openSpectrum";
            enterActsOnce("Spectrum", spectrumButton, function () {
                return spy.count;
            });
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
            spy.target = browse;
            spy.signalName = "chosen";
            enterActsOnce("Browse row", item(browse, "browseRow0"), function () {
                return spy.count;
            }, false);
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
            }, false);
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
            testContext.setRadioReference("sites", []);
            testContext.setRadioReference("systemDetails", {});
            testContext.setRadioReference("countries", []);
            testContext.setRadioReference("credentialsReady", false);
        }

        function test_wizard_file_choice_takes_keypad_enter() {
            var path = testContext.writeFixtureCsv("keypad-groups.csv", "id,mode,name\n1001,A,Dispatch\n");
            var imported = importedFiles.importFile(path, "keypad-groups.csv", "group");
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
