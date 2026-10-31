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

ScreenReaderState::ScreenReaderState(QObject* parent) : QObject(parent) {
    // Observe first, then read: an activation on Android's thread between a read and the install would otherwise be
    // neither read nor told. One that lands after the install is both, and apply() takes the same state once.
    QAccessible::installActivationObserver(this);
    m_active = QAccessible::isActive();
}

ScreenReaderState::~ScreenReaderState() { QAccessible::removeActivationObserver(this); }

void
ScreenReaderState::accessibilityActiveChanged(bool on) {
    // Queued even on this thread: the observer list is being walked, and a slot that ran a QML binding from inside
    // it could install or remove another observer.
    QMetaObject::invokeMethod(this, [this, on]() { apply(on); }, Qt::QueuedConnection);
}

void
ScreenReaderState::apply(bool on) {
    if (m_active == on) {
        return;
    }
    m_active = on;
    Q_EMIT activeChanged();
}

} // namespace dsd_qt
