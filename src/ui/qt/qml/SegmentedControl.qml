// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// Segmented selector (the Appearance setting): equal segments in one outlined
// track, selected segment tinted cyan.
Rectangle {
    id: control

    property var model: []
    property int currentIndex: 0
    signal selected(int index)

    property real labelHeight: 0
    // Issue #646: every label at one size, the smallest any of them needs for
    // its words to fit (WholeWordLabel), so a shrunk label never sits beside
    // larger ones.
    property real labelPixelSize: 0
    function shareLabelSize() {
        var size = 0;
        for (var i = 0; i < segments.count; ++i) {
            var item = segments.itemAt(i);
            if (item && (size <= 0 || item.fitPixelSize < size))
                size = item.fitPixelSize;
        }
        labelPixelSize = size;
    }
    function measureLabels() {
        var measured = 0;
        for (var i = 0; i < segments.count; ++i) {
            var item = segments.itemAt(i);
            if (item)
                measured = Math.max(measured, item.textHeight);
        }
        labelHeight = measured;
    }
    implicitHeight: Math.max(54, labelHeight + 30)
    radius: Theme.radiusButton
    color: Theme.dark ? Theme.bg : Theme.panel
    border.width: 1
    border.color: Theme.controlBorder

    Row {
        anchors.fill: parent
        anchors.margins: 3

        Repeater {
            id: segments
            model: control.model
            onItemAdded: {
                control.shareLabelSize();
                Qt.callLater(control.measureLabels);
            }
            onItemRemoved: {
                control.shareLabelSize();
                Qt.callLater(control.measureLabels);
            }

            Rectangle {
                required property int index
                required property var modelData
                // The label's laid-out height (code_map "Qt QML editing
                // convention"), at the size it is drawn at.
                readonly property real textHeight: segmentLabel.height
                onTextHeightChanged: Qt.callLater(control.measureLabels)
                readonly property real fitPixelSize: segmentLabel.fitPixelSize
                onFitPixelSizeChanged: control.shareLabelSize()

                // Held while it has the focus (see OutlineButton).
                activeFocusOnTab: (Navigation.allows(control) && enabled) || activeFocus
                Accessible.role: Accessible.RadioButton
                Accessible.name: modelData
                Accessible.checkable: true
                Accessible.checked: active
                readonly property bool navigationAllowed: Navigation.allows(control)
                Accessible.ignored: !navigationAllowed || !visible
                Accessible.onPressAction: choose()
                function choose() {
                    if (!enabled || !Navigation.allows(control))
                        return;
                    // Issue #718: a choice made mid-word keeps the word.
                    Navigation.commitComposing(false);
                    control.selected(index);
                }
                Keys.onSpacePressed: choose()
                Keys.onReturnPressed: choose()
                Keys.onEnterPressed: choose()
                FocusFrame {}
                readonly property bool active: control.currentIndex === index

                width: (control.width - 6) / control.model.length
                height: parent.height
                radius: Theme.radiusButton - 3
                color: active ? Theme.chipSelectedFill : "transparent"
                border.width: active ? 1 : 0
                border.color: Theme.cyan

                // Issue #646: whole words at every text size; the segment's
                // name reads the label in full when it is elided.
                WholeWordLabel {
                    id: segmentLabel
                    anchors.centerIn: parent
                    width: parent.width - 12
                    horizontalAlignment: Text.AlignHCenter
                    text: modelData
                    basePixelSize: 13
                    measureWeight: Font.DemiBold
                    sharedPixelSize: control.labelPixelSize
                    font.weight: parent.active ? Font.DemiBold : Font.Normal
                    color: parent.active ? Theme.cyan : Theme.textSecondary
                }

                GatedTapHandler {
                    onAllowedTap: choose()
                }
            }
        }
    }
}
