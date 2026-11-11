// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "app_prefs.h"

#include <QAnyStringView>
#include <QChar>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLatin1String>
#include <QList>
#include <QMap>
#include <QMetaType>
#include <QSet>
#include <QStringList>
#include <QUuid>
#include <QVariant>
#include <QVariantMap>
#include <algorithm>
#include <cmath>
#include <dsd-neo/core/opts.h>
#include <iterator>
#include <utility>
#include "realtime_clock.h"
#include "settings_path.h"
#include "stored_int.h"

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
    // Call recording (issue #708): what to record and how much storage it may take. Portable: they are choices,
    // never a path, and never the recordings themselves.
    kRecordCalls,
    kRecordUnknownOnly,
    kRecordingLimitMb,
    // The session the host last accepted a start for, written when it accepts, before the post-initialization edge
    // writes lastStarted* (issue #671): what a recreated Activity finds running, which this device's service runs.
    kAcceptedStartKind,
    kAcceptedStartUid,
    // Issue #756: the longest pause one analog call recording keeps going through (--analog-record-max-gap-ms).
    // Portable with the other recording choices.
    kRecordingAnalogMaxGapMs,
    // Issue #668: how much of what was heard just before an NFM call each recording starts with
    // (--analog-record-preroll-ms). Portable with the other recording choices.
    kRecordingAnalogPrerollMs,
    // Issue #697: the other scan lists a scan session scans with its own, as accepted and as last started. Device-local
    // (list UIDs a restore can re-mint), and removed by a restore.
    kAcceptedStartCompanions,
    kLastStartedCompanions,
    // Issue #644: the targets the accepted scan session runs, as its start wrote them (JSON), so a recreated Activity
    // takes them up from here rather than from the saved lists' files, which a list deleted mid-session takes away.
    // Device-local with the rest of the accepted start.
    kAcceptedStartTargets,
    // Issue #801: the shortcuts pinned to Listen, in order (JSON). Portable: they name saved systems and scan lists by
    // UID, which a restore keeps, as it keeps lastStartedUid.
    kPinned,
    // Issue #646: the Listen tab last chosen. Device-local: how this device's screen was arranged, like
    // kMonitorDetailsExpanded.
    kListenSection,
    // Issue #830: DSD-neo's file browser before Android's picker. Device-local: whether this device's picker works is
    // the device's, not something a backup should carry to another one.
    kChooseFilesInApp,
    // The order the saved-systems list is shown in. Device-local: it follows how this device's screen was arranged,
    // like kListenSection.
    kSystemsSort,
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
    {"recording/recordCalls", true},
    {"recording/recordUnknownOnly", true},
    {"recording/limitMb", true},
    {"listen/acceptedStartKind", false},
    {"listen/acceptedStartUid", false},
    {"recording/analogMaxGapMs", true},
    {"recording/analogPrerollMs", true},
    {"listen/acceptedStartCompanions", false},
    {"listen/lastStartedCompanions", false},
    {"listen/acceptedStartTargets", false},
    {"listen/pinned", true},
    {"listen/section", false},
    {"ui/chooseFilesInApp", false},
    {"listen/systemsSort", false},
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

constexpr double kDefaultHangtimeSec = 2.0;

/** @brief Finite seconds in the UI range, rounded to the displayed precision. */
double
sane_hangtime(double seconds) {
    if (!std::isfinite(seconds)) {
        return kDefaultHangtimeSec;
    }
    return std::round(qBound(0.0, seconds, 30.0) * 10.0) / 10.0;
}

/** @brief The hang time @p raw holds, else the default: text that is no number would otherwise read as 0 s. */
double
stored_hangtime(const QVariant& raw) {
    bool ok = false;
    const double seconds = raw.toDouble(&ok);
    return ok ? sane_hangtime(seconds) : kDefaultHangtimeSec;
}

/** @brief Whether writing @p next would change the hang time stored as @p stored; one that is no number differs. */
bool
stored_hangtime_differs(const QVariant& stored, double next) {
    bool ok = false;
    const double seconds = stored.toDouble(&ok);
    return !ok || !qFuzzyCompare(seconds, next);
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

/** @brief Whether @p section names a Listen tab (issue #646). The ids are persisted: never rename one. */
bool
is_listen_section(const QString& section) {
    return section == QLatin1String("systems") || section == QLatin1String("scanLists")
           || section == QLatin1String("explore");
}

/** @brief The Listen tab @p raw names, else Systems: text QSettings did not write as one string (a list) included. */
QString
sane_listen_section(const QVariant& raw) {
    const QString section = raw.metaType().id() == QMetaType::QString ? raw.toString() : QString();
    return is_listen_section(section) ? section : QStringLiteral("systems");
}

/** @brief Whether @p sort names a saved-systems order. The ids are persisted: never rename one. */
bool
is_systems_sort(const QString& sort) {
    return sort == QLatin1String("name") || sort == QLatin1String("place") || sort == QLatin1String("nearest")
           || sort == QLatin1String("recent") || sort == QLatin1String("added");
}

/** @brief The order @p raw names, else the order added: text QSettings did not write as one string included. */
QString
sane_systems_sort(const QVariant& raw) {
    const QString sort = raw.metaType().id() == QMetaType::QString ? raw.toString() : QString();
    return is_systems_sort(sort) ? sort : QStringLiteral("added");
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
    int mode = 0;
    const bool ok = stored_int(raw, &mode);
    return (ok && mode >= AppPrefs::ScreenSystem && mode <= AppPrefs::ScreenOffBetweenCalls) ? mode
                                                                                             : AppPrefs::ScreenSystem;
}

/** @brief @p raw if it holds one of the offered delays, else the default; text that is not a number included. */
int
sane_screen_delay(const QVariant& raw) {
    int seconds = 0;
    const bool ok = stored_int(raw, &seconds);
    const bool offered = ok
                         && std::any_of(std::begin(kScreenDelayChoices), std::end(kScreenDelayChoices),
                                        [seconds](int choice) { return choice == seconds; });
    return offered ? seconds : kDefaultScreenDelaySec;
}

// The storage limits the settings offer, in decimal MB (0: no limit), and the 1 GB default (D2). The recording store
// counts in the same units (RecordingStore::kBytesPerMb).
constexpr int kRecordingLimitChoices[] = {250, 500, 1000, 2000, 5000, 0};
constexpr int kDefaultRecordingLimitMb = 1000;

/** @brief @p raw if it holds one of the offered limits, else the default; text that is not a number included. */
int
sane_recording_limit(const QVariant& raw) {
    int mb = 0;
    const bool ok = stored_int(raw, &mb);
    const bool offered = ok
                         && std::any_of(std::begin(kRecordingLimitChoices), std::end(kRecordingLimitChoices),
                                        [mb](int choice) { return choice == mb; });
    return offered ? mb : kDefaultRecordingLimitMb;
}

// The analog pause holds the settings offer, in ms (200: off, the decoder's default; 200 or less is off), and the range
// the decoder takes (--analog-record-max-gap-ms). A value in range but not offered is kept as it is: it came from a
// backup or an older build, and Settings shows it as itself.
constexpr int kAnalogMaxGapChoices[] = {DSD_ANALOG_RECORD_MAX_GAP_DEFAULT_MS, 500, 1000, 2000, 3000, 5000,
                                        DSD_ANALOG_RECORD_MAX_GAP_MAX_MS};
constexpr int kDefaultAnalogMaxGapMs = DSD_ANALOG_RECORD_MAX_GAP_DEFAULT_MS;
constexpr int kMaxAnalogMaxGapMs = DSD_ANALOG_RECORD_MAX_GAP_MAX_MS;

/** @brief @p raw if it holds a value the decoder takes (0..10000 ms), else the default; text that is not a number
 *         included. */
int
sane_analog_max_gap(const QVariant& raw) {
    int ms = 0;
    const bool ok = stored_int(raw, &ms);
    return ok && ms >= 0 && ms <= kMaxAnalogMaxGapMs ? ms : kDefaultAnalogMaxGapMs;
}

// The NFM pre-rolls the settings offer, in ms (0: off), and the range the decoder takes (--analog-record-preroll-ms).
// As with the pause hold, a value in range but not offered is kept as it is.
constexpr int kAnalogPrerollChoices[] = {0, 100, 200, 300, 500, DSD_ANALOG_RECORD_PREROLL_MAX_MS};
constexpr int kDefaultAnalogPrerollMs = DSD_ANALOG_RECORD_PREROLL_DEFAULT_MS;
constexpr int kMaxAnalogPrerollMs = DSD_ANALOG_RECORD_PREROLL_MAX_MS;

/** @brief @p raw if it holds a value the decoder takes (0..1000 ms), else the default; text that is not a number
 *         included. */
int
sane_analog_preroll(const QVariant& raw) {
    int ms = 0;
    const bool ok = stored_int(raw, &ms);
    return ok && ms >= 0 && ms <= kMaxAnalogPrerollMs ? ms : kDefaultAnalogPrerollMs;
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
    int value = 0;
    return !stored_int(stored, &value) || value != next;
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
// launched rather than on the app. The one place the settings file is opened. It is opened as that one file, so no
// fallback file is read: the app never writes one, and a malformed one, read again whenever it changes, would set
// FormatError on every instance, failing replacePortable() (issue #828).
AppPrefs::Settings::Settings()
    : QSettings(settings_file_path(QSettings::IniFormat, settingsOrganization(), settingsApplication()),
                QSettings::IniFormat) {}

QString
AppPrefs::settingsOrganization() {
    return QStringLiteral("dsd-neo");
}

QString
AppPrefs::settingsApplication() {
    return QStringLiteral("dsd-neo-app");
}

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
    return sane_appearance(stored_int_or(m_settings.value(key_name(kAppearance)), FollowSystem));
}

void
AppPrefs::setAppearance(int mode) {
    const int next = sane_appearance(mode);
    if (!stored_int_differs(m_settings.value(key_name(kAppearance), FollowSystem), next)) {
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
    return stored_hangtime(m_settings.value(key_name(kHangtimeSec), kDefaultHangtimeSec));
}

void
AppPrefs::setHangtimeSec(double seconds) {
    const double value = sane_hangtime(seconds);
    if (!stored_hangtime_differs(m_settings.value(key_name(kHangtimeSec), kDefaultHangtimeSec), value)) {
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
    return stored_int_or(m_settings.value(key_name(kGainDb)), 30);
}

void
AppPrefs::setGainDb(int db) {
    if (!stored_int_differs(m_settings.value(key_name(kGainDb), 30), db)) {
        return;
    }
    m_settings.setValue(key_name(kGainDb), db);
    Q_EMIT gainDbChanged();
}

int
AppPrefs::ppm() const {
    return stored_int_or(m_settings.value(key_name(kPpm)), 0);
}

void
AppPrefs::setPpm(int value) {
    if (!stored_int_differs(m_settings.value(key_name(kPpm), 0), value)) {
        return;
    }
    m_settings.setValue(key_name(kPpm), value);
    Q_EMIT ppmChanged();
}

int
AppPrefs::bandwidthKhz() const {
    return stored_int_or(m_settings.value(key_name(kBandwidthKhz)), 48);
}

void
AppPrefs::setBandwidthKhz(int khz) {
    if (!stored_int_differs(m_settings.value(key_name(kBandwidthKhz), 48), khz)) {
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
    return sane_tuner_port(stored_int_or(m_settings.value(key_name(kExplorePort)), 1234));
}

void
AppPrefs::setExplorePort(int port) {
    const int next = sane_tuner_port(port);
    if (!stored_int_differs(m_settings.value(key_name(kExplorePort), 1234), next)) {
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
    return sane_tuner_port(stored_int_or(m_settings.value(key_name(kSearchPort)), 1234));
}

void
AppPrefs::setSearchPort(int port) {
    const int next = sane_tuner_port(port);
    if (!stored_int_differs(m_settings.value(key_name(kSearchPort), 1234), next)) {
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
AppPrefs::chooseFilesInApp() const {
    return m_settings.value(key_name(kChooseFilesInApp), false).toBool();
}

void
AppPrefs::setChooseFilesInApp(bool on) {
    if (on == chooseFilesInApp()) {
        return;
    }
    m_settings.setValue(key_name(kChooseFilesInApp), on);
    Q_EMIT chooseFilesInAppChanged();
}

QString
AppPrefs::listenSection() const {
    return sane_listen_section(m_settings.value(key_name(kListenSection)));
}

void
AppPrefs::setListenSection(const QString& section) {
    if (!is_listen_section(section)) {
        return;
    }
    // Compared with the raw stored value (absent counts as Systems), so choosing Systems over a value that only reads
    // as Systems still lands.
    const QVariant stored = m_settings.value(key_name(kListenSection), QStringLiteral("systems"));
    if (stored.metaType().id() == QMetaType::QString && stored.toString() == section) {
        return;
    }
    m_settings.setValue(key_name(kListenSection), section);
    Q_EMIT listenSectionChanged();
}

QString
AppPrefs::systemsSort() const {
    return sane_systems_sort(m_settings.value(key_name(kSystemsSort)));
}

void
AppPrefs::setSystemsSort(const QString& sort) {
    if (!is_systems_sort(sort)) {
        return;
    }
    // Compared with the raw stored value (absent counts as the order added), so choosing it over a value that only
    // reads as that still lands.
    const QVariant stored = m_settings.value(key_name(kSystemsSort), QStringLiteral("added"));
    if (stored.metaType().id() == QMetaType::QString && stored.toString() == sort) {
        return;
    }
    m_settings.setValue(key_name(kSystemsSort), sort);
    Q_EMIT systemsSortChanged();
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

QString
AppPrefs::acceptedStartKind() const {
    return m_settings.value(key_name(kAcceptedStartKind), QString()).toString();
}

void
AppPrefs::setAcceptedStartKind(const QString& value) {
    if (acceptedStartKind() == value) {
        return;
    }
    m_settings.setValue(key_name(kAcceptedStartKind), value);
    Q_EMIT acceptedStartChanged();
}

QString
AppPrefs::acceptedStartUid() const {
    return m_settings.value(key_name(kAcceptedStartUid), QString()).toString();
}

void
AppPrefs::setAcceptedStartUid(const QString& value) {
    if (acceptedStartUid() == value) {
        return;
    }
    m_settings.setValue(key_name(kAcceptedStartUid), value);
    Q_EMIT acceptedStartChanged();
}

namespace {
/* A list of UIDs as stored: a list, or one UID the ini backend handed back as text; empty entries dropped. */
QStringList
stored_uids(const QVariant& stored) {
    QStringList uids = stored.toStringList();
    uids.removeAll(QString());
    return uids;
}
} // namespace

QStringList
AppPrefs::acceptedStartCompanions() const {
    return stored_uids(m_settings.value(key_name(kAcceptedStartCompanions)));
}

void
AppPrefs::setAcceptedStartCompanions(const QStringList& value) {
    if (acceptedStartCompanions() == value) {
        return;
    }
    if (value.isEmpty()) {
        m_settings.remove(key_name(kAcceptedStartCompanions));
    } else {
        m_settings.setValue(key_name(kAcceptedStartCompanions), value);
    }
    Q_EMIT acceptedStartChanged();
}

QStringList
AppPrefs::lastStartedCompanions() const {
    return stored_uids(m_settings.value(key_name(kLastStartedCompanions)));
}

void
AppPrefs::setLastStartedCompanions(const QStringList& value) {
    if (lastStartedCompanions() == value) {
        return;
    }
    if (value.isEmpty()) {
        m_settings.remove(key_name(kLastStartedCompanions));
    } else {
        m_settings.setValue(key_name(kLastStartedCompanions), value);
    }
    Q_EMIT lastStartedCompanionsChanged();
}

QVariantList
AppPrefs::acceptedStartTargets() const {
    const QJsonDocument stored =
        QJsonDocument::fromJson(m_settings.value(key_name(kAcceptedStartTargets)).toString().toUtf8());
    return stored.isArray() ? stored.array().toVariantList() : QVariantList();
}

void
AppPrefs::setAcceptedStartTargets(const QVariantList& value) {
    const QString text =
        value.isEmpty()
            ? QString()
            : QString::fromUtf8(QJsonDocument(QJsonArray::fromVariantList(value)).toJson(QJsonDocument::Compact));
    if (m_settings.value(key_name(kAcceptedStartTargets)).toString() == text) {
        return;
    }
    if (text.isEmpty()) {
        m_settings.remove(key_name(kAcceptedStartTargets));
    } else {
        m_settings.setValue(key_name(kAcceptedStartTargets), text);
    }
    Q_EMIT acceptedStartChanged();
}

namespace {
// Issue #801: a restored scan list's UID is any non-empty text, so a pinned one is bounded rather than shaped.
constexpr int kMaxPinnedScanUidLength = 128;

/* One pinned shortcut as stored, or empty when it is none: a known kind, and the UID that kind takes -- a saved row's
 * UUID in the form the model stores it, a scan list's UID as stored, none for Signal search and Explore. */
QVariantMap
sane_pin(const QJsonValue& value) {
    const QJsonObject pin = value.toObject();
    const QString kind = pin.value(QLatin1String("kind")).toString();
    const QString uid = pin.value(QLatin1String("uid")).toString();
    QString kept;
    if (kind == QLatin1String("system") || kind == QLatin1String("site")) {
        const QUuid parsed(uid);
        if (parsed.isNull()) {
            return {};
        }
        kept = parsed.toString(QUuid::WithoutBraces);
    } else if (kind == QLatin1String("scan")) {
        if (uid.isEmpty() || uid.size() > kMaxPinnedScanUidLength) {
            return {};
        }
        kept = uid;
    } else if (kind != QLatin1String("search") && kind != QLatin1String("explore")) {
        return {};
    }
    return {{QStringLiteral("kind"), kind}, {QStringLiteral("uid"), kept}};
}

/* The pins of @p stored in order: those that are none dropped, a repeat of a kind and UID dropped after its first, and
 * no more than the cap. */
QVariantList
sane_pins(const QJsonArray& stored) {
    QVariantList pins;
    QSet<QString> seen;
    for (const auto& value : stored) {
        const QVariantMap pin = sane_pin(value);
        const QString key = pin.value(QStringLiteral("kind")).toString() + QLatin1Char(':')
                            + pin.value(QStringLiteral("uid")).toString();
        if (pin.isEmpty() || seen.contains(key)) {
            continue;
        }
        if (pins.size() == AppPrefs::kPinnedCap) {
            break;
        }
        seen.insert(key);
        pins.append(pin);
    }
    return pins;
}
} // namespace

QVariantList
AppPrefs::pinned() const {
    // Only text QSettings wrote reads back as one string: a hand-edited unquoted value arrives as a list, split at its
    // commas and stripped of its quotes, and reads as no pins, as text that is no JSON array does.
    const QVariant stored = m_settings.value(key_name(kPinned));
    if (stored.metaType().id() != QMetaType::QString) {
        return {};
    }
    const QJsonDocument parsed = QJsonDocument::fromJson(stored.toString().toUtf8());
    return parsed.isArray() ? sane_pins(parsed.array()) : QVariantList();
}

void
AppPrefs::setPinned(const QVariantList& value) {
    const QVariantList pins = sane_pins(QJsonArray::fromVariantList(value));
    const QString text =
        pins.isEmpty()
            ? QString()
            : QString::fromUtf8(QJsonDocument(QJsonArray::fromVariantList(pins)).toJson(QJsonDocument::Compact));
    // Compared with the raw stored value, so a value the getter reads as no pins is still replaced or removed.
    const QVariant stored = m_settings.value(key_name(kPinned));
    const bool same =
        text.isEmpty() ? !stored.isValid() : stored.metaType().id() == QMetaType::QString && stored.toString() == text;
    if (same) {
        return;
    }
    if (text.isEmpty()) {
        m_settings.remove(key_name(kPinned));
    } else {
        m_settings.setValue(key_name(kPinned), text);
    }
    Q_EMIT pinnedChanged();
}

bool
AppPrefs::forgetScanCompanions() {
    Settings settings;
    settings.remove(key_name(kAcceptedStartCompanions));
    settings.remove(key_name(kLastStartedCompanions));
    settings.sync();
    return settings.status() == QSettings::NoError;
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

bool
AppPrefs::recordCalls() const {
    return m_settings.value(key_name(kRecordCalls), false).toBool();
}

void
AppPrefs::setRecordCalls(bool on) {
    if (on == recordCalls()) {
        return;
    }
    m_settings.setValue(key_name(kRecordCalls), on);
    Q_EMIT recordCallsChanged();
}

bool
AppPrefs::recordUnknownOnly() const {
    return m_settings.value(key_name(kRecordUnknownOnly), false).toBool();
}

void
AppPrefs::setRecordUnknownOnly(bool on) {
    if (on == recordUnknownOnly()) {
        return;
    }
    m_settings.setValue(key_name(kRecordUnknownOnly), on);
    Q_EMIT recordUnknownOnlyChanged();
}

int
AppPrefs::recordingLimitMb() const {
    return sane_recording_limit(m_settings.value(key_name(kRecordingLimitMb), kDefaultRecordingLimitMb));
}

void
AppPrefs::setRecordingLimitMb(int mb) {
    const int next = sane_recording_limit(mb);
    if (!stored_int_differs(m_settings.value(key_name(kRecordingLimitMb), kDefaultRecordingLimitMb), next)) {
        return;
    }
    m_settings.setValue(key_name(kRecordingLimitMb), next);
    Q_EMIT recordingLimitMbChanged();
}

QVariantList
AppPrefs::recordingLimitChoices() const {
    QVariantList choices;
    for (const int choice : kRecordingLimitChoices) {
        choices.append(choice);
    }
    return choices;
}

int
AppPrefs::analogMaxGapMs() const {
    return sane_analog_max_gap(m_settings.value(key_name(kRecordingAnalogMaxGapMs), kDefaultAnalogMaxGapMs));
}

void
AppPrefs::setAnalogMaxGapMs(int ms) {
    const int next = sane_analog_max_gap(ms);
    if (!stored_int_differs(m_settings.value(key_name(kRecordingAnalogMaxGapMs), kDefaultAnalogMaxGapMs), next)) {
        return;
    }
    m_settings.setValue(key_name(kRecordingAnalogMaxGapMs), next);
    Q_EMIT analogMaxGapMsChanged();
}

QVariantList
AppPrefs::analogMaxGapChoices() const {
    QVariantList choices;
    for (const int choice : kAnalogMaxGapChoices) {
        choices.append(choice);
    }
    return choices;
}

int
AppPrefs::analogPrerollMs() const {
    return sane_analog_preroll(m_settings.value(key_name(kRecordingAnalogPrerollMs), kDefaultAnalogPrerollMs));
}

void
AppPrefs::setAnalogPrerollMs(int ms) {
    const int next = sane_analog_preroll(ms);
    if (!stored_int_differs(m_settings.value(key_name(kRecordingAnalogPrerollMs), kDefaultAnalogPrerollMs), next)) {
        return;
    }
    m_settings.setValue(key_name(kRecordingAnalogPrerollMs), next);
    Q_EMIT analogPrerollMsChanged();
}

QVariantList
AppPrefs::analogPrerollChoices() {
    QVariantList choices;
    for (const int choice : kAnalogPrerollChoices) {
        choices.append(choice);
    }
    return choices;
}

} // namespace dsd_qt
