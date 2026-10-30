// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "screen_reader_state.h"

#include <QAccessible>
#include <QMetaObject>
#include <QObject>
#include <Qt>

namespace dsd_qt {

ScreenReaderState::ScreenReaderState(QObject* parent) : QObject(parent), m_active(QAccessible::isActive()) {
    QAccessible::installActivationObserver(this);
}

ScreenReaderState::~ScreenReaderState() { QAccessible::removeActivationObserver(this); }

void
ScreenReaderState::accessibilityActiveChanged(bool active) {
    // Queued even on this thread: the observer list is being walked, and a slot that ran a QML binding from inside
    // it could install or remove another observer.
    QMetaObject::invokeMethod(this, [this, active]() { apply(active); }, Qt::QueuedConnection);
}

void
ScreenReaderState::apply(bool active) {
    if (m_active == active) {
        return;
    }
    m_active = active;
    Q_EMIT activeChanged();
}

} // namespace dsd_qt
