// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "app_prefs.h"
#include "app_squelch_default.h"

#include <QAnyStringView>
#include <QChar>
#include <QCoreApplication>
#include <QGuiApplication>
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
#include <Qt>
#include <algorithm>
#include <cmath>
#include <dsd-neo/core/opts.h>
#include <initializer_list> // IWYU pragma: keep -- braced range-for lists require this header.
#include <iterator>
#include <limits>
#include <utility>
#include "map_math.h"
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
    // Issue #786: GPS site roaming. The switch is device-local (whether this device may use its location for it); the
    // two distances, in km, are portable with the other listening choices.
    kRoamingEnabled,
    kRoamingMaxDistanceKm,
    kRoamingMarginKm,
    // Issue #800: the map (spec 4.8). Device-local: a tile URL template can carry a key, the saved view is a place
    // the user looked at, and the tiles switch and the age filter follow this device. Following is never saved.
    kMapTilesEnabled,
    kMapTileUrl,
    kMapTileAttribution,
    kMapViewport,
    kMapAgeFilter,
    // Issue #899: the squelch analog sessions start with (off, auto+N, noise+N). Portable with the other tuner
    // defaults; a backup without it restores the default, as an install that never chose one reads it.
    kRadioSquelch,
    // Issue #899: who owns the accepted session's squelch (SquelchDefaultPolicy::record()), so a recreated Activity
    // takes it back. Device-local with the rest of the accepted start.
    kAcceptedStartSquelch,
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
    {"roaming/enabled", false},
    {"roaming/maxDistanceKm", true},
    {"roaming/marginKm", true},
    {"map/tilesEnabled", false},
    {"map/tileUrl", false},
    {"map/tileAttribution", false},
    {"map/viewport", false},
    {"map/ageFilter", false},
    {"radio/squelch", true},
    {"listen/acceptedStartSquelch", false},
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

/** @brief The squelch @p raw holds when it is one app_squelch_default_valid() takes, else the default. */
QString
sane_squelch(const QVariant& raw) {
    const QString setting = raw.metaType().id() == QMetaType::QString ? raw.toString() : QString();
    return app_squelch_default_valid(setting) ? setting : QString::fromLatin1(kAppSquelchDefault);
}

/*
 * Issue #786: GPS roaming's distances, stored in km. Each unit offers its own choices; a value offered in either unit
 * is kept as it was picked (62 mi stays 99.779… km in metric), any other reads as the default of the unit shown now.
 */
constexpr double kKmPerMile = 1.609344;
constexpr double kRoamingMatchKm = 1e-6;

struct RoamingChoices {
    QList<double> km;
    QList<double> miles;
    double kmDefault = 0;
    double milesDefault = 0;
};

const RoamingChoices&
roaming_max_choices() {
    static const RoamingChoices choices{{25, 50, 100, 200}, {15, 30, 60, 125}, 100, 60};
    return choices;
}

const RoamingChoices&
roaming_margin_choices() {
    static const RoamingChoices choices{{1, 2, 5}, {0.5, 1, 3}, 2, 1};
    return choices;
}

/** @brief The choice @p km is, in km, from either unit's list; false when it is none of them. */
bool
roaming_offered(const RoamingChoices& choices, double km, double* offered) {
    if (!std::isfinite(km)) {
        return false;
    }
    const auto inKm = [km](double value) { return std::fabs(value - km) < kRoamingMatchKm; };
    const auto km_it = std::find_if(choices.km.cbegin(), choices.km.cend(), inKm);
    if (km_it != choices.km.cend()) {
        *offered = *km_it;
        return true;
    }
    const auto inMiles = [km](double value) { return std::fabs(value * kKmPerMile - km) < kRoamingMatchKm; };
    const auto mi_it = std::find_if(choices.miles.cbegin(), choices.miles.cend(), inMiles);
    if (mi_it != choices.miles.cend()) {
        *offered = *mi_it * kKmPerMile;
        return true;
    }
    return false;
}

/** @brief The distance @p stored holds when it is an offered choice, else the default in the unit shown. */
double
stored_roaming_km(const RoamingChoices& choices, const QVariant& stored, bool metric) {
    bool ok = false;
    const double km = stored.toDouble(&ok);
    double offered = 0;
    if (stored.isValid() && ok && roaming_offered(choices, km, &offered)) {
        return offered;
    }
    return metric ? choices.kmDefault : choices.milesDefault * kKmPerMile;
}

/** @brief The choices of the unit shown, in km. */
QVariantList
roaming_unit_choices(const RoamingChoices& choices, bool metric) {
    QVariantList out;
    for (const double value : metric ? choices.km : choices.miles) {
        out.append(metric ? value : value * kKmPerMile);
    }
    return out;
}

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

/**
 * @brief @p db held to the tuner gains Settings edits: 0 (automatic) and 1..49 dB.
 *
 * Below 0 is what the engine runs as automatic; above 49 is past every librtlsdr
 * tuner's top step (49.6 dB at most), so 49 picks the same step.
 */
int
sane_gain_db(int db) {
    return qBound(0, db, 49);
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

// Issue #800: the map's defaults (spec 4.4) and the age filters its pills offer, in seconds (0: all).
constexpr char kDefaultMapTileUrl[] = "https://tile.openstreetmap.org/{z}/{x}/{y}.png";
constexpr char kDefaultMapTileAttribution[] = "\xC2\xA9 OpenStreetMap contributors"; // UTF-8 copyright sign
constexpr int kMapAgeFilterChoices[] = {0, 900, 3600};
// How long a map view waits in memory after its last write before it goes to the file.
constexpr int kMapViewportWriteDelayMs = 1000;

/** @brief Whether @p seconds is one of the age filters the map offers. */
bool
is_map_age_filter(int seconds) {
    return std::any_of(std::begin(kMapAgeFilterChoices), std::end(kMapAgeFilterChoices),
                       [seconds](int choice) { return choice == seconds; });
}

/** @brief The text @p raw holds (a list the ini backend split at commas joined again), else empty. */
QString
stored_text(const QVariant& raw) {
    return as_scalar(raw).toString();
}

/*
 * A stored switch: a bool, or the text QSettings writes for one ("true"/"false", any case) or 1/0. Anything else, a
 * garbled or hand-edited value such as "off", is @p fallback: QVariant::toBool() would read every other non-empty
 * text as true, and switches like the map's tile opt-in must never turn on that way.
 */
bool
stored_flag(const QVariant& raw, bool fallback) {
    const QVariant value = as_scalar(raw);
    if (!value.isValid()) {
        return fallback;
    }
    if (value.typeId() == QMetaType::Bool) {
        return value.toBool();
    }
    const QString text = value.toString().trimmed().toLower();
    if (text == QLatin1String("true") || text == QLatin1String("1")) {
        return true;
    }
    if (text == QLatin1String("false") || text == QLatin1String("0")) {
        return false;
    }
    return fallback;
}

/*
 * The saved map view is stored as JSON whole numbers, latitude and longitude in 1e-7 degrees and the zoom in
 * thousandths, so a value read back is exactly the one written and compares without a tolerance.
 */
constexpr double kE7 = 1e7;
constexpr double kZoomE3 = 1e3;
// Rounded down, so the edge read back is still within the map's range.
const qint64 kMaxViewLatE7 = static_cast<qint64>(std::floor(map_math::kMaxLatitudeDeg * kE7));
constexpr qint64 kMaxViewLonE7 = 1800000000;
constexpr qint64 kMinViewZoomE3 = static_cast<qint64>(map_math::kMinZoom * kZoomE3);
constexpr qint64 kMaxViewZoomE3 = static_cast<qint64>(map_math::kMaxZoom * kZoomE3);

/** @brief One whole-number field of a stored view, if @p value is one within [@p low, @p high]. */
bool
view_field(const QJsonValue& value, qint64 low, qint64 high, qint64* out) {
    if (!value.isDouble()) {
        return false;
    }
    constexpr qint64 kNotWhole = std::numeric_limits<qint64>::min();
    const qint64 whole = value.toInteger(kNotWhole);
    if (whole == kNotWhole || whole < low || whole > high) {
        return false;
    }
    *out = whole;
    return true;
}

/** @brief The view the stored text @p text names, else {}: text that is no such JSON object reads as no view. */
QVariantMap
view_from_text(const QString& text) {
    const QJsonObject view = QJsonDocument::fromJson(text.toUtf8()).object();
    qint64 lat = 0;
    qint64 lon = 0;
    qint64 zoom = 0;
    if (!view_field(view.value(QLatin1String("latE7")), -kMaxViewLatE7, kMaxViewLatE7, &lat)
        || !view_field(view.value(QLatin1String("lonE7")), -kMaxViewLonE7, kMaxViewLonE7, &lon)
        || !view_field(view.value(QLatin1String("zoomE3")), kMinViewZoomE3, kMaxViewZoomE3, &zoom)) {
        return {};
    }
    return {{QStringLiteral("latitude"), static_cast<double>(lat) / kE7},
            {QStringLiteral("longitude"), static_cast<double>(lon) / kE7},
            {QStringLiteral("zoom"), static_cast<double>(zoom) / kZoomE3}};
}

/** @brief A finite number within [@p low, @p high] that @p value holds, checked before any conversion. */
bool
view_number(const QVariant& value, double low, double high, double* out) {
    bool ok = false;
    const double number = value.toDouble(&ok);
    if (!ok || !std::isfinite(number) || number < low || number > high) {
        return false;
    }
    *out = number;
    return true;
}

/** @brief The stored text for @p view: "" for {}, the JSON for a valid view; false when @p view is neither. */
bool
view_to_text(const QVariantMap& view, QString* text) {
    if (view.isEmpty()) {
        text->clear();
        return true;
    }
    double lat = 0;
    double lon = 0;
    double zoom = 0;
    if (!view_number(view.value(QStringLiteral("latitude")), -map_math::kMaxLatitudeDeg, map_math::kMaxLatitudeDeg,
                     &lat)
        || !view_number(view.value(QStringLiteral("longitude")), -180.0, 180.0, &lon)
        || !view_number(view.value(QStringLiteral("zoom")), map_math::kMinZoom, map_math::kMaxZoom, &zoom)) {
        return false;
    }
    const qint64 latE7 = std::clamp<qint64>(std::llround(lat * kE7), -kMaxViewLatE7, kMaxViewLatE7);
    const QJsonObject json{{QStringLiteral("latE7"), latE7},
                           {QStringLiteral("lonE7"), static_cast<qint64>(std::llround(lon * kE7))},
                           {QStringLiteral("zoomE3"), static_cast<qint64>(std::llround(zoom * kZoomE3))}};
    *text = QString::fromUtf8(QJsonDocument(json).toJson(QJsonDocument::Compact));
    return true;
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
    // A roaming distance never picked reads as the default of the unit shown, so the unit moves it.
    connect(this, &AppPrefs::metricUnitsChanged, this, &AppPrefs::roamingDistancesChanged);
    m_locationExpiry.setSingleShot(true);
    connect(&m_locationExpiry, &QTimer::timeout, this, [this]() {
        armLocationExpiry(); // Re-arm if the wall clock moved backwards before the timer fired.
        Q_EMIT locationChanged();
    });
    armLocationExpiry();
    // Issue #800: a map view goes to the file a second after its last write, or at once when the app leaves the
    // foreground (Android may end a background app before the timer runs) or this object goes.
    m_viewportWrite.setSingleShot(true);
    m_viewportWrite.setInterval(kMapViewportWriteDelayMs);
    connect(&m_viewportWrite, &QTimer::timeout, this, &AppPrefs::flushMapViewport);
    if (auto* app = qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
        connect(app, &QGuiApplication::applicationStateChanged, this, &AppPrefs::onApplicationStateChanged);
    }
}

AppPrefs::~AppPrefs() { flushMapViewport(); }

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
    (void)expireLocation();
    const qint64 at = m_settings.value(key_name(kLastFixAt), 0).toLongLong();
    if (at > 0) {
        const qint64 remaining = 24LL * 60 * 60 * 1000 - (realtimeMSecsSinceEpoch() - at);
        m_locationExpiry.start(static_cast<int>(qMax<qint64>(1, remaining)));
    }
}

// Location is a short-lived private hint. Delete the whole fix on expiry so
// stale coordinates cannot silently be reused or survive in a settings export.
bool
AppPrefs::expireLocation() const {
    const qint64 at = m_settings.value(key_name(kLastFixAt), 0).toLongLong();
    const qint64 now = realtimeMSecsSinceEpoch();
    if (at > 0 && at <= now && now - at < 24LL * 60 * 60 * 1000) {
        return false;
    }
    bool held = false;
    for (const Key key : {kLastLat, kLastLon, kLastFixAt, kLastAccuracyM}) {
        held = held || m_settings.contains(key_name(key));
        m_settings.remove(key_name(key));
    }
    return held;
}

// The expiry timer runs on the monotonic clock, which stops while a phone
// sleeps, so a fix can lapse first at a read. Every reader hears of it once,
// queued: a reader in the middle of its own rebuild is never re-entered.
void
AppPrefs::expireLocationOnRead() const {
    if (!expireLocation()) {
        return;
    }
    auto* self = const_cast<AppPrefs*>(this);
    QMetaObject::invokeMethod(
        self,
        [self]() {
            self->armLocationExpiry();
            Q_EMIT self->locationChanged();
        },
        Qt::QueuedConnection);
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
    return stored_flag(m_settings.value(key_name(kMetricUnits)), false);
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
    return stored_flag(m_settings.value(key_name(kOnboardingDone)), false);
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
    return stored_flag(m_settings.value(key_name(kNotificationExplained)), false);
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
    return stored_flag(m_settings.value(key_name(kBackgroundListening)), true);
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
    return stored_flag(m_settings.value(key_name(kLegacyKeepAwake)), false) ? ScreenAlwaysOn : ScreenSystem;
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
    return stored_flag(m_settings.value(key_name(kPersistTgLockouts)), true);
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
    return stored_flag(m_settings.value(key_name(kSkipEncrypted)), true);
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
    return stored_flag(m_settings.value(key_name(kAutoPpm)), false);
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
    return stored_flag(m_settings.value(key_name(kRelaxedCrc)), false);
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
    return sane_gain_db(stored_int_or(m_settings.value(key_name(kGainDb)), 30));
}

void
AppPrefs::setGainDb(int db) {
    const int next = sane_gain_db(db);
    if (!stored_int_differs(m_settings.value(key_name(kGainDb), 30), next)) {
        return;
    }
    m_settings.setValue(key_name(kGainDb), next);
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
    return stored_flag(m_settings.value(key_name(kBiasTee)), false);
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
AppPrefs::squelch() const {
    return sane_squelch(m_settings.value(key_name(kRadioSquelch)));
}

void
AppPrefs::setSquelch(const QString& setting) {
    if (!app_squelch_default_valid(setting)) {
        return;
    }
    // Compared with the raw stored value (absent reads as the default), so a write over a value that only reads as
    // the same setting still lands and repairs it.
    const QVariant stored = m_settings.value(key_name(kRadioSquelch), QString::fromLatin1(kAppSquelchDefault));
    if (stored.metaType().id() == QMetaType::QString && stored.toString() == setting) {
        return;
    }
    m_settings.setValue(key_name(kRadioSquelch), setting);
    Q_EMIT squelchChanged();
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
    return stored_flag(m_settings.value(key_name(kMonitorDetailsExpanded)), false);
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
    return stored_flag(m_settings.value(key_name(kChooseFilesInApp)), false);
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
AppPrefs::roamingEnabled() const {
    return stored_flag(m_settings.value(key_name(kRoamingEnabled)), false);
}

void
AppPrefs::setRoamingEnabled(bool on) {
    if (on == roamingEnabled()) {
        return;
    }
    m_settings.setValue(key_name(kRoamingEnabled), on);
    Q_EMIT roamingEnabledChanged();
}

double
AppPrefs::roamingMaxDistanceKm() const {
    return stored_roaming_km(roaming_max_choices(), m_settings.value(key_name(kRoamingMaxDistanceKm)), metricUnits());
}

void
AppPrefs::setRoamingMaxDistanceKm(double km) {
    setRoamingKm(kRoamingMaxDistanceKm, km);
}

double
AppPrefs::roamingMarginKm() const {
    return stored_roaming_km(roaming_margin_choices(), m_settings.value(key_name(kRoamingMarginKm)), metricUnits());
}

void
AppPrefs::setRoamingMarginKm(double km) {
    setRoamingKm(kRoamingMarginKm, km);
}

void
AppPrefs::setRoamingKm(int key, double km) {
    const RoamingChoices& choices = key == kRoamingMarginKm ? roaming_margin_choices() : roaming_max_choices();
    double offered = 0;
    if (!roaming_offered(choices, km, &offered)) {
        return;
    }
    // Compared with what is stored, not with what reads: picking the default writes it, so a later unit change keeps
    // the value the user picked.
    const QVariant stored = m_settings.value(key_name(static_cast<Key>(key)));
    bool ok = false;
    const double was = stored.toDouble(&ok);
    if (stored.isValid() && ok && std::fabs(was - offered) < kRoamingMatchKm) {
        return;
    }
    m_settings.setValue(key_name(static_cast<Key>(key)), offered);
    Q_EMIT roamingDistancesChanged();
}

QVariantList
AppPrefs::roamingMaxDistanceChoices() const {
    return roaming_unit_choices(roaming_max_choices(), metricUnits());
}

QVariantList
AppPrefs::roamingMarginChoices() const {
    return roaming_unit_choices(roaming_margin_choices(), metricUnits());
}

bool
AppPrefs::autoStartOnAttach() const {
    return stored_flag(m_settings.value(key_name(kAutoStartOnAttach)), false);
}

void
AppPrefs::setAutoStartOnAttach(bool value) {
    if (stored_flag(m_settings.value(key_name(kAutoStartOnAttach)), false) == value) {
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

QString
AppPrefs::acceptedStartSquelch() const {
    return m_settings.value(key_name(kAcceptedStartSquelch), QString()).toString();
}

void
AppPrefs::setAcceptedStartSquelch(const QString& value) {
    if (acceptedStartSquelch() == value) {
        return;
    }
    m_settings.setValue(key_name(kAcceptedStartSquelch), value);
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
    expireLocationOnRead();
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
    expireLocationOnRead();
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
    fixAtMs = std::min(fixAtMs, realtimeMSecsSinceEpoch());
    m_settings.setValue(key_name(kLastAccuracyM), accuracyM);
    m_settings.setValue(key_name(kLastLat), lat);
    m_settings.setValue(key_name(kLastLon), lon);
    m_settings.setValue(key_name(kLastFixAt), fixAtMs);
    armLocationExpiry();
    Q_EMIT locationChanged();
}

double
AppPrefs::lastAccuracyM() const {
    expireLocationOnRead();
    return m_settings.value(key_name(kLastAccuracyM), 0.0).toDouble();
}

qint64
AppPrefs::lastFixAt() const {
    expireLocationOnRead();
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
    return stored_flag(m_settings.value(key_name(kRecordCalls)), false);
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
    return stored_flag(m_settings.value(key_name(kRecordUnknownOnly)), false);
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

bool
AppPrefs::mapTilesEnabled() const {
    return stored_flag(m_settings.value(key_name(kMapTilesEnabled)), false);
}

void
AppPrefs::setMapTilesEnabled(bool on) {
    if (on == mapTilesEnabled()) {
        return;
    }
    m_settings.setValue(key_name(kMapTilesEnabled), on);
    Q_EMIT mapTilesChanged();
}

QString
AppPrefs::mapTileUrl() const {
    const QString url = stored_text(m_settings.value(key_name(kMapTileUrl)));
    return url.isEmpty() ? QString::fromUtf8(kDefaultMapTileUrl) : url;
}

void
AppPrefs::setMapTileUrl(const QString& url) {
    setMapText(kMapTileUrl, url);
}

QString
AppPrefs::mapTileAttribution() const {
    const QString text = stored_text(m_settings.value(key_name(kMapTileAttribution)));
    return text.isEmpty() ? QString::fromUtf8(kDefaultMapTileAttribution) : text;
}

void
AppPrefs::setMapTileAttribution(const QString& text) {
    setMapText(kMapTileAttribution, text);
}

void
AppPrefs::setMapText(int key, const QString& text) {
    // Compared with the raw stored value, so text over a stored list or a removal over a stored empty string lands.
    const QString name = key_name(static_cast<Key>(key));
    const QVariant stored = m_settings.value(name);
    if (text.isEmpty() ? !stored.isValid()
                       : stored.metaType().id() == QMetaType::QString && stored.toString() == text) {
        return;
    }
    if (text.isEmpty()) {
        m_settings.remove(name);
    } else {
        m_settings.setValue(name, text);
    }
    Q_EMIT mapTilesChanged();
}

int
AppPrefs::mapAgeFilter() const {
    int seconds = 0;
    return stored_int(m_settings.value(key_name(kMapAgeFilter)), &seconds) && is_map_age_filter(seconds) ? seconds : 0;
}

void
AppPrefs::setMapAgeFilter(int seconds) {
    if (!is_map_age_filter(seconds) || !stored_int_differs(m_settings.value(key_name(kMapAgeFilter), 0), seconds)) {
        return;
    }
    m_settings.setValue(key_name(kMapAgeFilter), seconds);
    Q_EMIT mapAgeFilterChanged();
}

QVariantMap
AppPrefs::mapViewport() const {
    if (m_viewportHeld) {
        return view_from_text(m_viewportText);
    }
    // Only text QSettings wrote reads back as one string; anything else (a hand-edited list) is no view.
    const QVariant stored = m_settings.value(key_name(kMapViewport));
    return stored.metaType().id() == QMetaType::QString ? view_from_text(stored.toString()) : QVariantMap();
}

void
AppPrefs::setMapViewport(const QVariantMap& view) {
    QString text;
    if (!view_to_text(view, &text)) {
        return;
    }
    const QVariant stored = m_settings.value(key_name(kMapViewport));
    const bool same = m_viewportHeld   ? m_viewportText == text
                      : text.isEmpty() ? !stored.isValid()
                                       : stored.metaType().id() == QMetaType::QString && stored.toString() == text;
    if (same) {
        return;
    }
    m_viewportText = std::move(text);
    m_viewportHeld = true;
    m_viewportWrite.start();
    Q_EMIT mapViewportChanged();
}

void
AppPrefs::flushMapViewport() {
    m_viewportWrite.stop();
    if (!m_viewportHeld) {
        return;
    }
    m_viewportHeld = false;
    if (m_viewportText.isEmpty()) {
        m_settings.remove(key_name(kMapViewport));
    } else {
        m_settings.setValue(key_name(kMapViewport), m_viewportText);
    }
}

void
AppPrefs::onApplicationStateChanged(Qt::ApplicationState state) {
    if (state == Qt::ApplicationActive || !m_viewportHeld) {
        return;
    }
    flushMapViewport();
    // QSettings writes the file at its next event, which a suspended app may never get.
    m_settings.sync();
}

} // namespace dsd_qt
