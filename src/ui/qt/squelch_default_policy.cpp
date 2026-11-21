// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "squelch_default_policy.h"

#include <QLatin1String>

#include "app_squelch_default.h"

namespace dsd_qt {

SquelchDefaultPolicy::SquelchDefaultPolicy(QObject* parent) : QObject(parent) {}

void
SquelchDefaultPolicy::sessionStarted(const QString& appDefault, bool defaultInSpec, bool sessionHasOwnSquelch) {
    const bool sendable = app_squelch_default_valid(appDefault) && appDefault != QLatin1String("off");
    if (sessionHasOwnSquelch || !sendable) {
        sessionStartedWithoutDefault();
        return;
    }
    m_owner = Owner::App;
    m_applied = defaultInSpec;
    m_default = appDefault;
}

void
SquelchDefaultPolicy::sessionStartedWithoutDefault() {
    m_owner = Owner::Session;
    m_applied = false;
    m_default.clear();
}

void
SquelchDefaultPolicy::userChangedSquelch() {
    // Only a session's: with none running there is nothing of the user's to keep, and the next start says who owns it.
    if (m_owner == Owner::App) {
        m_owner = Owner::User;
    }
}

void
SquelchDefaultPolicy::noteUserSquelchChange() {
    userChangedSquelch();
}

SquelchDefaultPolicy::Action
SquelchDefaultPolicy::decodeModeChanged(bool analog) {
    if (m_owner != Owner::App) {
        return Action::None;
    }
    if (analog && !m_applied) {
        m_applied = true;
        return Action::PushDefault;
    }
    if (!analog && m_applied) {
        m_applied = false;
        return Action::PushOff;
    }
    return Action::None;
}

QString
SquelchDefaultPolicy::defaultSetting() const {
    return m_owner == Owner::App ? m_default : QString();
}

} // namespace dsd_qt
