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
// Where the last explore session was pointed, so the next one resumes there
// instead of asking again. Not a saved system: exploring has no card, no name and
// no trunking, and it must not appear in the list of things to listen to.
constexpr const char kExploreSourceType[] = "explore/sourceType";
constexpr const char kExploreHost[] = "explore/host";
constexpr const char kExplorePort[] = "explore/port";
constexpr const char kExploreFreqMhz[] = "explore/freqMhz";

/*
 * Three preferences are range-checked on the way out. They are checked on the way
 * in as well, and the setters below compare against what is *stored* rather than
 * against the checked reading — otherwise an out-of-range write persists (the
 * getter hides it) and the corrective write that follows looks like a no-op and is
 * dropped, leaving the file permanently disagreeing with the app.
 */

/** @brief @p mode if it names an appearance, else the default. */
int
sane_appearance(int mode) {
    return (mode >= AppPrefs::FollowSystem && mode <= AppPrefs::Dark) ? mode : AppPrefs::FollowSystem;
}

/**
 * @brief @p type if it is a source that can explore, else empty.
 *
 * Only the two tuner sources can explore -- a PCM feed or a file has nothing to
 * point anywhere -- so anything else reads as "not chosen yet" and sends the user
 * to the setup sheet rather than starting something that cannot tune. Empty is the
 * honest default: it is what makes the first tap ask.
 */
QString
sane_explore_source_type(const QString& type) {
    return (type == QLatin1String("usb") || type == QLatin1String("rtltcp")) ? type : QString();
}

/** @brief @p port if it is a usable TCP port, else the rtl_tcp default. */
int
sane_explore_port(int port) {
    return (port >= 1 && port <= 65535) ? port : 1234;
}

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
    return sane_appearance(m_settings.value(QLatin1String(kAppearance), FollowSystem).toInt());
}

void
AppPrefs::setAppearance(int mode) {
    const int next = sane_appearance(mode);
    if (next == m_settings.value(QLatin1String(kAppearance), FollowSystem).toInt()) {
        return;
    }
    m_settings.setValue(QLatin1String(kAppearance), next);
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

QString
AppPrefs::exploreSourceType() const {
    return sane_explore_source_type(m_settings.value(QLatin1String(kExploreSourceType), QString()).toString());
}

void
AppPrefs::setExploreSourceType(const QString& type) {
    const QString next = sane_explore_source_type(type);
    if (next == m_settings.value(QLatin1String(kExploreSourceType), QString()).toString()) {
        return;
    }
    m_settings.setValue(QLatin1String(kExploreSourceType), next);
    Q_EMIT exploreChanged();
}

QString
AppPrefs::exploreHost() const {
    return m_settings.value(QLatin1String(kExploreHost), QString()).toString();
}

void
AppPrefs::setExploreHost(const QString& host) {
    if (host == exploreHost()) {
        return;
    }
    m_settings.setValue(QLatin1String(kExploreHost), host);
    Q_EMIT exploreChanged();
}

int
AppPrefs::explorePort() const {
    return sane_explore_port(m_settings.value(QLatin1String(kExplorePort), 1234).toInt());
}

void
AppPrefs::setExplorePort(int port) {
    const int next = sane_explore_port(port);
    if (next == m_settings.value(QLatin1String(kExplorePort), 1234).toInt()) {
        return;
    }
    m_settings.setValue(QLatin1String(kExplorePort), next);
    Q_EMIT exploreChanged();
}

QString
AppPrefs::exploreFreqMhz() const {
    /* A string, like every other frequency in this app: the session-args builder and
     * the card meta both read freqMhz as text, and a number here loses the trailing
     * digits that tell a user which channel they were on. */
    return m_settings.value(QLatin1String(kExploreFreqMhz), QString()).toString();
}

void
AppPrefs::setExploreFreqMhz(const QString& mhz) {
    if (mhz == exploreFreqMhz()) {
        return;
    }
    m_settings.setValue(QLatin1String(kExploreFreqMhz), mhz);
    Q_EMIT exploreChanged();
}

} // namespace dsd_qt
