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
#include <QVariant>

namespace dsd_qt {

namespace {

/* How deep commitComposing() calls are nested (an item's commit handling can ask again); GUI thread only. */
int g_commit_depth = 0;

/* The span in which a commit is the one asked for (InputCommit::committing()), ended on every return. */
class CommitSpan {
  public:
    CommitSpan() { ++g_commit_depth; }

    ~CommitSpan() { --g_commit_depth; }

    CommitSpan(const CommitSpan&) = delete;
    CommitSpan& operator=(const CommitSpan&) = delete;
};

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

bool
InputCommit::committing() {
    return g_commit_depth > 0;
}

void
InputCommit::commitComposing(bool finishEditing) {
    const CommitSpan span;

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

void
InputCommit::commitComposingOf(QObject* item) {
    if (item == nullptr) {
        return;
    }
    if (item == QGuiApplication::focusObject()) {
        commitComposing(false);
        return;
    }
    if (!is_composing(item)) {
        return;
    }
    /* No longer the focus object (its window left): the platform input method is someone else's now, so the item is
     * handed its own composing text as a commit, as a keyboard finishing the word would. */
    const CommitSpan span;
    QInputMethodEvent event;
    event.setCommitString(item->property("preeditText").toString());
    QCoreApplication::sendEvent(item, &event);
    updateComposing();
}

} // namespace dsd_qt
