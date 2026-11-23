// SPDX-License-Identifier: GPL-3.0-or-later
#include "key_gate.h"
#include <QCoreApplication>
#include <QEvent>
#include <QFocusEvent>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMetaMethod>
#include <QMetaProperty>
#include <QQuickWindow>
#include <QTimer>
#include <QVariant>
#include "input_commit.h"

namespace dsd_qt {

namespace {

bool
isKeyEvent(QEvent::Type type) {
    return type == QEvent::KeyPress || type == QEvent::KeyRelease || type == QEvent::ShortcutOverride;
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

void
PressGate::setFocusItem(QQuickItem* item, int index) {
    disconnect(m_watch);
    m_watch = {};
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
    m_focus_in_reach = focusInReach();
    if (!m_focus_in_reach) {
        // Taking the focus while unreachable (a deferred hand-off landing on it after it hid): let go.
        m_composing = isComposing(item);
        Q_EMIT strayed(item);
    }
}

void
PressGate::recheck() {
    // The control with the focus turning unreachable strays.
    if (!m_focus) {
        return;
    }
    const bool now = focusInReach();
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
            setFocusItem(item, index);
        }
        return;
    }
    if (item != m_focus.data()) {
        return;
    }
    const auto* focus = dynamic_cast<const QFocusEvent*>(event);
    if (focus == nullptr || focus->reason() == Qt::ActiveWindowFocusReason) {
        return; // The window leaving: the control keeps the stored focus, and stays watched.
    }
    // Seen before the control's own handling ends its edit (editingFinished, which a form saves on): a word composing
    // in a control losing the focus out of reach, or disabled under it (an ancestor disabled as a sheet comes), is
    // committed first, while the gate still knows the control.
    if ((!focusInReach() || !item->isEnabled()) && isComposing(item)) {
        m_focus_in_reach = false;
        m_composing = true;
        Q_EMIT strayed(item);
    }
    setFocusItem(nullptr, -1);
    // Told after the change settles: Qt may drop the focus to the window root (the control disabled), which
    // Navigation then hands on to the Back owner.
    QTimer::singleShot(0, this, [this, lost = QPointer<QQuickItem>(item)] {
        if (lost) {
            Q_EMIT focusLeft(lost);
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
    event->accept();
    return true;
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
               && !focusInReach() && filterClosed(event)) {
        return true;
    }
    return QObject::eventFilter(watched, event);
}

} // namespace dsd_qt
