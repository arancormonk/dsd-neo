// SPDX-License-Identifier: GPL-3.0-or-later
pragma Singleton
import QtQuick

QtObject {
    id: navigation
    property var rootSurfaces: []
    property var layers: []
    property var modals: []
    readonly property var topModal: modals.length ? modals[modals.length - 1] : null
    // Issue #782: how stale a moving reading's accessible name may get, and the
    // most often it renames between meaningful steps (SpeechPacer). Tests
    // shorten it to keep real-time cases short; the app never changes it.
    property int speechPeriodMs: 5000
    // The least time between two steps a moving reading is heard at once for
    // (SpeechPacer): a lock or gate that flaps is heard once a second at most.
    property int speechStepFloorMs: 1000

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
    // A binding that also reads `visible` asks this first: walking up the
    // parents can settle a pending reparent (the Monitor's parts that move into
    // its list), and a visibility read before the walk would be stale, a
    // binding loop to Qt.
    function allows(item) {
        if (topModal)
            return contains(topModal.navigationSurface || topModal, item);
        return layers.length ? contains(layers[layers.length - 1].surface, item) : !rootSurfaces.length || rootSurfaces.some(function (surface) {
            return contains(surface, item);
        });
    }
    // Space, Return and Enter (the keypad's too) act on the focused control
    // on the key's press, the way every control's Keys.onPressed asks here
    // with the item that has the keyboard (@p item). A held key's repeats act
    // too, but only on the control that took the key's first press: a repeat
    // that reaches any other control is taken and does nothing. An action
    // that moves the keyboard while the key is down (a sheet's choice hands
    // it back to the pill that opened the sheet) otherwise had the repeats
    // act on the control it landed on, and open the sheet again. The first
    // press is noted until that key's release, which every such control
    // passes to activationRelease(). No control acts on a release. True when
    // @p event should act; an activation key's press, repeat or not, is
    // taken, so it goes no further.
    //
    // With a screen reader on, an activation key held with Alt is the screen
    // reader's own command, never the app's: TalkBack's Alt+Enter clicks the
    // item TalkBack has focused and Alt+Shift+Enter long-presses it. TalkBack
    // takes the key and makes that click itself, but Android also hands the
    // app a key an accessibility service has not answered within 500 ms. A
    // click that keeps TalkBack waiting (a sheet's choice that re-sorts
    // Listen's cards) had the same press reach the app after the click had
    // closed the sheet and the keyboard was back on its opener, which opened
    // the sheet again; a busy TalkBack passes the key on before its click
    // instead. Such a press, its repeats included, is taken and does nothing:
    // the screen reader's click is the one action.
    property var heldActivationItem: null
    property int heldActivationKey: 0
    function activationPress(event, item) {
        if (event.key !== Qt.Key_Space && event.key !== Qt.Key_Return && event.key !== Qt.Key_Enter)
            return false;
        event.accepted = true;
        if ((event.modifiers & Qt.AltModifier) && typeof screenReader !== "undefined" && screenReader !== null && screenReader.active)
            return false;
        if (!event.isAutoRepeat) {
            heldActivationItem = item;
            heldActivationKey = event.key;
            return true;
        }
        return item !== null && item !== undefined && item === heldActivationItem && event.key === heldActivationKey;
    }
    // A key's release ends what activationPress() noted for its first press.
    function activationRelease(event) {
        if (!event.isAutoRepeat && event.key === heldActivationKey) {
            heldActivationItem = null;
            heldActivationKey = 0;
        }
    }
    // Read @p text out once, a repeat included, while @p line is reachable
    // (no gate when null). @p line speaks it, @p owner when there is none:
    // speaking makes the speaker an accessible node, and an owner (a screen)
    // has no gate of its own, so it would stay in the tree under a sheet.
    // @p owner keeps the last text read and how many have been (`announced`,
    // `announcements`) for tests. True when read.
    function announce(owner, line, text) {
        if (!text.length || (line && !allows(line)))
            return false;
        owner.announced = text;
        owner.announcements++;
        var speaker = line || owner;
        if (typeof speaker.Accessible.announce === "function")
            speaker.Accessible.announce(text);
        return true;
    }
    function presented(item) {
        for (var node = item; node; node = node.parent) {
            if (!node.visible || (node !== item && node.opacity < 0.9))
                return false;
        }
        return true;
    }
    // Issue #718: an Android keyboard holds the word being typed as composing
    // text, which a field's `text` leaves out. Every action that reads a form
    // (a button, a sheet dismissal, Back) finishes that word first, as native
    // Android's EditText.getText() would include it. finishEditing also takes
    // focus off the field so fields that publish on editingFinished update their
    // drafts first. Missing only where no frontend context is loaded.
    readonly property bool composing: typeof inputCommit !== "undefined" && inputCommit !== null && inputCommit.composing
    function commitComposing(finishEditing) {
        if (typeof inputCommit !== "undefined" && inputCommit !== null)
            inputCommit.commitComposing(finishEditing === true);
    }
    // The composing word is committed first (#831): a field that saves on
    // editingFinished would otherwise save what it held without it, when a
    // sheet or a layer opens by itself (a file another app sent) and takes
    // the focus from the field.
    function clearInput(window) {
        commitComposing(false);
        if (window && window.activeFocusItem)
            window.activeFocusItem.focus = false;
        Qt.inputMethod.hide();
    }
    function back(window) {
        commitComposing(false);
        if (Qt.inputMethod.visible) {
            clearInput(window);
            return true;
        }
        // Read before clearing input: taking focus off an open selector closes
        // its popup, and Back must then stop there rather than reach the sheet
        // under it.
        var modal = topModal;
        clearInput(window);
        if (modal) {
            modal.requestDismiss();
            return true;
        }
        if (layers.length) {
            layers[layers.length - 1].leave();
            return true;
        }
        return false;
    }
}
