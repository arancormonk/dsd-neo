// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "app_prefs.h"

#include <QChar>
#include <QLatin1String>
#include <QList>
#include <QMap>
#include <QMetaType>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>
#include <algorithm>
#include <cmath>
#include <iterator>
#include "realtime_clock.h"

namespace dsd_qt {

namespace {

// Keys are flat and stable: renaming one silently resets that preference for
// every existing install, so treat them as a persistence format. This table is
// the one place that names them and says whether a key travels with a backup
// (portable) or belongs to this device only. Append new keys; never reorder
// without moving the Key enum with them.
enum Key {
    kAppearance,
    kMetricUnits,
    kOnboardingDone,
    kNotificationExplained,
    kBackgroundListening,
    kScreenMode,
    kScreenDelaySec,
    // The keep-awake switch the screen mode replaced (#574). Read only, to migrate an
    // install that never chose a mode; never written, so a downgrade finds it intact.
    kLegacyKeepAwake,
    kAutoStartOnAttach,
    kLastStartedKind,
    kLastStartedUid,
    kSkipEncrypted,
    kPersistTgLockouts,
    kHangtimeSec,
    kAutoPpm,
    // Relaxed CRC (-F) for every session the app starts; a system's own -F or --strict-crc wins.
    kRelaxedCrc,
    kGainDb,
    kPpm,
    kBandwidthKhz,
    kBiasTee,
    kExtraArgs,
    // RadioReference account. There is deliberately no password key: the password is
    // held in memory for the session and re-prompted next launch.
    kRrUsername,
    kRrAppKey,
    // Where the last explore session was pointed, so the next one resumes there
    // instead of asking again. Not a saved system: exploring has no card, no name and
    // no trunking, and it must not appear in the list of things to listen to.
    kExploreSourceType,
    kExploreHost,
    kExplorePort,
    kExploreFreqMhz,
    // The radio the last signal search started on. Kept apart from the explore keys
    // so starting a search never rewrites where Explore resumes.
    kSearchSourceType,
    kSearchHost,
    kSearchPort,
    // The last location fix is a short-lived private hint tied to this device.
    kLastLat,
    kLastLon,
    kLastFixAt,
    kLastAccuracyM,
    // Whether the Monitor's "Signal & site" readings were left open (#718): how
    // this device's screen was arranged, so it is not carried by a backup.
    kMonitorDetailsExpanded,
    kKeyCount
};

constexpr AppPrefs::KeySpec kKeyTable[] = {
    {"ui/appearance", true},
    {"ui/metricUnits", true},
    {"ui/onboardingDone", false},
    {"ui/notificationExplained", false},
    {"listen/background", true},
    {"listen/screenMode", true},
    {"listen/screenDelaySec", true},
    {"listen/keepAwake", true},
    {"listen/autoStartOnAttach", true},
    {"listen/lastStartedKind", true},
    {"listen/lastStartedUid", true},
    {"decode/skipEncrypted", true},
    {"decode/persistTgLockouts", true},
    {"decode/hangtimeSec", true},
    {"decode/autoPpm", true},
    {"decode/relaxedCrc", true},
    {"tuner/gainDb", true},
    {"tuner/ppm", true},
    {"tuner/bandwidthKhz", true},
    {"tuner/biasTee", true},
    {"decode/extraArgs", true},
    {"rr/username", true},
    {"rr/appKey", true},
    {"explore/sourceType", true},
    {"explore/host", true},
    {"explore/port", true},
    {"explore/freqMhz", true},
    {"search/sourceType", true},
    {"search/host", true},
    {"search/port", true},
    {"location/lastLat", false},
    {"location/lastLon", false},
    {"location/lastFixAt", false},
    {"location/lastAccuracyM", false},
    {"monitor/detailsExpanded", false},
};
static_assert(sizeof(kKeyTable) / sizeof(kKeyTable[0]) == kKeyCount, "key table and Key enum must stay in step");

/** @brief The settings key for @p id. */
QString
key_name(Key id) {
    return QLatin1String(kKeyTable[id].name);
}

/** @brief A JSON scalar for a stored value: the ini backend can hand back a list for text with commas. */
QVariant
as_scalar(const QVariant& stored) {
    if (stored.metaType().id() == QMetaType::QStringList) {
        return stored.toStringList().join(QLatin1Char(','));
    }
    return stored;
}

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
 * @brief @p type if it is a tuner source Explore and Signal search can start on, else empty.
 *
 * Only the tuner sources can explore or search -- an RTL-SDR dongle, an Airspy, or
 * an rtl_tcp server; a PCM feed or a file has nothing to point anywhere -- so
 * anything else reads as "not chosen yet" and sends the user to the setup sheet
 * rather than starting something that cannot tune. Empty is the honest default: it
 * is what makes the first tap ask.
 */
QString
sane_tuner_source_type(const QString& type) {
    return (type == QLatin1String("usb") || type == QLatin1String("airspy") || type == QLatin1String("rtltcp"))
               ? type
               : QString();
}

/** @brief @p port if it is a usable TCP port, else the rtl_tcp default. */
int
sane_tuner_port(int port) {
    return (port >= 1 && port <= 65535) ? port : 1234;
}

// The delays the settings offer, in seconds. Must match ScreenPolicy.kt's
// DELAY_CHOICES_SECONDS and DEFAULT_DELAY_SECONDS.
constexpr int kScreenDelayChoices[] = {10, 30, 60, 120, 300};
constexpr int kDefaultScreenDelaySec = 30;

/** @brief @p raw if it holds a ScreenMode code, else System default; text that is not a number included. */
int
sane_screen_mode(const QVariant& raw) {
    bool ok = false;
    const int mode = raw.toInt(&ok);
    return (ok && mode >= AppPrefs::ScreenSystem && mode <= AppPrefs::ScreenOffBetweenCalls) ? mode
                                                                                             : AppPrefs::ScreenSystem;
}

/** @brief @p raw if it holds one of the offered delays, else the default; text that is not a number included. */
int
sane_screen_delay(const QVariant& raw) {
    bool ok = false;
    const int seconds = raw.toInt(&ok);
    const bool offered = ok
                         && std::any_of(std::begin(kScreenDelayChoices), std::end(kScreenDelayChoices),
                                        [seconds](int choice) { return choice == seconds; });
    return offered ? seconds : kDefaultScreenDelaySec;
}

/**
 * @brief Whether writing @p next would change what is stored as @p stored.
 *
 * Compares with the raw stored value, per the rule above: a stored value that
 * is out of range or not a number differs from every clean value, so the
 * corrective write lands.
 */
bool
stored_int_differs(const QVariant& stored, int next) {
    bool ok = false;
    const int value = stored.toInt(&ok);
    return !ok || value != next;
}

} // namespace

AppPrefs::AppPrefs(QObject* parent) : QObject(parent), m_settings() {
    m_locationExpiry.setSingleShot(true);
    connect(&m_locationExpiry, &QTimer::timeout, this, [this]() {
        armLocationExpiry(); // Re-arm if the wall clock moved backwards before the timer fired.
        Q_EMIT locationChanged();
    });
    armLocationExpiry();
}

AppPrefs::~AppPrefs() = default;

// Explicit scope and names: the organization name is empty on some platforms,
// and QSettings' fallback location would then depend on how the process was
// launched rather than on the app. The one place the settings file is opened.
AppPrefs::Settings::Settings()
    : QSettings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("dsd-neo"), QStringLiteral("dsd-neo-app")) {}

QList<AppPrefs::KeySpec>
AppPrefs::keyTable() {
    QList<KeySpec> table;
    for (const KeySpec& spec : kKeyTable) {
        table.append(spec);
    }
    return table;
}

QStringList
AppPrefs::portableKeys() {
    QStringList keys;
    for (const KeySpec& spec : kKeyTable) {
        if (spec.portable) {
            keys.append(QLatin1String(spec.name));
        }
    }
    return keys;
}

QVariantMap
AppPrefs::readPortable() {
    Settings settings;
    QVariantMap values;
    for (const KeySpec& spec : kKeyTable) {
        const QString name = QLatin1String(spec.name);
        if (spec.portable && settings.contains(name)) {
            values.insert(name, as_scalar(settings.value(name)));
        }
    }
    return values;
}

bool
AppPrefs::replacePortable(const QVariantMap& values) {
    Settings settings;
    for (const KeySpec& spec : kKeyTable) {
        if (!spec.portable) {
            continue;
        }
        const QString name = QLatin1String(spec.name);
        const auto it = values.constFind(name);
        if (it != values.constEnd()) {
            settings.setValue(name, it.value());
        } else {
            settings.remove(name);
        }
    }
    settings.sync();
    return settings.status() == QSettings::NoError;
}

void
AppPrefs::armLocationExpiry() {
    m_locationExpiry.stop();
    expireLocation();
    const qint64 at = m_settings.value(key_name(kLastFixAt), 0).toLongLong();
    if (at > 0) {
        const qint64 remaining = 24LL * 60 * 60 * 1000 - (realtimeMSecsSinceEpoch() - at);
        m_locationExpiry.start(static_cast<int>(qMax<qint64>(1, remaining)));
    }
}

// Location is a short-lived private hint. Delete the whole fix on expiry so
// stale coordinates cannot silently be reused or survive in a settings export.
void
AppPrefs::expireLocation() const {
    const qint64 at = m_settings.value(key_name(kLastFixAt), 0).toLongLong();
    const qint64 now = realtimeMSecsSinceEpoch();
    if (at > 0 && at <= now && now - at < 24LL * 60 * 60 * 1000) {
        return;
    }
    m_settings.remove(key_name(kLastLat));
    m_settings.remove(key_name(kLastLon));
    m_settings.remove(key_name(kLastFixAt));
    m_settings.remove(key_name(kLastAccuracyM));
}

int
AppPrefs::appearance() const {
    return sane_appearance(m_settings.value(key_name(kAppearance), FollowSystem).toInt());
}

void
AppPrefs::setAppearance(int mode) {
    const int next = sane_appearance(mode);
    if (next == m_settings.value(key_name(kAppearance), FollowSystem).toInt()) {
        return;
    }
    m_settings.setValue(key_name(kAppearance), next);
    Q_EMIT appearanceChanged();
}

bool
AppPrefs::metricUnits() const {
    return m_settings.value(key_name(kMetricUnits), false).toBool();
}

void
AppPrefs::setMetricUnits(bool on) {
    if (on == metricUnits()) {
        return;
    }
    m_settings.setValue(key_name(kMetricUnits), on);
    Q_EMIT metricUnitsChanged();
}

bool
AppPrefs::onboardingDone() const {
    return m_settings.value(key_name(kOnboardingDone), false).toBool();
}

void
AppPrefs::setOnboardingDone(bool done) {
    if (done == onboardingDone()) {
        return;
    }
    m_settings.setValue(key_name(kOnboardingDone), done);
    Q_EMIT onboardingDoneChanged();
}

bool
AppPrefs::notificationExplained() const {
    return m_settings.value(key_name(kNotificationExplained), false).toBool();
}

void
AppPrefs::setNotificationExplained(bool value) {
    if (value == notificationExplained()) {
        return;
    }
    m_settings.setValue(key_name(kNotificationExplained), value);
    Q_EMIT notificationExplainedChanged();
}

bool
AppPrefs::backgroundListening() const {
    return m_settings.value(key_name(kBackgroundListening), true).toBool();
}

void
AppPrefs::setBackgroundListening(bool on) {
    if (on == backgroundListening()) {
        return;
    }
    m_settings.setValue(key_name(kBackgroundListening), on);
    Q_EMIT backgroundListeningChanged();
}

int
AppPrefs::screenMode() const {
    const QVariant stored = m_settings.value(key_name(kScreenMode));
    if (stored.isValid()) {
        return sane_screen_mode(stored);
    }
    // Never chosen: carry the old switch over. Reading only — the new key is
    // written when the user picks a mode, and the old one never is.
    return m_settings.value(key_name(kLegacyKeepAwake), false).toBool() ? ScreenAlwaysOn : ScreenSystem;
}

void
AppPrefs::setScreenMode(int mode) {
    const int next = sane_screen_mode(mode);
    const QVariant stored = m_settings.value(key_name(kScreenMode));
    // With no stored mode, what the user sees is the migrated one: choosing it
    // again is not a change.
    if (stored.isValid() ? !stored_int_differs(stored, next) : next == screenMode()) {
        return;
    }
    // Storage only. The platform effect (the Android screen policy) is applied by
    // the DecoderHost the shared UI wires this preference to — this layer stays
    // free of platform APIs.
    m_settings.setValue(key_name(kScreenMode), next);
    Q_EMIT screenPolicyChanged();
}

int
AppPrefs::screenDelaySec() const {
    return sane_screen_delay(m_settings.value(key_name(kScreenDelaySec), kDefaultScreenDelaySec));
}

void
AppPrefs::setScreenDelaySec(int seconds) {
    const int next = sane_screen_delay(seconds);
    if (!stored_int_differs(m_settings.value(key_name(kScreenDelaySec), kDefaultScreenDelaySec), next)) {
        return;
    }
    m_settings.setValue(key_name(kScreenDelaySec), next);
    Q_EMIT screenPolicyChanged();
}

QVariantList
AppPrefs::screenDelayChoices() const {
    QVariantList choices;
    for (const int choice : kScreenDelayChoices) {
        choices.append(choice);
    }
    return choices;
}

bool
AppPrefs::persistTgLockouts() const {
    return m_settings.value(key_name(kPersistTgLockouts), true).toBool();
}

void
AppPrefs::setPersistTgLockouts(bool on) {
    if (on == persistTgLockouts()) {
        return;
    }
    m_settings.setValue(key_name(kPersistTgLockouts), on);
    Q_EMIT persistTgLockoutsChanged();
}

bool
AppPrefs::skipEncrypted() const {
    return m_settings.value(key_name(kSkipEncrypted), true).toBool();
}

void
AppPrefs::setSkipEncrypted(bool on) {
    if (on == skipEncrypted()) {
        return;
    }
    m_settings.setValue(key_name(kSkipEncrypted), on);
    Q_EMIT skipEncryptedChanged();
}

double
AppPrefs::hangtimeSec() const {
    return sane_hangtime(m_settings.value(key_name(kHangtimeSec), 2.0).toDouble());
}

void
AppPrefs::setHangtimeSec(double seconds) {
    const double value = sane_hangtime(seconds);
    if (qFuzzyCompare(value, m_settings.value(key_name(kHangtimeSec), 2.0).toDouble())) {
        return;
    }
    m_settings.setValue(key_name(kHangtimeSec), value);
    Q_EMIT hangtimeSecChanged();
}

bool
AppPrefs::autoPpm() const {
    return m_settings.value(key_name(kAutoPpm), false).toBool();
}

void
AppPrefs::setAutoPpm(bool on) {
    if (on == autoPpm()) {
        return;
    }
    m_settings.setValue(key_name(kAutoPpm), on);
    Q_EMIT autoPpmChanged();
}

bool
AppPrefs::relaxedCrc() const {
    return m_settings.value(key_name(kRelaxedCrc), false).toBool();
}

void
AppPrefs::setRelaxedCrc(bool on) {
    if (on == relaxedCrc()) {
        return;
    }
    m_settings.setValue(key_name(kRelaxedCrc), on);
    Q_EMIT relaxedCrcChanged();
}

int
AppPrefs::gainDb() const {
    return m_settings.value(key_name(kGainDb), 30).toInt();
}

void
AppPrefs::setGainDb(int db) {
    if (db == gainDb()) {
        return;
    }
    m_settings.setValue(key_name(kGainDb), db);
    Q_EMIT gainDbChanged();
}

int
AppPrefs::ppm() const {
    return m_settings.value(key_name(kPpm), 0).toInt();
}

void
AppPrefs::setPpm(int value) {
    if (value == ppm()) {
        return;
    }
    m_settings.setValue(key_name(kPpm), value);
    Q_EMIT ppmChanged();
}

int
AppPrefs::bandwidthKhz() const {
    return m_settings.value(key_name(kBandwidthKhz), 48).toInt();
}

void
AppPrefs::setBandwidthKhz(int khz) {
    if (khz == bandwidthKhz()) {
        return;
    }
    m_settings.setValue(key_name(kBandwidthKhz), khz);
    Q_EMIT bandwidthKhzChanged();
}

bool
AppPrefs::biasTee() const {
    return m_settings.value(key_name(kBiasTee), false).toBool();
}

void
AppPrefs::setBiasTee(bool on) {
    if (on == biasTee()) {
        return;
    }
    m_settings.setValue(key_name(kBiasTee), on);
    Q_EMIT biasTeeChanged();
}

QString
AppPrefs::extraArgs() const {
    return m_settings.value(key_name(kExtraArgs), QString()).toString();
}

void
AppPrefs::setExtraArgs(const QString& args) {
    if (args == extraArgs()) {
        return;
    }
    m_settings.setValue(key_name(kExtraArgs), args);
    Q_EMIT extraArgsChanged();
}

QString
AppPrefs::rrUsername() const {
    return m_settings.value(key_name(kRrUsername), QString()).toString();
}

void
AppPrefs::setRrUsername(const QString& username) {
    if (username == rrUsername()) {
        return;
    }
    m_settings.setValue(key_name(kRrUsername), username);
    Q_EMIT rrUsernameChanged();
}

QString
AppPrefs::rrAppKey() const {
    return m_settings.value(key_name(kRrAppKey), QString()).toString();
}

void
AppPrefs::setRrAppKey(const QString& key) {
    if (key == rrAppKey()) {
        return;
    }
    m_settings.setValue(key_name(kRrAppKey), key);
    Q_EMIT rrAppKeyChanged();
}

QString
AppPrefs::exploreSourceType() const {
    return sane_tuner_source_type(m_settings.value(key_name(kExploreSourceType), QString()).toString());
}

void
AppPrefs::setExploreSourceType(const QString& type) {
    const QString next = sane_tuner_source_type(type);
    if (next == m_settings.value(key_name(kExploreSourceType), QString()).toString()) {
        return;
    }
    m_settings.setValue(key_name(kExploreSourceType), next);
    Q_EMIT exploreChanged();
}

QString
AppPrefs::exploreHost() const {
    return m_settings.value(key_name(kExploreHost), QString()).toString();
}

void
AppPrefs::setExploreHost(const QString& host) {
    if (host == exploreHost()) {
        return;
    }
    m_settings.setValue(key_name(kExploreHost), host);
    Q_EMIT exploreChanged();
}

int
AppPrefs::explorePort() const {
    return sane_tuner_port(m_settings.value(key_name(kExplorePort), 1234).toInt());
}

void
AppPrefs::setExplorePort(int port) {
    const int next = sane_tuner_port(port);
    if (next == m_settings.value(key_name(kExplorePort), 1234).toInt()) {
        return;
    }
    m_settings.setValue(key_name(kExplorePort), next);
    Q_EMIT exploreChanged();
}

QString
AppPrefs::exploreFreqMhz() const {
    /* A string, like every other frequency in this app: the session-args builder and
     * the card meta both read freqMhz as text, and a number here loses the trailing
     * digits that tell a user which channel they were on. */
    return m_settings.value(key_name(kExploreFreqMhz), QString()).toString();
}

void
AppPrefs::setExploreFreqMhz(const QString& mhz) {
    if (mhz == exploreFreqMhz()) {
        return;
    }
    m_settings.setValue(key_name(kExploreFreqMhz), mhz);
    Q_EMIT exploreChanged();
}

QString
AppPrefs::searchSourceType() const {
    return sane_tuner_source_type(m_settings.value(key_name(kSearchSourceType), QString()).toString());
}

void
AppPrefs::setSearchSourceType(const QString& type) {
    const QString next = sane_tuner_source_type(type);
    if (next == m_settings.value(key_name(kSearchSourceType), QString()).toString()) {
        return;
    }
    m_settings.setValue(key_name(kSearchSourceType), next);
    Q_EMIT searchChanged();
}

QString
AppPrefs::searchHost() const {
    return m_settings.value(key_name(kSearchHost), QString()).toString();
}

void
AppPrefs::setSearchHost(const QString& host) {
    if (host == searchHost()) {
        return;
    }
    m_settings.setValue(key_name(kSearchHost), host);
    Q_EMIT searchChanged();
}

int
AppPrefs::searchPort() const {
    return sane_tuner_port(m_settings.value(key_name(kSearchPort), 1234).toInt());
}

void
AppPrefs::setSearchPort(int port) {
    const int next = sane_tuner_port(port);
    if (next == m_settings.value(key_name(kSearchPort), 1234).toInt()) {
        return;
    }
    m_settings.setValue(key_name(kSearchPort), next);
    Q_EMIT searchChanged();
}

bool
AppPrefs::monitorDetailsExpanded() const {
    return m_settings.value(key_name(kMonitorDetailsExpanded), false).toBool();
}

void
AppPrefs::setMonitorDetailsExpanded(bool on) {
    if (on == monitorDetailsExpanded()) {
        return;
    }
    m_settings.setValue(key_name(kMonitorDetailsExpanded), on);
    Q_EMIT monitorDetailsExpandedChanged();
}

bool
AppPrefs::autoStartOnAttach() const {
    return m_settings.value(key_name(kAutoStartOnAttach), false).toBool();
}

void
AppPrefs::setAutoStartOnAttach(bool value) {
    if (m_settings.value(key_name(kAutoStartOnAttach), false).toBool() == value) {
        return;
    }
    m_settings.setValue(key_name(kAutoStartOnAttach), value);
    Q_EMIT autoStartOnAttachChanged();
}

QString
AppPrefs::lastStartedKind() const {
    return m_settings.value(key_name(kLastStartedKind), QString()).toString();
}

void
AppPrefs::setLastStartedKind(const QString& value) {
    if (m_settings.value(key_name(kLastStartedKind), QString()).toString() == value) {
        return;
    }
    m_settings.setValue(key_name(kLastStartedKind), value);
    Q_EMIT lastStartedKindChanged();
}

QString
AppPrefs::lastStartedUid() const {
    return m_settings.value(key_name(kLastStartedUid), QString()).toString();
}

void
AppPrefs::setLastStartedUid(const QString& value) {
    if (m_settings.value(key_name(kLastStartedUid), QString()).toString() == value) {
        return;
    }
    m_settings.setValue(key_name(kLastStartedUid), value);
    Q_EMIT lastStartedUidChanged();
}

double
AppPrefs::lastLat() const {
    expireLocation();
    return m_settings.value(key_name(kLastLat), 0.0).toDouble();
}

void
AppPrefs::setLastLat(double value) {
    if (qFuzzyCompare(m_settings.value(key_name(kLastLat), 0.0).toDouble(), value)) {
        return;
    }
    m_settings.setValue(key_name(kLastLat), value);
    Q_EMIT locationChanged();
}

double
AppPrefs::lastLon() const {
    expireLocation();
    return m_settings.value(key_name(kLastLon), 0.0).toDouble();
}

void
AppPrefs::setLastLon(double value) {
    if (qFuzzyCompare(m_settings.value(key_name(kLastLon), 0.0).toDouble(), value)) {
        return;
    }
    m_settings.setValue(key_name(kLastLon), value);
    Q_EMIT locationChanged();
}

void
AppPrefs::setLocationFix(double lat, double lon, qint64 fixAtMs, double accuracyM) {
    m_settings.setValue(key_name(kLastAccuracyM), accuracyM);
    m_settings.setValue(key_name(kLastLat), lat);
    m_settings.setValue(key_name(kLastLon), lon);
    m_settings.setValue(key_name(kLastFixAt), fixAtMs);
    armLocationExpiry();
    Q_EMIT locationChanged();
}

double
AppPrefs::lastAccuracyM() const {
    expireLocation();
    return m_settings.value(key_name(kLastAccuracyM), 0.0).toDouble();
}

qint64
AppPrefs::lastFixAt() const {
    expireLocation();
    return m_settings.value(key_name(kLastFixAt), 0).toLongLong();
}

void
AppPrefs::setLastFixAt(qint64 value) {
    if (m_settings.value(key_name(kLastFixAt), 0).toLongLong() == value) {
        return;
    }
    m_settings.setValue(key_name(kLastFixAt), value);
    armLocationExpiry();
    Q_EMIT locationChanged();
}

} // namespace dsd_qt
