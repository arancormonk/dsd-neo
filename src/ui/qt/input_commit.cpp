// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "input_commit.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QMetaMethod>
#include <QQuickItem>
#include <QString>
#include <QVariant>

namespace dsd_qt {

namespace {

/* TextInput, TextEdit and the Controls fields built on them all publish these. */
bool
is_composing(const QObject* object) {
    return object != nullptr && object->property("inputMethodComposing").toBool();
}

} // namespace

InputCommit::InputCommit(QObject* parent) : QObject(parent) {
    const auto* app = qobject_cast<QGuiApplication*>(QCoreApplication::instance());
    if (app != nullptr) {
        connect(app, &QGuiApplication::focusObjectChanged, this, &InputCommit::followFocusObject);
        followFocusObject(QGuiApplication::focusObject());
    }
}

bool
InputCommit::composing() const {
    return m_composing;
}

void
InputCommit::followFocusObject(QObject* object) {
    disconnect(m_focus_connection);
    m_focus_connection = QMetaObject::Connection();
    m_focus = object;
    if (object != nullptr) {
        /* Static: this also runs from the constructor, where a virtual call would not dispatch. */
        const QMetaObject& self = InputCommit::staticMetaObject;
        const QMetaObject* meta = object->metaObject();
        const int signal = meta->indexOfSignal("inputMethodComposingChanged()");
        const int slot = self.indexOfMethod("updateComposing()");
        if (signal >= 0 && slot >= 0) {
            m_focus_connection = connect(object, meta->method(signal), this, self.method(slot));
        }
    }
    updateComposing();
}

void
InputCommit::updateComposing() {
    const bool now = is_composing(m_focus.data());
    if (now == m_composing) {
        return;
    }
    m_composing = now;
    Q_EMIT composingChanged();
}

void
InputCommit::commitComposing(bool finishEditing) {
    QInputMethod* method = QGuiApplication::inputMethod();
    if (method != nullptr) {
        method->commit();
    }
    QObject* focus = QGuiApplication::focusObject();
    if (is_composing(focus)) {
        /* No platform input method finished the word: hand the item its own
         * composing text as a commit, which is what a keyboard sends when it
         * finishes composing. */
        QInputMethodEvent event;
        event.setCommitString(focus->property("preeditText").toString());
        QCoreApplication::sendEvent(focus, &event);
        if (method != nullptr) {
            method->reset();
        }
    }
    if (finishEditing) {
        auto* item = qobject_cast<QQuickItem*>(QGuiApplication::focusObject());
        if (item != nullptr && item->flags().testFlag(QQuickItem::ItemAcceptsInputMethod)) {
            item->setFocus(false);
            if (method != nullptr) {
                method->hide();
            }
        }
    }
    updateComposing();
}

} // namespace dsd_qt
