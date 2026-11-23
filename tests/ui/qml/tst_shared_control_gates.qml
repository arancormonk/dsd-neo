// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick
import QtTest
import "../../../src/ui/qt/qml" as Ui

// Issue #902 B9: the shared controls take their accessible name, their
// description, and whether a screen reader may stop on them, from their own
// navigation gate (docs/code_map.md, issue #830). Covered by a sheet or hidden,
// each is out of the tree, nameless and not focusable; uncovered or shown
// again, each is named and focusable once more. A covered "Export
// detections…" used to keep its name, and a covered IconButton stayed a stop.
// A scroll pane is never a stop (it scrolls; its contents are the stops), so
// only its name follows the gate.
Item {
    id: root

    width: 360
    height: 640

    // What a sheet over the screen leaves reachable: only itself.
    Item {
        id: cover
    }

    Column {
        width: parent.width

        Ui.IconButton {
            id: iconButton
            icon: "close"
        }
        Ui.OutlineButton {
            id: outlineButton
            text: "Export detections…"
        }
        Ui.GradientButton {
            id: gradientButton
            width: parent.width
            text: "Start search"
        }
        Ui.DisclosureRow {
            id: disclosureRow
            title: "Call recordings"
            subtitle: "3 folders"
        }
        // A list view writes the accessible flag of each delegate it lays out,
        // with no change signal (issue #830): a row made under a cover is
        // named from its gate once the cover goes, never from that flag.
        ListView {
            id: list

            width: parent.width
            height: 200
            model: rows
            delegate: Ui.OutlineButton {
                required property string label
                text: label
            }
        }
    }

    ListModel {
        id: rows
    }

    Column {
        x: root.width / 2
        width: root.width / 2

        Ui.PlayCircle {
            id: playCircle
            accessibleName: "Listen to County"
        }
        Ui.MapControlButton {
            id: mapButton
            accessibleName: "Zoom in"
            accessibleDescription: "Shows more detail"
        }
        Ui.DecodeChip {
            id: decodeChip
            text: "P25"
        }
        Ui.DashedActionButton {
            id: dashedButton
            width: parent.width
            text: "Add a system"
        }
        Ui.PinButton {
            id: pinButton
            text: "Pin"
        }
        Ui.SegmentedControl {
            id: segmented
            width: parent.width
            accessibleName: "Appearance"
            model: ["Light", "Dark"]
        }
        Ui.ToggleRow {
            id: toggleRow
            title: "Record calls"
            subtitle: "Keeps a WAV of each call"
        }
        Ui.PlexSwitch {
            id: plexSwitch
            accessibleName: "Listen to Fire"
            accessibleDescription: "Rank 1"
        }
        Ui.PlexInput {
            id: plexInput
            width: parent.width
            label: "Gain"
        }
        Ui.PlexTextField {
            id: plexTextField
            width: parent.width
            label: "Host"
            unit: "IP address or name"
        }
        Ui.FrequencyField {
            id: frequencyField
            width: parent.width
            fieldObjectName: "gateFrequencyInput"
        }
        Ui.PlexComboBox {
            id: namedCombo
            width: parent.width
            accessibleName: "Entry protocol"
            model: ["P25", "DMR"]
        }
        Ui.PlexComboBox {
            id: unnamedCombo
            width: parent.width
            model: ["Auto", "Manual"]
        }
        Ui.MicroLabel {
            id: microLabel
            text: "Saved systems"
        }
        Ui.ListenTabs {
            id: listenTabs
            width: parent.width
            model: [{id: "systems", label: "Systems"}, {id: "lists", label: "Scan lists"}]
            currentId: "systems"
        }
        Ui.BottomNav {
            id: bottomNav
            width: parent.width
        }
        Ui.PlexFlickable {
            id: flickable
            width: parent.width
            height: 40
            contentHeight: 200
        }
    }

    TestCase {
        name: "SharedControlGates"
        when: windowShown

        readonly property var hideable: [iconButton, outlineButton, gradientButton, disclosureRow, playCircle, mapButton,
            decodeChip, dashedButton, pinButton, segmented, toggleRow, plexSwitch, plexInput, plexTextField, frequencyField,
            namedCombo, unnamedCombo, microLabel, listenTabs, bottomNav, flickable]

        function cleanup() {
            Ui.Navigation.modals = [];
            rows.clear();
            for (var control of hideable)
                control.visible = true;
        }
        // The descendants of @p item whose accessible name is @p name.
        function named(item, name) {
            for (var child of item.children) {
                if (child.Accessible.name === name)
                    return child;
                var found = named(child, name);
                if (found)
                    return found;
            }
            return null;
        }

        function controls_data() {
            return [
                {tag: "IconButton", control: iconButton, name: "Close"},
                {tag: "OutlineButton", control: outlineButton, name: "Export detections…"},
                {tag: "GradientButton", control: gradientButton, name: "Start search"},
                {tag: "DisclosureRow", control: disclosureRow, name: "Call recordings", description: "3 folders"},
                {tag: "PlayCircle", control: playCircle, name: "Listen to County"},
                {tag: "MapControlButton", control: mapButton, name: "Zoom in", description: "Shows more detail"},
                {tag: "DecodeChip", control: decodeChip, name: "P25"},
                {tag: "DashedActionButton", control: dashedButton, name: "Add a system"},
                {tag: "PinButton", control: pinButton, name: "Pin"},
                {tag: "SegmentedControl", control: segmented, name: "Appearance", notAStop: true},
                {tag: "SegmentedControl segment", control: segmented, node: "Dark", name: "Dark"},
                {tag: "ToggleRow", control: toggleRow, name: "Record calls", description: "Keeps a WAV of each call"},
                {tag: "PlexSwitch", control: plexSwitch, name: "Listen to Fire", description: "Rank 1"},
                {tag: "PlexInput", control: plexInput, name: "Gain"},
                {tag: "PlexTextField", control: plexTextField, node: function () { return plexTextField.input; }, name: "Host",
                    description: "IP address or name"},
                {tag: "FrequencyField", control: frequencyField, node: function () { return findChild(frequencyField, "gateFrequencyInput"); },
                    name: "Frequency in MHz"},
                {tag: "PlexComboBox named", control: namedCombo, name: "Entry protocol", description: "P25"},
                {tag: "PlexComboBox unnamed", control: unnamedCombo, name: "Auto", description: ""},
                {tag: "MicroLabel", control: microLabel, name: "Saved systems"},
                {tag: "ListenTabs tab", control: listenTabs, node: "Scan lists", name: "Scan lists"},
                {tag: "BottomNav tab", control: bottomNav, node: "History", name: "History"},
                {tag: "PlexFlickable", control: flickable, name: "Scrollable content", notAStop: true}
            ];
        }
        // The accessible node a row checks: the control, a part of it, or the
        // part named @p data.node while in the tree (found before the cover).
        function nodeOf(data) {
            if (typeof data.node === "function")
                return data.node();
            if (typeof data.node === "string")
                return named(data.control, data.node);
            return data.control;
        }
        function inTree(control, data, label) {
            verify(!control.Accessible.ignored, label + ": in the tree");
            compare(control.Accessible.name, data.name, label + ": named");
            if (data.notAStop)
                verify(!control.Accessible.focusable, label + ": never a stop");
            else
                verify(control.Accessible.focusable, label + ": a stop");
            if (data.description !== undefined)
                compare(control.Accessible.description, data.description, label + ": described");
        }
        function outOfTree(control, data, label) {
            verify(control.Accessible.ignored, label + ": out of the tree");
            compare(control.Accessible.name, "", label + ": nameless");
            verify(!control.Accessible.focusable, label + ": no stop");
            if (data.description !== undefined)
                compare(control.Accessible.description, "", label + ": no description");
        }

        function test_controls_under_a_cover_data() {
            return controls_data();
        }
        function test_controls_under_a_cover(data) {
            var node = nodeOf(data);
            verify(node !== null, "the node is there");
            inTree(node, data, "before");
            Ui.Navigation.modals = [cover];
            outOfTree(node, data, "covered");
            Ui.Navigation.modals = [];
            inTree(node, data, "dismissed");
        }

        function test_controls_hidden_data() {
            return controls_data();
        }
        function test_controls_hidden(data) {
            var node = nodeOf(data);
            verify(node !== null, "the node is there");
            data.control.visible = false;
            outOfTree(node, data, "hidden");
            data.control.visible = true;
            inTree(node, data, "shown");
        }

        function test_a_row_made_under_a_cover_is_named_once_dismissed() {
            Ui.Navigation.modals = [cover];
            rows.append({label: "Export all"});
            tryVerify(function () { return list.itemAtIndex(0) !== null; });
            var made = list.itemAtIndex(0);
            compare(made.Accessible.name, "");
            verify(!made.Accessible.focusable);
            Ui.Navigation.modals = [];
            compare(made.Accessible.name, "Export all");
            verify(made.Accessible.focusable);
            verify(!made.Accessible.ignored);
        }
    }
}
