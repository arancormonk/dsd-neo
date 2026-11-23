// SPDX-License-Identifier: GPL-3.0-or-later
pragma Singleton
import QtQuick
import DsdNeo

QtObject {
    id: navigation
    property var rootSurfaces: []
    property var layers: []
    property var modals: []
    readonly property var topModal: modals.length ? modals[modals.length - 1] : null
    // Issue #902: counts every real change of the modals, the layers and the
    // root surfaces: a stack whose entries differ from what they were, not an
    // assignment of the same entries again (a binding re-evaluated). An open
    // selector list closes on one (PlexComboBox).
    property int changes: 0
    property var seenModals: []
    property var seenLayers: []
    property var seenRootSurfaces: []
    function sameEntries(a, b) {
        if (a.length !== b.length)
            return false;
        for (var i = 0; i < a.length; ++i) {
            if (a[i] !== b[i])
                return false;
        }
        return true;
    }
    onModalsChanged: {
        if (!sameEntries(modals, seenModals))
            changes++;
        seenModals = modals.slice();
    }
    onLayersChanged: {
        if (!sameEntries(layers, seenLayers))
            changes++;
        seenLayers = layers.slice();
    }
    onRootSurfacesChanged: {
        if (!sameEntries(rootSurfaces, seenRootSurfaces))
            changes++;
        seenRootSurfaces = rootSurfaces.slice();
    }
    // Issue #782: how stale a moving reading's accessible name may get, and the
    // most often it renames between meaningful steps (SpeechPacer). Tests
    // shorten it to keep real-time cases short; the app never changes it.
    property int speechPeriodMs: 5000
    // The least time between two steps a moving reading is heard at once for
    // (SpeechPacer): a lock or gate that flaps is heard once a second at most.
    property int speechStepFloorMs: 1000

    // Adding what is on top already, or removing what is not there, leaves
    // the stack as it is: no change, so an open list stays open.
    function addLayer(layer) {
        if (layers.length && layers[layers.length - 1] === layer)
            return;
        removeLayer(layer);
        layers = layers.concat([layer]);
    }
    function removeLayer(layer) {
        if (layers.indexOf(layer) < 0)
            return;
        layers = layers.filter(function (entry) {
            return entry !== layer;
        });
    }
    function addModal(modal) {
        if (topModal === modal)
            return;
        removeModal(modal);
        modals = modals.concat([modal]);
    }
    function removeModal(modal) {
        if (modals.indexOf(modal) < 0)
            return;
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
    // Issue #902: the app's one Escape and Back owner, an item that always
    // takes both and runs the app's Back (Main registers its safe area).
    // Null where no app is loaded (a test registers its own).
    property Item backTarget: null
    // Where a control that turned unreachable hands its keyboard focus: an
    // item that takes Escape and Back. The top modal's, else the top layer's,
    // own handler when it declares one (`keyTarget`: a sheet is its own), else
    // the app's backTarget. Never a surface by itself: a screen may pass
    // Escape to nothing (the Spectrum's loaded screen; a tab Main anchors to
    // its safe area but does not parent under it). Null with neither.
    function keyOwner() {
        var top = topModal || (layers.length ? layers[layers.length - 1] : null);
        if (top && top.keyTarget)
            return top.keyTarget;
        return backTarget;
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
    // Issue #902: what the gate between the keyboard and the controls
    // (PressGate) tells: a control that strayed (turned unreachable with the
    // focus, or took it while unreachable) or lost the focus is handed on.
    readonly property Connections pressGate: Connections {
        target: PressGate
        function onStrayed(item) {
            navigation.strayed(item);
        }
        function onFinishComposing(item) {
            if (navigation.editsText(item))
                navigation.commitComposingOf(item);
        }
        function onFocusLeft(item) {
            navigation.focusDropped(item);
        }
    }
    // Issue #902: a control that turns unreachable gives up the keyboard. Qt
    // keeps an invisible item's focus, and its own key handling with it: a
    // hidden check box still toggled on Space, a hidden combo opened on Down,
    // a hidden field took typing. From the moment it turns unreachable the
    // control takes no key at all (PressGate, ahead of its Keys handlers and
    // its own handling), a key already queued included. An editor's word
    // still composing is committed at once, before anything can publish the
    // edit (another field taking the keyboard ends this one's edit,
    // editingFinished, which a form saves on). The focus moves on once the
    // change that hid the control has settled (Qt.callLater): an edit that
    // ends inside it can change what hid it (a form that hides on a verdict
    // reopens for a check), a binding loop. Active focus goes to an item that
    // takes Escape and Back (keyOwner()), never to the window root or a
    // screen, where they could reach nothing. Stored focus goes too, or a
    // window that comes back would hand the focus to the hidden control; a
    // window that is away and would hand it back to the control hands it to
    // the same key owner when it returns, nothing made active meanwhile
    // (PressGate.passStoredFocus()).
    property var strays: []
    function editsText(item) {
        return !!item && item.inputMethodComposing !== undefined && item.readOnly !== true;
    }
    function strayed(item) {
        if (editsText(item))
            commitComposingOf(item);
        strays = strays.concat([item]);
        Qt.callLater(navigation.letGoStrays);
    }
    function letGoStrays() {
        var items = strays;
        strays = [];
        for (var item of items)
            letGo(item);
    }
    function letGo(item) {
        if (!item || item.inReach)
            return;
        // Again here for a word composed since (none is let through a closed
        // gate, so this finds nothing as a rule), whatever happened to the
        // keyboard meanwhile; only the hand-off below depends on the control
        // still holding the focus.
        var edits = editsText(item);
        if (edits)
            commitComposingOf(item);
        if (!item.focus)
            return;
        var active = item.activeFocus;
        var owner = keyOwner();
        if (owner && owner !== item && owner.visible && owner.enabled) {
            if (active)
                owner.forceActiveFocus();
            else
                PressGate.passStoredFocus(item, owner);
        }
        if (item.focus)
            item.focus = false;
        if (active && edits)
            Qt.inputMethod.hide();
    }
    // Qt itself can drop a control's focus to the window root or to the focus
    // scope that encloses it (the Spectrum's loader), the control or its
    // panel disabled as it turns unreachable, where Escape and Back can reach
    // nothing; letGo() never runs then (the control no longer holds the
    // focus). Handed on to the key owner, unless the focus moved to another
    // control (neither the root nor an item enclosing the control) or the
    // window is away.
    function encloses(outer, item) {
        for (var at = item ? item.parent : null; at; at = at.parent) {
            if (at === outer)
                return true;
        }
        return false;
    }
    function focusDropped(item) {
        if (!item || (item.inReach && item.enabled))
            return;
        var window = item.Window.window;
        var dropped = window ? window.activeFocusItem : null;
        if (!dropped || (dropped !== window.contentItem && !encloses(dropped, item)))
            return;
        var owner = keyOwner();
        if (owner && owner !== item && owner.visible && owner.enabled)
            owner.forceActiveFocus();
    }
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
    // Issue #902: @p item's composing word, whether or not it still has the
    // focus (a field hidden as its window left).
    function commitComposingOf(item) {
        if (typeof inputCommit !== "undefined" && inputCommit !== null)
            inputCommit.commitComposingOf(item);
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
