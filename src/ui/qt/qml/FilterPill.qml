// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// History-screen filter pill: fully rounded, cyan-tinted when active.
Rectangle {
    id: control

    property string text: ""
    property bool active: false
    property bool caret: true
    // Opt-in (#831 B8): the label wraps between words inside the width the
    // pill is given, and the pill grows taller, where a one-line pill would be
    // cut off by a narrow sheet or large text. The consumer bounds the width.
    // It is a WholeWordLabel (issue #646): a word wider than the pill is drawn
    // smaller, down to its normal-text size, and only then elided.
    property bool wrapLabel: false
    signal clicked

    property string accessibleName: text
    // Held while it has the focus (see OutlineButton).
    activeFocusOnTab: (Navigation.allows(control) && enabled && visible) || activeFocus
    Accessible.role: Accessible.Button
    Accessible.name: accessibleName
    // Issue #782: focusable only while in the tree, so a copy Android still
    // holds of a covered or hidden control (no event tells it the control
    // left the tree) is not a swipe stop.
    Accessible.focusable: navigationAllowed && visible
    readonly property bool navigationAllowed: Navigation.allows(control)
    Accessible.ignored: !navigationAllowed || !visible
    Accessible.onPressAction: activate()
    function activate() {
        if (!enabled || !Navigation.allows(control))
            return;
        // Issue #718: a choice made mid-word keeps the word; focus stays put.
        Navigation.commitComposing(false);
        clicked();
    }
    Keys.onSpacePressed: activate()
    Keys.onReturnPressed: activate()
    Keys.onEnterPressed: activate()
    FocusFrame {}
    Accessible.selected: active

    readonly property real caretSpace: caret ? caretMark.implicitWidth + pillContent.spacing : 0
    // A wrapping pill's natural width is its label on one line at the full
    // text size: never the wrapped label's, and never a size a fit shrank it
    // to, so it grows back once there is room.
    implicitWidth: (wrapLabel ? wrapped.lineWidth + caretSpace : pillContent.implicitWidth) + 30
    implicitHeight: Math.max(Theme.minimumTouchSize, (wrapLabel ? wrapped.height : label.implicitHeight) + 24)
    // Wrapped over several lines, the ends keep a one-line pill's curve.
    radius: wrapped.lineCount > 1 ? Math.max(Theme.minimumTouchSize, wrapped.height / wrapped.lineCount + 24) / 2 : height / 2
    color: active ? Theme.chipSelectedFill : Theme.panel
    border.width: 1
    border.color: active ? Theme.cyan : Theme.controlBorder

    Row {
        id: pillContent
        anchors.centerIn: parent
        spacing: 6

        Text {
            id: label
            objectName: control.wrapLabel ? "" : "filterPillLabel"
            visible: !control.wrapLabel
            anchors.verticalCenter: parent.verticalCenter
            text: control.text
            // A pill can name a saved system or an imported category.
            textFormat: Text.PlainText
            font.family: Theme.sans
            font.pixelSize: Theme.fontSize(13)
            font.weight: Font.DemiBold
            color: control.active ? Theme.cyan : Theme.buttonSecondaryText
        }

        // Built only for a wrapping pill.
        Loader {
            id: wrapped
            readonly property real lineWidth: item ? item.lineWidth : 0
            readonly property int lineCount: item ? item.lineCount : 0
            active: control.wrapLabel
            visible: active
            anchors.verticalCenter: parent.verticalCenter
            sourceComponent: Component {
                WholeWordLabel {
                    objectName: "filterPillLabel"
                    readonly property real lineWidth: Math.ceil(oneLine.advanceWidth) + 1
                    width: Math.max(0, Math.min(lineWidth, control.width - 30 - control.caretSpace))
                    horizontalAlignment: Text.AlignHCenter
                    text: control.text
                    textFormat: Text.PlainText
                    basePixelSize: 13
                    measureWeight: Font.DemiBold
                    font.weight: Font.DemiBold
                    color: label.color

                    // At the whole size a Text draws the full text size at.
                    TextMetrics {
                        id: oneLine
                        font.family: Theme.sans
                        font.pixelSize: Math.round(Theme.fontSize(13))
                        font.weight: Font.DemiBold
                        text: control.text
                    }
                }
            }
        }

        Caret {
            id: caretMark
            visible: control.caret
            anchors.verticalCenter: parent.verticalCenter
            color: label.color
        }
    }

    TapHandler {
        onTapped: control.activate()
    }
}
