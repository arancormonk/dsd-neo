// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Window

Item {
    id: layer
    property bool active: false
    property var surface: parent
    signal leave
    onActiveChanged: {
        Navigation.clearInput(layer.Window.window);
        if (active)
            Navigation.addLayer(layer);
        else
            Navigation.removeLayer(layer);
    }
    Component.onCompleted: {
        if (active)
            Navigation.addLayer(layer);
    }
    Component.onDestruction: Navigation.removeLayer(layer)
}
