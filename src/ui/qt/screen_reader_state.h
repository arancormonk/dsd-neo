// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Whether a screen reader is reading the app (issue #782), for QML to bind to.
 *
 * "On" is Qt's accessibility being active: QAccessible::isActive(). On Android that is the
 * accessibility bridge being on, which the launch gate allows only with TalkBack (touch
 * exploration) on at launch; on a desktop it is an assistive technology attached through the
 * platform's accessibility bus. Qt tells its activation observers when it changes
 * (QPlatformAccessibility::setActive()); on Android that call comes from the Android main
 * thread, so the change is queued onto this object's thread. Nothing is polled.
 */

#ifndef DSD_NEO_SRC_UI_QT_SCREEN_READER_STATE_H_
#define DSD_NEO_SRC_UI_QT_SCREEN_READER_STATE_H_

#include <QAccessible>
#include <QObject>

namespace dsd_qt {

class ScreenReaderState : public QObject, public QAccessible::ActivationObserver {
    Q_OBJECT
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)

  public:
    explicit ScreenReaderState(QObject* parent = nullptr);
    ~ScreenReaderState() override;

    bool
    active() const {
        return m_active;
    }

    /** @brief QAccessible::ActivationObserver; may run on another thread (Android's main thread). */
    void accessibilityActiveChanged(bool on) override;

  Q_SIGNALS:
    void activeChanged();

  private:
    void apply(bool on);

    bool m_active = false;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_SCREEN_READER_STATE_H_ */
