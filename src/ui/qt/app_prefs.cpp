// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "app_prefs.h"

#include <QDateTime>
#include <QLatin1String>
#include <QVariant>
#include <cmath>

namespace dsd_qt {

namespace {

// Keys are flat and stable: renaming one silently resets that preference for
// every existing install, so treat them as a persistence format.
constexpr const char kAppearance[] = "ui/appearance";
constexpr const char kMetricUnits[] = "ui/metricUnits";
constexpr const char kOnboardingDone[] = "ui/onboardingDone";
constexpr const char kBackgroundListening[] = "listen/background";
constexpr const char kKeepScreenAwake[] = "listen/keepAwake";
constexpr const char kSkipEncrypted[] = "decode/skipEncrypted";
constexpr const char kHangtimeSec[] = "decode/hangtimeSec";
constexpr const char kAutoPpm[] = "decode/autoPpm";
constexpr const char kGainDb[] = "tuner/gainDb";
constexpr const char kPpm[] = "tuner/ppm";
constexpr const char kBandwidthKhz[] = "tuner/bandwidthKhz";
constexpr const char kBiasTee[] = "tuner/biasTee";
constexpr const char kExtraArgs[] = "decode/extraArgs";
// RadioReference account. There is deliberately no password key: the password is
// held in memory for the session and re-prompted next launch.
constexpr const char kRrUsername[] = "rr/username";
constexpr const char kRrAppKey[] = "rr/appKey";
// Where the last explore session was pointed, so the next one resumes there
// instead of asking again. Not a saved system: exploring has no card, no name and
// no trunking, and it must not appear in the list of things to listen to.
constexpr const char kExploreSourceType[] = "explore/sourceType";
constexpr const char kExploreHost[] = "explore/host";
constexpr const char kExplorePort[] = "explore/port";
constexpr const char kExploreFreqMhz[] = "explore/freqMhz";

/*
 * Preferences are range-checked on the way out. They are checked on the way
 * in as well, and the setters below compare against what is *stored* rather than
 * against the checked reading — otherwise an out-of-range write persists (the
 * getter hides it) and the corrective write that follows looks like a no-op and is
 * dropped, leaving the file permanently disagreeing with the app.
 */

/** @brief Finite seconds in the UI range, rounded to the displayed precision. */
double
sane_hangtime(double seconds) {
    if (!std::isfinite(seconds)) {
        return 2.0;
    }
    return std::round(qBound(0.0, seconds, 30.0) * 10.0) / 10.0;
}

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
    m_locationExpiry.setSingleShot(true);
    connect(&m_locationExpiry, &QTimer::timeout, this, [this]() {
        armLocationExpiry(); // Re-arm if the wall clock moved backwards before the timer fired.
        Q_EMIT locationChanged();
    });
    armLocationExpiry();
}

AppPrefs::~AppPrefs() = default;

void
AppPrefs::armLocationExpiry() {
    m_locationExpiry.stop();
    expireLocation();
    const qint64 at = m_settings.value(QStringLiteral("location/lastFixAt"), 0).toLongLong();
    if (at > 0) {
        const qint64 remaining = 24LL * 60 * 60 * 1000 - (QDateTime::currentMSecsSinceEpoch() - at);
        m_locationExpiry.start(static_cast<int>(qMax<qint64>(1, remaining)));
    }
}

// Location is a short-lived private hint. Delete the whole fix on expiry so
// stale coordinates cannot silently be reused or survive in a settings export.
void
AppPrefs::expireLocation() const {
    const qint64 at = m_settings.value(QStringLiteral("location/lastFixAt"), 0).toLongLong();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (at > 0 && at <= now && now - at < 24LL * 60 * 60 * 1000) {
        return;
    }
    m_settings.remove(QStringLiteral("location/lastLat"));
    m_settings.remove(QStringLiteral("location/lastLon"));
    m_settings.remove(QStringLiteral("location/lastFixAt"));
    m_settings.remove(QStringLiteral("location/lastAccuracyM"));
}

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
AppPrefs::metricUnits() const {
    return m_settings.value(QLatin1String(kMetricUnits), false).toBool();
}

void
AppPrefs::setMetricUnits(bool on) {
    if (on == metricUnits()) {
        return;
    }
    m_settings.setValue(QLatin1String(kMetricUnits), on);
    Q_EMIT metricUnitsChanged();
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
AppPrefs::notificationExplained() const {
    return m_settings.value(QStringLiteral("ui/notificationExplained"), false).toBool();
}

void
AppPrefs::setNotificationExplained(bool value) {
    if (value == notificationExplained()) {
        return;
    }
    m_settings.setValue(QStringLiteral("ui/notificationExplained"), value);
    Q_EMIT notificationExplainedChanged();
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

double
AppPrefs::hangtimeSec() const {
    return sane_hangtime(m_settings.value(QLatin1String(kHangtimeSec), 2.0).toDouble());
}

void
AppPrefs::setHangtimeSec(double seconds) {
    const double value = sane_hangtime(seconds);
    if (qFuzzyCompare(value, m_settings.value(QLatin1String(kHangtimeSec), 2.0).toDouble())) {
        return;
    }
    m_settings.setValue(QLatin1String(kHangtimeSec), value);
    Q_EMIT hangtimeSecChanged();
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
AppPrefs::rrUsername() const {
    return m_settings.value(QLatin1String(kRrUsername), QString()).toString();
}

void
AppPrefs::setRrUsername(const QString& username) {
    if (username == rrUsername()) {
        return;
    }
    m_settings.setValue(QLatin1String(kRrUsername), username);
    Q_EMIT rrUsernameChanged();
}

QString
AppPrefs::rrAppKey() const {
    return m_settings.value(QLatin1String(kRrAppKey), QString()).toString();
}

void
AppPrefs::setRrAppKey(const QString& key) {
    if (key == rrAppKey()) {
        return;
    }
    m_settings.setValue(QLatin1String(kRrAppKey), key);
    Q_EMIT rrAppKeyChanged();
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

bool
AppPrefs::autoStartOnAttach() const {
    return m_settings.value(QStringLiteral("listen/autoStartOnAttach"), false).toBool();
}

void
AppPrefs::setAutoStartOnAttach(bool value) {
    if (m_settings.value(QStringLiteral("listen/autoStartOnAttach"), false).toBool() == value) {
        return;
    }
    m_settings.setValue(QStringLiteral("listen/autoStartOnAttach"), value);
    Q_EMIT autoStartOnAttachChanged();
}

QString
AppPrefs::lastStartedKind() const {
    return m_settings.value(QStringLiteral("listen/lastStartedKind"), QString()).toString();
}

void
AppPrefs::setLastStartedKind(const QString& value) {
    if (m_settings.value(QStringLiteral("listen/lastStartedKind"), QString()).toString() == value) {
        return;
    }
    m_settings.setValue(QStringLiteral("listen/lastStartedKind"), value);
    Q_EMIT lastStartedKindChanged();
}

QString
AppPrefs::lastStartedUid() const {
    return m_settings.value(QStringLiteral("listen/lastStartedUid"), QString()).toString();
}

void
AppPrefs::setLastStartedUid(const QString& value) {
    if (m_settings.value(QStringLiteral("listen/lastStartedUid"), QString()).toString() == value) {
        return;
    }
    m_settings.setValue(QStringLiteral("listen/lastStartedUid"), value);
    Q_EMIT lastStartedUidChanged();
}

double
AppPrefs::lastLat() const {
    expireLocation();
    return m_settings.value(QStringLiteral("location/lastLat"), 0.0).toDouble();
}

void
AppPrefs::setLastLat(double value) {
    if (qFuzzyCompare(m_settings.value(QStringLiteral("location/lastLat"), 0.0).toDouble(), value)) {
        return;
    }
    m_settings.setValue(QStringLiteral("location/lastLat"), value);
    Q_EMIT locationChanged();
}

double
AppPrefs::lastLon() const {
    expireLocation();
    return m_settings.value(QStringLiteral("location/lastLon"), 0.0).toDouble();
}

void
AppPrefs::setLastLon(double value) {
    if (qFuzzyCompare(m_settings.value(QStringLiteral("location/lastLon"), 0.0).toDouble(), value)) {
        return;
    }
    m_settings.setValue(QStringLiteral("location/lastLon"), value);
    Q_EMIT locationChanged();
}

void
AppPrefs::setLocationFix(double lat, double lon, qint64 fixAtMs, double accuracyM) {
    m_settings.setValue(QStringLiteral("location/lastAccuracyM"), accuracyM);
    m_settings.setValue(QStringLiteral("location/lastLat"), lat);
    m_settings.setValue(QStringLiteral("location/lastLon"), lon);
    m_settings.setValue(QStringLiteral("location/lastFixAt"), fixAtMs);
    armLocationExpiry();
    Q_EMIT locationChanged();
}

double
AppPrefs::lastAccuracyM() const {
    expireLocation();
    return m_settings.value(QStringLiteral("location/lastAccuracyM"), 0.0).toDouble();
}

qint64
AppPrefs::lastFixAt() const {
    expireLocation();
    return m_settings.value(QStringLiteral("location/lastFixAt"), 0).toLongLong();
}

void
AppPrefs::setLastFixAt(qint64 value) {
    if (m_settings.value(QStringLiteral("location/lastFixAt"), 0).toLongLong() == value) {
        return;
    }
    m_settings.setValue(QStringLiteral("location/lastFixAt"), value);
    armLocationExpiry();
    Q_EMIT locationChanged();
}

} // namespace dsd_qt
