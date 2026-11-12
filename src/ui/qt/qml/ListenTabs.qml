// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

import QtQuick

// The Listen tabs (issue #646): equal text tabs for the groups on Listen, drawn
// as Material primary tabs in the app's colours. The current tab's label is
// cyan with a cyan bar under it; the rest are secondary text. A hairline runs
// under the strip, on the fixed header's background. No icons, counts or
// motion. The strip is one Tab stop, the current tab: Left and Right move to
// the next tab and choose it, Home and End the first and last, and Return,
// keypad Enter and Space choose the focused one. The caller owns `currentId`
// and sets it from `selected`, which fires only for a change of tab.
Rectangle {
    id: strip

    objectName: "listenTabs"

    // [{id, label}], in order.
    property var model: []
    property string currentId: ""
    signal selected(string id)

    // The tallest label as the selected weight lays it out (see `measured`).
    property real labelHeight: 0
    // Every label at one size, the smallest any of them needs for its words to
    // fit (WholeWordLabel, as SegmentedControl).
    property real labelPixelSize: 0
    function shareLabelSize() {
        var size = 0;
        for (var i = 0; i < tabs.count; ++i) {
            var item = tabs.itemAt(i);
            if (item && (size <= 0 || item.fitPixelSize < size))
                size = item.fitPixelSize;
        }
        labelPixelSize = size;
    }
    function measureLabels() {
        var measured = 0;
        for (var i = 0; i < tabs.count; ++i) {
            var item = tabs.itemAt(i);
            if (item)
                measured = Math.max(measured, item.textHeight);
        }
        labelHeight = measured;
    }

    function tabAt(index) {
        return index >= 0 && index < tabs.count ? tabs.itemAt(index) : null;
    }
    function focusedTab() {
        for (var i = 0; i < tabs.count; ++i) {
            var item = tabs.itemAt(i);
            if (item && item.activeFocus)
                return item;
        }
        return null;
    }
    // Moves the keyboard focus to the current tab, as long as it can take it.
    // Returns whether it did. Reads `currentId` itself: in onCurrentIdChanged
    // a tab's `active` can still hold the old answer.
    function focusCurrent() {
        for (var i = 0; i < tabs.count; ++i) {
            var item = tabs.itemAt(i);
            if (item && item.tabId === currentId) {
                if (!item.canChoose())
                    return false;
                item.forceActiveFocus(Qt.TabFocusReason);
                return item.activeFocus;
            }
        }
        return false;
    }
    // The focus stays on the current tab when the caller changes it, so the
    // strip keeps exactly one Tab stop.
    function settleFocus() {
        var focused = focusedTab();
        if (focused && focused.tabId !== currentId)
            focusCurrent();
    }
    onCurrentIdChanged: settleFocus()

    implicitHeight: Math.max(Theme.minimumTouchSize, labelHeight + 24)
    color: Theme.bg

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: 1
        color: Theme.divider
    }

    Row {
        anchors.fill: parent

        Repeater {
            id: tabs
            model: strip.model
            onItemAdded: {
                strip.shareLabelSize();
                strip.measureLabels();
            }
            onItemRemoved: {
                strip.shareLabelSize();
                strip.measureLabels();
            }

            Item {
                id: tab

                required property int index
                required property var modelData
                readonly property string tabId: modelData.id
                readonly property bool active: strip.currentId === tabId
                objectName: "listenTab_" + tabId

                width: strip.width / Math.max(1, tabs.count)
                height: parent.height

                // The label's height in the selected weight, which can wrap where
                // the regular weight does not, so choosing a tab never changes
                // the strip's height (code_map "Qt QML editing convention": a
                // size reads a Text's `height`).
                readonly property real textHeight: measured.height
                onTextHeightChanged: strip.measureLabels()
                readonly property real fitPixelSize: tabLabel.fitPixelSize
                onFitPixelSizeChanged: strip.shareLabelSize()

                readonly property bool navigationAllowed: Navigation.allows(strip)
                // A tab the caller made current while the strip was gated
                // (under a sheet, hidden or disabled) takes the focus once the
                // gate lifts.
                readonly property bool choosable: navigationAllowed && enabled && visible
                onChoosableChanged: {
                    if (choosable)
                        strip.settleFocus();
                }
                // The one Tab stop is the current tab, held while it has the
                // focus (see OutlineButton).
                activeFocusOnTab: (navigationAllowed && enabled && visible && active) || activeFocus
                // Issue #646: a radio button, checked while it is the current
                // tab (see BottomNav for why not a page tab).
                Accessible.role: Accessible.RadioButton
                Accessible.name: modelData.label
                Accessible.checkable: true
                Accessible.checked: active
                Accessible.selected: active
                // Focusable only while in the tree (see OutlineButton).
                Accessible.focusable: navigationAllowed && visible
                // Gate first: see OutlineButton.
                Accessible.ignored: !navigationAllowed || !visible
                Accessible.onPressAction: choose()
                function canChoose() {
                    // The gate itself, not `choosable`, which may not have
                    // caught up with a change made in the same turn.
                    return Navigation.allows(strip) && enabled && visible;
                }
                // Every way of choosing a tab comes here. With the focus in the
                // strip the focus moves to this tab first, so the tab it leaves
                // drops its Tab stop once the caller makes this one current.
                function choose() {
                    if (!canChoose())
                        return;
                    var focused = strip.focusedTab();
                    if (focused && focused !== tab)
                        forceActiveFocus(Qt.TabFocusReason);
                    if (active)
                        return;
                    // Issue #718: leaving a group finishes the word being typed
                    // there first, as the bottom navigation does.
                    Navigation.commitComposing(true);
                    strip.selected(tabId);
                }
                // Chooses the tab @p step places away, held at the ends (no
                // wrap), through that tab's guard.
                function chooseBy(step) {
                    var next = strip.tabAt(Math.max(0, Math.min(tabs.count - 1, index + step)));
                    if (next)
                        next.choose();
                }
                Keys.onLeftPressed: chooseBy(-1)
                Keys.onRightPressed: chooseBy(1)
                Keys.onPressed: function (event) {
                    if (Navigation.activationPress(event)) {
                        choose();
                    } else if (event.key === Qt.Key_Home) {
                        chooseBy(-tabs.count);
                        event.accepted = true;
                    } else if (event.key === Qt.Key_End) {
                        chooseBy(tabs.count);
                        event.accepted = true;
                    }
                }
                FocusFrame {}

                // Pressed: the chip tint, shown at once, clear of the hairline.
                Rectangle {
                    width: parent.width
                    height: parent.height - 1
                    color: tap.pressed && tab.enabled ? Theme.chipSelectedFill : "transparent"
                }

                // Issue #646: whole words at every text size; the tab's name
                // reads the label in full when it is elided.
                WholeWordLabel {
                    id: tabLabel
                    objectName: "listenTabLabel"
                    anchors.centerIn: parent
                    width: Math.max(0, parent.width - 16)
                    horizontalAlignment: Text.AlignHCenter
                    text: tab.modelData.label
                    basePixelSize: 14
                    measureWeight: Font.DemiBold
                    sharedPixelSize: strip.labelPixelSize
                    font.weight: tab.active ? Font.DemiBold : Font.Normal
                    color: tab.active ? Theme.cyan : Theme.textSecondary
                    // PinButton's disabled look, on the label only: the bar
                    // still says which tab is current.
                    opacity: tab.enabled ? 1.0 : 0.5
                }
                // The label laid out in the selected weight, never drawn.
                Text {
                    id: measured
                    visible: false
                    width: tabLabel.width
                    text: tabLabel.text
                    font.family: tabLabel.font.family
                    font.pixelSize: tabLabel.font.pixelSize
                    font.weight: Font.DemiBold
                    wrapMode: tabLabel.wrapMode
                    elide: tabLabel.elide
                    Accessible.ignored: true
                }

                // Under the label, as wide as its text (24 px at least).
                Rectangle {
                    objectName: "listenTabIndicator"
                    visible: tab.active
                    anchors.bottom: parent.bottom
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: Math.min(tabLabel.width, Math.max(24, tabLabel.contentWidth))
                    height: 3
                    topLeftRadius: 1.5
                    topRightRadius: 1.5
                    color: Theme.cyan
                }

                GatedTapHandler {
                    id: tap
                    onAllowedTap: tab.choose()
                }
            }
        }
    }
}
