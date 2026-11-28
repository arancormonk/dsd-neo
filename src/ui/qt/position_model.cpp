// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "position_model.h"

#include "position_kind_text.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QGuiApplication>
#include <QLatin1String>
#include <QMap>
#include <QString>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <dsd-neo/app_control/position_view.h>
#include <dsd-neo/core/position.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <iterator>
#include <utility>

namespace dsd_qt {

namespace {

/* Records one read copies: a page of about 47 KB, read until the view has no more. */
constexpr size_t kPage = 256;

qint64
system_wall_seconds() {
    // Real time: a SYSTEM fix was stamped with dsd_decode_time() on the SYSTEM source, which reads the real wall clock.
    return static_cast<qint64>(dsd_realtime_time());
}

QString
fixed_text(const char* text, size_t size) {
    const void* end = std::memchr(text, '\0', size);
    const auto length = end != nullptr ? static_cast<const char*>(end) - text : static_cast<std::ptrdiff_t>(size);
    return QString::fromUtf8(text, static_cast<qsizetype>(length));
}

QString
time_text(qint64 seconds) {
    return seconds > 0 ? QDateTime::fromSecsSinceEpoch(seconds).toString(QStringLiteral("HH:mm:ss")) : QString();
}

bool
anonymous_fix(const dsd_position_fix& fix) {
    return (fix.flags & (DSD_POS_FLAG_SOURCE_HASHED | DSD_POS_FLAG_SOURCE_UNVERIFIED)) != 0U
           || (fix.source_id == 0U && fix.source_text[0] == '\0');
}

/* The typed source (D7): the protocol family, the sender scope with its identity discriminator, the radio ID and the
 * callsign. Two reports are one track exactly when these agree (dsd_position_same_track()). */
QByteArray
track_key(const dsd_position_fix& fix) {
    QByteArray key;
    key.reserve(80);
    key += QByteArray::number(dsd_position_protocol_family(fix.protocol));
    key += ':';
    key += QByteArray::number(fix.sender_scope.kind);
    key += ':';
    key += QByteArray::number(fix.sender_scope.a);
    key += ':';
    key += QByteArray::number(fix.sender_scope.b);
    key += ':';
    key += QByteArray::number(static_cast<qlonglong>(fix.identity_freq_hz));
    key += ':';
    key += QByteArray::number(fix.source_id);
    key += ':';
    const void* end = std::memchr(fix.source_text, '\0', sizeof fix.source_text);
    key.append(fix.source_text, end != nullptr ? static_cast<const char*>(end) - fix.source_text
                                               : static_cast<qsizetype>(sizeof fix.source_text));
    return key;
}

/* Now on the clock @p fix was received on, into @p now; false when that clock cannot be read now. A SYSTEM fix ages
 * on the real wall clock; a REPLAY fix only while the sample names its run on the replay clock, running. */
bool
clock_now(const dsd_position_fix& fix, const dsd_app_position_sample& sample, qint64 wall_now, qint64* now) {
    if (fix.rx_clock == DSD_DECODE_CLOCK_SYSTEM) {
        *now = wall_now;
        return true;
    }
    if (fix.rx_clock == DSD_DECODE_CLOCK_REPLAY && sample.clock_source == DSD_DECODE_CLOCK_REPLAY
        && sample.running != 0U && sample.stream_id == fix.stream_id) {
        *now = sample.decode_now_s;
        return true;
    }
    return false;
}

/* Seconds since @p at (a receive stamp) on the clock @p fix was received on, or -1 when that clock cannot be read
 * now. */
qint64
age_since(const dsd_position_fix& fix, qint64 at, const dsd_app_position_sample& sample, qint64 wall_now) {
    qint64 now = 0;
    if (!clock_now(fix, sample, wall_now, &now)) {
        return -1;
    }
    return now > at ? now - at : 0;
}

/* How far a measurement may lie ahead of the receive clock and still be "now": a radio stamping GPS time against a
 * phone clock a fraction of a second behind rounds to a second ahead now and then. */
constexpr qint64 kMeasuredSkewS = 2;

/* Seconds since the report's own measurement time (fix_time_s, the radio's UTC), or -1. Unlike a receive stamp it
 * comes from another clock: one clearly ahead of the receive clock (a radio clock well ahead, a replay whose clock
 * has no UTC anchor and starts in 2000) cannot be compared, and is never "just now": no age, so the time itself is
 * shown. Within kMeasuredSkewS ahead it is age 0. */
qint64
measured_age(const dsd_position_fix& fix, const dsd_app_position_sample& sample, qint64 wall_now) {
    qint64 now = 0;
    if (fix.fix_time_s == 0 || !clock_now(fix, sample, wall_now, &now) || now < fix.fix_time_s - kMeasuredSkewS) {
        return -1;
    }
    return now > fix.fix_time_s ? now - fix.fix_time_s : 0;
}

QVariant
degrees(qint32 e7) {
    return static_cast<double>(e7) / 1e7;
}

/* Whether the sample's run, clock or running flag changed between two reads: every row's age is read again then. Its
 * decode time alone moving changes nothing until the map's next age tick. */
bool
sample_moved(const dsd_app_position_sample& before, const dsd_app_position_sample& after) {
    return before.stream_id != after.stream_id || before.clock_source != after.clock_source
           || before.running != after.running;
}

const QList<int>&
age_roles() {
    static const QList<int> roles = {PositionModel::AgeSecondsRole,       PositionModel::UnknownAgeSecondsRole,
                                     PositionModel::ReportAgeSecondsRole, PositionModel::ShownRole,
                                     PositionModel::CurrentShownRole,     PositionModel::UnknownShownRole};
    return roles;
}

} // namespace

/* What one poll or handoff changed. */
struct PositionModel::Ingest {
    QVector<quint64> touched; /* tracks that took a report */
    bool changed = false;     /* rows, trails or positions changed */
    bool sampleChanged = false;
};

PositionModel::PositionModel(QObject* parent)
    : QAbstractListModel(parent), m_source(&dsd_app_position_copy_since), m_wallClock(&system_wall_seconds),
      m_tap(std::make_unique<PositionTap>(m_source)), m_page(new dsd_position_fix[kPage]) {
    m_ageTimer.setInterval(kAgeTickMs);
    connect(&m_ageTimer, &QTimer::timeout, this, &PositionModel::refreshAges);
    if (QGuiApplication* app = qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
        connect(app, &QGuiApplication::applicationStateChanged, this, &PositionModel::onApplicationStateChanged);
    }
}

PositionModel::~PositionModel() = default;

int
PositionModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

QVariant
PositionModel::gapValue(const Row& row, int role) {
    switch (role) {
        case TrackIdRole: return 0;
        case GapRole:
        case ShownRole: return true;
        case TimeTextRole: return row.gapTimeKnown ? time_text(row.gapTimeS) : QString();
        case ReceivedTimeRole: return row.gapTimeKnown ? QVariant(static_cast<double>(row.gapTimeS)) : QVariant();
        case ReceivedClockRole: return row.gapTimeKnown ? QVariant(row.gapClock) : QVariant();
        default: return {};
    }
}

namespace {

using Row = PositionModel::Row;

QVariant
known(bool is_known, const QVariant& v) {
    return is_known ? v : QVariant();
}

/* The roles that are one flag of the newest report: @p out gets the flag; @return whether @p role is one. */
bool
flag_value(int role, quint16 flags, QVariant* out) {
    struct FlagRole {
        int role = 0;
        quint16 flag = 0;
    };

    static const FlagRole kFlagRoles[] = {
        {PositionModel::SourceHashedRole, DSD_POS_FLAG_SOURCE_HASHED},
        {PositionModel::SourceUnverifiedRole, DSD_POS_FLAG_SOURCE_UNVERIFIED},
        {PositionModel::TargetGroupRole, DSD_POS_FLAG_TARGET_GROUP},
        {PositionModel::ValidityUnknownRole, DSD_POS_FLAG_VALIDITY_UNKNOWN},
        {PositionModel::ProvisionalRole, DSD_POS_FLAG_FORMAT_PROVISIONAL},
        {PositionModel::WeakCheckRole, DSD_POS_FLAG_WEAK_CHECK},
        {PositionModel::EmergencyRole, DSD_POS_FLAG_EMERGENCY},
        {PositionModel::ErrorLowerBoundRole, DSD_POS_FLAG_ERR_LOWER_BOUND},
        {PositionModel::SpeedLowerBoundRole, DSD_POS_FLAG_SPEED_LOWER_BOUND},
        {PositionModel::HeadingAmbiguousRole, DSD_POS_FLAG_HEADING_AMBIGUOUS},
    };

    const auto* end = std::end(kFlagRoles);
    const auto* entry = std::find_if(std::begin(kFlagRoles), end, [role](const FlagRole& f) { return f.role == role; });
    if (entry == end) {
        return false;
    }
    *out = (flags & entry->flag) != 0U;
    return true;
}

QVariant
identity_value(const Row& row, int role) {
    const dsd_position_fix& fix = row.latest();
    switch (role) {
        case PositionModel::TrackIdRole: return row.id;
        case PositionModel::GapRole: return false;
        case PositionModel::AnonymousRole: return row.anonymous;
        case PositionModel::SourceIdRole: return known(fix.source_id != 0U, fix.source_id);
        case PositionModel::SourceTextRole: return fixed_text(fix.source_text, sizeof fix.source_text);
        case PositionModel::LabelRole: return row.label;
        case PositionModel::TargetIdRole: return known(fix.target_id != 0U, fix.target_id);
        case PositionModel::ProtocolRole: return fix.protocol;
        case PositionModel::KindRole: return fix.kind;
        case PositionModel::KindTextRole: return position_kind_text(fix.kind);
        default: return {};
    }
}

QVariant
position_value(const Row& row, int role) {
    const dsd_position_fix& fix = row.latest();
    switch (role) {
        case PositionModel::HasPositionRole: return row.hasCurrent;
        case PositionModel::LatitudeRole: return known(row.hasCurrent, degrees(row.current.lat_e7));
        case PositionModel::LongitudeRole: return known(row.hasCurrent, degrees(row.current.lon_e7));
        case PositionModel::HasUnknownFixRole: return row.hasUnknown;
        case PositionModel::UnknownLatitudeRole: return known(row.hasUnknown, degrees(row.unknown.lat_e7));
        case PositionModel::UnknownLongitudeRole: return known(row.hasUnknown, degrees(row.unknown.lon_e7));
        case PositionModel::AltitudeDmRole: return known(fix.alt_dm != DSD_POSITION_ALT_UNKNOWN, fix.alt_dm);
        case PositionModel::ErrorMRole: return known(fix.err_m != DSD_POSITION_ERR_UNKNOWN, fix.err_m);
        case PositionModel::SpeedDkmhRole: return known(fix.speed_dkmh != DSD_POSITION_SPEED_UNKNOWN, fix.speed_dkmh);
        case PositionModel::HeadingDdegRole:
            return known(fix.heading_ddeg != DSD_POSITION_HEADING_UNKNOWN, fix.heading_ddeg);
        case PositionModel::SatsRole: return known(fix.sats != DSD_POSITION_SATS_NONE, fix.sats);
        case PositionModel::FreqHzRole: return known(fix.freq_hz > 0, static_cast<double>(fix.freq_hz));
        default: return {};
    }
}

QVariant
time_value(const Row& row, int role) {
    const dsd_position_fix& fix = row.latest();
    switch (role) {
        case PositionModel::FixTimeRole: return known(fix.fix_time_s != 0, static_cast<double>(fix.fix_time_s));
        case PositionModel::ReceivedTimeRole: return static_cast<double>(fix.rx_time_s);
        case PositionModel::ReceivedClockRole: return fix.rx_clock;
        case PositionModel::TimeTextRole: return time_text(fix.rx_time_s);
        case PositionModel::AgeSecondsRole: return row.ageS;
        case PositionModel::UnknownAgeSecondsRole: return row.unknownAgeS;
        case PositionModel::ReportAgeSecondsRole: return row.reportAgeS;
        case PositionModel::ShownRole: return row.currentShown || row.unknownShown;
        case PositionModel::PointCountRole: return static_cast<int>(row.trail.size());
        default: return {};
    }
}

/* @p value into @p out under @p key when @p is_known. */
void
put_known(QVariantMap& out, const char* key, bool is_known, const QVariant& value) {
    if (is_known) {
        out.insert(QLatin1String(key), value);
    }
}

/* One report's own flags and measured fields (CurrentFixRole, UnknownFixRole): what its mark on the canvas is drawn
 * from, so the sheet and the List say exactly that (spec 4.6). @p newest: it is the track's newest report. */
QVariantMap
fix_record(const dsd_position_fix& fix, bool newest) {
    QVariantMap out;
    const auto flag = [&fix](unsigned bit) { return (fix.flags & bit) != 0U; };
    out.insert(QStringLiteral("latitude"), degrees(fix.lat_e7));
    out.insert(QStringLiteral("longitude"), degrees(fix.lon_e7));
    out.insert(QStringLiteral("kindText"), position_kind_text(fix.kind));
    put_known(out, "targetId", fix.target_id != 0U, static_cast<double>(fix.target_id));
    out.insert(QStringLiteral("targetGroup"), flag(DSD_POS_FLAG_TARGET_GROUP));
    out.insert(QStringLiteral("weakCheck"), flag(DSD_POS_FLAG_WEAK_CHECK));
    out.insert(QStringLiteral("provisional"), flag(DSD_POS_FLAG_FORMAT_PROVISIONAL));
    out.insert(QStringLiteral("validityUnknown"), flag(DSD_POS_FLAG_VALIDITY_UNKNOWN));
    out.insert(QStringLiteral("sourceHashed"), flag(DSD_POS_FLAG_SOURCE_HASHED));
    out.insert(QStringLiteral("sourceUnverified"), flag(DSD_POS_FLAG_SOURCE_UNVERIFIED));
    out.insert(QStringLiteral("errorLowerBound"), flag(DSD_POS_FLAG_ERR_LOWER_BOUND));
    out.insert(QStringLiteral("speedLowerBound"), flag(DSD_POS_FLAG_SPEED_LOWER_BOUND));
    out.insert(QStringLiteral("headingAmbiguous"), flag(DSD_POS_FLAG_HEADING_AMBIGUOUS));
    put_known(out, "errorM", fix.err_m != DSD_POSITION_ERR_UNKNOWN, fix.err_m);
    put_known(out, "speedDkmh", fix.speed_dkmh != DSD_POSITION_SPEED_UNKNOWN, fix.speed_dkmh);
    put_known(out, "headingDdeg", fix.heading_ddeg != DSD_POSITION_HEADING_UNKNOWN, fix.heading_ddeg);
    put_known(out, "altitudeDm", fix.alt_dm != DSD_POSITION_ALT_UNKNOWN, fix.alt_dm);
    put_known(out, "sats", fix.sats != DSD_POSITION_SATS_NONE, fix.sats);
    put_known(out, "freqHz", fix.freq_hz > 0, static_cast<double>(fix.freq_hz));
    put_known(out, "fixTime", fix.fix_time_s != 0, static_cast<double>(fix.fix_time_s));
    // LIP's time elapsed (TS 100 392-18-1 Table 6.111): how old the position was when sent, at most, when the report
    // carries no time of its own. Codes 0..2: under 5 s, 5 min, 30 min; 3 (not known) and none: absent.
    static const int kAgeAtSendMaxS[3] = {5, 300, 1800};
    put_known(out, "ageAtSendMaxSeconds", fix.age_class < 3U, fix.age_class < 3U ? kAgeAtSendMaxS[fix.age_class] : 0);
    out.insert(QStringLiteral("receivedTime"), static_cast<double>(fix.rx_time_s));
    out.insert(QStringLiteral("timeText"), time_text(fix.rx_time_s));
    out.insert(QStringLiteral("newest"), newest);
    return out;
}

} // namespace

QVariant
PositionModel::value(const Row& row, int role) {
    if (row.gap) {
        return gapValue(row, role);
    }
    if (role == CurrentFixRole) {
        return row.hasCurrent ? fix_record(row.current, !row.latestIsUnknown) : QVariantMap();
    }
    if (role == UnknownFixRole) {
        return row.hasUnknown ? fix_record(row.unknown, row.latestIsUnknown) : QVariantMap();
    }
    if (role == CurrentShownRole || role == UnknownShownRole) {
        return role == CurrentShownRole ? row.hasCurrent && row.currentShown : row.hasUnknown && row.unknownShown;
    }
    QVariant flag;
    if (flag_value(role, row.latest().flags, &flag)) {
        return flag;
    }
    if (role <= KindTextRole) {
        return identity_value(row, role);
    }
    return role <= FreqHzRole ? position_value(row, role) : time_value(row, role);
}

QVariant
PositionModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) {
        return {};
    }
    return value(m_rows.at(index.row()), role);
}

QHash<int, QByteArray>
PositionModel::roleNames() const {
    return {{TrackIdRole, QByteArrayLiteral("trackId")},
            {GapRole, QByteArrayLiteral("gap")},
            {AnonymousRole, QByteArrayLiteral("anonymous")},
            {SourceHashedRole, QByteArrayLiteral("sourceHashed")},
            {SourceUnverifiedRole, QByteArrayLiteral("sourceUnverified")},
            {SourceIdRole, QByteArrayLiteral("sourceId")},
            {SourceTextRole, QByteArrayLiteral("sourceText")},
            {LabelRole, QByteArrayLiteral("label")},
            {TargetIdRole, QByteArrayLiteral("targetId")},
            {TargetGroupRole, QByteArrayLiteral("targetGroup")},
            {ProtocolRole, QByteArrayLiteral("protocol")},
            {KindRole, QByteArrayLiteral("kind")},
            {KindTextRole, QByteArrayLiteral("kindText")},
            {HasPositionRole, QByteArrayLiteral("hasPosition")},
            {LatitudeRole, QByteArrayLiteral("latitude")},
            {LongitudeRole, QByteArrayLiteral("longitude")},
            {HasUnknownFixRole, QByteArrayLiteral("hasUnknownFix")},
            {UnknownLatitudeRole, QByteArrayLiteral("unknownLatitude")},
            {UnknownLongitudeRole, QByteArrayLiteral("unknownLongitude")},
            {ValidityUnknownRole, QByteArrayLiteral("validityUnknown")},
            {ProvisionalRole, QByteArrayLiteral("provisional")},
            {WeakCheckRole, QByteArrayLiteral("weakCheck")},
            {EmergencyRole, QByteArrayLiteral("emergency")},
            {AltitudeDmRole, QByteArrayLiteral("altitudeDm")},
            {ErrorMRole, QByteArrayLiteral("errorM")},
            {ErrorLowerBoundRole, QByteArrayLiteral("errorLowerBound")},
            {SpeedDkmhRole, QByteArrayLiteral("speedDkmh")},
            {SpeedLowerBoundRole, QByteArrayLiteral("speedLowerBound")},
            {HeadingDdegRole, QByteArrayLiteral("headingDdeg")},
            {HeadingAmbiguousRole, QByteArrayLiteral("headingAmbiguous")},
            {SatsRole, QByteArrayLiteral("sats")},
            {FreqHzRole, QByteArrayLiteral("freqHz")},
            {FixTimeRole, QByteArrayLiteral("fixTime")},
            {ReceivedTimeRole, QByteArrayLiteral("receivedTime")},
            {ReceivedClockRole, QByteArrayLiteral("receivedClock")},
            {TimeTextRole, QByteArrayLiteral("timeText")},
            {AgeSecondsRole, QByteArrayLiteral("ageSeconds")},
            {UnknownAgeSecondsRole, QByteArrayLiteral("unknownAgeSeconds")},
            {ReportAgeSecondsRole, QByteArrayLiteral("reportAgeSeconds")},
            {ShownRole, QByteArrayLiteral("shown")},
            {PointCountRole, QByteArrayLiteral("pointCount")},
            {CurrentFixRole, QByteArrayLiteral("currentFix")},
            {UnknownFixRole, QByteArrayLiteral("unknownFix")},
            {CurrentShownRole, QByteArrayLiteral("currentShown")},
            {UnknownShownRole, QByteArrayLiteral("unknownShown")}};
}

/* The first row whose order is at most @p order: where a row of that order is, or goes. */
int
PositionModel::positionFor(quint64 order) const {
    const auto it = std::lower_bound(m_rows.cbegin(), m_rows.cend(), order,
                                     [](const Row& row, quint64 wanted) { return row.order > wanted; });
    return static_cast<int>(it - m_rows.cbegin());
}

int
PositionModel::gapRow() const {
    const auto it = std::find_if(m_rows.cbegin(), m_rows.cend(), [](const Row& row) { return row.gap; });
    return it == m_rows.cend() ? -1 : static_cast<int>(it - m_rows.cbegin());
}

int
PositionModel::rowForTrack(qulonglong trackId) const {
    if (trackId == 0U) {
        return gapRow();
    }
    // A track's id is its order.
    const int at = positionFor(trackId);
    return at < m_rows.size() && !m_rows.at(at).gap && m_rows.at(at).id == trackId ? at : -1;
}

QVariantMap
PositionModel::trackFor(qulonglong trackId) const {
    QVariantMap track;
    const int row = rowForTrack(trackId);
    if (row < 0) {
        return track;
    }
    const QHash<int, QByteArray> names = roleNames();
    for (auto it = names.cbegin(); it != names.cend(); ++it) {
        track.insert(QString::fromLatin1(it.value()), value(m_rows.at(row), it.key()));
    }
    return track;
}

void
PositionModel::setCount(int held) {
    if (held != m_count) {
        m_count = held;
        Q_EMIT countChanged();
    }
}

void
PositionModel::setAgeFilterSeconds(int seconds) {
    const int chosen = qMax(0, seconds);
    if (chosen == m_ageFilterS) {
        return;
    }
    m_ageFilterS = chosen;
    refreshAges();
    Q_EMIT ageFilterSecondsChanged();
}

void
PositionModel::setAgeing(bool on) {
    if (on == m_ageTimer.isActive()) {
        return;
    }
    if (on) {
        refreshAges();
        m_ageTimer.start();
    } else {
        m_ageTimer.stop();
    }
    Q_EMIT ageingChanged();
}

/* Read @p row's ages now; @return whether an age or what the filter shows changed. */
bool
PositionModel::updateAges(Row& row, qint64 wallNow) const {
    if (row.gap) {
        return false;
    }
    const dsd_app_position_sample& sample = m_read.sample;
    const qint64 age = row.hasCurrent ? age_since(row.current, row.current.rx_time_s, sample, wallNow) : -1;
    const qint64 unknownAge = row.hasUnknown ? age_since(row.unknown, row.unknown.rx_time_s, sample, wallNow) : -1;
    const dsd_position_fix& latest = row.latest();
    const qint64 reportAge = measured_age(latest, sample, wallNow);
    const auto shows = [this](bool held, qint64 a) {
        return held && (m_ageFilterS == 0 || (a >= 0 && a <= m_ageFilterS));
    };
    const bool currentShown = shows(row.hasCurrent, age);
    const bool unknownShown = shows(row.hasUnknown, unknownAge);
    const bool changed = age != row.ageS || unknownAge != row.unknownAgeS || reportAge != row.reportAgeS
                         || currentShown != row.currentShown || unknownShown != row.unknownShown;
    row.ageS = age;
    row.unknownAgeS = unknownAge;
    row.reportAgeS = reportAge;
    row.currentShown = currentShown;
    row.unknownShown = unknownShown;
    return changed;
}

void
PositionModel::refreshAges() {
    const qint64 now = m_wallClock();
    bool any = false;
    for (int i = 0; i < m_rows.size(); ++i) {
        if (updateAges(m_rows[i], now)) {
            const QModelIndex idx = index(i, 0);
            Q_EMIT dataChanged(idx, idx, age_roles());
            any = true;
        }
    }
    if (any) {
        Q_EMIT agesChanged();
    }
}

/* The one gap row, at the top: positions may be missing after the last report read. A later loss moves it. */
void
PositionModel::placeGap() {
    const int held = gapRow();
    if (held >= 0) {
        beginRemoveRows(QModelIndex(), held, held);
        m_rows.remove(held);
        endRemoveRows();
    }
    Row gap;
    gap.gap = true;
    gap.order = m_nextOrder++;
    gap.gapTimeKnown = m_lastRxKnown;
    gap.gapTimeS = m_lastRxS;
    gap.gapClock = m_lastRxClock;
    const int at = positionFor(gap.order);
    beginInsertRows(QModelIndex(), at, at);
    m_rows.insert(at, gap);
    endInsertRows();
}

/* The row whose newest report arrived first leaves (spec 4.5: oldest newest-fix first); with @p anonymousOnly, the
 * oldest anonymous row (the anonymous share is full). */
void
PositionModel::evictOldest(bool anonymousOnly) {
    int oldest = -1;
    for (int i = 0; i < m_rows.size(); ++i) {
        const Row& row = m_rows.at(i);
        if (row.gap || (anonymousOnly && !row.anonymous)) {
            continue;
        }
        if (oldest < 0 || row.lastArrival < m_rows.at(oldest).lastArrival) {
            oldest = i;
        }
    }
    if (oldest < 0) {
        return;
    }
    if (!m_rows.at(oldest).anonymous) {
        m_trackIds.remove(track_key(m_rows.at(oldest).latest()));
    }
    beginRemoveRows(QModelIndex(), oldest, oldest);
    m_rows.remove(oldest);
    endRemoveRows();
    setCount(m_count - 1);
}

int
PositionModel::anonymousCount() const {
    return static_cast<int>(
        std::count_if(m_rows.cbegin(), m_rows.cend(), [](const Row& row) { return !row.gap && row.anonymous; }));
}

/* A new row for @p fix's track at the top; @return the row it is at. A new anonymous row first makes room within the
 * anonymous share (each anonymous report is a row of its own: a hashed or unverified flood would otherwise push out
 * every radio that has a trail); then, when all 200 are held, the row whose newest report is oldest leaves, named or
 * not, so a fresh fix never gives way to a stale one. */
int
PositionModel::insertTrack(const dsd_position_fix& fix) {
    const bool anonymous = anonymous_fix(fix);
    if (anonymous && anonymousCount() >= kAnonymousCapacity) {
        evictOldest(true);
    }
    if (m_count >= kTrackCapacity) {
        evictOldest(false);
    }
    Row row;
    row.order = m_nextOrder++;
    row.id = row.order;
    row.anonymous = anonymous;
    const int at = positionFor(row.order);
    beginInsertRows(QModelIndex(), at, at);
    m_rows.insert(at, std::move(row));
    endInsertRows();
    setCount(m_count + 1);
    return at;
}

/* @p fix (the @p arrival-th report) on @p row: a validity-unknown fix is held apart; any other extends the trail,
 * breaking it when the break epoch moved (@p breakEpoch) since the trail's last point. */
void
PositionModel::addFix(Row& row, const dsd_position_fix& fix, quint64 arrival, quint64 breakEpoch) {
    row.lastArrival = arrival;
    if ((fix.flags & DSD_POS_FLAG_VALIDITY_UNKNOWN) != 0U) {
        row.unknown = fix;
        row.hasUnknown = true;
        row.latestIsUnknown = true;
    } else {
        Point point;
        point.latE7 = fix.lat_e7;
        point.lonE7 = fix.lon_e7;
        point.errM = fix.err_m;
        point.flags = fix.flags;
        point.breakBefore = !row.trail.isEmpty() && row.breakEpoch != breakEpoch;
        row.trail.append(point);
        if (row.trail.size() > kPointsPerTrack) {
            row.trail.removeFirst();
        }
        row.breakEpoch = breakEpoch;
        row.current = fix;
        row.hasCurrent = true;
        row.latestIsUnknown = false;
    }
    if (!row.anonymous && fix.label[0] != '\0') {
        row.label = fixed_text(fix.label, sizeof fix.label);
    }
}

/* @p fix is the last report read: a gap found after it is "missing from" its receive time. */
void
PositionModel::noteReceived(const dsd_position_fix& fix) {
    m_lastRxKnown = fix.rx_time_s > 0;
    m_lastRxS = fix.rx_time_s;
    m_lastRxClock = fix.rx_clock;
}

/* The marks before a report: a break holds for the next point any track takes, a loss shows the gap row now. */
void
PositionModel::applyMarks(quint8 gap, Ingest& batch) {
    if ((gap & kPositionGapBreak) != 0U) {
        ++m_breakEpoch;
    }
    if ((gap & kPositionGapLoss) != 0U) {
        placeGap();
        batch.changed = true;
    }
}

void
PositionModel::ingest(const dsd_position_fix& fix, quint8 gap, Ingest& batch) {
    applyMarks(gap, batch);
    noteReceived(fix);
    ++m_arrivals;
    int at = -1;
    QByteArray key;
    if (!anonymous_fix(fix)) {
        key = track_key(fix);
        const auto held = m_trackIds.constFind(key);
        at = held != m_trackIds.cend() ? rowForTrack(held.value()) : -1;
    }
    if (at < 0) {
        at = insertTrack(fix);
        if (!key.isEmpty()) {
            m_trackIds.insert(key, m_rows.at(at).id);
        }
    }
    addFix(m_rows[at], fix, m_arrivals, m_breakEpoch);
    batch.touched.append(m_rows.at(at).id);
    batch.changed = true;
}

void
PositionModel::ingestPage(const dsd_position_fix* fixes, int n, quint8 gap, Ingest& batch) {
    if (n <= 0) {
        applyMarks(gap, batch);
        return;
    }
    ingest(fixes[0], gap, batch);
    for (int i = 1; i < n; ++i) {
        ingest(fixes[i], 0U, batch);
    }
}

void
PositionModel::finishBatch(const Ingest& batch) {
    const qint64 now = m_wallClock();
    QVector<quint64> touched = batch.touched;
    std::sort(touched.begin(), touched.end());
    touched.erase(std::unique(touched.begin(), touched.end()), touched.end());
    for (const quint64 id : std::as_const(touched)) {
        const int at = rowForTrack(id);
        if (at < 0) {
            continue; // Evicted later in the same batch.
        }
        (void)updateAges(m_rows[at], now);
        const QModelIndex idx = index(at, 0);
        Q_EMIT dataChanged(idx, idx);
    }
    if (batch.sampleChanged) {
        refreshAges();
    }
    if (batch.changed) {
        Q_EMIT tracksChanged();
    }
}

void
PositionModel::readView() {
    const dsd_app_position_sample before = m_read.sample;
    Ingest batch;
    for (;;) {
        const PositionPage page = position_read_page(m_source, m_read, m_page.get(), kPage);
        if (page.count < 0) {
            break;
        }
        ingestPage(m_page.get(), page.count, page.gap, batch);
        if (!page.more || page.count == 0) {
            break;
        }
    }
    batch.sampleChanged = sample_moved(before, m_read.sample);
    finishBatch(batch);
}

void
PositionModel::handOff() {
    m_handoffOwed = false;
    if (!m_tap || !m_tap->running()) {
        return;
    }
    const PositionTap::Result tapped = m_tap->stop();
    if (!tapped.ran) {
        return;
    }
    const dsd_app_position_sample before = m_read.sample;
    Ingest batch;
    for (int i = 0; i < tapped.count; ++i) {
        ingest(tapped.fixAt(i), tapped.gapAt(i), batch);
    }
    ingestPage(nullptr, 0, tapped.trailingGap, batch);
    if (tapped.dropped > 0U) {
        qInfo("positions: %llu reports received in the background were dropped (the queue was full)",
              static_cast<unsigned long long>(tapped.dropped));
    }
    m_read = tapped.read;
    batch.sampleChanged = sample_moved(before, m_read.sample);
    finishBatch(batch);
}

void
PositionModel::poll() {
    if (m_handoffOwed) {
        handOff();
    }
    if (m_tap && m_tap->running()) {
        return; // Still away: the tap reads.
    }
    if (m_source != nullptr) {
        readView();
    }
}

void
PositionModel::clear() {
    // The read position moves to what is published now (the clear's boundary): a report published since the last
    // poll belongs to before the clear, and is not read. A tap still running (a desktop window hidden) gives up what
    // it holds, and goes on from that boundary.
    bool restartTap = false;
    if (m_tap && m_tap->running()) {
        const PositionTap::Result tapped = m_tap->stop();
        if (tapped.ran) {
            m_read = tapped.read;
        }
        if (tapped.count > 0) {
            // Read, then dropped by the clear: a later gap's "missing from" time starts after the last of them.
            noteReceived(tapped.fixAt(tapped.count - 1));
        }
        restartTap = !m_handoffOwed;
    }
    dsd_position_fix head{};
    bool haveHead = false;
    if (position_skip_to_head(m_source, m_read, &head, &haveHead) && haveHead) {
        noteReceived(head);
    }
    if (restartTap && !m_tap->start(m_read)) {
        qWarning("positions: the background capture could not start");
    }
    if (!m_rows.isEmpty()) {
        beginResetModel();
        m_rows.clear();
        endResetModel();
        setCount(0);
        Q_EMIT tracksChanged();
    }
    m_trackIds.clear();
}

void
PositionModel::onApplicationStateChanged(Qt::ApplicationState state) {
    if (state == Qt::ApplicationSuspended || state == Qt::ApplicationHidden) {
        m_handoffOwed = false;
        if (m_tap && !m_tap->running()) {
            if (m_tapPollMs > 0) {
                m_tap->setPollIntervalForTest(m_tapPollMs);
            }
            if (!m_tap->start(m_read)) {
                qWarning("positions: the background capture could not start");
            }
        }
        return;
    }
    // Back on screen, or at least running: the next poll takes over from the tap.
    if (m_tap && m_tap->running()) {
        m_handoffOwed = true;
    }
}

void
PositionModel::setSourceForTest(PositionSource source) {
    if (m_tap) {
        (void)m_tap->stop();
    }
    m_handoffOwed = false;
    m_source = source;
    m_tap = source != nullptr ? std::make_unique<PositionTap>(source) : nullptr;
}

void
PositionModel::setWallClockForTest(WallClock clock) {
    m_wallClock = clock != nullptr ? clock : &system_wall_seconds;
}

void
PositionModel::setBackgroundPollIntervalForTest(int pollMs) {
    m_tapPollMs = pollMs;
}

bool
PositionModel::backgroundCaptureRunningForTest() const {
    return m_tap && m_tap->running();
}

void
PositionModel::waitForBackgroundPollForTest() {
    if (m_tap) {
        m_tap->waitForPollForTest();
    }
}

PositionFilterModel::PositionFilterModel(QObject* parent) : QSortFilterProxyModel(parent) {
    setFilterRole(PositionModel::ShownRole);
    connect(this, &QAbstractItemModel::rowsInserted, this, &PositionFilterModel::countChanged);
    connect(this, &QAbstractItemModel::rowsRemoved, this, &PositionFilterModel::countChanged);
    connect(this, &QAbstractItemModel::modelReset, this, &PositionFilterModel::countChanged);
    connect(this, &QAbstractItemModel::layoutChanged, this, &PositionFilterModel::countChanged);
}

bool
PositionFilterModel::filterAcceptsRow(int source_row, const QModelIndex& source_parent) const {
    const QAbstractItemModel* source = sourceModel();
    if (source == nullptr) {
        return false;
    }
    return source->data(source->index(source_row, 0, source_parent), PositionModel::ShownRole).toBool();
}

int
PositionFilterModel::trackCount() const {
    return rowForTrack(0U) >= 0 ? rowCount() - 1 : rowCount();
}

int
PositionFilterModel::rowForTrack(qulonglong trackId) const {
    const auto* source = qobject_cast<const PositionModel*>(sourceModel());
    const int row = source != nullptr ? source->rowForTrack(trackId) : -1;
    return row < 0 ? -1 : mapFromSource(source->index(row, 0)).row();
}

} // namespace dsd_qt
