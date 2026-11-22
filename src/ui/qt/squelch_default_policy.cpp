// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "squelch_default_policy.h"

#include <QLatin1String>

#include "app_squelch_default.h"

namespace dsd_qt {

namespace {

constexpr char kRecordApp[] = "app:";
constexpr char kRecordUser[] = "user";
constexpr char kRecordSession[] = "session";

/* Whether @p appDefault is one the app can send: a dynamic default, never off. */
bool
sendable(const QString& appDefault) {
    return app_squelch_default_valid(appDefault) && appDefault != QLatin1String("off");
}

} // namespace

SquelchDefaultPolicy::SquelchDefaultPolicy(QObject* parent) : QObject(parent) {}

void
SquelchDefaultPolicy::setOwner(Owner owner, const QString& appDefault) {
    const QString before = record();
    m_owner = owner;
    m_default = owner == Owner::App ? appDefault : QString();
    if (record() != before) {
        Q_EMIT changed();
    }
}

void
SquelchDefaultPolicy::sessionStarted(const QString& appDefault, bool sessionHasOwnSquelch) {
    setOwner(sessionHasOwnSquelch || !sendable(appDefault) ? Owner::Session : Owner::App, appDefault);
}

void
SquelchDefaultPolicy::sessionStartedWithoutDefault() {
    setOwner(Owner::Session, QString());
}

void
SquelchDefaultPolicy::userChangedSquelch() {
    // Only a session's: with none running there is nothing of the user's to keep, and the next start says who owns it.
    if (m_owner == Owner::App) {
        setOwner(Owner::User, QString());
    }
}

void
SquelchDefaultPolicy::noteUserSquelchChange() {
    userChangedSquelch();
}

QString
SquelchDefaultPolicy::record() const {
    switch (m_owner) {
        case Owner::App: return QLatin1String(kRecordApp) + m_default;
        case Owner::User: return QLatin1String(kRecordUser);
        case Owner::Session: return QLatin1String(kRecordSession);
        case Owner::None: break;
    }
    return QString();
}

void
SquelchDefaultPolicy::adopt(const QString& text) {
    if (text == QLatin1String(kRecordUser)) {
        setOwner(Owner::User, QString());
        return;
    }
    const QString appDefault = text.startsWith(QLatin1String(kRecordApp)) ? text.mid(4) : QString();
    if (sendable(appDefault)) {
        setOwner(Owner::App, appDefault);
        return;
    }
    setOwner(Owner::Session, QString());
}

SquelchDefaultPolicy::Action
SquelchDefaultPolicy::decodeModeChanged(bool analog) const {
    if (m_owner != Owner::App) {
        return Action::None;
    }
    return analog ? Action::DefaultIfOff : Action::OffIfDefault;
}

QString
SquelchDefaultPolicy::defaultSetting() const {
    return m_owner == Owner::App ? m_default : QString();
}

} // namespace dsd_qt
