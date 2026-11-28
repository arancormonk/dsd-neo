// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/**
 * @file
 * @brief The map's tracks (issue #800): one row per typed source, anonymous single points, validity-unknown fixes held
 * apart from the trail, the point and track caps, Clear, gaps that break trails and show one gap row, ages on the right
 * clock (system fixes on the wall clock; replay fixes only while the sample names their running replay), the age
 * filter, and the background tap: an overflow while hidden, a stop and start while hidden, and the handoff on show.
 *
 * The view is the real cursor read (dsd_app_position_ring_read()) over a ring this test publishes into, as the
 * snapshot publish would. Every coordinate, ID and scope here is made up.
 */

#include <QAbstractListModel>
#include <QByteArray>
#include <QGuiApplication>
#include <QHash>
#include <QList>
#include <QMap>
#include <QObject>
#include <QSignalSpy>
#include <QString>
#include <QVariant>
#include <QVariantMap>
#include <Qt>
#include <QtGlobal>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <dsd-neo/app_control/position_view.h>
#include <dsd-neo/core/position.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <mutex>
#include <utility>
#include <vector>
#if defined(__linux__)
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#include "position_model.h"
#include "position_ring.h"
#include "position_tap.h"

namespace {

using dsd_qt::PositionFilterModel;
using dsd_qt::PositionModel;
using dsd_qt::PositionReadState;
using dsd_qt::PositionTap;

void
check_at(bool condition, const char* what, int line) {
    if (!condition) {
        (void)DSD_FPRINTF(stderr, "UI_QT_POSITION_MODEL: line %d: %s\n", line, what);
        std::abort();
    }
}

#define CHECK(cond) check_at((cond), #cond, __LINE__)

constexpr int32_t kLat = 225000000; /* 22.5 N */
constexpr int32_t kLon = 450000000; /* 45.0 E */
constexpr int64_t kT0 = 1700000000; /* receive times, seconds */

/* The published ring the model reads, and the lock a snapshot publish would hold. */
std::mutex g_mu;
dsd_app_position_ring g_ring;
qint64 g_wall = kT0;

int
ring_source(uint64_t stream_id, uint64_t after_seq, dsd_position_fix* out, size_t cap, dsd_app_position_cursor* next) {
    if (next == nullptr || (out == nullptr && cap > 0U)) {
        return -1;
    }
    const std::lock_guard<std::mutex> lock(g_mu);
    return dsd_app_position_ring_read(&g_ring, stream_id, after_seq, out, cap, next);
}

qint64
wall_clock() {
    return g_wall;
}

void
reset_ring() {
    const std::lock_guard<std::mutex> lock(g_mu);
    DSD_MEMSET(&g_ring, 0, sizeof g_ring);
}

dsd_app_position_sample
sample_of(uint64_t stream, uint8_t clock, int64_t now, bool running) {
    dsd_app_position_sample s{};
    s.stream_id = stream;
    s.clock_source = clock;
    s.decode_now_s = now;
    s.running = running ? 1U : 0U;
    return s;
}

/* A publish as dsd_app_position_ring_publish() makes it: a new stream replaces the ring, the records are kept with
 * the next seqs, the sample is stored with them. The caller holds g_mu. */
void
publish_locked(uint64_t stream, std::vector<dsd_position_fix> fixes, const dsd_app_position_sample& sample) {
    if (stream != g_ring.stream_id) {
        g_ring.stream_id = stream;
        g_ring.newest_seq = 0U;
    }
    for (dsd_position_fix& fix : fixes) {
        fix.stream_id = stream;
        fix.seq = ++g_ring.newest_seq;
        g_ring.entries[(fix.seq - 1U) % DSD_POSITION_FEED_CAPACITY] = fix;
    }
    g_ring.sample = sample;
}

void
publish(uint64_t stream, std::vector<dsd_position_fix> fixes, const dsd_app_position_sample& sample) {
    const std::lock_guard<std::mutex> lock(g_mu);
    publish_locked(stream, std::move(fixes), sample);
}

/* A publish with no run (the engine stopped): the records stay, the sample names no stream. */
void
publish_idle() {
    const std::lock_guard<std::mutex> lock(g_mu);
    g_ring.sample = sample_of(0U, DSD_DECODE_CLOCK_SYSTEM, 0, false);
}

dsd_position_fix
fix_of(uint32_t source, int32_t step, int64_t rx, uint8_t clock = DSD_DECODE_CLOCK_SYSTEM) {
    dsd_position_fix f;
    dsd_position_fix_init(&f);
    f.protocol = DSD_SYNC_DMR_BS_DATA_POS;
    f.kind = DSD_POS_DMR_LRRP;
    f.source_id = source;
    f.target_id = 100U;
    f.flags = DSD_POS_FLAG_TARGET_GROUP;
    f.lat_e7 = kLat + step;
    f.lon_e7 = kLon + step;
    f.rx_time_s = rx;
    f.rx_clock = clock;
    f.reception_scope.kind = DSD_POS_SCOPE_DMR;
    f.reception_scope.a = 1U;
    f.reception_scope.b = 2U;
    f.sender_scope = f.reception_scope;
    return f;
}

std::vector<dsd_position_fix>
run_of(uint32_t source, int count, int64_t rx, uint8_t clock = DSD_DECODE_CLOCK_SYSTEM) {
    std::vector<dsd_position_fix> fixes;
    fixes.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        fixes.push_back(fix_of(source, i, rx + i, clock));
    }
    return fixes;
}

struct Fresh {
    Fresh() {
        reset_ring();
        g_wall = kT0;
        model.setSourceForTest(&ring_source);
        model.setWallClockForTest(&wall_clock);
        model.setBackgroundPollIntervalForTest(5);
    }

    PositionModel model;
};

QVariant
role(const PositionModel& model, int row, const char* name) {
    const QHash<int, QByteArray> names = model.roleNames();
    for (auto it = names.cbegin(); it != names.cend(); ++it) {
        if (it.value() == name) {
            return model.data(model.index(row, 0), it.key());
        }
    }
    return {};
}

int
gap_rows(const PositionModel& model) {
    int gaps = 0;
    for (const PositionModel::Row& row : model.rows()) {
        gaps += row.gap ? 1 : 0;
    }
    return gaps;
}

/* The track of radio @p source in the made-up DMR system (1, @p system). */
const PositionModel::Row*
track_of(const PositionModel& model, uint32_t source, uint32_t system = 2U) {
    for (const PositionModel::Row& row : model.rows()) {
        if (!row.gap && !row.anonymous && row.latest().source_id == source && row.latest().sender_scope.b == system) {
            return &row;
        }
    }
    return nullptr;
}

void
set_app_state(Qt::ApplicationState state) {
    Q_EMIT qGuiApp->applicationStateChanged(state);
}

/* One radio's reports make one track whose trail grows; another scope, another radio and P25 Phase 1 vs Phase 2 of
 * one system are told apart or joined by the typed source; names follow the newest label. */
void
test_tracks_by_typed_source() {
    Fresh f;
    const uint64_t s = 11U;
    std::vector<dsd_position_fix> fixes = run_of(7U, 3, kT0);
    DSD_SNPRINTF(fixes[1].label, sizeof fixes[1].label, "%s", "Unit Seven");
    dsd_position_fix other_scope = fix_of(7U, 9, kT0 + 3);
    other_scope.sender_scope.b = 3U;
    fixes.push_back(other_scope);
    fixes.push_back(fix_of(8U, 0, kT0 + 4));
    dsd_position_fix p1 = fix_of(9U, 0, kT0 + 5);
    p1.protocol = DSD_SYNC_P25P1_POS;
    p1.kind = DSD_POS_P25_HARRIS;
    p1.sender_scope.kind = DSD_POS_SCOPE_P25;
    dsd_position_fix p2 = p1;
    p2.protocol = DSD_SYNC_P25P2_POS;
    p2.lat_e7 += 10;
    fixes.push_back(p1);
    fixes.push_back(p2);
    publish(s, fixes, sample_of(s, DSD_DECODE_CLOCK_SYSTEM, kT0 + 6, true));
    f.model.poll();

    CHECK(f.model.count() == 4);
    CHECK(f.model.rowCount() == 4);
    const PositionModel::Row* seven = track_of(f.model, 7U);
    CHECK(seven != nullptr && seven->trail.size() == 3 && seven->label == QStringLiteral("Unit Seven"));
    CHECK(track_of(f.model, 7U, 3U) != nullptr && track_of(f.model, 7U, 3U)->trail.size() == 1);
    CHECK(track_of(f.model, 7U, 3U)->label.isEmpty());
    CHECK(seven->trail.last().latE7 == kLat + 2 && !seven->trail.at(1).breakBefore);
    const PositionModel::Row* nine = track_of(f.model, 9U);
    CHECK(nine != nullptr && nine->trail.size() == 2);
    // Newest first: the last track to appear is row 0.
    CHECK(role(f.model, 0, "sourceId").toUInt() == 9U);
    CHECK(role(f.model, 0, "kindText").toString() == QStringLiteral("L3Harris GPS"));
    CHECK(role(f.model, 0, "targetGroup").toBool() && role(f.model, 0, "targetId").toUInt() == 100U);
    CHECK(!role(f.model, 0, "altitudeDm").isValid() && !role(f.model, 0, "speedDkmh").isValid());
    CHECK(!role(f.model, 0, "fixTime").isValid() && !role(f.model, 0, "sats").isValid());
    const double lat = role(f.model, 0, "latitude").toDouble();
    CHECK(lat > 22.4999 && lat < 22.5001);
    const int at = f.model.rowForTrack(seven->id);
    CHECK(at >= 0 && f.model.trackFor(seven->id).value(QStringLiteral("pointCount")).toInt() == 3);
    CHECK(f.model.trackFor(seven->id).value(QStringLiteral("label")).toString() == QStringLiteral("Unit Seven"));
    CHECK(f.model.trackFor(999U).isEmpty());
}

/* Hashed, unverified and source-less reports are anonymous: one row each, one point, no name, never joined. */
void
test_anonymous_points() {
    Fresh f;
    const uint64_t s = 12U;
    dsd_position_fix hashed = fix_of(0U, 0, kT0);
    hashed.flags = DSD_POS_FLAG_SOURCE_HASHED;
    dsd_position_fix unverified = fix_of(0U, 1, kT0 + 1);
    unverified.flags = DSD_POS_FLAG_SOURCE_UNVERIFIED;
    DSD_SNPRINTF(unverified.label, sizeof unverified.label, "%s", "never shown");
    dsd_position_fix none = fix_of(0U, 2, kT0 + 2);
    publish(s, {hashed, hashed, unverified, none}, sample_of(s, DSD_DECODE_CLOCK_SYSTEM, kT0 + 3, true));
    f.model.poll();
    CHECK(f.model.count() == 4);
    for (int i = 0; i < f.model.rowCount(); ++i) {
        CHECK(role(f.model, i, "anonymous").toBool());
        CHECK(role(f.model, i, "pointCount").toInt() == 1);
        CHECK(role(f.model, i, "label").toString().isEmpty());
        CHECK(!role(f.model, i, "sourceId").isValid());
    }
    CHECK(role(f.model, 3, "sourceHashed").toBool() && role(f.model, 1, "sourceUnverified").toBool());
}

/* A validity-unknown fix (D11) never extends the trail or moves the current position; it is the track's hollow fix.
 * A provisional format (D12) marks its points and the row. */
void
test_validity_unknown_and_provisional() {
    Fresh f;
    const uint64_t s = 13U;
    std::vector<dsd_position_fix> fixes = run_of(7U, 2, kT0);
    dsd_position_fix unknown = fix_of(7U, 50, kT0 + 2);
    unknown.flags |= DSD_POS_FLAG_VALIDITY_UNKNOWN;
    fixes.push_back(unknown);
    dsd_position_fix provisional = fix_of(8U, 0, kT0 + 3);
    provisional.flags |= DSD_POS_FLAG_FORMAT_PROVISIONAL;
    fixes.push_back(provisional);
    publish(s, fixes, sample_of(s, DSD_DECODE_CLOCK_SYSTEM, kT0 + 4, true));
    f.model.poll();

    const PositionModel::Row* seven = track_of(f.model, 7U);
    CHECK(seven != nullptr && seven->trail.size() == 2 && seven->hasUnknown && seven->hasCurrent);
    CHECK(seven->current.lat_e7 == kLat + 1 && seven->unknown.lat_e7 == kLat + 50);
    const int at = f.model.rowForTrack(seven->id);
    CHECK(role(f.model, at, "validityUnknown").toBool() && role(f.model, at, "hasUnknownFix").toBool());
    const double unknownLat = role(f.model, at, "unknownLatitude").toDouble();
    CHECK(unknownLat > 22.50000 && unknownLat < 22.50001);

    // A valid fix after it extends the trail again, from the last valid point; the hollow fix stays.
    publish(s, {fix_of(7U, 3, kT0 + 5)}, sample_of(s, DSD_DECODE_CLOCK_SYSTEM, kT0 + 6, true));
    f.model.poll();
    seven = track_of(f.model, 7U);
    CHECK(seven->trail.size() == 3 && !seven->trail.last().breakBefore && seven->hasUnknown);
    CHECK(!role(f.model, f.model.rowForTrack(seven->id), "validityUnknown").toBool());

    // A track known only from validity-unknown fixes has no current position.
    dsd_position_fix only_unknown = fix_of(9U, 0, kT0 + 7);
    only_unknown.flags |= DSD_POS_FLAG_VALIDITY_UNKNOWN;
    publish(s, {only_unknown}, sample_of(s, DSD_DECODE_CLOCK_SYSTEM, kT0 + 8, true));
    f.model.poll();
    const PositionModel::Row* nine = track_of(f.model, 9U);
    CHECK(nine != nullptr && !nine->hasCurrent && nine->trail.isEmpty() && nine->hasUnknown);
    CHECK(!role(f.model, f.model.rowForTrack(nine->id), "latitude").isValid());

    const PositionModel::Row* eight = track_of(f.model, 8U);
    CHECK((eight->trail.last().flags & DSD_POS_FLAG_FORMAT_PROVISIONAL) != 0U);
    CHECK(role(f.model, f.model.rowForTrack(eight->id), "provisional").toBool());
}

/* 100 points a trail (the oldest leave first); 200 tracks (the one whose newest report came first leaves). */
void
test_caps() {
    Fresh f;
    const uint64_t s = 14U;
    publish(s, run_of(7U, PositionModel::kPointsPerTrack + 5, kT0), sample_of(s, 0, kT0, true));
    f.model.poll();
    const PositionModel::Row* seven = track_of(f.model, 7U);
    CHECK(seven->trail.size() == PositionModel::kPointsPerTrack);
    CHECK(seven->trail.first().latE7 == kLat + 5);

    std::vector<dsd_position_fix> others;
    for (uint32_t id = 1000U; id < 1000U + PositionModel::kTrackCapacity - 1; ++id) {
        others.push_back(fix_of(id, 0, kT0 + 200));
    }
    publish(s, others, sample_of(s, 0, kT0, true));
    f.model.poll();
    CHECK(f.model.count() == PositionModel::kTrackCapacity);
    // Radio 7 reports again: radio 1000's newest report is now the oldest, so it leaves for the next new radio.
    publish(s, {fix_of(7U, 200, kT0 + 300), fix_of(5000U, 0, kT0 + 301)}, sample_of(s, 0, kT0, true));
    f.model.poll();
    CHECK(f.model.count() == PositionModel::kTrackCapacity);
    CHECK(track_of(f.model, 1000U) == nullptr);
    CHECK(track_of(f.model, 7U) != nullptr && track_of(f.model, 5000U) != nullptr);
    // The evicted radio comes back as a new track at the top.
    publish(s, {fix_of(1000U, 1, kT0 + 302)}, sample_of(s, 0, kT0, true));
    f.model.poll();
    CHECK(track_of(f.model, 1000U) != nullptr && track_of(f.model, 1000U)->trail.size() == 1);
    CHECK(role(f.model, 0, "sourceId").toUInt() == 1000U);

    // Revised ruling (final review): rows leave by the age of their newest report, named or not (spec 4.5), and
    // anonymous rows are held to their share (kAnonymousCapacity). A flood of hashed reports first fills the share,
    // each one pushing out the stalest row of any kind while all 200 are held; past the share, a new one replaces
    // the oldest anonymous row and every named radio stays.
    const int share = PositionModel::kAnonymousCapacity;
    std::vector<dsd_position_fix> hashed;
    for (int i = 0; i < share + 10; ++i) {
        dsd_position_fix h = fix_of(0U, i, kT0 + 400 + i);
        h.flags = DSD_POS_FLAG_SOURCE_HASHED;
        hashed.push_back(h);
    }
    publish(s, hashed, sample_of(s, 0, kT0, true));
    f.model.poll();
    int anonymous = 0;
    for (const PositionModel::Row& row : f.model.rows()) {
        anonymous += row.anonymous ? 1 : 0;
    }
    CHECK(f.model.count() == PositionModel::kTrackCapacity);
    CHECK(anonymous == share);
    CHECK(f.model.rows().first().anonymous && f.model.rows().first().current.lat_e7 == kLat + share + 9);
    // The freshest named radios stayed; the stalest named ones made room for the share.
    CHECK(track_of(f.model, 7U) != nullptr && track_of(f.model, 5000U) != nullptr
          && track_of(f.model, 1000U) != nullptr);
    CHECK(f.model.count() - anonymous == PositionModel::kTrackCapacity - share);

    // A fresh anonymous fix never gives way to a stale named track: with all 200 held, a new radio pushes out the
    // row whose newest report is oldest, here a named radio that last reported long before the hashed flood.
    const PositionModel::Row* stalest = nullptr;
    for (const PositionModel::Row& row : f.model.rows()) {
        if (!row.gap && (stalest == nullptr || row.lastArrival < stalest->lastArrival)) {
            stalest = &row;
        }
    }
    CHECK(stalest != nullptr && !stalest->anonymous);
    const uint32_t stale_id = stalest->latest().source_id;
    publish(s, {fix_of(6000U, 0, kT0 + 500)}, sample_of(s, 0, kT0, true));
    f.model.poll();
    anonymous = 0;
    for (const PositionModel::Row& row : f.model.rows()) {
        anonymous += row.anonymous ? 1 : 0;
    }
    CHECK(track_of(f.model, 6000U) != nullptr && track_of(f.model, stale_id) == nullptr);
    CHECK(anonymous == share);
}

/* Final review M1: the sheet and the List describe the fix that is drawn. The current position's own report and the
 * open ring's report each have their flags and measured fields (currentFix, unknownFix), apart from the newest
 * report's (the top-level roles): a weak-check fix on the trail stays described as weak when a newer error-7 report
 * becomes the newest. */
void
test_shown_fix_records() {
    Fresh f;
    const uint64_t s = 19U;
    dsd_position_fix weak = fix_of(7U, 0, kT0);
    weak.flags = DSD_POS_FLAG_WEAK_CHECK | DSD_POS_FLAG_FORMAT_PROVISIONAL;
    weak.speed_dkmh = 420;
    weak.err_m = 25;
    dsd_position_fix later = fix_of(7U, 3, kT0 + 5);
    later.flags = DSD_POS_FLAG_VALIDITY_UNKNOWN;
    later.alt_dm = 1230;
    publish(s, {weak, later}, sample_of(s, DSD_DECODE_CLOCK_SYSTEM, kT0 + 6, true));
    f.model.poll();
    const PositionModel::Row* seven = track_of(f.model, 7U);
    CHECK(seven != nullptr);
    const QVariantMap track = f.model.trackFor(seven->id);
    // The top-level roles describe the newest report (the error-7 one) ...
    CHECK(track.value(QStringLiteral("validityUnknown")).toBool()
          && !track.value(QStringLiteral("weakCheck")).toBool());
    // ... the current fix's record what its marker is drawn from: weak, provisional (the double outline), its speed.
    const QVariantMap current = track.value(QStringLiteral("currentFix")).toMap();
    CHECK(current.value(QStringLiteral("weakCheck")).toBool() && current.value(QStringLiteral("provisional")).toBool());
    CHECK(!current.value(QStringLiteral("validityUnknown")).toBool()
          && !current.value(QStringLiteral("newest")).toBool());
    CHECK(current.value(QStringLiteral("speedDkmh")).toInt() == 420
          && current.value(QStringLiteral("errorM")).toInt() == 25);
    CHECK(!current.contains(QStringLiteral("altitudeDm")));
    const QVariantMap ring = track.value(QStringLiteral("unknownFix")).toMap();
    CHECK(ring.value(QStringLiteral("validityUnknown")).toBool() && ring.value(QStringLiteral("newest")).toBool());
    CHECK(ring.value(QStringLiteral("altitudeDm")).toInt() == 1230 && !ring.contains(QStringLiteral("speedDkmh")));
    CHECK(std::fabs(ring.value(QStringLiteral("latitude")).toDouble() - (kLat + 3) / 1e7) < 1e-9);
    CHECK(track.value(QStringLiteral("currentShown")).toBool() && track.value(QStringLiteral("unknownShown")).toBool());

    // F2 review: the words follow what the canvas draws. A filter that hides the 40-minute-old current fix but shows
    // the 5-minute-old ring: currentShown goes false, unknownShown stays true, and an age tick announces it.
    Fresh g;
    const uint64_t s2 = 22U;
    dsd_position_fix old_fix = fix_of(8U, 0, kT0);
    dsd_position_fix newer = fix_of(8U, 1, kT0 + 2100);
    newer.flags = DSD_POS_FLAG_VALIDITY_UNKNOWN;
    g_wall = kT0 + 2400;
    publish(s2, {old_fix, newer}, sample_of(s2, DSD_DECODE_CLOCK_SYSTEM, kT0 + 2400, true));
    g.model.poll();
    QSignalSpy changed(&g.model, &QAbstractItemModel::dataChanged);
    g.model.setAgeFilterSeconds(900);
    const PositionModel::Row* eight = track_of(g.model, 8U);
    CHECK(eight != nullptr);
    const QVariantMap filtered = g.model.trackFor(eight->id);
    CHECK(!filtered.value(QStringLiteral("currentShown")).toBool());
    CHECK(filtered.value(QStringLiteral("unknownShown")).toBool());
    bool announced = false;
    for (const QList<QVariant>& args : changed) {
        announced = announced || qvariant_cast<QList<int>>(args.at(2)).contains(PositionModel::CurrentShownRole);
    }
    CHECK(announced);
}

/* Final review M2: the filter's track count leaves the gap row out, so a filter that hides every radio reads as no
 * radio shown even after a loss. */
void
test_filter_track_count() {
    Fresh f;
    PositionFilterModel view;
    view.setSourceModel(&f.model);
    const uint64_t s = 20U;
    publish(s, {fix_of(7U, 0, kT0)}, sample_of(s, DSD_DECODE_CLOCK_SYSTEM, kT0, true));
    f.model.poll();
    CHECK(view.trackCount() == 1 && view.count() == 1);
    // A new run: the old one's tail was unread, so a gap row is placed.
    const uint64_t s2 = 21U;
    publish(s2, {fix_of(8U, 0, kT0 + 10)}, sample_of(s2, DSD_DECODE_CLOCK_SYSTEM, kT0 + 10, true));
    f.model.poll();
    g_wall = kT0 + 2000;
    f.model.setAgeFilterSeconds(900);
    CHECK(view.count() >= 1);
    CHECK(view.trackCount() == 0);
}

/* A loss breaks every trail there and shows one gap row (at the top, with the time of the last report read); a later
 * loss moves it. */
void
test_loss_breaks_trails_and_one_gap_row() {
    Fresh f;
    const uint64_t s = 15U;
    publish(s, {fix_of(7U, 0, kT0), fix_of(8U, 0, kT0 + 1)}, sample_of(s, 0, kT0, true));
    f.model.poll();
    // More than the ring holds arrives between two polls: the oldest are gone.
    std::vector<dsd_position_fix> burst;
    burst.reserve(DSD_POSITION_FEED_CAPACITY + 10U);
    for (int i = 0; i < DSD_POSITION_FEED_CAPACITY + 10; ++i) {
        burst.push_back(fix_of(7U, i + 1, kT0 + 10 + i));
    }
    publish(s, burst, sample_of(s, 0, kT0, true));
    publish(s, {fix_of(8U, 1, kT0 + 2000)}, sample_of(s, 0, kT0, true));
    f.model.poll();
    CHECK(gap_rows(f.model) == 1);
    CHECK(f.model.count() == 2);
    const int gap = f.model.rowForTrack(0U);
    CHECK(gap >= 0 && role(f.model, gap, "gap").toBool() && role(f.model, gap, "shown").toBool());
    CHECK(role(f.model, gap, "receivedTime").toLongLong() == kT0 + 1);
    CHECK(!role(f.model, gap, "timeText").toString().isEmpty());
    const PositionModel::Row* seven = track_of(f.model, 7U);
    const PositionModel::Row* eight = track_of(f.model, 8U);
    // Radio 7: the newest 100 of the burst, joined among themselves (its point before the break left with the cap).
    CHECK(seven->trail.size() == PositionModel::kPointsPerTrack && !seven->trail.last().breakBefore);
    CHECK(eight->trail.size() == 2 && eight->trail.at(1).breakBefore);

    // A second loss moves the one gap row up, below only the tracks that appear after it.
    std::vector<dsd_position_fix> second;
    second.reserve(DSD_POSITION_FEED_CAPACITY + 1U);
    for (int i = 0; i < DSD_POSITION_FEED_CAPACITY + 1; ++i) {
        second.push_back(fix_of(9U, i, kT0 + 3000 + i));
    }
    publish(s, second, sample_of(s, 0, kT0, true));
    f.model.poll();
    CHECK(gap_rows(f.model) == 1 && f.model.count() == 3);
    CHECK(f.model.rowForTrack(0U) == 1 && role(f.model, 0, "sourceId").toUInt() == 9U);
    CHECK(role(f.model, 1, "receivedTime").toLongLong() == kT0 + 2000);
}

/* Clear drops every row and the gap row; what was published before it is not read again. */
void
test_clear() {
    Fresh f;
    const uint64_t s = 16U;
    publish(s, run_of(7U, 3, kT0), sample_of(s, 0, kT0, true));
    f.model.poll();
    QSignalSpy tracks(&f.model, &PositionModel::tracksChanged);
    f.model.clear();
    CHECK(f.model.count() == 0 && f.model.rowCount() == 0 && tracks.count() == 1);
    f.model.poll();
    CHECK(f.model.count() == 0);
    publish(s, {fix_of(7U, 10, kT0 + 10)}, sample_of(s, 0, kT0, true));
    f.model.poll();
    CHECK(f.model.count() == 1 && track_of(f.model, 7U)->trail.size() == 1);

    // Hidden, the tap holds reports; a clear drops them too, and the tap goes on after them.
    set_app_state(Qt::ApplicationHidden);
    CHECK(f.model.backgroundCaptureRunningForTest());
    publish(s, {fix_of(8U, 0, kT0 + 11)}, sample_of(s, 0, kT0, true));
    f.model.waitForBackgroundPollForTest();
    f.model.clear();
    CHECK(f.model.backgroundCaptureRunningForTest());
    publish(s, {fix_of(9U, 0, kT0 + 12)}, sample_of(s, 0, kT0, true));
    f.model.waitForBackgroundPollForTest();
    set_app_state(Qt::ApplicationActive);
    f.model.poll();
    CHECK(!f.model.backgroundCaptureRunningForTest());
    CHECK(f.model.count() == 1 && track_of(f.model, 9U) != nullptr && track_of(f.model, 8U) == nullptr);

    // A clear while away drops what the tap read; a later loss is "missing from" the last of those, not from before.
    set_app_state(Qt::ApplicationHidden);
    publish(s, {fix_of(10U, 0, kT0 + 13)}, sample_of(s, 0, kT0, true));
    f.model.waitForBackgroundPollForTest();
    f.model.clear();
    set_app_state(Qt::ApplicationActive);
    f.model.poll();
    CHECK(f.model.count() == 0);
    publish(s, run_of(11U, DSD_POSITION_FEED_CAPACITY + 1, kT0 + 100), sample_of(s, 0, kT0, true));
    f.model.poll();
    CHECK(gap_rows(f.model) == 1);
    CHECK(role(f.model, f.model.rowForTrack(0U), "receivedTime").toLongLong() == kT0 + 13);
}

/* System fixes age on the wall clock, also after the run stops; the filter shows what is young enough. */
void
test_system_ages_and_filter() {
    Fresh f;
    PositionFilterModel view;
    view.setSourceModel(&f.model);
    const uint64_t s = 17U;
    g_wall = kT0 + 100;
    dsd_position_fix seven = fix_of(7U, 0, kT0 + 40);
    seven.fix_time_s = kT0 + 30;
    publish(s, {fix_of(8U, 0, kT0), seven}, sample_of(s, DSD_DECODE_CLOCK_SYSTEM, kT0 + 5, true));
    f.model.poll();
    // The decoder's own sample time is not used for system fixes: only the wall clock.
    CHECK(track_of(f.model, 8U)->ageS == 100 && track_of(f.model, 7U)->ageS == 60);
    CHECK(track_of(f.model, 7U)->reportAgeS == 70 && track_of(f.model, 8U)->reportAgeS == -1);

    // The run stops: the records stay, the wall clock goes on.
    publish(s, {}, sample_of(s, DSD_DECODE_CLOCK_SYSTEM, kT0 + 6, false));
    publish_idle();
    f.model.poll();
    g_wall = kT0 + 1000;
    QSignalSpy ages(&f.model, &PositionModel::agesChanged);
    f.model.refreshAges();
    CHECK(ages.count() == 1);
    CHECK(track_of(f.model, 8U)->ageS == 1000 && track_of(f.model, 7U)->ageS == 960);

    CHECK(view.count() == 2);
    f.model.setAgeFilterSeconds(980);
    CHECK(view.count() == 1);
    CHECK(view.rowForTrack(track_of(f.model, 7U)->id) == 0 && view.rowForTrack(track_of(f.model, 8U)->id) == -1);
    CHECK(track_of(f.model, 7U)->currentShown && !track_of(f.model, 8U)->currentShown);
    f.model.setAgeFilterSeconds(0);
    CHECK(view.count() == 2);

    // The map's timer: once a second while ageing.
    f.model.setAgeing(true);
    CHECK(f.model.ageing());
    g_wall += 5;
    QSignalSpy ticked(&f.model, &PositionModel::agesChanged);
    CHECK(ticked.wait(3000));
    CHECK(track_of(f.model, 8U)->ageS == 1005);
    f.model.setAgeing(false);
    CHECK(!f.model.ageing());
}

/* Replay fixes age only on their own running replay's sample: paused ageing holds, a stopped replay has no age (the
 * receive time stays), another run's sample never ages them, and the filter counts no age as too old. */
void
test_replay_ages() {
    Fresh f;
    const uint64_t a = 21U;
    const int64_t capture = 1500000000; /* the capture's own time */
    g_wall = kT0 + 5000;
    publish(a, {fix_of(7U, 0, capture, DSD_DECODE_CLOCK_REPLAY)},
            sample_of(a, DSD_DECODE_CLOCK_REPLAY, capture + 30, true));
    f.model.poll();
    CHECK(track_of(f.model, 7U)->ageS == 30);
    // Paused: the decode clock holds, and so does the age, whatever the wall clock does.
    g_wall += 100;
    publish(a, {}, sample_of(a, DSD_DECODE_CLOCK_REPLAY, capture + 30, true));
    f.model.poll();
    f.model.refreshAges();
    CHECK(track_of(f.model, 7U)->ageS == 30);
    publish(a, {}, sample_of(a, DSD_DECODE_CLOCK_REPLAY, capture + 45, true));
    f.model.poll();
    f.model.refreshAges();
    CHECK(track_of(f.model, 7U)->ageS == 45);

    // Stopped, with no later start: no age, the receive time stays.
    QSignalSpy ages(&f.model, &PositionModel::agesChanged);
    publish(a, {}, sample_of(a, DSD_DECODE_CLOCK_REPLAY, capture + 46, false));
    f.model.poll();
    CHECK(ages.count() >= 1);
    CHECK(track_of(f.model, 7U)->ageS == -1);
    const int at = f.model.rowForTrack(track_of(f.model, 7U)->id);
    CHECK(role(f.model, at, "ageSeconds").toLongLong() == -1);
    CHECK(role(f.model, at, "receivedTime").toLongLong() == capture);
    CHECK(role(f.model, at, "receivedClock").toInt() == DSD_DECODE_CLOCK_REPLAY);
    CHECK(!role(f.model, at, "timeText").toString().isEmpty());
    publish_idle();
    f.model.poll();
    f.model.refreshAges();
    CHECK(track_of(f.model, 7U)->ageS == -1);
    // No age is older than any filter.
    f.model.setAgeFilterSeconds(3600);
    CHECK(!role(f.model, at, "shown").toBool());
    f.model.setAgeFilterSeconds(0);
    CHECK(role(f.model, at, "shown").toBool());

    // Another replay starts: its sample never ages the first one's fixes.
    const uint64_t b = 22U;
    publish(b, {fix_of(8U, 0, capture + 10, DSD_DECODE_CLOCK_REPLAY)},
            sample_of(b, DSD_DECODE_CLOCK_REPLAY, capture + 500, true));
    f.model.poll();
    CHECK(track_of(f.model, 7U)->ageS == -1 && track_of(f.model, 8U)->ageS == 490);
}

/* Live tracks stay when a replay starts (D4), and keep ageing on the wall clock beside the replay's. */
void
test_live_to_replay_retention() {
    Fresh f;
    const uint64_t live = 31U;
    publish(live, run_of(7U, 2, kT0), sample_of(live, DSD_DECODE_CLOCK_SYSTEM, kT0 + 2, true));
    f.model.poll();
    publish(live, {}, sample_of(live, DSD_DECODE_CLOCK_SYSTEM, kT0 + 3, false));
    f.model.poll();
    const uint64_t replay = 32U;
    const int64_t capture = 1500000000;
    publish(replay, {fix_of(7U, 5, capture, DSD_DECODE_CLOCK_REPLAY)},
            sample_of(replay, DSD_DECODE_CLOCK_REPLAY, capture + 4, true));
    g_wall = kT0 + 61;
    f.model.poll();
    // One radio of one system: one track across the runs, its trail broken at the run change, no gap row (the live
    // run was read to its end).
    const PositionModel::Row* seven = track_of(f.model, 7U);
    CHECK(f.model.count() == 1 && gap_rows(f.model) == 0);
    CHECK(seven->trail.size() == 3 && seven->trail.last().breakBefore && !seven->trail.at(1).breakBefore);
    CHECK(seven->ageS == 4);
    publish(replay, {fix_of(9U, 0, kT0 + 50)}, sample_of(replay, DSD_DECODE_CLOCK_REPLAY, capture + 5, true));
    f.model.poll();
    CHECK(track_of(f.model, 9U)->ageS == 11); // a system-clock fix in the same model ages on the wall clock
}

/* A Stop and a replay-to-replay restart between two polls: the old run's last reports were never read, so trails break
 * and the gap row says reports may be missing; the old run's fixes lose their age. */
void
test_restart_between_two_polls() {
    Fresh f;
    const uint64_t a = 41U;
    const int64_t capture = 1500000000;
    publish(a, {fix_of(7U, 0, capture, DSD_DECODE_CLOCK_REPLAY)},
            sample_of(a, DSD_DECODE_CLOCK_REPLAY, capture + 1, true));
    f.model.poll();
    publish(a, {fix_of(7U, 1, capture + 2, DSD_DECODE_CLOCK_REPLAY)},
            sample_of(a, DSD_DECODE_CLOCK_REPLAY, capture + 3, false));
    const uint64_t b = 42U;
    publish(b, {fix_of(7U, 2, capture + 100, DSD_DECODE_CLOCK_REPLAY)},
            sample_of(b, DSD_DECODE_CLOCK_REPLAY, capture + 101, true));
    f.model.poll();
    const PositionModel::Row* seven = track_of(f.model, 7U);
    CHECK(gap_rows(f.model) == 1);
    CHECK(seven->trail.size() == 2 && seven->trail.last().breakBefore);
    CHECK(seven->ageS == 1);
    CHECK(role(f.model, f.model.rowForTrack(0U), "receivedTime").toLongLong() == capture);
}

/* Hidden: the tap reads once a second into its queue; more than it holds drops the oldest with a counted loss, which
 * breaks the trails and shows the gap row after the handoff on show. */
void
test_hidden_overflow() {
    Fresh f;
    const uint64_t s = 51U;
    publish(s, {fix_of(7U, 0, kT0)}, sample_of(s, 0, kT0, true));
    f.model.poll();
    set_app_state(Qt::ApplicationHidden);
    CHECK(f.model.backgroundCaptureRunningForTest());
    f.model.poll(); // a tick while away reads nothing itself
    CHECK(track_of(f.model, 7U)->trail.size() == 1);
    const int total = dsd_qt::PositionTap::kCapacity + 500;
    int published = 0;
    while (published < total) {
        std::vector<dsd_position_fix> batch;
        for (int i = 0; i < 1000 && published < total; ++i, ++published) {
            // Radio 7's first 500 reports are the ones the queue drops.
            batch.push_back(fix_of(published < 600 ? 7U : 2000U + static_cast<uint32_t>(published % 150), published,
                                   kT0 + 1 + published));
        }
        publish(s, batch, sample_of(s, 0, kT0, true));
        f.model.waitForBackgroundPollForTest();
    }
    CHECK(track_of(f.model, 7U)->trail.size() == 1); // nothing reaches the rows until the handoff
    set_app_state(Qt::ApplicationActive);
    f.model.poll();
    CHECK(!f.model.backgroundCaptureRunningForTest());
    CHECK(gap_rows(f.model) == 1);
    CHECK(f.model.count() == 151);
    const PositionModel::Row* seven = track_of(f.model, 7U);
    // The first kept report of radio 7 is number 500; its trail breaks there and joins after (the point from before
    // the break left with the cap).
    CHECK(seven->trail.size() == PositionModel::kPointsPerTrack && seven->trail.first().latE7 == kLat + 500);
    CHECK(seven->trail.first().breakBefore && !seven->trail.at(1).breakBefore);
    CHECK(role(f.model, f.model.rowForTrack(0U), "receivedTime").toLongLong() == kT0);
    // After the handoff the model reads the view again, from where the tap left off.
    publish(s, {fix_of(7U, 9999, kT0 + 99999)}, sample_of(s, 0, kT0, true));
    f.model.poll();
    CHECK(track_of(f.model, 7U)->current.lat_e7 == kLat + 9999);
    CHECK(gap_rows(f.model) == 1);
}

/* Hidden, a stop and a start a poll apart: the tap reads the old run's tail first (its final publish), then the new
 * run; trails break at the run change and nothing is said to be missing. */
void
test_hidden_restart() {
    Fresh f;
    const uint64_t a = 61U;
    publish(a, {fix_of(7U, 0, kT0)}, sample_of(a, 0, kT0, true));
    f.model.poll();
    set_app_state(Qt::ApplicationSuspended);
    CHECK(f.model.backgroundCaptureRunningForTest());
    publish(a, {fix_of(7U, 1, kT0 + 1)}, sample_of(a, 0, kT0 + 1, true));
    f.model.waitForBackgroundPollForTest();
    publish(a, {fix_of(7U, 2, kT0 + 2)}, sample_of(a, 0, kT0 + 2, false)); // the final publish
    publish_idle();
    f.model.waitForBackgroundPollForTest();
    const uint64_t b = 62U;
    publish(b, {fix_of(7U, 3, kT0 + 10)}, sample_of(b, 0, kT0 + 10, true));
    f.model.waitForBackgroundPollForTest();
    set_app_state(Qt::ApplicationActive);
    f.model.poll();
    const PositionModel::Row* seven = track_of(f.model, 7U);
    CHECK(gap_rows(f.model) == 0);
    CHECK(seven->trail.size() == 4);
    CHECK(!seven->trail.at(1).breakBefore && !seven->trail.at(2).breakBefore && seven->trail.at(3).breakBefore);

    // A start inside one tap period (the old run's final reports never read): a gap, said so after the handoff.
    set_app_state(Qt::ApplicationHidden);
    const uint64_t c = 63U;
    {
        const std::lock_guard<std::mutex> lock(g_mu); // no tap poll between the two
        publish_locked(b, {fix_of(7U, 4, kT0 + 11)}, sample_of(b, 0, kT0 + 11, false));
        publish_locked(c, {fix_of(7U, 5, kT0 + 20)}, sample_of(c, 0, kT0 + 20, true));
    }
    f.model.waitForBackgroundPollForTest();
    set_app_state(Qt::ApplicationActive);
    f.model.poll();
    CHECK(gap_rows(f.model) == 1);
    CHECK(track_of(f.model, 7U)->trail.last().latE7 == kLat + 5 && track_of(f.model, 7U)->trail.last().breakBefore);
}

/* A loss the tap found with nothing after it is carried to the handoff: the gap row shows then. */
void
test_hidden_trailing_loss() {
    Fresh f;
    const uint64_t a = 71U;
    publish(a, {fix_of(7U, 0, kT0)}, sample_of(a, 0, kT0, true));
    f.model.poll();
    set_app_state(Qt::ApplicationHidden);
    const uint64_t b = 72U;
    {
        const std::lock_guard<std::mutex> lock(g_mu); // no tap poll between the two
        publish_locked(a, {fix_of(7U, 1, kT0 + 1)}, sample_of(a, 0, kT0 + 1, false));
        publish_locked(b, {}, sample_of(b, 0, kT0 + 2, true)); // a new run with no reports yet
    }
    f.model.waitForBackgroundPollForTest();
    set_app_state(Qt::ApplicationActive);
    f.model.poll();
    CHECK(gap_rows(f.model) == 1);
    publish(b, {fix_of(7U, 2, kT0 + 3)}, sample_of(b, 0, kT0 + 3, true));
    f.model.poll();
    CHECK(track_of(f.model, 7U)->trail.size() == 2 && track_of(f.model, 7U)->trail.last().breakBefore);
}

/* The tap's queue: more than kCapacity records drops the oldest, counts them, and marks the loss on the record then
 * oldest; the read position comes back with the queue. */
void
test_tap_queue_drops_oldest() {
    reset_ring();
    dsd_qt::PositionTap tap(&ring_source);
    tap.setPollIntervalForTest(5);
    dsd_qt::PositionReadState from;
    CHECK(tap.start(from));
    CHECK(!tap.start(from));
    const int total = dsd_qt::PositionTap::kCapacity + 300;
    int published = 0;
    while (published < total) {
        std::vector<dsd_position_fix> batch;
        for (int i = 0; i < 1000 && published < total; ++i, ++published) {
            batch.push_back(fix_of(7U, published, kT0 + published));
        }
        publish(91U, batch, sample_of(91U, 0, kT0, true));
        tap.waitForPollForTest();
    }
    const dsd_qt::PositionTap::Result r = tap.stop();
    CHECK(!tap.running() && r.ran);
    CHECK(r.dropped == 300U && r.count == dsd_qt::PositionTap::kCapacity);
    CHECK(r.fixAt(0).seq == 301U && r.gapAt(0) == (dsd_qt::kPositionGapBreak | dsd_qt::kPositionGapLoss));
    CHECK(r.gapAt(1) == 0U && r.fixAt(r.count - 1).seq == static_cast<uint64_t>(total));
    CHECK(r.read.stream == 91U && r.read.seq == static_cast<uint64_t>(total) && r.trailingGap == 0U);
    CHECK(!tap.stop().ran);
}

/* position_read_page(): a reset after a run read to its end is a break alone; otherwise a break and a loss. */
void
test_read_page_marks() {
    reset_ring();
    dsd_position_fix page[8];
    dsd_qt::PositionReadState state;
    publish(81U, {fix_of(7U, 0, kT0)}, sample_of(81U, 0, kT0, true));
    dsd_qt::PositionPage p = dsd_qt::position_read_page(&ring_source, state, page, 8);
    CHECK(p.count == 1 && p.gap == 0U && state.stream == 81U && state.seq == 1U && !state.ended);
    publish(81U, {}, sample_of(81U, 0, kT0, false));
    p = dsd_qt::position_read_page(&ring_source, state, page, 8);
    CHECK(p.count == 0 && state.ended);
    publish(82U, {fix_of(7U, 1, kT0)}, sample_of(82U, 0, kT0, true));
    p = dsd_qt::position_read_page(&ring_source, state, page, 8);
    CHECK(p.count == 1 && p.gap == dsd_qt::kPositionGapBreak && state.stream == 82U);
    publish(83U, {fix_of(7U, 2, kT0)}, sample_of(83U, 0, kT0, true));
    p = dsd_qt::position_read_page(&ring_source, state, page, 8);
    CHECK(p.count == 1 && p.gap == (dsd_qt::kPositionGapBreak | dsd_qt::kPositionGapLoss));
    CHECK(dsd_qt::position_read_page(nullptr, state, page, 8).count == -1);
}

/* PR2 review: a clear is a boundary at what is published now. A report published after the last poll but before the
 * clear belongs to before it, and never reappears; one published after it does. */
void
test_clear_boundary() {
    Fresh f;
    const uint64_t s = 31U;
    publish(s, {fix_of(7U, 0, kT0)}, sample_of(s, 0, kT0, true));
    f.model.poll();
    CHECK(f.model.count() == 1);
    publish(s, {fix_of(8U, 0, kT0 + 1)}, sample_of(s, 0, kT0 + 1, true)); /* not polled yet */
    f.model.clear();
    f.model.poll();
    CHECK(f.model.count() == 0);
    publish(s, {fix_of(9U, 0, kT0 + 2)}, sample_of(s, 0, kT0 + 2, true));
    f.model.poll();
    CHECK(f.model.count() == 1 && track_of(f.model, 9U) != nullptr && track_of(f.model, 8U) == nullptr);
    /* Nothing published at all: a clear moves nothing and breaks nothing. */
    Fresh g;
    g.model.clear();
    g.model.poll();
    CHECK(g.model.count() == 0 && gap_rows(g.model) == 0);
}

/* PR2 review: LIP's time elapsed (how old the position was when sent) reaches the fix record, so the sheet can say it
 * when the report has no time of its own. */
void
test_age_at_send() {
    Fresh f;
    const uint64_t s = 32U;
    dsd_position_fix lip = fix_of(7U, 0, kT0);
    lip.kind = DSD_POS_LIP;
    lip.age_class = 2U; /* under 30 minutes */
    dsd_position_fix unknown_age = fix_of(8U, 0, kT0);
    unknown_age.kind = DSD_POS_LIP;
    unknown_age.age_class = 3U;                 /* not known */
    dsd_position_fix none = fix_of(9U, 0, kT0); /* none sent (DSD_POSITION_AGE_CLASS_NONE) */
    dsd_position_fix fresh = fix_of(10U, 0, kT0);
    fresh.age_class = 0U; /* under 5 seconds */
    publish(s, {lip, unknown_age, none, fresh}, sample_of(s, 0, kT0, true));
    f.model.poll();
    const auto record = [&f](uint32_t source) {
        return f.model.trackFor(track_of(f.model, source)->id).value(QStringLiteral("currentFix")).toMap();
    };
    CHECK(record(7U).value(QStringLiteral("ageAtSendMaxSeconds")).toInt() == 1800);
    CHECK(!record(8U).contains(QStringLiteral("ageAtSendMaxSeconds")));
    CHECK(!record(9U).contains(QStringLiteral("ageAtSendMaxSeconds")));
    CHECK(record(10U).value(QStringLiteral("ageAtSendMaxSeconds")).toInt() == 5);
}

/* PR2 review: a background tap whose thread cannot be had (a process out of threads) reports that it did not start,
 * rather than throwing through the application-state handler. Run in a child process held to one process by
 * RLIMIT_NPROC, so no thread can be made there. */
void
test_tap_without_a_thread() {
#if defined(__linux__)
    if (geteuid() == 0) {
        return; /* root is not held by RLIMIT_NPROC */
    }
    std::fflush(stdout);
    const pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        rlimit one{1, 1};
        if (setrlimit(RLIMIT_NPROC, &one) != 0) {
            _exit(3);
        }
        PositionTap tap(&ring_source);
        const bool started = tap.start(PositionReadState{});
        _exit((!started && !tap.running()) ? 0 : 2);
    }
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
#endif
}

/* PR2 review 2: a measurement time ahead of the receive clock (a radio clock ahead; a replay whose clock has no UTC
 * anchor) cannot be compared: no age ("Measured at" its time), never "just now". One in the past ages as before. */
void
test_future_measurement_has_no_age() {
    Fresh f;
    const uint64_t s = 33U;
    dsd_position_fix ahead = fix_of(7U, 0, kT0);
    ahead.fix_time_s = kT0 + 100;
    dsd_position_fix behind = fix_of(8U, 0, kT0);
    behind.fix_time_s = kT0 - 40;
    g_wall = kT0;
    publish(s, {ahead, behind}, sample_of(s, DSD_DECODE_CLOCK_SYSTEM, kT0, true));
    f.model.poll();
    CHECK(role(f.model, f.model.rowForTrack(track_of(f.model, 7U)->id), "reportAgeSeconds").toLongLong() == -1);
    CHECK(role(f.model, f.model.rowForTrack(track_of(f.model, 8U)->id), "reportAgeSeconds").toLongLong() == 40);
    CHECK(role(f.model, f.model.rowForTrack(track_of(f.model, 7U)->id), "ageSeconds").toLongLong() == 0);

    /* A second or two ahead is clock skew (GPS time against a phone clock a little behind): "now", age 0. */
    Fresh g;
    const uint64_t s2 = 34U;
    dsd_position_fix skewed = fix_of(9U, 0, kT0);
    skewed.fix_time_s = kT0 + 1;
    g_wall = kT0;
    publish(s2, {skewed}, sample_of(s2, DSD_DECODE_CLOCK_SYSTEM, kT0, true));
    g.model.poll();
    CHECK(role(g.model, g.model.rowForTrack(track_of(g.model, 9U)->id), "reportAgeSeconds").toLongLong() == 0);

    /* A replay whose clock has no UTC anchor (it starts in 2000) against a measurement stamped years later: no age,
     * while the receive age runs on the replay clock. */
    Fresh h;
    const uint64_t s3 = 35U;
    const int64_t replay_now = 946684800 + 600; /* 2000-01-01, ten minutes in */
    dsd_position_fix replayed = fix_of(10U, 0, replay_now - 30, DSD_DECODE_CLOCK_REPLAY);
    replayed.fix_time_s = kT0;
    publish(s3, {replayed}, sample_of(s3, DSD_DECODE_CLOCK_REPLAY, replay_now, true));
    h.model.poll();
    const int row = h.model.rowForTrack(track_of(h.model, 10U)->id);
    CHECK(role(h.model, row, "reportAgeSeconds").toLongLong() == -1);
    CHECK(role(h.model, row, "ageSeconds").toLongLong() == 30);
}

} // namespace

int
main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    test_read_page_marks();
    test_tap_queue_drops_oldest();
    test_tracks_by_typed_source();
    test_anonymous_points();
    test_shown_fix_records();
    test_filter_track_count();
    test_validity_unknown_and_provisional();
    test_caps();
    test_loss_breaks_trails_and_one_gap_row();
    test_clear();
    test_clear_boundary();
    test_age_at_send();
    test_future_measurement_has_no_age();
    test_tap_without_a_thread();
    test_system_ages_and_filter();
    test_replay_ages();
    test_live_to_replay_retention();
    test_restart_between_two_polls();
    test_hidden_overflow();
    test_hidden_restart();
    test_hidden_trailing_loss();
    (void)DSD_FPRINTF(stdout, "UI_QT_POSITION_MODEL: OK\n");
    return 0;
}
