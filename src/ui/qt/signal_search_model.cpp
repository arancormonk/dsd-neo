// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <QByteArray>
#include <QChar>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLatin1String>
#include <QList>
#include <QStringList>
#include <QtGlobal>
#include <algorithm>
#include <cmath>
#include <dsd-neo/app_control/signal_search.h>
#include <dsd-neo/core/airspy_config.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/runtime/airspy_config.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/signal_search.h>
#include <iterator>
#include <stddef.h>
#include <utility>
#include <vector>
#include "decoder_host.h"
#include "json_store.h"
#include "known_frequency_index.h"
#include "signal_search_model.h"

namespace dsd_qt {
static QVariantMap
airspyMap(const dsd_airspy_config& a) {
    return {{QStringLiteral("serial"), QString::fromLatin1(a.serial)},
            {QStringLiteral("sample_rate"), a.sample_rate},
            {QStringLiteral("gain_mode"), QString::fromLatin1(a.gain_mode == 2   ? "manual"
                                                              : a.gain_mode == 1 ? "linearity"
                                                                                 : "sensitivity")},
            {QStringLiteral("sensitivity_gain"), a.sensitivity_gain},
            {QStringLiteral("linearity_gain"), a.linearity_gain},
            {QStringLiteral("lna_gain"), a.lna_gain},
            {QStringLiteral("mixer_gain"), a.mixer_gain},
            {QStringLiteral("vga_gain"), a.vga_gain},
            {QStringLiteral("lna_agc"), a.lna_agc},
            {QStringLiteral("mixer_agc"), a.mixer_agc},
            {QStringLiteral("bias_tee"), a.bias_tee}};
}

static QVariantMap
rangeMap(const dsd_signal_search_range& r) {
    return {{QStringLiteral("name"), QString::fromUtf8(r.name)},
            {QStringLiteral("startMHz"), QString::number(r.start_hz / 1e6, 'f', 6)},
            {QStringLiteral("endMHz"), QString::number(r.end_hz / 1e6, 'f', 6)},
            {QStringLiteral("stepNum"), r.step_num},
            {QStringLiteral("stepDen"), r.step_den},
            {QStringLiteral("bandwidth"), r.bandwidth_hz},
            {QStringLiteral("enabled"), r.enabled != 0},
            {QStringLiteral("am"), r.am != 0},
            {QStringLiteral("digital"), r.digital},
            {QStringLiteral("gainSet"), r.gain_set != 0},
            {QStringLiteral("gainAuto"), r.gain_auto != 0},
            {QStringLiteral("gain"), r.gain_tenth_db / 10.0},
            {QStringLiteral("sensitivity"), r.sensitivity_db},
            {QStringLiteral("minimumSet"), r.minimum_set != 0},
            {QStringLiteral("minimum"), r.minimum_dbfs},
            {QStringLiteral("squelchMode"), r.squelch_mode},
            {QStringLiteral("squelchMargin"), r.squelch_margin_db},
            {QStringLiteral("squelchLevel"), r.squelch_dbfs},
            {QStringLiteral("probe"), r.probe_ms / 1000},
            {QStringLiteral("visit"), r.max_visit_ms / 1000},
            {QStringLiteral("airspyGainSet"), r.airspy_gain_set != 0},
            {QStringLiteral("airspy"), airspyMap(r.airspy)}};
}

static bool
parseRangeNumbers(const QVariantMap& m, dsd_signal_search_range* r) {
    auto integer = [&](const char* key, int* value) -> bool {
        bool ok = false;
        double v = m.value(QString::fromLatin1(key)).toDouble(&ok);
        if (!ok || !std::isfinite(v) || v < INT32_MIN || v > INT32_MAX || std::abs(v - std::round(v)) > 1e-8) {
            return false;
        }
        *value = (int)v;
        return true;
    };
    int num = 0, den = 0, bw = 0, probe = 0, visit = 0;

    const struct {
        const char* key;
        int* value;
    } fields[] = {{"stepNum", &num},
                  {"stepDen", &den},
                  {"bandwidth", &bw},
                  {"digital", &r->digital},
                  {"sensitivity", &r->sensitivity_db},
                  {"minimum", &r->minimum_dbfs},
                  {"squelchMode", &r->squelch_mode},
                  {"squelchMargin", &r->squelch_margin_db},
                  {"squelchLevel", &r->squelch_dbfs},
                  {"probe", &probe},
                  {"visit", &visit}};

    if (!std::all_of(std::begin(fields), std::end(fields),
                     [&](const auto& field) { return integer(field.key, field.value); })) {
        return false;
    }
    if (num <= 0 || den <= 0 || bw <= 0 || probe < 2 || probe > 10 || visit < 0 || visit > 600) {
        return false;
    }
    r->step_num = (uint32_t)num;
    r->step_den = (uint32_t)den;
    r->bandwidth_hz = (uint32_t)bw;
    r->probe_ms = probe * 1000;
    r->max_visit_ms = visit * 1000;
    return true;
}

static bool
parseRangeFrequencies(const QVariantMap& m, dsd_signal_search_range* r) {
    for (const auto& key : {QStringLiteral("startMHz"), QStringLiteral("endMHz")}) {
        bool ok = false;
        double f = m.value(key).toDouble(&ok) * 1e6;
        if (!ok || !std::isfinite(f) || f < 1 || f > UINT32_MAX) {
            return false;
        }
        if (key == QStringLiteral("startMHz")) {
            r->start_hz = (uint32_t)std::llround(f);
        } else {
            r->end_hz = (uint32_t)std::llround(f);
        }
    }
    return true;
}

static QString
parseRange(const QVariantMap& m, dsd_signal_search_range* r) {
    dsd_signal_search_range_default(r);
    QByteArray name = m.value(QStringLiteral("name")).toString().trimmed().toUtf8();
    if (name.isEmpty() || name.size() >= 64 || name.contains('\n') || name.contains('\r')) {
        return QObject::tr("Use a range name of 1 to 63 UTF-8 bytes.");
    }
    DSD_SNPRINTF(r->name, sizeof(r->name), "%s", name.constData());
    if (!parseRangeFrequencies(m, r)) {
        return QObject::tr("Enter a valid start and end frequency in MHz.");
    }
    if (!parseRangeNumbers(m, r)) {
        return QObject::tr("Check the numeric range settings.");
    }
    r->enabled = m.value(QStringLiteral("enabled")).toBool();
    r->am = m.value(QStringLiteral("am")).toBool();
    r->gain_set = m.value(QStringLiteral("gainSet")).toBool();
    r->gain_auto = m.value(QStringLiteral("gainAuto")).toBool();
    bool ok = false;
    double gain = m.value(QStringLiteral("gain")).toDouble(&ok) * 10;
    if (!ok || !std::isfinite(gain) || gain < -100 || gain > 1000) {
        return QObject::tr("Enter a valid gain.");
    }
    r->gain_tenth_db = (int)std::llround(gain);
    r->minimum_set = m.value(QStringLiteral("minimumSet")).toBool();
    r->airspy_gain_set = m.value(QStringLiteral("airspyGainSet")).toBool();
    const auto airspy = m.value(QStringLiteral("airspy")).toMap();
    for (auto it = airspy.cbegin(); it != airspy.cend(); ++it) {
        if (dsd_airspy_config_set(&r->airspy, (QStringLiteral("airspy_") + it.key()).toLatin1().constData(),
                                  it.value().toString().toLatin1().constData())) {
            return QObject::tr("Check the Airspy gain settings.");
        }
    }
    char error[192];
    return dsd_signal_search_validate(r, error, sizeof(error)) ? QString::fromUtf8(error) : QString();
}

/* A range saved when a dynamic squelch took 0 to 2 dB ran with the 3 dB the receiver clamps those margins to: it
   loads with that margin rather than failing validation and being dropped. */
static QVariantMap
storedRange(QVariantMap m) {
    /* Only the whole numbers parseRangeNumbers() takes: a fraction or an out-of-range value is not rounded into one. */
    auto whole = [&](const QString& key, double* value) {
        bool ok = false;
        *value = m.value(key).toDouble(&ok);
        return ok && std::isfinite(*value) && std::abs(*value - std::round(*value)) <= 1e-8;
    };
    double mode = 0, margin = 0;
    const bool dynamic = whole(QStringLiteral("squelchMode"), &mode) && mode > -0.5 && mode < 2.5
                         && dsd_squelch_mode_is_dynamic((int)std::round(mode));
    if (dynamic && whole(QStringLiteral("squelchMargin"), &margin) && margin > -0.5
        && margin < DSD_SQUELCH_MARGIN_MIN_DB) {
        m[QStringLiteral("squelchMargin")] = (int)DSD_SQUELCH_MARGIN_MIN_DB;
    }
    return m;
}

SignalSearchModel::SignalSearchModel(DecoderHost* host, KnownFrequencyIndex* index, QObject* parent)
    : QObject(parent), m_host(host), m_index(index) {
    auto array = json_store_load_array(QStringLiteral("signal-search-ranges.json"));
    for (const auto& value : array) {
        dsd_signal_search_range r;
        if (parseRange(storedRange(value.toObject().toVariantMap()), &r).isEmpty()) {
            m_ranges.append(rangeMap(r));
        }
    }
    if (m_ranges.isEmpty()) {
        m_ranges.append(defaultRange());
    }
    QString path = json_store_path(QStringLiteral("signal-search-history.v1"));
    QDir().mkpath(QFileInfo(path).absolutePath());
    dsd_app_signal_search_store_open(path.toUtf8().constData());
    m_timer.setInterval(250);
    connect(&m_timer, &QTimer::timeout, this, [this] {
        if (armSessionUp() && m_host->sessionState() == DecoderHost::Running) {
            submitArm();
        }
        refresh();
    });
    if (m_index) {
        connect(m_index, &KnownFrequencyIndex::changed, this, [this] {
            m_revision = UINT64_MAX;
            m_historyRevision = UINT64_MAX;
            refresh();
        });
    }
    if (m_host) {
        connect(m_host, &DecoderHost::sessionStateChanged, this, &SignalSearchModel::followSession);
        /* Admission is open by then. The host can report it ahead of the phase it arrives with, while Starting. */
        connect(m_host, &DecoderHost::sessionInitialized, this, [this] {
            if (armSessionUp()) {
                submitArm();
            }
        });
    }
    m_timer.start();
    refresh();
}

/* An arm waits for one decoder session: the one up when it was made (a paused search's Resume), or else the next one
   to come up, since Signal search arms before it asks for the start, and a USB grant can hold that start while the host
   stays Idle. That session's end ends the arm, whether or not the start went in; it never carries into another. */
void
SignalSearchModel::followSession() {
    const DecoderHost::SessionState state = m_host->sessionState();
    if (state == DecoderHost::Starting || state == DecoderHost::Running) {
        m_armSessionSeen = m_armed;
        if (state == DecoderHost::Running) {
            submitArm();
        }
    } else if (state != DecoderHost::Idle || m_armSessionSeen) {
        disarm(); // Stopping or Failed, or Idle after the session came up
    }
}

bool
SignalSearchModel::armSessionUp() const {
    if (!m_armed || !m_armSessionSeen || m_host == nullptr) {
        return false;
    }
    const DecoderHost::SessionState state = m_host->sessionState();
    return state == DecoderHost::Starting || state == DecoderHost::Running;
}

/* Running does not mean the decoder takes commands yet. The Android engine thread reports Running a moment before its
   dsd_app_frontend_runtime_start() opens command admission (and a session winding down closes admission before Running
   drops), so a submission can be refused. A refused one queues nothing: the arm stays for the next chance, the host's
   initialization signal (which follows the admission edge) or a status tick while Running. An accepted one ends the
   arm, so the search is started once. */
void
SignalSearchModel::submitArm() {
    if (m_armed && dsd_app_signal_search_start(&m_pending) >= 0) {
        disarm();
    }
}

void
SignalSearchModel::disarm() {
    m_armed = false;
    m_armSessionSeen = false;
}

/* The foreground service can outlive the entire QML engine on Android. Its native
   journal worker stays alive until process shutdown; a replacement model reattaches. */
SignalSearchModel::~SignalSearchModel() { dsd_app_signal_search_store_flush(); }

QVariantMap
// cppcheck-suppress functionStatic -- Q_INVOKABLE entry point on the QML context object.
SignalSearchModel::defaultRange() const {
    dsd_signal_search_range r;
    dsd_signal_search_range_default(&r);
    return rangeMap(r);
}

bool
SignalSearchModel::persistRanges(const QVariantList& list) {
    QJsonArray array;
    for (const auto& r : list) {
        array.append(QJsonObject::fromVariantMap(r.toMap()));
    }
    if (!json_store_save_array(QStringLiteral("signal-search-ranges.json"), array)) {
        m_error = tr("Ranges could not be saved. Check available storage and try again.");
        Q_EMIT changed();
        return false;
    }
    m_ranges = list;
    Q_EMIT rangesChanged();
    return true;
}

QString
SignalSearchModel::saveRange(int row, const QVariantMap& value) {
    dsd_signal_search_range r;
    QString validationError = parseRange(value, &r);
    if (!validationError.isEmpty()) {
        return validationError;
    }
    if (row < 0 && m_ranges.size() >= DSD_SEARCH_MAX_RANGES) {
        return tr("Up to 16 ranges are supported.");
    }
    auto list = m_ranges;
    if (row >= 0 && row < list.size()) {
        list[row] = rangeMap(r);
    } else if (row == -1) {
        list.append(rangeMap(r));
    } else {
        return tr("This range is no longer available.");
    }
    return persistRanges(list) ? QString() : m_error;
}

void
SignalSearchModel::removeRange(int row) {
    if (row < 0 || row >= m_ranges.size()) {
        return;
    }
    auto list = m_ranges;
    list.removeAt(row);
    persistRanges(list);
}

void
SignalSearchModel::moveRange(int row, int direction) {
    int to = row + direction;
    if (row < 0 || row >= m_ranges.size() || to < 0 || to >= m_ranges.size()) {
        return;
    }
    auto list = m_ranges;
    list.move(row, to);
    persistRanges(list);
}

QString
SignalSearchModel::arm() {
    /* Each attempt replaces the last, and a refused one leaves nothing armed: m_pending is rebuilt as it is checked, so
       an earlier arm left standing would start a half-built configuration when the session comes up. */
    disarm();
    if (m_ranges.isEmpty()) {
        return tr("Add a range before searching.");
    }
    m_pending = {};
    /* A Resume restarts the paused search under its own session, so its hits keep numbering on. Only while that search
       still holds the tuner in the running decoder session: m_status can be a status tick old, and a start after a Stop
       that reused the ID would number its hits from 1 again and overwrite that session's history. */
    dsd_signal_search_status live{};
    dsd_app_signal_search_status_get(&live, nullptr, 0);
    const uint64_t newest = std::max(m_session, live.session_id);
    m_pending.session_id = (uint64_t)(dsd_realtime_now_s() * 1000000);
    if (m_pending.session_id <= newest) {
        m_pending.session_id = newest + 1;
    }
    if (live.phase == DSD_SEARCH_PAUSED && live.owned && m_host != nullptr
        && m_host->sessionState() == DecoderHost::Running) {
        m_pending.session_id = live.session_id;
    }
    int enabled = 0;
    for (const auto& v : m_ranges) {
        if (m_pending.range_count >= DSD_SEARCH_MAX_RANGES) {
            return tr("Up to 16 ranges are supported.");
        }
        auto& r = m_pending.ranges[m_pending.range_count];
        QString validationError = parseRange(v.toMap(), &r);
        if (!validationError.isEmpty()) {
            return validationError;
        }
        enabled += r.enabled;
        ++m_pending.range_count;
    }
    if (!enabled) {
        return tr("Enable at least one range.");
    }
    m_pending.avoid_count = (uint32_t)dsd_app_signal_search_avoids_get(m_pending.avoids, DSD_SEARCH_MAX_AVOIDS);
    /* The engine's own aggregate rules, so a start it would reject is never submitted (the channel total included). */
    char configError[192];
    if (dsd_signal_search_validate_config(&m_pending, configError, sizeof(configError)) != 0) {
        return QString::fromUtf8(configError);
    }
    m_armed = true;
    m_armSessionSeen = m_host != nullptr && m_host->sessionActive();
    m_error.clear();
    if (m_host != nullptr && m_host->sessionState() == DecoderHost::Running) {
        submitArm();
    }
    return {};
}

void
SignalSearchModel::cancelArm() {
    disarm();
}

bool
SignalSearchModel::control(int action, const QString& session, const QString& hit) {
    dsd_signal_search_control c{};
    c.action = action;
    c.session_id = session.toULongLong();
    c.hit_id = hit.toULongLong();
    if (dsd_app_signal_search_control(&c) < 0) {
        m_error = tr("The decoder did not accept this action.");
        Q_EMIT changed();
        return false;
    }
    return true;
}

bool
SignalSearchModel::unavoid(uint32_t frequency) {
    dsd_signal_search_control c{};
    c.action = DSD_SEARCH_ACTION_UNAVOID;
    c.session_id = m_session;
    c.frequency_hz = frequency;
    if (dsd_app_signal_search_control(&c) < 0) {
        m_error = tr("The decoder did not accept this action.");
        Q_EMIT changed();
        return false;
    }
    return true;
}

void
SignalSearchModel::removeAvoid(double frequency) {
    if (std::isfinite(frequency) && frequency > 0 && frequency <= UINT32_MAX) {
        dsd_app_signal_search_avoid_remove((uint32_t)frequency);
        /* A running session loaded this avoid when it started; lift it there too. */
        if (m_owned) {
            (void)unavoid((uint32_t)frequency);
        }
        refresh();
    }
}

bool
SignalSearchModel::removeSessionAvoid(double frequency) {
    if (!m_owned || !std::isfinite(frequency) || frequency <= 0 || frequency > UINT32_MAX) {
        return false;
    }
    return unavoid((uint32_t)frequency);
}

QVariantMap
SignalSearchModel::hitMap(const dsd_signal_search_hit& h) const {
    double width = h.bandwidth_hz;
    if (h.step_num && h.step_den) {
        width = std::min(width, (double)h.step_num / h.step_den);
    }
    auto matches = m_index ? m_index->lookup(h.frequency_hz, width) : QVariantList();
    QStringList labels;
    for (const auto& m : matches) {
        labels.append(m.toMap().value(QStringLiteral("name")).toString());
    }
    return {{QStringLiteral("session"), QString::number(h.session_id)},
            {QStringLiteral("id"), QString::number(h.hit_id)},
            {QStringLiteral("frequency"), h.frequency_hz},
            {QStringLiteral("freqMHz"), QString::number(h.frequency_hz / 1e6, 'f', 6)},
            {QStringLiteral("width"), h.bandwidth_hz},
            {QStringLiteral("power"), h.power_dbfs},
            {QStringLiteral("noise"), h.noise_dbfs},
            {QStringLiteral("confirmed"), h.confirmed != 0},
            {QStringLiteral("ended"), h.ended != 0},
            {QStringLiteral("unresolved"), h.unresolved != 0},
            {QStringLiteral("am"), h.am != 0},
            {QStringLiteral("protocol"), QString::fromUtf8(h.protocol)},
            {QStringLiteral("access"), QString::fromUtf8(h.access)},
            {QStringLiteral("tone"), QString::fromUtf8(h.tone)},
            {QStringLiteral("target"), h.target},
            {QStringLiteral("source"), h.source},
            {QStringLiteral("started"), (qulonglong)h.started_ms},
            {QStringLiteral("targetText"), QString::fromUtf8(h.target_text)},
            {QStringLiteral("sourceText"), QString::fromUtf8(h.source_text)},
            {QStringLiteral("inputSpec"), QString::fromUtf8(h.input_spec)},
            {QStringLiteral("ppm"), h.ppm},
            {QStringLiteral("biasTee"), h.bias_tee != 0},
            {QStringLiteral("airspy"), airspyMap(h.airspy)},
            {QStringLiteral("range"), QString::fromUtf8(h.range_name)},
            {QStringLiteral("gain"), h.gain_tenth_db / 10.0},
            {QStringLiteral("gainAuto"), h.gain_auto != 0},
            {QStringLiteral("matches"), matches},
            {QStringLiteral("knownLabel"), labels.join(QStringLiteral(" / "))}};
}

static QVariantList
frequencyList(size_t (*get)(uint32_t*, size_t)) {
    std::vector<uint32_t> frequencies(DSD_SEARCH_MAX_AVOIDS);
    const size_t n = get(frequencies.data(), frequencies.size());
    QVariantList list;
    for (size_t i = 0; i < n; ++i) {
        list.append(frequencies[i]);
    }
    return list;
}

bool
SignalSearchModel::refreshAvoids() {
    const uint64_t revision = dsd_app_signal_search_avoids_revision();
    if (revision == m_avoidsRevision) {
        return false;
    }
    m_avoidsRevision = revision;
    m_avoids = frequencyList(dsd_app_signal_search_avoids_get);
    m_sessionAvoids = frequencyList(dsd_app_signal_search_session_avoids_get);
    return true;
}

/* History is copied and converted only when the store changed it (or the saved-frequency labels did), not on every
   status revision: a search publishes those several times a second, the history far less often. */
bool
SignalSearchModel::refreshHistory() {
    if (dsd_app_signal_search_history_revision() == m_historyRevision) {
        return false;
    }
    std::vector<dsd_signal_search_hit> hits(DSD_SEARCH_HISTORY_LIMIT);
    const size_t n = dsd_app_signal_search_history_snapshot(hits.data(), hits.size(), &m_historyRevision);
    m_history.clear();
    for (size_t i = 0; i < n; ++i) {
        m_history.append(hitMap(hits[i]));
    }
    return true;
}

void
SignalSearchModel::refresh() {
    if (refreshAvoids()) {
        Q_EMIT avoidsChanged();
    }
    if (refreshHistory()) {
        Q_EMIT historyChanged();
    }
    dsd_signal_search_status s{};
    char errorText[256];
    dsd_app_signal_search_status_get(&s, errorText, sizeof(errorText));
    const QString persistenceError = QString::fromUtf8(errorText);
    bool changedError = persistenceError != m_persistenceError;
    m_persistenceError = persistenceError;
    if (s.revision == m_revision && s.session_id == m_session && !changedError) {
        return;
    }
    m_revision = s.revision;
    m_session = s.session_id;
    m_owned = s.owned != 0;
    m_status = {{QStringLiteral("phase"), (int)s.phase},
                {QStringLiteral("owned"), s.owned != 0},
                {QStringLiteral("identifying"), s.identifying != 0},
                {QStringLiteral("session"), QString::number(s.session_id)},
                {QStringLiteral("pass"), (qulonglong)s.pass},
                {QStringLiteral("range"), s.range_index},
                {QStringLiteral("channel"), s.channel_index},
                {QStringLiteral("count"), s.channel_count},
                {QStringLiteral("livePowerValid"), s.live_power_valid != 0},
                {QStringLiteral("livePower"), s.live_power_dbfs},
                {QStringLiteral("message"), QString::fromUtf8(s.message)},
                {QStringLiteral("hit"), hitMap(s.hit)}};
    Q_EMIT changed();
}

static QString
confirmedDecodeFlag(const QString& protocol) {
    static const QHash<QString, QString> flags{
        {QStringLiteral("P25"), QStringLiteral("-ft")},    {QStringLiteral("DMR"), QStringLiteral("-fs")},
        {QStringLiteral("NXDN"), QStringLiteral("-fa")},   {QStringLiteral("D-STAR"), QStringLiteral("-fd")},
        {QStringLiteral("NXDN48"), QStringLiteral("-fi")}, {QStringLiteral("NXDN96"), QStringLiteral("-fn")},
        {QStringLiteral("dPMR"), QStringLiteral("-fm")},   {QStringLiteral("M17"), QStringLiteral("-fz")},
        {QStringLiteral("YSF"), QStringLiteral("-fy")},    {QStringLiteral("ProVoice"), QStringLiteral("-fp")}};
    return flags.value(protocol);
}

/* A detection's tuner gain as the saved system's gainDb: the whole-dB override a saved session starts with, the -i
   spec's gain field (session_args_build()). There 0 is the tuner's AGC and a manual gain is 1 to 49 dB, so AGC stays 0
   and a manual gain is rounded to the nearest whole dB in that range. A manual 0 or 0.9 dB therefore stays manual, at
   1 dB, rather than becoming AGC, and no fraction is truncated. A detection without a gain leaves the app default. */
static int
savedGainDb(const QVariantMap& hit) {
    if (hit.value(QStringLiteral("gainAuto")).toBool()) {
        return 0;
    }
    bool ok = false;
    const double db = hit.value(QStringLiteral("gain")).toDouble(&ok);
    if (!ok || !std::isfinite(db)) {
        return -1;
    }
    return (int)std::lround(std::clamp(db, 1.0, 49.0));
}

QVariantMap
SignalSearchModel::systemForHit(const QVariantMap& hit, const QVariantMap& source) const {
    QVariantMap result = source;
    QString spec = hit.value(QStringLiteral("inputSpec")).toString();
    if (!spec.isEmpty()) {
        result.clear();
        const bool airspy = spec.startsWith(QLatin1String("airspy"));
        result[QStringLiteral("sourceType")] = airspy                                     ? QStringLiteral("airspy")
                                               : spec.startsWith(QLatin1String("rtltcp")) ? QStringLiteral("rtltcp")
                                                                                          : QStringLiteral("usb");
        if (spec.startsWith(QLatin1String("rtltcp:"))) {
            const auto parts = spec.split(QLatin1Char(':'));
            result[QStringLiteral("host")] = parts.value(1);
            result[QStringLiteral("port")] = parts.value(2).toInt();
        }
        /* The receiver settings the detection was made with, in the saved-system shape the wizard copies: an Airspy's
           native map (its own bias tee lives there; it takes no PPM), or an RTL's PPM text and bias tee. The tee is
           explicit either way, so a later app-wide default can neither power nor unpower this antenna. */
        if (airspy) {
            result[QStringLiteral("airspy")] = hit.value(QStringLiteral("airspy"));
        } else {
            result[QStringLiteral("ppm")] = QString::number(hit.value(QStringLiteral("ppm")).toInt());
            result[QStringLiteral("biasTee")] = hit.value(QStringLiteral("biasTee")).toBool() ? 1 : 0;
        }
        result[QStringLiteral("bandwidthKhz")] = 48;
    }
    result[QStringLiteral("freqMhz")] = hit.value(QStringLiteral("freqMHz"));
    QString flag = hit.value(QStringLiteral("confirmed")).toBool()
                       ? confirmedDecodeFlag(hit.value(QStringLiteral("protocol")).toString())
                       : (hit.value(QStringLiteral("am")).toBool() ? QStringLiteral("-fM") : QStringLiteral("-fA"));
    result[QStringLiteral("decodeFlag")] = flag;
    /* An Airspy's gains are its native map's; the RTL tuner gain the detection records does not describe it. */
    result[QStringLiteral("gainDb")] =
        result.value(QStringLiteral("sourceType")).toString() == QLatin1String("airspy") ? -1 : savedGainDb(hit);
    result[QStringLiteral("trunking")] = false;
    if (!hit.value(QStringLiteral("confirmed")).toBool()) {
        result[QStringLiteral("extraArgs")] =
            QStringLiteral("%1 %2")
                .arg(hit.value(QStringLiteral("am")).toBool() ? QStringLiteral("--am-bandwidth-hz")
                                                              : QStringLiteral("--nfm-bandwidth-hz"))
                .arg(hit.value(QStringLiteral("width")).toUInt());
    }
    result[QStringLiteral("searchUnresolved")] = hit.value(QStringLiteral("unresolved"));
    return result;
}
} // namespace dsd_qt
