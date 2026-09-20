// SPDX-License-Identifier: GPL-3.0-or-later
pragma Singleton
import QtQuick

QtObject {
    id: navigation
    property var rootSurfaces: []
    property var layers: []
    property var modals: []
    readonly property var topModal: modals.length ? modals[modals.length - 1] : null

    function addLayer(layer) {
        removeLayer(layer);
        layers = layers.concat([layer]);
    }
    function removeLayer(layer) {
        layers = layers.filter(function (entry) {
            return entry !== layer;
        });
    }
    function addModal(modal) {
        removeModal(modal);
        modals = modals.concat([modal]);
    }
    function removeModal(modal) {
        modals = modals.filter(function (entry) {
            return entry !== modal;
        });
    }
    function contains(parent, item) {
        for (var node = item; node; node = node.parent) {
            if (node === parent)
                return true;
        }
        return false;
    }
    function allows(item) {
        if (topModal)
            return contains(topModal.navigationSurface || topModal, item);
        return layers.length ? contains(layers[layers.length - 1].surface, item) : !rootSurfaces.length || rootSurfaces.some(function (surface) {
            return contains(surface, item);
        });
    }
    function presented(item) {
        for (var node = item; node; node = node.parent) {
            if (!node.visible || (node !== item && node.opacity < 0.9))
                return false;
        }
        return true;
    }
    function clearInput(window) {
        if (window && window.activeFocusItem)
            window.activeFocusItem.focus = false;
        Qt.inputMethod.hide();
    }
    function back(window) {
        if (Qt.inputMethod.visible) {
            clearInput(window);
            return true;
        }
        clearInput(window);
        if (topModal) {
            topModal.requestDismiss();
            return true;
        }
        if (layers.length) {
            layers[layers.length - 1].leave();
            return true;
        }
        return false;
    }
}
