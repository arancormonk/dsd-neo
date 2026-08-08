// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "app_prefs.h"

#include <QLatin1String>
#include <QVariant>

namespace dsd_qt {

namespace {

// Keys are flat and stable: renaming one silently resets that preference for
// every existing install, so treat them as a persistence format.
constexpr const char kAppearance[] = "ui/appearance";
constexpr const char kOnboardingDone[] = "ui/onboardingDone";
constexpr const char kBackgroundListening[] = "listen/background";
constexpr const char kKeepScreenAwake[] = "listen/keepAwake";
constexpr const char kSkipEncrypted[] = "decode/skipEncrypted";
constexpr const char kAutoPpm[] = "decode/autoPpm";
constexpr const char kGainDb[] = "tuner/gainDb";
constexpr const char kPpm[] = "tuner/ppm";
constexpr const char kBandwidthKhz[] = "tuner/bandwidthKhz";
constexpr const char kBiasTee[] = "tuner/biasTee";
constexpr const char kExtraArgs[] = "decode/extraArgs";

} // namespace

AppPrefs::AppPrefs(QObject* parent)
    // Explicit scope and names: the organization name is empty on some platforms,
    // and QSettings' fallback location would then depend on how the process was
    // launched rather than on the app.
    : QObject(parent),
      m_settings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("dsd-neo"), QStringLiteral("dsd-neo-app")) {
}

AppPrefs::~AppPrefs() = default;

int
AppPrefs::appearance() const {
    const int mode = m_settings.value(QLatin1String(kAppearance), FollowSystem).toInt();
    return (mode >= FollowSystem && mode <= Dark) ? mode : FollowSystem;
}

void
AppPrefs::setAppearance(int mode) {
    if (mode == appearance()) {
        return;
    }
    m_settings.setValue(QLatin1String(kAppearance), mode);
    Q_EMIT appearanceChanged();
}

bool
AppPrefs::onboardingDone() const {
    return m_settings.value(QLatin1String(kOnboardingDone), false).toBool();
}

void
AppPrefs::setOnboardingDone(bool done) {
    if (done == onboardingDone()) {
        return;
    }
    m_settings.setValue(QLatin1String(kOnboardingDone), done);
    Q_EMIT onboardingDoneChanged();
}

bool
AppPrefs::backgroundListening() const {
    return m_settings.value(QLatin1String(kBackgroundListening), true).toBool();
}

void
AppPrefs::setBackgroundListening(bool on) {
    if (on == backgroundListening()) {
        return;
    }
    m_settings.setValue(QLatin1String(kBackgroundListening), on);
    Q_EMIT backgroundListeningChanged();
}

bool
AppPrefs::keepScreenAwake() const {
    return m_settings.value(QLatin1String(kKeepScreenAwake), false).toBool();
}

void
AppPrefs::setKeepScreenAwake(bool on) {
    if (on == keepScreenAwake()) {
        return;
    }
    // Storage only. The platform effect (the Android window flag) is applied by
    // the DecoderHost the shared UI wires this preference to — this layer stays
    // free of platform APIs.
    m_settings.setValue(QLatin1String(kKeepScreenAwake), on);
    Q_EMIT keepScreenAwakeChanged();
}

bool
AppPrefs::skipEncrypted() const {
    return m_settings.value(QLatin1String(kSkipEncrypted), true).toBool();
}

void
AppPrefs::setSkipEncrypted(bool on) {
    if (on == skipEncrypted()) {
        return;
    }
    m_settings.setValue(QLatin1String(kSkipEncrypted), on);
    Q_EMIT skipEncryptedChanged();
}

bool
AppPrefs::autoPpm() const {
    return m_settings.value(QLatin1String(kAutoPpm), false).toBool();
}

void
AppPrefs::setAutoPpm(bool on) {
    if (on == autoPpm()) {
        return;
    }
    m_settings.setValue(QLatin1String(kAutoPpm), on);
    Q_EMIT autoPpmChanged();
}

int
AppPrefs::gainDb() const {
    return m_settings.value(QLatin1String(kGainDb), 30).toInt();
}

void
AppPrefs::setGainDb(int db) {
    if (db == gainDb()) {
        return;
    }
    m_settings.setValue(QLatin1String(kGainDb), db);
    Q_EMIT gainDbChanged();
}

int
AppPrefs::ppm() const {
    return m_settings.value(QLatin1String(kPpm), 0).toInt();
}

void
AppPrefs::setPpm(int value) {
    if (value == ppm()) {
        return;
    }
    m_settings.setValue(QLatin1String(kPpm), value);
    Q_EMIT ppmChanged();
}

int
AppPrefs::bandwidthKhz() const {
    return m_settings.value(QLatin1String(kBandwidthKhz), 48).toInt();
}

void
AppPrefs::setBandwidthKhz(int khz) {
    if (khz == bandwidthKhz()) {
        return;
    }
    m_settings.setValue(QLatin1String(kBandwidthKhz), khz);
    Q_EMIT bandwidthKhzChanged();
}

bool
AppPrefs::biasTee() const {
    return m_settings.value(QLatin1String(kBiasTee), false).toBool();
}

void
AppPrefs::setBiasTee(bool on) {
    if (on == biasTee()) {
        return;
    }
    m_settings.setValue(QLatin1String(kBiasTee), on);
    Q_EMIT biasTeeChanged();
}

QString
AppPrefs::extraArgs() const {
    return m_settings.value(QLatin1String(kExtraArgs), QString()).toString();
}

void
AppPrefs::setExtraArgs(const QString& args) {
    if (args == extraArgs()) {
        return;
    }
    m_settings.setValue(QLatin1String(kExtraArgs), args);
    Q_EMIT extraArgsChanged();
}

} // namespace dsd_qt
