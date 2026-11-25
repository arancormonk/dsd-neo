// SPDX-License-Identifier: GPL-3.0-or-later
#include "key_gate.h"
#include <QCoreApplication>
#include <QEvent>
#include <QFocusEvent>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMetaMethod>
#include <QMetaProperty>
#include <QPointingDevice>
#include <QQuickItem>
#include <QQuickWindow>
#include <QString>
#include <QTimer>
#include <QTouchEvent>
#include <QVariant>
#include <Qt>
#include "input_commit.h"

namespace dsd_qt {

namespace {

bool
isKeyEvent(QEvent::Type type) {
    return type == QEvent::KeyPress || type == QEvent::KeyRelease || type == QEvent::ShortcutOverride;
}

QPointer<QQuickItem>
rootOf(const QQuickItem* item) {
    const QQuickWindow* const window = item != nullptr ? item->window() : nullptr;
    return QPointer<QQuickItem>(window != nullptr ? window->contentItem() : nullptr);
}

bool
isComposing(const QQuickItem* item) {
    return item != nullptr && !item->property("preeditText").toString().isEmpty();
}

} // namespace

PressGate::PressGate(QObject* parent) : QObject(parent) {
    QCoreApplication* const app = QCoreApplication::instance();
    if (app != nullptr) {
        app->installEventFilter(this);
    }
}

PressGate*
PressGate::instance() {
    // Created with the first registration, after the application exists, and parented to it.
    static QPointer<PressGate> gate;
    if (!gate) {
        gate = new PressGate(QCoreApplication::instance());
    }
    return gate;
}

bool
PressGate::focusInReach() const {
    // The index is the focused control's own, looked up from its live type as it took the focus.
    return !m_focus || m_focus_index < 0 || m_focus->metaObject()->property(m_focus_index).read(m_focus).toBool();
}

bool
PressGate::focusHeld() const {
    // Disabled counts only while the control holds the window's stored focus (the window away): with the window
    // active Qt itself takes the focus off a disabled control.
    return focusInReach() && (!m_focus || m_focus->isEnabled() || m_focus->hasActiveFocus());
}

void
PressGate::setFocusItem(QQuickItem* item, int index) {
    disconnect(m_watch);
    disconnect(m_watch_enabled);
    disconnect(m_watch_destroyed);
    m_watch = {};
    m_watch_enabled = {};
    m_watch_destroyed = {};
    m_focus = item;
    m_focus_index = item != nullptr ? index : -1;
    m_composing = false;
    m_focus_in_reach = true;
    if (item == nullptr) {
        return;
    }
    // Its reachability is followed only while it has the focus.
    const QMetaProperty property = item->metaObject()->property(index);
    if (property.hasNotifySignal()) {
        const QMetaMethod slot = staticMetaObject.method(staticMetaObject.indexOfSlot("recheck()"));
        m_watch = connect(item, property.notifySignal(), this, slot);
    }
    m_watch_enabled = connect(item, &QQuickItem::enabledChanged, this, &PressGate::recheck);
    // Destroyed with the focus and no focus-out (its row taken away by a model reset): told once that has settled.
    m_watch_destroyed = connect(item, &QObject::destroyed, this, [this, root = rootOf(item)] {
        QTimer::singleShot(0, this, [this, root] {
            if (root) {
                Q_EMIT focusVanished(root);
            }
        });
    });
    m_focus_in_reach = focusInReach();
    if (!m_focus_in_reach) {
        // Taking the focus while unreachable (a deferred hand-off landing on it after it hid): let go.
        m_composing = isComposing(item);
        Q_EMIT strayed(item);
    }
}

void
PressGate::recheck() {
    // The control with the focus turning unreachable strays, or disabled while it holds the stored focus of a window
    // that is away.
    if (!m_focus) {
        return;
    }
    const bool now = focusHeld();
    if (m_focus_in_reach && !now) {
        m_focus_in_reach = false;
        m_composing = isComposing(m_focus);
        Q_EMIT strayed(m_focus);
    } else if (now) {
        m_focus_in_reach = true;
    }
}

void
PressGate::filterFocus(QQuickItem* item, const QEvent* event) {
    if (event->type() == QEvent::FocusIn) {
        // Looked up per focus change, from the item's own type: a type's metaobject goes with its engine.
        const int index = item->metaObject()->indexOfProperty("inReach");
        if (index >= 0) {
            m_lost_this_turn = nullptr;
            setFocusItem(item, index);
        }
        return;
    }
    if (item != m_focus.data()) {
        return;
    }
    const auto* focus = dynamic_cast<const QFocusEvent*>(event);
    if (focus == nullptr) {
        return;
    }
    // Seen before the control's own handling ends its edit (editingFinished, which a form saves on), so the edit it
    // publishes holds the whole word.
    if (focus->reason() == Qt::ActiveWindowFocusReason) {
        // The window leaving: a word composing is finished first; the control keeps the stored focus, and stays
        // watched.
        if (isComposing(item)) {
            Q_EMIT finishComposing(item);
        }
        return;
    }
    // A word composing in a control losing the focus out of reach, or disabled under it (an ancestor disabled as a
    // sheet comes), is committed first, while the gate still knows the control, and through the closed gate: one
    // commit, at the composing word.
    if ((!focusInReach() || !item->isEnabled()) && isComposing(item)) {
        m_focus_in_reach = false;
        m_composing = true;
        m_closing = true;
        Q_EMIT strayed(item);
        m_closing = false;
    }
    setFocusItem(nullptr, -1);
    m_lost_this_turn = item;
    // Told after the change settles: Qt may drop the focus to the window root (the control disabled), which
    // Navigation then hands on to the Back owner.
    QTimer::singleShot(0, this, [this, lost = QPointer<QQuickItem>(item), root = rootOf(item)] {
        if (m_lost_this_turn == lost) {
            m_lost_this_turn = nullptr; // The turn it lost the focus in is over.
        }
        if (lost && lost->window() != nullptr) {
            Q_EMIT focusLeft(lost);
        } else if (root) {
            // Destroyed, or taken out of its window, as it lost the focus (its row taken away by a model reset).
            Q_EMIT focusVanished(root);
        }
    });
}

bool
PressGate::filterClosed(QEvent* event) {
    // The word the field was composing as it turned unreachable is the edit it finishes (committed then, issue #718):
    // only the commit the app asks for, as whatever word the keyboard finishes it as (a converting keyboard's
    // candidate differs from what it showed), once, at the composing word itself (no replacement, and no offset that
    // would put it elsewhere in the text), with nothing new composed along with it. A commit nobody asked for is no
    // edit. Any key, a key already queued included: taken.
    const auto* input = event->type() == QEvent::InputMethod ? dynamic_cast<const QInputMethodEvent*>(event) : nullptr;
    const bool finishes = input != nullptr && m_composing && InputCommit::committing()
                          && !input->commitString().isEmpty() && input->preeditString().isEmpty()
                          && input->replacementStart() == 0 && input->replacementLength() == 0;
    if (finishes) {
        m_composing = false;
        return false;
    }
    // A key's release taken from the control its press reached (Space on a check box that hid meanwhile) would leave
    // it pressed if it is shown again before the focus moves on: the press is let go of with it, unacted.
    const auto* key = event->type() == QEvent::KeyRelease ? dynamic_cast<const QKeyEvent*>(event) : nullptr;
    if (key != nullptr && !key->isAutoRepeat()) {
        cancelPress(m_focus);
        Q_EMIT releaseTaken(key->key());
    }
    event->accept();
    return true;
}

void
PressGate::cancelPress(QQuickItem* control) const {
    if (control == nullptr) {
        return;
    }
    control->ungrabMouse();
    control->ungrabTouchPoints();
    if (control->property("pressed").toBool()) {
        QTouchEvent cancel(QEvent::TouchCancel, QPointingDevice::primaryPointingDevice());
        QCoreApplication::sendEvent(control, &cancel);
    }
}

void
PressGate::followStoredFocus(QQuickItem* item) {
    const int index = item != nullptr ? item->metaObject()->indexOfProperty("inReach") : -1;
    if (index >= 0 && item != m_focus.data()) {
        m_lost_this_turn = nullptr;
        setFocusItem(item, index);
    }
}

QQuickItem*
PressGate::focusLostThisTurn() const {
    return m_lost_this_turn.data();
}

QQuickItem*
PressGate::focusItemOf(QQuickItem* item) const {
    const QQuickWindow* const window = item != nullptr ? item->window() : nullptr;
    QQuickItem* const root = window != nullptr ? window->contentItem() : nullptr;
    if (root == nullptr) {
        return nullptr;
    }
    QQuickItem* focused = window->activeFocusItem();
    if (!root->hasActiveFocus()) {
        // Away: down each focus scope's own focus item from the root, where the window's focus goes back to.
        focused = root;
        while (focused->isFocusScope() && focused->scopedFocusItem() != nullptr) {
            focused = focused->scopedFocusItem();
        }
    }
    return focused != root ? focused : nullptr;
}

bool
PressGate::passStoredFocus(QQuickItem* from, QQuickItem* to) const {
    const QQuickWindow* const window = from != nullptr ? from->window() : nullptr;
    const QQuickItem* const root = window != nullptr ? window->contentItem() : nullptr;
    if (to == nullptr || root == nullptr || root->hasActiveFocus() || to->window() != window) {
        return false;
    }
    // Where the window's focus goes back to: down each focus scope's own focus item from its root.
    const QQuickItem* stored = root;
    while (stored->isFocusScope() && stored->scopedFocusItem() != nullptr) {
        stored = stored->scopedFocusItem();
    }
    if (stored != from) {
        return false;
    }
    // Below a scope holding no active focus, a focus set is stored only: from the item up to the root's child, each
    // scope takes it in its own scope. The root itself is left alone: focused, it would make the chain active.
    for (QQuickItem* node = to; node != nullptr && node != root; node = node->parentItem()) {
        if (node == to || node->isFocusScope()) {
            node->setFocus(true, Qt::OtherFocusReason);
        }
    }
    return true;
}

bool
PressGate::eventFilter(QObject* watched, QEvent* event) {
    const QEvent::Type type = event->type();
    if (type == QEvent::FocusIn || type == QEvent::FocusOut) {
        auto* item = qobject_cast<QQuickItem*>(watched);
        if (item != nullptr) {
            filterFocus(item, event);
        }
    } else if ((isKeyEvent(type) || type == QEvent::InputMethod) && m_focus && watched == m_focus.data()
               && (m_closing || !focusInReach()) && filterClosed(event)) {
        return true;
    }
    return QObject::eventFilter(watched, event);
}

} // namespace dsd_qt
