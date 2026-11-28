// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/*
 * The site roaming decision core and the call wait cap, driven on a scripted clock, then the executor (SiteRoaming)
 * over a scripted host and the same kind of clock. Distances are made up and
 * generous; where a case needs a distance near the margin it measures it with site_distance_km itself, so the cases
 * hold for any accurate distance formula.
 */
#include <QByteArray>
#include <QCoreApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QLatin1String>
#include <QList>
#include <QMap>
#include <QObject>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariant>
#include <QVariantMap>
#include <Qt>
#include <QtGlobal>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <string>
#include <utility>
#include <vector>
#include "../test_support/qt_test_paths.h"
#include "app_prefs.h"
#include "decoder_host.h"
#include "location_feed.h"
#include "saved_systems_model.h"
#include "site_groups.h"
#include "site_roaming.h"
#include "site_roaming_controller.h"

namespace {

using dsd_qt::CallWait;
using dsd_qt::CallWaitClock;
using dsd_qt::SiteRoamingCandidate;
using dsd_qt::SiteRoamingCore;
using dsd_qt::SiteRoamingMeasurement;
using dsd_qt::SiteRoamingPause;
using dsd_qt::SiteRoamingSettings;
using dsd_qt::SiteRoamingVerdict;
using Kind = SiteRoamingVerdict::Kind;
using Sites = std::vector<SiteRoamingCandidate>;

int g_failures = 0;

void
check(bool ok, const char* label) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", label);
        ++g_failures;
    }
}

const SiteRoamingSettings kSettings{100, 2};

// Sites A and B lie about 55 km apart on one meridian; C lies east of B, about 50 km from it.
SiteRoamingCandidate
site(const char* uid, double lat, double lon) {
    SiteRoamingCandidate c;
    c.uid = uid;
    c.has_position = true;
    c.lat = lat;
    c.lon = lon;
    c.roamable = true;
    return c;
}

SiteRoamingCandidate
siteA() {
    return site("A", 10.0, 20.0);
}

SiteRoamingCandidate
siteB() {
    return site("B", 10.5, 20.0);
}

SiteRoamingCandidate
siteC() {
    return site("C", 10.5, 20.45);
}

// A new fix: its elapsed-realtime stamp follows its fix time unless the case says otherwise.
SiteRoamingMeasurement
fix(double lat, double lon, std::int64_t at_ms, double accuracy_m = 10) {
    SiteRoamingMeasurement m;
    m.id.provider = "gps";
    m.id.elapsed_realtime_ns = at_ms * 1000000 + 1;
    m.lat = lat;
    m.lon = lon;
    m.has_accuracy = true;
    m.accuracy_m = accuracy_m;
    m.fix_at_ms = at_ms;
    return m;
}

// Deep in B's area: far nearer to B than to A or C.
SiteRoamingMeasurement
nearB(std::int64_t at_ms) {
    return fix(10.48, 20.0, at_ms);
}

bool
is(const SiteRoamingVerdict& v, Kind kind, const char* uid) {
    return v.kind == kind && v.uid == uid;
}

bool
paused(const SiteRoamingVerdict& v, SiteRoamingPause reason) {
    return v.kind == Kind::Paused && v.reason == reason && v.uid.empty();
}

double
gap_km(const SiteRoamingCandidate& from, const SiteRoamingCandidate& to, double lat, double lon) {
    return dsd_qt::site_distance_km(lat, lon, from.lat, from.lon) - dsd_qt::site_distance_km(lat, lon, to.lat, to.lon);
}

// Feed new fixes near B at the given fix times (evaluated at their fix time) and return the last verdict.
SiteRoamingVerdict
feed_near_b(SiteRoamingCore& core, const Sites& sites, const std::vector<std::int64_t>& times) {
    SiteRoamingVerdict v;
    for (const std::int64_t t : times) {
        const SiteRoamingMeasurement m = nearB(t);
        v = core.evaluate(t, sites, "A", &m, kSettings);
    }
    return v;
}

// The verdict at fix @p i of the route, @p first_qualifying being the first fix past the margin (-1: none yet).
void
check_route_verdict(const SiteRoamingCore& core, const SiteRoamingVerdict& v, const std::string& current, int i,
                    int first_qualifying) {
    if (current != "A") {
        check(is(v, Kind::Following, "B"), "route: Following B after the switch");
        return;
    }
    if (first_qualifying < 0) {
        check(is(v, Kind::Following, "A") && core.evidence().count == 0, "route: Following until past the margin");
        return;
    }
    const int step = i - first_qualifying;
    check(step < 2 ? is(v, Kind::Candidate, "B") : is(v, Kind::Switch, "B"),
          "route: Candidate for two qualifying fixes, Switch at the third");
    check(core.evidence().count == step + 1
              && core.evidence().last_ms - core.evidence().first_ms == std::int64_t{10000} * step,
          "route: evidence counts the qualifying fixes and spans their fix times");
}

// A 1 s timer for the 9 s after fix @p m: each tick derives the same verdict and adds no evidence.
void
check_route_ticks(SiteRoamingCore& core, const Sites& sites, const std::string& current,
                  const SiteRoamingMeasurement& m, const SiteRoamingVerdict& v) {
    for (int tick = 1; tick < 10; ++tick) {
        const int count = core.evidence().count;
        const SiteRoamingVerdict ticked =
            core.evaluate(m.fix_at_ms + std::int64_t{1000} * tick, sites, current, &m, kSettings);
        check(ticked.kind == v.kind && ticked.uid == v.uid && core.evidence().count == count,
              "route: a timer tick adds nothing");
    }
}

/* Route A -> B: Following until B is nearer by margin + 2 x accuracy, then Candidate, and one Switch exactly at the
 * third qualifying measurement, 20 s after the first. A 1 s timer between fixes never changes the evidence. */
void
check_route() {
    SiteRoamingCore core;
    const Sites sites{siteA(), siteB()};
    std::string current = "A";
    int switches = 0;
    int first_qualifying = -1;
    for (int i = 0; i <= 60; ++i) {
        const double lat = 10.0 + 0.01 * i;
        if (first_qualifying < 0 && gap_km(siteA(), siteB(), lat, 20.0) >= 2 + 2 * 0.010) {
            first_qualifying = i;
        }
        const SiteRoamingMeasurement m = fix(lat, 20.0, 10000LL * i);
        const SiteRoamingVerdict v = core.evaluate(m.fix_at_ms, sites, current, &m, kSettings);
        check_route_verdict(core, v, current, i, first_qualifying);
        if (v.kind == Kind::Switch) {
            ++switches;
            current = v.uid;
        } else {
            check_route_ticks(core, sites, current, m, v);
        }
    }
    check(first_qualifying > 0 && first_qualifying < 50, "route: the route passes the margin");
    check(switches == 1 && current == "B", "route: exactly one switch, to B");
}

/* A switch needs both: three measurements spanning less than 20 s do not qualify, nor do two spanning more. */
void
check_dwell_needs_span_and_count() {
    const Sites sites{siteA(), siteB()};
    {
        SiteRoamingCore core;
        check(is(feed_near_b(core, sites, {0, 10000, 19999}), Kind::Candidate, "B") && core.evidence().count == 3,
              "dwell: three fixes over 19.999 s are a Candidate");
        const SiteRoamingMeasurement last = nearB(19999);
        check(is(core.evaluate(20000, sites, "A", &last, kSettings), Kind::Candidate, "B"),
              "dwell: the clock passing 20 s adds no span");
        check(is(feed_near_b(core, sites, {20000}), Kind::Switch, "B") && core.evidence().count == 4,
              "dwell: a fourth fix at 20 s switches");
    }
    {
        SiteRoamingCore core;
        check(is(feed_near_b(core, sites, {0, 25000}), Kind::Candidate, "B") && core.evidence().count == 2,
              "dwell: two fixes over 25 s are a Candidate");
        check(is(feed_near_b(core, sites, {26000}), Kind::Switch, "B"), "dwell: a third fix switches");
        const SiteRoamingMeasurement last = nearB(26000);
        check(is(core.evaluate(27000, sites, "A", &last, kSettings), Kind::Switch, "B"),
              "dwell: a timer tick derives the Switch again");
    }
}

/* Clustered fixes then silence: the evidence never spans the dwell, and the fix expires after 60 s. */
void
check_cluster_then_silence() {
    SiteRoamingCore core;
    const Sites sites{siteA(), siteB()};
    check(is(feed_near_b(core, sites, {0, 500, 1000}), Kind::Candidate, "B"), "cluster: three quick fixes");
    const SiteRoamingMeasurement last = nearB(1000);
    bool switched = false;
    for (std::int64_t now = 2000; now <= 61000; now += 1000) {
        const SiteRoamingVerdict v = core.evaluate(now, sites, "A", &last, kSettings);
        switched = switched || v.kind == Kind::Switch;
        check(is(v, Kind::Candidate, "B") && core.evidence().count == 3, "cluster: Candidate while the fix is fresh");
    }
    check(!switched, "cluster: silence never qualifies");
    check(paused(core.evaluate(61001, sites, "A", &last, kSettings), SiteRoamingPause::StaleFix),
          "cluster: the fix expires 60 s after it was taken");
    check(core.evidence().count == 0, "cluster: expiry clears the evidence");
    check(paused(core.evaluate(62000, sites, "A", &last, kSettings), SiteRoamingPause::StaleFix),
          "cluster: and stays paused");
}

/* A replayed cached fix (same provider and stamp) adds nothing, however often or late it arrives, even restamped, and
 * neither does one fix relayed by another provider (its stamp within 5 ms); a new fix at identical coordinates counts. */
void
check_replayed_and_identical_fixes() {
    SiteRoamingCore core;
    const Sites sites{siteA(), siteB()};
    const SiteRoamingMeasurement first = nearB(0);
    core.evaluate(0, sites, "A", &first, kSettings);
    for (std::int64_t t = 10000; t <= 40000; t += 10000) {
        SiteRoamingMeasurement replay = first;
        replay.fix_at_ms = t;
        check(is(core.evaluate(t, sites, "A", &replay, kSettings), Kind::Candidate, "B") && core.evidence().count == 1,
              "replay: a replayed fix adds no evidence");
    }
    SiteRoamingMeasurement older = nearB(41000);
    older.id.elapsed_realtime_ns = first.id.elapsed_realtime_ns - 1;
    core.evaluate(41000, sites, "A", &older, kSettings);
    check(core.evidence().count == 1, "replay: an older stamp from the same provider is not new");
    // The fused provider re-reports a GPS fix under its own name with the GPS stamp: one physical fix, counted once.
    SiteRoamingMeasurement relay = nearB(42000);
    relay.id.provider = "fused";
    relay.id.elapsed_realtime_ns = first.id.elapsed_realtime_ns;
    core.evaluate(42000, sites, "A", &relay, kSettings);
    check(core.evidence().count == 1, "replay: the same stamp from another provider is the same fix");
    relay.id.elapsed_realtime_ns = first.id.elapsed_realtime_ns + dsd_qt::kSameFixNs;
    core.evaluate(42000, sites, "A", &relay, kSettings);
    check(core.evidence().count == 1, "replay: a stamp within 5 ms of another provider's newest is not new");
    SiteRoamingMeasurement other = nearB(43000);
    other.id.provider = "fused";
    other.id.elapsed_realtime_ns = first.id.elapsed_realtime_ns + dsd_qt::kSameFixNs + 1;
    core.evaluate(43000, sites, "A", &other, kSettings);
    check(core.evidence().count == 2, "replay: a stamp more than 5 ms from another provider's newest is new");
    const SiteRoamingMeasurement same_place = nearB(50000);
    check(is(core.evaluate(50000, sites, "A", &same_place, kSettings), Kind::Switch, "B") && core.evidence().count == 3
              && core.evidence().last_ms - core.evidence().first_ms == 50000,
          "replay: a new fix at identical coordinates counts");
}

/* Ten minutes of fixes either side of the midpoint between A and B (gap well under the margin), and ten minutes
 * alternating a deep and a boundary fix: zero switches, with a 1 s timer between fixes. */
int
jitter_switches(const std::vector<double>& lats, const char* start) {
    SiteRoamingCore core;
    const Sites sites{siteA(), siteB()};
    std::string current = start;
    int switches = 0;
    for (std::size_t i = 0; i < lats.size(); ++i) {
        const std::int64_t t = 10000LL * static_cast<std::int64_t>(i);
        const SiteRoamingMeasurement m = fix(lats[i], 20.0, t, 25);
        for (int tick = 0; tick < 10; ++tick) {
            const SiteRoamingVerdict v = core.evaluate(t + std::int64_t{1000} * tick, sites, current, &m, kSettings);
            if (v.kind == Kind::Switch) {
                ++switches;
                current = v.uid;
            }
        }
    }
    return switches;
}

void
check_boundary_jitter() {
    std::vector<double> boundary;
    std::vector<double> alternating;
    std::uint32_t seed = 12345;
    bool small_gaps = true;
    for (int i = 0; i <= 60; ++i) {
        seed = seed * 1103515245u + 12345u;
        const double offset = (static_cast<double>((seed >> 8) % 801) - 400.0) * 0.00001; // within +-0.004 deg
        const double lat = 10.25 + offset;
        const double gap = gap_km(siteA(), siteB(), lat, 20.0);
        small_gaps = small_gaps && gap < 1.5 && gap > -1.5;
        boundary.push_back(lat);
        alternating.push_back(i % 2 == 0 ? 10.45 : 10.25);
    }
    check(small_gaps, "jitter: the boundary fixes stay well inside the margin");
    check(jitter_switches(boundary, "A") == 0, "jitter: no switch from A on the boundary");
    check(jitter_switches(boundary, "B") == 0, "jitter: no switch from B on the boundary");
    check(jitter_switches(alternating, "A") == 0, "jitter: a boundary fix restarts the evidence every time");
}

/* The winner changes from B to C mid-dwell: C starts its own evidence. A list change mid-dwell only revokes. */
void
check_winner_change() {
    const Sites sites{siteA(), siteB(), siteC()};
    {
        SiteRoamingCore core;
        const SiteRoamingMeasurement b0 = fix(10.5, 20.05, 0);
        const SiteRoamingMeasurement b1 = fix(10.5, 20.05, 10000);
        core.evaluate(0, sites, "A", &b0, kSettings);
        check(is(core.evaluate(10000, sites, "A", &b1, kSettings), Kind::Candidate, "B") && core.evidence().count == 2,
              "winner: B builds evidence");
        std::vector<SiteRoamingVerdict> verdicts;
        for (std::int64_t t = 20000; t <= 40000; t += 10000) {
            const SiteRoamingMeasurement c = fix(10.5, 20.40, t);
            verdicts.push_back(core.evaluate(t, sites, "A", &c, kSettings));
        }
        check(is(verdicts[0], Kind::Candidate, "C") && is(verdicts[1], Kind::Candidate, "C")
                  && is(verdicts[2], Kind::Switch, "C"),
              "winner: C restarts the dwell and switches only on its own span");
        check(core.evidence().uid == "C" && core.evidence().first_ms == 20000, "winner: C's evidence starts at C");
    }
    {
        SiteRoamingCore core;
        Sites changed = sites;
        const SiteRoamingMeasurement b0 = fix(10.5, 20.05, 0);
        const SiteRoamingMeasurement b1 = fix(10.5, 20.05, 10000);
        core.evaluate(0, sites, "A", &b0, kSettings);
        core.evaluate(10000, sites, "A", &b1, kSettings);
        changed[1].avoid = true;
        check(is(core.evaluate(11000, changed, "A", &b1, kSettings), Kind::Following, "A")
                  && core.evidence().count == 0,
              "winner: avoiding B mid-dwell revokes, and the seen fix gives C nothing");
        check(is(core.evaluate(12000, sites, "A", &b1, kSettings), Kind::Following, "A"),
              "winner: lifting the avoid does not bring the evidence back");
        const SiteRoamingMeasurement b2 = fix(10.5, 20.05, 20000);
        check(is(core.evaluate(20000, sites, "A", &b2, kSettings), Kind::Candidate, "B") && core.evidence().count == 1,
              "winner: the next new fix starts again");
    }
}

/* reset() clears the evidence and the memory of seen fixes. */
void
check_reset() {
    SiteRoamingCore core;
    const Sites sites{siteA(), siteB()};
    feed_near_b(core, sites, {0, 10000});
    check(core.evidence().count == 2, "reset: evidence before");
    core.reset();
    check(core.evidence().count == 0 && core.evidence().uid.empty(), "reset: evidence cleared");
    const SiteRoamingMeasurement last = nearB(10000);
    check(is(core.evaluate(11000, sites, "A", &last, kSettings), Kind::Candidate, "B") && core.evidence().count == 1,
          "reset: a fix seen before the reset is new again");
}

/* Avoided, unpositioned (flag off, or at 0,0) and non-roamable candidates are never ranked. */
void
check_ineligible_candidates() {
    const SiteRoamingCandidate d = site("D", 10.6, 20.0);
    const SiteRoamingMeasurement m = fix(10.5, 20.0, 0);
    std::vector<SiteRoamingCandidate> variants(4, siteB());
    variants[0].avoid = true;
    variants[1].has_position = false;
    variants[2].lat = 0;
    variants[2].lon = 0;
    variants[3].roamable = false;
    for (const SiteRoamingCandidate& b : variants) {
        SiteRoamingCore core;
        check(is(core.evaluate(0, Sites{siteA(), b, d}, "A", &m, kSettings), Kind::Candidate, "D"),
              "ineligible: the next eligible site is ranked instead");
        SiteRoamingCandidate a = siteA();
        a.avoid = true;
        SiteRoamingCandidate far = d;
        far.roamable = false;
        check(paused(core.evaluate(1000, Sites{a, b, far}, "A", &m, kSettings), SiteRoamingPause::NoRankableSite),
              "ineligible: no rankable site");
    }
    SiteRoamingCore core;
    check(paused(core.evaluate(0, Sites{}, "A", &m, kSettings), SiteRoamingPause::NoRankableSite),
          "ineligible: an empty list");
}

/* A current site that is avoided, unpositioned, non-roamable or not listed needs no margin; the dwell still holds. */
void
check_current_ineligible() {
    double lat = 10.25;
    while (gap_km(siteA(), siteB(), lat, 20.0) < 0.5) {
        lat += 0.0005;
    }
    check(gap_km(siteA(), siteB(), lat, 20.0) < 1.0, "current: B nearer, but by less than the margin");
    {
        SiteRoamingCore core;
        const SiteRoamingMeasurement m = fix(lat, 20.0, 0);
        check(is(core.evaluate(0, Sites{siteA(), siteB()}, "A", &m, kSettings), Kind::Following, "A"),
              "current: an eligible current site keeps the margin");
    }
    std::vector<SiteRoamingCandidate> variants(3, siteA());
    variants[0].avoid = true;
    variants[1].has_position = false;
    variants[2].roamable = false;
    for (const SiteRoamingCandidate& a : variants) {
        SiteRoamingCore core;
        for (std::int64_t t = 0; t <= 20000; t += 10000) {
            const SiteRoamingMeasurement m = fix(lat, 20.0, t);
            const SiteRoamingVerdict v = core.evaluate(t, Sites{a, siteB()}, "A", &m, kSettings);
            check(t < 20000 ? is(v, Kind::Candidate, "B") : is(v, Kind::Switch, "B"),
                  "current: an ineligible current site needs no margin, but the dwell");
        }
    }
    {
        SiteRoamingCore core;
        const SiteRoamingMeasurement m = fix(lat, 20.0, 0);
        check(is(core.evaluate(0, Sites{siteA(), siteB()}, "Z", &m, kSettings), Kind::Candidate, "B"),
              "current: a current site not listed needs no margin");
    }
    SiteRoamingCore core;
    SiteRoamingCandidate avoided = siteA();
    avoided.avoid = true;
    const SiteRoamingMeasurement at_a = fix(10.0, 20.0, 0);
    check(is(core.evaluate(0, Sites{avoided, siteB()}, "A", &at_a, kSettings), Kind::Candidate, "B"),
          "current: an avoided current site is left even where it is nearest");
}

/* A nearest site farther than the maximum pauses and clears the evidence. */
void
check_out_of_range() {
    const Sites sites{siteA(), siteB()};
    {
        SiteRoamingCore core;
        feed_near_b(core, sites, {0, 10000});
        // About 110 km north of B, the nearer site.
        const SiteRoamingMeasurement far = fix(11.5, 20.0, 20000);
        check(paused(core.evaluate(20000, sites, "A", &far, kSettings), SiteRoamingPause::OutOfRange),
              "range: nearest site past 100 km");
        check(core.evidence().count == 0, "range: out of range clears the evidence");
        check(is(feed_near_b(core, sites, {30000}), Kind::Candidate, "B") && core.evidence().count == 1,
              "range: in range again starts over");
    }
    SiteRoamingCore core;
    const SiteRoamingMeasurement middle = fix(10.9, 20.0, 0);
    check(paused(core.evaluate(0, sites, "A", &middle, SiteRoamingSettings{25, 2}), SiteRoamingPause::OutOfRange),
          "range: about 45 km is past 25 km");
    check(is(core.evaluate(1000, sites, "A", &middle, kSettings), Kind::Following, "A"),
          "range: within 100 km, though the seen fix adds no evidence");
}

/* No fix, a stale fix and an imprecise one pause, and each clears the evidence and any Switch. */
void
check_fix_quality() {
    const Sites sites{siteA(), siteB()};
    SiteRoamingCore core;
    check(paused(core.evaluate(0, sites, "A", nullptr, kSettings), SiteRoamingPause::NoFix), "fix: none");
    const SiteRoamingMeasurement null_island = fix(0, 0, 0);
    const SiteRoamingMeasurement off_globe = fix(95, 20, 0);
    check(paused(core.evaluate(0, sites, "A", &null_island, kSettings), SiteRoamingPause::NoFix)
              && paused(core.evaluate(0, sites, "A", &off_globe, kSettings), SiteRoamingPause::NoFix),
          "fix: an invalid position is no fix");
    const SiteRoamingMeasurement m = nearB(1000);
    check(is(core.evaluate(61000, sites, "A", &m, kSettings), Kind::Candidate, "B"), "fix: 60 s old is fresh");
    check(paused(core.evaluate(61001, sites, "A", &m, kSettings), SiteRoamingPause::StaleFix), "fix: past 60 s");
    SiteRoamingMeasurement edge = nearB(70000);
    edge.accuracy_m = 1000;
    check(is(core.evaluate(70000, sites, "A", &edge, kSettings), Kind::Candidate, "B"), "fix: 1000 m is precise");
    std::vector<SiteRoamingMeasurement> imprecise(3, nearB(80000));
    imprecise[0].has_accuracy = false;
    imprecise[1].accuracy_m = 1000.5;
    imprecise[2].accuracy_m = -1;
    for (std::size_t i = 0; i < imprecise.size(); ++i) {
        SiteRoamingCore fresh;
        check(is(feed_near_b(fresh, sites, {50000, 60000, 70000}), Kind::Switch, "B"), "fix: a Switch first");
        imprecise[i].id.elapsed_realtime_ns += static_cast<std::int64_t>(i);
        check(paused(fresh.evaluate(80000, sites, "A", &imprecise[i], kSettings), SiteRoamingPause::ImpreciseFix),
              "fix: accuracy missing, past 1000 m or negative");
        check(fresh.evidence().count == 0, "fix: an imprecise fix clears a Switch");
        check(is(feed_near_b(fresh, sites, {90000}), Kind::Candidate, "B") && fresh.evidence().count == 1,
              "fix: a precise fix after it starts over");
    }
}

/* The accuracy radius widens the margin: two accuracy radii are added to it. */
void
check_accuracy_widens_margin() {
    double lat = 10.25;
    while (gap_km(siteA(), siteB(), lat, 20.0) < 3.2) {
        lat += 0.0005;
    }
    check(gap_km(siteA(), siteB(), lat, 20.0) < 3.4, "accuracy: B nearer by about 3.2 km");
    SiteRoamingCore core;
    const Sites sites{siteA(), siteB()};
    const SiteRoamingMeasurement sharp = fix(lat, 20.0, 0, 10);
    check(is(core.evaluate(0, sites, "A", &sharp, kSettings), Kind::Candidate, "B"), "accuracy: 2 + 0.02 km");
    const SiteRoamingMeasurement blurred = fix(lat, 20.0, 10000, 900);
    check(is(core.evaluate(10000, sites, "A", &blurred, kSettings), Kind::Following, "A") && core.evidence().count == 0,
          "accuracy: 2 + 1.8 km is not reached, and the evidence restarts");
}

/* Equal distances go to the first site in row order, and a tie with the current site does not switch. */
void
check_ties() {
    SiteRoamingCandidate twin = siteB();
    twin.uid = "B2";
    const SiteRoamingMeasurement m = nearB(0);
    {
        SiteRoamingCore core;
        check(is(core.evaluate(0, Sites{siteA(), siteB(), twin}, "A", &m, kSettings), Kind::Candidate, "B"),
              "ties: first in row order");
    }
    {
        SiteRoamingCore core;
        check(is(core.evaluate(0, Sites{siteA(), twin, siteB()}, "A", &m, kSettings), Kind::Candidate, "B2"),
              "ties: row order, not uid");
    }
    SiteRoamingCore core;
    check(is(core.evaluate(0, Sites{siteA(), siteB(), twin}, "B2", &m, kSettings), Kind::Following, "B2")
              && core.evidence().count == 0,
          "ties: a twin of the current site is not worth a switch");
}

/* A settings change revokes the evidence, even with the same fix and a margin it still clears; equal settings do
 * not. */
void
check_settings_revoke() {
    const Sites sites{siteA(), siteB()};
    SiteRoamingCore core;
    feed_near_b(core, sites, {0, 10000});
    const SiteRoamingMeasurement last = nearB(10000);
    check(is(core.evaluate(11000, sites, "A", &last, SiteRoamingSettings{100, 2}), Kind::Candidate, "B")
              && core.evidence().count == 2,
          "settings: equal settings keep the evidence");
    check(is(core.evaluate(12000, sites, "A", &last, SiteRoamingSettings{100, 5}), Kind::Following, "A")
              && core.evidence().count == 0,
          "settings: a new margin revokes");
    feed_near_b(core, sites, {20000, 30000});
    check(is(feed_near_b(core, sites, {40000}), Kind::Switch, "B"), "settings: a Switch under the old settings");
    const SiteRoamingMeasurement switched = nearB(40000);
    check(is(core.evaluate(41000, sites, "A", &switched, SiteRoamingSettings{200, 2}), Kind::Following, "A")
              && core.evidence().count == 0,
          "settings: a new maximum revokes a Switch");
}

/* A fix stamped after the evaluation time (another clock) is never fresh: it pauses and clears the evidence. */
void
check_future_fix() {
    const Sites sites{siteA(), siteB()};
    SiteRoamingCore core;
    feed_near_b(core, sites, {0, 10000});
    const SiteRoamingMeasurement ahead = nearB(20001);
    check(paused(core.evaluate(20000, sites, "A", &ahead, kSettings), SiteRoamingPause::StaleFix)
              && core.evidence().count == 0,
          "future: a fix 1 ms ahead pauses and clears the evidence");
    // A fix time converted from the wrong clock; its stamp is the provider's own.
    SiteRoamingMeasurement far_ahead = nearB(25000);
    far_ahead.fix_at_ms = 10000000;
    for (std::int64_t now = 30000; now <= 200000; now += 10000) {
        check(paused(core.evaluate(now, sites, "A", &far_ahead, kSettings), SiteRoamingPause::StaleFix),
              "future: a fix far ahead never counts as fresh");
    }
    check(is(feed_near_b(core, sites, {210000}), Kind::Candidate, "B") && core.evidence().count == 1,
          "future: a fix on the right clock starts over");
}

/* A maximum or margin that is not finite and above zero pauses, clears the evidence and is never stored, so a later
 * change between valid settings still revokes. */
void
check_invalid_settings() {
    const Sites sites{siteA(), siteB()};
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    const std::vector<SiteRoamingSettings> invalid{{nan, 2},   {inf, 2},   {0, 2},   {-100, 2},
                                                   {100, nan}, {100, inf}, {100, 0}, {100, -2}};
    for (std::size_t i = 0; i < invalid.size(); ++i) {
        SiteRoamingCore core;
        feed_near_b(core, sites, {0, 10000});
        const SiteRoamingMeasurement m = nearB(20000);
        check(paused(core.evaluate(20000, sites, "A", &m, invalid[i]), SiteRoamingPause::InvalidSettings)
                  && core.evidence().count == 0,
              "settings: an invalid maximum or margin pauses and clears the evidence");
        check(is(feed_near_b(core, sites, {30000, 40000}), Kind::Candidate, "B") && core.evidence().count == 2,
              "settings: valid settings build evidence again");
        const SiteRoamingMeasurement last = nearB(40000);
        check(is(core.evaluate(41000, sites, "A", &last, SiteRoamingSettings{100, 5}), Kind::Following, "A")
                  && core.evidence().count == 0,
              "settings: a change after an invalid interlude still revokes");
    }
}

/* The call wait: Clear while no call is followed, Waiting from the first busy observation, Expired after 90 s of it;
 * a free observation or reset() starts it over. */
void
check_call_wait() {
    CallWaitClock wait;
    check(wait.busy(0, false) == CallWait::Clear, "wait: free");
    check(wait.busy(1000, true) == CallWait::Waiting, "wait: busy starts the wait");
    check(wait.busy(1000 + dsd_qt::kMaxCallWaitMs - 1, true) == CallWait::Waiting, "wait: just under 90 s");
    check(wait.busy(1000 + dsd_qt::kMaxCallWaitMs, true) == CallWait::Expired, "wait: 90 s of busy expires");
    check(wait.busy(500000, true) == CallWait::Expired, "wait: stays expired while busy");
    check(wait.busy(500001, false) == CallWait::Clear, "wait: free again");
    check(wait.busy(500002, true) == CallWait::Waiting, "wait: a new busy run starts over");
    wait.reset();
    check(wait.busy(500002 + dsd_qt::kMaxCallWaitMs - 1, true) == CallWait::Waiting, "wait: reset starts over");
    check(wait.busy(500002 + 2 * dsd_qt::kMaxCallWaitMs - 1, true) == CallWait::Expired, "wait: then expires");
}

// ---- The executor ----

using dsd_qt::SiteRoaming;

/* A host that does only what the executor asks, each call counted, every reading set by the case. A switch stop
 * leaves it Stopping until the case says Idle, as Android's does. */
class FakeHost : public dsd_qt::DecoderHost {
  public:
    SessionState phase = Idle;
    qint64 generation = 0;
    qint64 epochValue = 0;
    bool superseded = false;
    bool retireWins = true;
    int switchStops = 0;
    int retires = 0;
    int streamStarts = 0;
    int streamStops = 0;
    QList<qint64> permissions;
    QList<qint64> expectations;
    QList<qint64> epochExpectations;

    bool
    isRunning() const override {
        return phase == Running;
    }

    QString
    statusText() const override {
        return QString();
    }

    bool
    start(const QStringList& argv) override {
        (void)argv;
        setPhase(Starting);
        return true;
    }

    bool
    startWithStopExpectation(const QStringList& argv, qint64 expected, qint64 expectedEpoch) override {
        expectations.append(expected);
        epochExpectations.append(expectedEpoch);
        return start(argv);
    }

    void
    stop() override {
        setPhase(Idle);
    }

    SessionState
    sessionState() const override {
        return phase;
    }

    bool
    locationSupported() const override {
        return true;
    }

    qint64
    userStopGeneration() const override {
        return generation;
    }

    qint64
    foregroundEpoch() const override {
        return epochValue;
    }

    bool
    startSuperseded() const override {
        return superseded;
    }

    void
    stopForSwitch() override {
        ++switchStops;
        setPhase(Stopping);
    }

    bool
    retireRoamAttempt() override {
        ++retires;
        if (retireWins) {
            setPhase(Idle);
        }
        return retireWins;
    }

    void
    startLocationStream() override {
        ++streamStarts;
    }

    void
    stopLocationStream() override {
        ++streamStops;
    }

    void
    requestRoamingPermission(qint64 id) override {
        permissions.append(id);
    }

    void
    setPhase(SessionState next) {
        phase = next;
        Q_EMIT sessionStateChanged();
    }
};

/* Sites Hill (A), Lake (B) and Ridge (C) of one made-up system on one rtl_tcp receiver, where the core cases put A, B
 * and C. The clock is the case's; a fix is stamped now. */
struct Rig {
    qint64 now = 1000000;
    qint64 ns = 1;
    dsd_qt::AppPrefs prefs;
    dsd_qt::SavedSystemsModel systems;
    FakeHost host;
    SiteRoaming roaming{&prefs, &systems};
    QStringList uids;
    QList<QPair<qint64, QString>> switches;
    QStringList ended;
    QVariantMap readings;

    Rig() {
        QSettings stored;
        for (const char* key : {"roaming/maxDistanceKm", "roaming/marginKm", "decode/extraArgs"}) {
            stored.remove(QLatin1String(key));
        }
        prefs.setMetricUnits(true);
        prefs.setRoamingEnabled(true);
        while (systems.count() > 0 && systems.remove(0)) {}
        const char* names[] = {"Hill", "Lake", "Ridge"};
        const double lats[] = {10.0, 10.5, 10.5};
        const double lons[] = {20.0, 20.0, 20.45};
        for (int i = 0; i < 3; ++i) {
            systems.add({{"name", QStringLiteral("Valley — %1").arg(QLatin1String(names[i]))},
                         {"siteName", QLatin1String(names[i])},
                         {"rrSid", 7},
                         {"rrSiteId", 70 + i},
                         {"trunking", true},
                         {"sourceType", "rtltcp"},
                         {"host", "127.0.0.1"},
                         {"port", 1234},
                         {"freqMhz", QString::number(851 + i)},
                         {"siteLat", lats[i]},
                         {"siteLon", lons[i]},
                         {"hasSitePos", true}});
            uids.append(systems.get(i).value("uid").toString());
        }
        roaming.setClock([this] { return now; });
        roaming.setHost(&host);
        QObject::connect(&roaming, &SiteRoaming::switchRequested, &roaming,
                         [this](qint64 id, const QString& uid) { switches.append({id, uid}); });
        QObject::connect(&roaming, &SiteRoaming::transactionEnded, &roaming, [this](qint64 id, const QString& outcome) {
            (void)id;
            ended.append(outcome);
        });
        readings = {{"savedUid", uids[0]},  {"trunking", true},  {"radioInput", true},
                    {"replayInput", false}, {"scanKind", 0},     {"operatorTuned", false},
                    {"heldTg", 0},          {"callBusy", false}, {"optionsKnown", true}};
    }

    ~Rig() {
        roaming.setHost(nullptr);
        prefs.setRoamingEnabled(false);
        while (systems.count() > 0 && systems.remove(0)) {}
    }

    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    void
    set(const char* key, const QVariant& value) {
        readings[QLatin1String(key)] = value;
        roaming.setFacts(readings);
    }

    /* Site @p index running as a saved session, past its initialization edge unless @p initialized is false. */
    void
    running(int index = 0, bool initialized = true) {
        host.setPhase(dsd_qt::DecoderHost::Starting);
        roaming.noteSessionStart("saved", uids[index], 0);
        host.setPhase(dsd_qt::DecoderHost::Running);
        set("savedUid", uids[index]);
        if (initialized) {
            roaming.noteSessionInitialized();
        }
    }

    /* A new fix at @p lat, @p lon, @p after ms from the last moment, stamped then, judged as the event loop turns. */
    void
    fix(double lat, double lon, qint64 after = 0, double accuracy = 10) {
        fixHeld(lat, lon, after, accuracy);
        QCoreApplication::sendPostedEvents(&roaming, QEvent::MetaCall);
    }

    /* The same, as the host's poll delivers it inside a UI tick: the loop has not turned yet. */
    void
    fixHeld(double lat, double lon, qint64 after = 0, double accuracy = 10) {
        now += after;
        ns += 1000;
        Q_EMIT host.locationStreamUpdate({{"generation", 1},
                                          {"state", "ok"},
                                          {"hasFix", true},
                                          {"provider", "gps"},
                                          {"measurementNs", ns},
                                          {"lat", lat},
                                          {"lon", lon},
                                          {"accuracyM", accuracy},
                                          {"fixAtMs", now}});
    }

    /* The same measurement as the last fix, polled again (as a host may after its feed restarts), as @p provider
     * reports it (another provider relays it with its stamp). */
    void
    refix(double lat, double lon, const char* provider = "gps") {
        Q_EMIT host.locationStreamUpdate({{"generation", 2},
                                          {"state", "ok"},
                                          {"hasFix", true},
                                          {"provider", provider},
                                          {"measurementNs", ns},
                                          {"lat", lat},
                                          {"lon", lon},
                                          {"accuracyM", 10.0},
                                          {"fixAtMs", now}});
        QCoreApplication::sendPostedEvents(&roaming, QEvent::MetaCall);
    }

    void
    nearA(qint64 after) {
        fix(10.01, 20.0, after);
    }

    void
    nearB(qint64 after) {
        fix(10.49, 20.0, after);
    }

    void
    nearC(qint64 after) {
        fix(10.5, 20.44, after);
    }

    void
    tick(qint64 after) {
        now += after;
        roaming.tick();
    }

    /* Hill running, then three fixes near Lake over 20 s: the switch is due at the third. */
    void
    qualifyB() {
        running();
        nearB(0);
        nearB(10000);
        nearB(10000);
    }

    qint64
    roamId() const {
        return switches.isEmpty() ? 0 : switches.last().first;
    }
};

/* Roaming arms only once the session passed its initialization edge, runs location only while armed, once per need,
 * and idles or pauses with the reason. */
void
check_executor_arming() {
    Rig rig;
    check(rig.roaming.state() == SiteRoaming::Idle && rig.roaming.reason() == SiteRoaming::NoSession
              && rig.host.streamStarts == 0,
          "arming: nothing running is idle, with no location");
    rig.running(0, false);
    check(rig.roaming.reason() == SiteRoaming::SessionStarting && rig.host.streamStarts == 0,
          "arming: not before the initialization edge");
    rig.roaming.noteSessionInitialized();
    check(rig.roaming.state() == SiteRoaming::Paused && rig.roaming.reason() == SiteRoaming::NoFix
              && rig.host.streamStarts == 1 && rig.roaming.statusText() == QStringLiteral("Waiting for a location fix"),
          "arming: after it, location starts and roaming waits for a fix");
    rig.tick(1000);
    rig.tick(1000);
    rig.fix(10.0, 20.0);
    check(rig.host.streamStarts == 1 && rig.roaming.state() == SiteRoaming::Following
              && rig.roaming.currentUid() == rig.uids[0] && rig.roaming.currentDistanceKm() >= 0
              && rig.roaming.currentDistanceKm() < 0.5,
          "arming: location starts once, and a fix on the site follows it");
    check(rig.roaming.holdsLocation(), "arming: the running feed's fix is held");
    rig.set("heldTg", 5);
    check(rig.roaming.reason() == SiteRoaming::TalkgroupHeld && rig.host.streamStops == 1,
          "arming: a held talkgroup pauses it and stops location");
    check(!rig.roaming.holdsLocation(), "arming: a stopped feed's fix is not kept in memory");
    rig.set("heldTg", 0);
    check(rig.host.streamStarts == 2, "arming: released, location starts again");
    rig.set("scanKind", 2);
    check(rig.roaming.state() == SiteRoaming::Idle && rig.roaming.reason() == SiteRoaming::Scanning, "arming: a scan");
    rig.set("scanKind", 0);
    rig.set("operatorTuned", true);
    check(rig.roaming.reason() == SiteRoaming::OperatorTune, "arming: an operator hold pauses it");
    rig.set("savedUid", QString());
    check(rig.roaming.state() == SiteRoaming::Paused && rig.roaming.reason() == SiteRoaming::OperatorTune,
          "arming: still paused once Main names the held session Explore");
    rig.set("savedUid", rig.uids[0]);
    rig.set("operatorTuned", false);
    rig.set("savedUid", QString());
    check(rig.roaming.reason() == SiteRoaming::NotSavedSite, "arming: a session that is no longer the saved site");
    rig.set("savedUid", rig.uids[0]);
    rig.prefs.setExtraArgs("-Z");
    check(rig.roaming.state() == SiteRoaming::Idle && rig.roaming.reason() == SiteRoaming::GlobalExtraArgs
              && rig.roaming.statusText()
                     == QStringLiteral("GPS roaming is off while extra options are set in Settings"),
          "arming: extra options in Settings idle it");
    rig.prefs.setExtraArgs(QString());
    rig.systems.update(1, {{"extraArgs", "-Y"}});
    rig.systems.update(2, {{"trunking", false}});
    check(rig.roaming.reason() == SiteRoaming::FewSites, "arming: fewer than two roamable sites");
    rig.systems.update(1, {{"extraArgs", ""}});
    check(rig.roaming.reason() == SiteRoaming::NoFix, "arming: two again (the fix went with the paused feed)");
    rig.prefs.setRoamingEnabled(false);
    check(rig.roaming.state() == SiteRoaming::Off && rig.host.streamStops >= 3, "arming: off stops location");
    rig.roaming.setEnabledByUser(true);
    rig.roaming.setEnabledByUser(true);
    check(rig.host.permissions.size() == 2 && rig.host.permissions[0] != rig.host.permissions[1],
          "arming: each switch-on by the user asks once, with a new request id");
}

/* Hill to Lake: one switch, at the third new fix that spans 20 s; the stop, then the start with the generation the
 * session started under, then done once the destination initializes. */
void
check_executor_route() {
    Rig rig;
    rig.running();
    rig.nearB(0);
    check(rig.roaming.state() == SiteRoaming::Candidate && rig.roaming.candidateUid() == rig.uids[1]
              && rig.roaming.statusText() == QStringLiteral("Lake is closer. Checking before switching"),
          "route: the first fix makes Lake a candidate");
    rig.nearB(10000);
    for (int i = 0; i < 9; ++i) {
        rig.tick(1000); // timer ticks add nothing
    }
    check(rig.switches.isEmpty() && rig.host.switchStops == 0, "route: no switch before the dwell");
    rig.nearB(1000);
    check(rig.switches.size() == 1 && rig.switches[0].second == rig.uids[1] && rig.host.switchStops == 1
              && rig.roaming.state() == SiteRoaming::Switching && rig.roaming.transactionActive()
              && rig.roaming.statusText() == QStringLiteral("Switching to Lake…"),
          "route: the third fix spanning 20 s switches once");
    const qint64 id = rig.roamId();
    rig.tick(1000);
    rig.nearB(1000);
    check(rig.switches.size() == 1 && rig.host.switchStops == 1, "route: no second switch");
    rig.host.setPhase(dsd_qt::DecoderHost::Idle);
    const QVariantMap plan = rig.roaming.startPlan(id);
    check(plan.value("ok").toBool() && plan.value("uid").toString() == rig.uids[1]
              && plan.value("expectedStopGeneration").toLongLong() == 0
              && plan.value("expectedForegroundEpoch").toLongLong() == 0,
          "route: the plan starts Lake with the generation and the epoch the switch was decided under");
    rig.roaming.noteStartResult(id, {{"ok", true}, {"started", true}, {"superseded", false}});
    rig.host.setPhase(dsd_qt::DecoderHost::Starting);
    rig.roaming.noteSessionStart("saved", rig.uids[1], id);
    rig.host.setPhase(dsd_qt::DecoderHost::Running);
    rig.set("savedUid", rig.uids[1]);
    check(rig.roaming.transactionActive() && rig.ended.isEmpty(), "route: not done before initialization");
    rig.roaming.noteSessionInitialized();
    check(rig.ended == QStringList{QStringLiteral("done")} && !rig.roaming.transactionActive()
              && rig.roaming.currentUid() == rig.uids[1] && rig.roaming.state() == SiteRoaming::Following
              && rig.roaming.recoveryNotice().isEmpty(),
          "route: done, following Lake");
}

/* A due switch waits for the followed call, at most 90 s of it, measured from when that destination's switch first
 * found the call busy; a new destination waits its own 90 s. */
void
check_executor_call_wait() {
    {
        Rig rig;
        rig.running();
        rig.set("callBusy", true);
        rig.nearB(0);
        rig.nearB(10000);
        rig.nearB(10000);
        check(rig.roaming.state() == SiteRoaming::WaitingForCall && rig.roaming.destinationUid() == rig.uids[1]
                  && rig.host.switchStops == 0
                  && rig.roaming.statusText()
                         == QStringLiteral("Waiting for this call to end before switching to Lake (up to 90 seconds)"),
              "wait: a followed call holds the switch");
        rig.set("callBusy", false);
        check(rig.host.switchStops == 1, "wait: the call ending lets it go");
    }
    {
        Rig rig;
        rig.running();
        rig.set("callBusy", true);
        rig.nearB(0);
        rig.nearB(10000);
        rig.nearB(10000); // the wait starts here
        for (int i = 0; i < 8; ++i) {
            rig.nearB(10000);
        }
        rig.tick(9999); // 89.999 s
        check(rig.host.switchStops == 0, "wait: still waiting just under 90 s");
        rig.tick(1);
        check(rig.host.switchStops == 1, "wait: 90 s of the call and it goes ahead mid-call");
    }
    {
        Rig rig;
        rig.running();
        rig.set("callBusy", true);
        rig.nearB(0);
        rig.nearB(10000);
        rig.nearB(10000); // Lake's wait starts at t=20 s
        for (int i = 0; i < 4; ++i) {
            rig.nearB(10000);
        }
        rig.nearC(10000); // t=70 s: Ridge is the candidate now
        rig.nearC(10000);
        rig.nearC(10000); // t=90 s: Ridge's switch is due, and its wait starts
        check(rig.roaming.state() == SiteRoaming::WaitingForCall && rig.roaming.destinationUid() == rig.uids[2],
              "wait: Ridge now");
        for (int i = 0; i < 3; ++i) {
            rig.nearC(10000);
        }
        rig.tick(5000); // t=125 s: 105 s after Lake's wait began, 35 s into Ridge's
        check(rig.host.switchStops == 0, "wait: a new destination's wait starts over");
        for (int i = 0; i < 5; ++i) {
            rig.nearC(10000);
        }
        rig.tick(5000); // t=180 s
        check(rig.host.switchStops == 1 && rig.switches.last().second == rig.uids[2],
              "wait: Ridge goes after its own 90 s");
    }
}

/* A foreground loss forgets the evidence and every fix seen; the last fix, polled again after it, adds nothing, and
 * the feed is started again. */
void
check_executor_reset_drops_the_cached_fix() {
    Rig rig;
    rig.running();
    rig.nearB(0);
    rig.nearB(10000);
    check(rig.roaming.state() == SiteRoaming::Candidate, "reset: two fixes, a candidate");
    rig.host.epochValue = 1;
    rig.tick(500);
    check(rig.roaming.state() == SiteRoaming::Paused && rig.roaming.reason() == SiteRoaming::NoFix
              && rig.host.streamStarts == 2,
          "reset: the candidate goes, the cached fix is not fed again, and the feed restarts");
    check(!rig.roaming.holdsLocation(), "reset: the fix from before the foreground loss is not kept in memory");
    rig.refix(10.49, 20.0);
    check(rig.roaming.reason() == SiteRoaming::NoFix, "reset: the same fix polled again counts for nothing");
    rig.refix(10.49, 20.0, "fused");
    check(rig.roaming.reason() == SiteRoaming::NoFix,
          "reset: the same fix relayed by another provider counts for nothing");
    rig.nearB(9500);
    rig.nearB(10000);
    check(rig.host.switchStops == 0, "reset: two new fixes are not three");
    rig.nearB(10000);
    check(rig.host.switchStops == 1, "reset: the third new fix over 20 s switches");
}

/* An operator command blocks roaming until the decoder shows its outcome, however long that takes; a release
 * supersedes a hold that never applied; a manual tune that applies pauses it. */
void
check_executor_barrier() {
    Rig rig;
    rig.running();
    rig.nearB(0);
    rig.roaming.noteOperatorIntent("hold");
    check(rig.roaming.reason() == SiteRoaming::PendingChange
              && rig.roaming.statusText() == QStringLiteral("Waiting for your last change to take effect"),
          "barrier: a queued hold blocks");
    for (int i = 0; i < 5; ++i) {
        rig.nearB(10000);
    }
    check(rig.host.switchStops == 0, "barrier: no switch past a full dwell while the hold waits");
    rig.prefs.setRoamingEnabled(false);
    rig.prefs.setRoamingEnabled(true);
    for (int i = 0; i < 4; ++i) {
        rig.nearB(10000);
    }
    check(rig.host.switchStops == 0 && rig.roaming.reason() == SiteRoaming::PendingChange,
          "barrier: switching roaming off and on keeps the session's pending command");
    rig.roaming.noteOperatorIntent("release");
    check(rig.roaming.reason() == SiteRoaming::NoFix && rig.host.streamStarts == 2,
          "barrier: a release supersedes the hold that never applied; location starts again");
    rig.nearB(10000);
    rig.nearB(10000);
    check(rig.host.switchStops == 0 && rig.roaming.state() == SiteRoaming::Candidate,
          "barrier: and the dwell starts over");
    rig.nearB(10000);
    check(rig.host.switchStops == 1, "barrier: then the switch");

    Rig tune;
    tune.running();
    tune.nearB(0);
    tune.roaming.noteOperatorIntent("manualTune");
    for (int i = 0; i < 4; ++i) {
        tune.nearB(10000);
    }
    check(tune.host.switchStops == 0 && tune.roaming.reason() == SiteRoaming::PendingChange,
          "barrier: a manual tune not shown yet blocks past a full dwell");
    tune.set("operatorTuned", true);
    check(tune.roaming.reason() == SiteRoaming::OperatorTune, "barrier: once it applies, the operator hold pauses");
    tune.set("operatorTuned", false);
    tune.roaming.noteOperatorIntent("trunkingOff");
    tune.set("trunking", false);
    check(tune.roaming.reason() == SiteRoaming::NotTrunking, "barrier: trunking turned off applies and pauses");
}

/* A user stop moves the generation: seen when a candidate begins to qualify it ends roaming for the session; seen at
 * the start it leaves the session stopped, with no notice. */
void
check_executor_generation() {
    {
        Rig rig;
        rig.running();
        rig.host.generation = 1; // a Stop the app has not shown yet
        rig.nearB(0);
        check(rig.roaming.state() == SiteRoaming::Idle && rig.roaming.reason() == SiteRoaming::NoSession,
              "generation: read again as the candidate begins to qualify, it ends roaming for the session at once");
        rig.nearB(10000);
        rig.nearB(10000);
        check(rig.switches.isEmpty() && rig.roaming.reason() == SiteRoaming::NoSession,
              "generation: a stop before qualification begins is never absorbed");
    }
    {
        Rig rig;
        rig.qualifyB();
        rig.host.generation = 1; // during Stopping, no new lifecycle edge
        rig.host.setPhase(dsd_qt::DecoderHost::Idle);
        check(!rig.roaming.startPlan(rig.roamId()).value("ok").toBool()
                  && rig.ended == QStringList{QStringLiteral("cancelled")} && rig.roaming.recoveryNotice().isEmpty(),
              "generation: a stop during the switch's stop starts nothing, silently");
    }
    {
        Rig rig;
        rig.qualifyB();
        rig.host.setPhase(dsd_qt::DecoderHost::Idle);
        check(rig.roaming.startPlan(rig.roamId()).value("ok").toBool(), "generation: the plan");
        rig.roaming.noteStartResult(rig.roamId(), {{"ok", true}, {"started", false}, {"superseded", true}});
        check(rig.ended == QStringList{QStringLiteral("cancelled")} && rig.roaming.recoveryNotice().isEmpty()
                  && rig.roaming.reason() == SiteRoaming::NoSession,
              "generation: refused superseded after the last read: stopped, no notice, no failure");
    }
}

/* A foreground loss cancels a switch the host does not own yet; once the old session was told to stop, Home names the
 * site it left. */
void
check_executor_foreground() {
    {
        Rig rig;
        rig.running();
        rig.set("callBusy", true);
        rig.nearB(0);
        rig.nearB(10000);
        rig.nearB(10000);
        rig.host.epochValue = 1;
        rig.set("callBusy", false);
        check(rig.host.switchStops == 0 && rig.ended == QStringList{QStringLiteral("cancelled")}
                  && rig.roaming.recoveryNotice().isEmpty(),
              "foreground: waiting for a call, the switch goes and the session stays");
    }
    {
        Rig rig;
        rig.qualifyB();
        rig.host.setPhase(dsd_qt::DecoderHost::Idle);
        rig.host.epochValue = 1; // the queued start runs before the first resumed tick
        check(!rig.roaming.startPlan(rig.roamId()).value("ok").toBool()
                  && rig.roaming.recoveryNotice()
                         == QStringLiteral("Roaming did not switch to Lake. Listening stopped.")
                  && rig.roaming.recoveryActionText() == QStringLiteral("Listen to Hill")
                  && rig.roaming.recoveryUid() == rig.uids[0],
              "foreground: after the stop, nothing starts and Home names the site left");
        rig.running(2);
        check(rig.roaming.recoveryNotice().isEmpty(), "foreground: the next start clears the notice");
    }
}

/* #786 r2: the app leaves the foreground while the destination's start is on its way. Unless it committed, it never
 * runs and never reads as a refusal: listening stays stopped, with Home's notice, whether the host's retirement wins,
 * the service fenced the start itself (its START carries the epoch: superseded), or the host refused it superseded
 * as it was handed over. One that committed runs on. A supersede after a user stop stays silent. */
template <typename HandOver>
void
check_executor_starting_foreground(const HandOver& handOver) {
    const QString notice = QStringLiteral("Roaming did not switch to Lake. Listening stopped.");
    {
        Rig rig;
        handOver(rig);
        rig.host.epochValue = 1;
        rig.tick(1000);
        check(rig.host.retires == 1 && rig.ended == QStringList{QStringLiteral("cancelled")}
                  && rig.host.phase == dsd_qt::DecoderHost::Idle && rig.roaming.recoveryNotice() == notice
                  && rig.roaming.reason() != SiteRoaming::SwitchFailed,
              "starting, foreground: the uncommitted start is retired: stopped, the notice");
    }
    {
        Rig rig;
        handOver(rig);
        rig.host.retireWins = false;
        rig.host.epochValue = 1;
        rig.tick(1000);
        check(rig.host.retires == 1 && rig.roaming.transactionActive() && rig.ended.isEmpty(),
              "starting, foreground: a start the service took is followed, not cancelled");
        rig.host.superseded = true;
        rig.host.setPhase(dsd_qt::DecoderHost::Idle);
        check(rig.ended == QStringList{QStringLiteral("cancelled")} && rig.roaming.recoveryNotice() == notice
                  && rig.roaming.reason() != SiteRoaming::SwitchFailed,
              "starting, foreground: the service fenced it: stopped, the notice, no refusal");
    }
    {
        Rig rig;
        handOver(rig);
        rig.host.epochValue = 1; // the service fenced the START before Qt's next tick saw the epoch move
        rig.host.setPhase(dsd_qt::DecoderHost::Idle);
        check(rig.ended == QStringList{QStringLiteral("cancelled")} && rig.roaming.recoveryNotice() == notice
                  && rig.roaming.reason() != SiteRoaming::SwitchFailed,
              "starting, foreground: the Idle edge reads the epoch first: no refusal");
    }
    {
        Rig rig;
        handOver(rig);
        rig.host.retireWins = false;
        rig.host.setPhase(dsd_qt::DecoderHost::Running);
        rig.host.epochValue = 1;
        rig.tick(1000);
        rig.roaming.noteSessionInitialized();
        check(rig.ended == QStringList{QStringLiteral("done")} && rig.roaming.recoveryNotice().isEmpty(),
              "starting, foreground: a start that committed runs on");
    }
    for (const bool userStop : {false, true}) {
        Rig rig;
        rig.qualifyB();
        rig.host.setPhase(dsd_qt::DecoderHost::Idle);
        rig.roaming.startPlan(rig.roamId());
        if (userStop) {
            rig.host.generation = 1;
        }
        rig.host.epochValue = 1;
        rig.roaming.noteStartResult(rig.roamId(), {{"ok", true}, {"started", false}, {"superseded", true}});
        check(rig.ended == QStringList{QStringLiteral("cancelled")}
                  && rig.roaming.recoveryNotice() == (userStop ? QString() : notice)
                  && rig.roaming.reason() != SiteRoaming::SwitchFailed,
              userStop ? "handover superseded after a user stop too: silent"
                       : "handover superseded by leaving the foreground: stopped, the notice");
    }
}

/* The destination's start: a disable retires it (stopped, with the notice) unless it already committed (it runs);
 * an end with no initialization is refused; a supersede is the user's stop. */
void
check_executor_starting() {
    const auto handOver = [](Rig& rig) {
        rig.qualifyB();
        rig.host.setPhase(dsd_qt::DecoderHost::Idle);
        rig.roaming.startPlan(rig.roamId());
        rig.roaming.noteStartResult(rig.roamId(), {{"ok", true}, {"started", true}});
        rig.host.setPhase(dsd_qt::DecoderHost::Starting);
        rig.roaming.noteSessionStart("saved", rig.uids[1], rig.roamId());
    };
    {
        Rig rig;
        handOver(rig);
        rig.prefs.setRoamingEnabled(false);
        check(rig.host.retires == 1 && rig.ended == QStringList{QStringLiteral("cancelled")}
                  && rig.host.phase == dsd_qt::DecoderHost::Idle && !rig.roaming.recoveryNotice().isEmpty(),
              "starting: disabled while it configures: retired, stopped, the notice");
    }
    {
        Rig rig;
        handOver(rig);
        rig.host.retireWins = false;
        rig.host.setPhase(dsd_qt::DecoderHost::Running);
        rig.prefs.setRoamingEnabled(false);
        check(rig.host.retires == 1 && rig.ended == QStringList{QStringLiteral("done")}
                  && rig.roaming.recoveryNotice().isEmpty() && rig.roaming.state() == SiteRoaming::Off,
              "starting: committed already: Lake runs, no notice, roaming stays off");
    }
    {
        Rig rig;
        handOver(rig);
        rig.host.setPhase(dsd_qt::DecoderHost::Idle); // a bootstrap that exits, never initialized
        check(rig.ended == QStringList{QStringLiteral("refused")} && rig.roaming.reason() == SiteRoaming::SwitchFailed
                  && rig.roaming.statusText() == QStringLiteral("Could not switch to Lake")
                  && rig.roaming.recoveryNotice()
                         == QStringLiteral("Roaming did not switch to Lake. Listening stopped."),
              "starting: idle without initialization is refused, with the notice");
    }
    {
        Rig rig;
        handOver(rig);
        rig.host.superseded = true;
        rig.host.setPhase(dsd_qt::DecoderHost::Idle);
        check(rig.ended == QStringList{QStringLiteral("cancelled")} && rig.roaming.recoveryNotice().isEmpty()
                  && rig.roaming.reason() == SiteRoaming::NoSession,
              "starting: superseded while it configured is the user's stop");
    }
    {
        Rig rig;
        handOver(rig);
        // Main cancels for a failed session as it reacts to the same edge: the failure stands, refused, not retired.
        QObject::disconnect(&rig.host, nullptr, &rig.roaming, nullptr);
        rig.host.setPhase(dsd_qt::DecoderHost::Failed);
        rig.roaming.cancel("failed");
        check(rig.ended == QStringList{QStringLiteral("refused")} && rig.host.retires == 0
                  && rig.roaming.recoveryNotice().isEmpty(),
              "starting: a failed destination is refused even when Main's cancel comes first");
    }
    check_executor_starting_foreground(handOver);
    {
        Rig rig;
        rig.qualifyB();
        rig.host.setPhase(dsd_qt::DecoderHost::Idle);
        rig.roaming.startPlan(rig.roamId());
        rig.roaming.noteStartResult(rig.roamId(), {{"ok", false}, {"error", "frequency"}});
        check(rig.ended == QStringList{QStringLiteral("refused")} && rig.roaming.reason() == SiteRoaming::SwitchFailed
                  && rig.roaming.recoveryNotice().isEmpty(),
              "starting: a start the build refused pauses roaming; the failure card says why");
    }
}

/* #786 r4: the transaction against each phase edge the host can show before the destination's start. */
void
check_executor_stop_edges() {
    using Phase = dsd_qt::DecoderHost::SessionState;
    {
        // The host gave up waiting for the stop's acknowledgement (2 s) and shows the old session running again.
        Rig rig;
        rig.qualifyB();
        check(rig.host.phase == Phase::Stopping && rig.roaming.transactionActive(), "stop edges: the switch stops");
        rig.host.setPhase(Phase::Running);
        check(!rig.roaming.transactionActive() && rig.ended == QStringList{QStringLiteral("refused")}
                  && rig.roaming.recoveryNotice().isEmpty() && rig.roaming.reason() == SiteRoaming::SwitchFailed
                  && rig.roaming.statusText() == QStringLiteral("Could not switch to Lake"),
              "stop edges: running again ends the switch refused, no notice, paused on it");
        for (int i = 0; i < 70; ++i) {
            rig.tick(1000);
        }
        rig.nearB(1000); // the fix gone stale: one fresh fix starts the count again
        check(rig.host.switchStops == 1 && rig.roaming.state() == SiteRoaming::Candidate,
              "stop edges: the old evidence is gone; a fresh fix qualifies anew");
        rig.nearB(10000);
        rig.nearB(10000);
        check(rig.host.switchStops == 2 && rig.roaming.transactionActive(),
              "stop edges: a later valid verdict switches again");
    }
    {
        // After the stop went idle, Running is the destination's own start, even when Main acted on the Idle edge
        // before roaming saw it (its restart runs from a queued call): nothing ends.
        Rig rig;
        rig.qualifyB();
        rig.host.blockSignals(true);
        rig.host.setPhase(Phase::Idle);
        rig.host.blockSignals(false);
        const QVariantMap plan = rig.roaming.startPlan(rig.roamId());
        rig.host.setPhase(Phase::Running); // a host that runs the destination at once, before the start's result
        check(plan.value("ok").toBool() && rig.roaming.transactionActive() && rig.ended.isEmpty(),
              "stop edges: the roam's own start running is no stop given up");
        rig.roaming.noteStartResult(rig.roamId(), {{"ok", true}, {"started", true}});
        rig.roaming.noteSessionStart("saved", rig.uids[1], rig.roamId());
        rig.roaming.noteSessionInitialized();
        check(rig.ended == QStringList{QStringLiteral("done")}, "stop edges: and it completes");
    }
    {
        // Failed while waiting for USB access: the failure card says why; the switch is cancelled.
        Rig rig;
        rig.qualifyB();
        rig.host.setPhase(Phase::Idle);
        rig.roaming.startPlan(rig.roamId());
        rig.roaming.noteWaitingForUsb(rig.roamId());
        rig.host.setPhase(Phase::Failed);
        check(rig.ended == QStringList{QStringLiteral("cancelled")} && !rig.roaming.transactionActive(),
              "stop edges: failed during the USB wait cancels");
    }
    {
        // Waiting for a call, the session stops (the user's Stop from the notification): the switch never starts.
        Rig rig;
        rig.running();
        rig.set("callBusy", true);
        rig.nearB(0);
        rig.nearB(10000);
        rig.nearB(10000);
        rig.host.setPhase(Phase::Stopping);
        rig.set("callBusy", false);
        check(rig.host.switchStops == 0 && rig.ended == QStringList{QStringLiteral("cancelled")},
              "stop edges: a session stopping during the call wait ends the switch");
    }
}

/* #786 r5: a fix the host's poll delivers inside a UI tick is judged after that tick's decoder snapshot, so a call that
 * began between polls holds the switch. Fixes judged while a roam's destination started count for nothing at its
 * initialization edge, which the host also signals before the tick's snapshot. */
void
check_executor_same_tick() {
    {
        Rig rig;
        rig.running();
        rig.nearB(0);
        rig.nearB(10000);
        rig.fixHeld(10.49, 20.0, 10000); // the qualifying fix, polled at the start of the tick
        rig.set("callBusy", true);       // the same tick's snapshot: a call began since the last poll
        QCoreApplication::sendPostedEvents(&rig.roaming, QEvent::MetaCall);
        check(rig.host.switchStops == 0 && rig.roaming.state() == SiteRoaming::WaitingForCall,
              "same tick: the fix and a new busy snapshot wait for the call");
        rig.set("callBusy", false);
        check(rig.host.switchStops == 1, "same tick: and the call's end switches");
    }
    {
        Rig rig;
        rig.qualifyB();
        rig.host.setPhase(dsd_qt::DecoderHost::Idle);
        rig.roaming.startPlan(rig.roamId());
        rig.roaming.noteStartResult(rig.roamId(), {{"ok", true}, {"started", true}});
        rig.host.setPhase(dsd_qt::DecoderHost::Starting);
        rig.roaming.noteSessionStart("saved", rig.uids[1], rig.roamId());
        rig.host.setPhase(dsd_qt::DecoderHost::Running);
        rig.set("savedUid", rig.uids[1]);
        rig.nearC(0);
        rig.nearC(10000);
        rig.nearC(10000); // Ridge qualifies from Lake while Lake starts
        rig.roaming.noteSessionInitialized();
        check(rig.host.switchStops == 1 && rig.ended == QStringList{QStringLiteral("done")}
                  && !rig.roaming.transactionActive(),
              "init edge: what was judged before Lake's edge does not switch at it");
    }
}

/* #786 r5: one precedence decides how a roam ends when several causes are present: the user's stop, then the user's
 * own choice, then roaming turned off, then the foreground lost, then the rest (a revoked verdict, a stop the host gave
 * up on, a failure). A pair of causes, in either order of arrival, ends as the stronger one says. */
void
check_executor_end_precedence() {
    using Phase = dsd_qt::DecoderHost::SessionState;
    const QString notice = QStringLiteral("Roaming did not switch to Lake. Listening stopped.");

    struct Case {
        const char* name;
        std::function<void(Rig&)> causes;
        QString outcome;
        bool notice;
    };

    const QList<Case> cases = {
        {"user stop, then the foreground lost",
         [](Rig& rig) {
             rig.host.generation = 1;
             rig.host.epochValue = 1;
             rig.tick(500);
         },
         "cancelled", false},
        {"user stop, then roaming off",
         [](Rig& rig) {
             rig.host.generation = 1;
             rig.prefs.setRoamingEnabled(false);
         },
         "cancelled", false},
        {"user stop, then the stop given up",
         [](Rig& rig) {
             rig.host.generation = 1;
             rig.host.setPhase(Phase::Running);
         },
         "cancelled", false},
        {"user stop, then the checks at Idle",
         [](Rig& rig) {
             rig.host.generation = 1;
             rig.host.setPhase(Phase::Idle);
             rig.roaming.startPlan(rig.roamId());
         },
         "cancelled", false},
        {"the foreground lost, then another start",
         [](Rig& rig) {
             rig.host.epochValue = 1;
             rig.roaming.cancel("start");
         },
         "cancelled", false},
        {"the foreground lost, then roaming off",
         [](Rig& rig) {
             rig.host.epochValue = 1;
             rig.prefs.setRoamingEnabled(false);
         },
         "cancelled", true},
        {"the foreground lost, then the stop given up",
         [](Rig& rig) {
             rig.host.epochValue = 1;
             rig.host.setPhase(Phase::Running);
         },
         "cancelled", false},
        {"the foreground lost, then a failure",
         [](Rig& rig) {
             rig.host.epochValue = 1;
             rig.host.setPhase(Phase::Failed);
         },
         "cancelled", false},
        {"the verdict revoked, then the foreground lost",
         [](Rig& rig) {
             rig.nearA(1000);
             rig.host.epochValue = 1;
             rig.host.setPhase(Phase::Idle);
             rig.roaming.startPlan(rig.roamId());
         },
         "cancelled", true},
        {"the stop given up alone", [](Rig& rig) { rig.host.setPhase(Phase::Running); }, "refused", false},
    };
    for (const Case& c : cases) {
        Rig rig;
        rig.qualifyB();
        c.causes(rig);
        const bool shown = rig.roaming.recoveryNotice() == notice;
        check(rig.ended == QStringList{c.outcome} && shown == c.notice
                  && (c.outcome == QLatin1String("refused")) == (rig.roaming.reason() == SiteRoaming::SwitchFailed),
              QStringLiteral("precedence: %1").arg(QLatin1String(c.name)).toUtf8().constData());
    }
    {
        // In the destination's start: the user's stop beside a foreground retirement is silent too.
        Rig rig;
        rig.qualifyB();
        rig.host.setPhase(Phase::Idle);
        rig.roaming.startPlan(rig.roamId());
        rig.roaming.noteStartResult(rig.roamId(), {{"ok", true}, {"started", true}});
        rig.host.setPhase(Phase::Starting);
        rig.roaming.noteSessionStart("saved", rig.uids[1], rig.roamId());
        rig.host.generation = 1;
        rig.host.epochValue = 1;
        rig.tick(500);
        check(rig.ended == QStringList{QStringLiteral("cancelled")} && rig.roaming.recoveryNotice().isEmpty(),
              "precedence: a user stop beside a foreground retirement is silent");
    }
    {
        // The destination failed after the app left the foreground: no "Could not switch", the card says why.
        Rig rig;
        rig.qualifyB();
        rig.host.setPhase(Phase::Idle);
        rig.roaming.startPlan(rig.roamId());
        rig.roaming.noteStartResult(rig.roamId(), {{"ok", true}, {"started", true}});
        rig.host.setPhase(Phase::Starting);
        rig.roaming.noteSessionStart("saved", rig.uids[1], rig.roamId());
        rig.host.retireWins = false;
        rig.host.epochValue = 1;
        rig.host.setPhase(Phase::Failed);
        check(rig.ended == QStringList{QStringLiteral("cancelled")} && rig.roaming.reason() != SiteRoaming::SwitchFailed
                  && rig.roaming.recoveryNotice().isEmpty(),
              "precedence: a failure after the foreground loss is no refusal");
    }
}

/* #786: the foreground loss cancels a switch during its drain, with Home's notice; the host then gives up the stop and
 * shows the old session running again. The notice said listening stopped, so it goes. One whose session did stop
 * stays. */
void
check_executor_notice_withdrawn() {
    using Phase = dsd_qt::DecoderHost::SessionState;
    const QString notice = QStringLiteral("Roaming did not switch to Lake. Listening stopped.");
    {
        Rig rig;
        rig.qualifyB();
        rig.host.epochValue = 1;
        rig.tick(500);
        check(rig.roaming.recoveryNotice() == notice, "notice: the foreground loss during the drain leaves it");
        rig.host.setPhase(Phase::Running);
        check(rig.roaming.recoveryNotice().isEmpty() && rig.roaming.recoveryUid().isEmpty(),
              "notice: the session running again withdraws it");
    }
    {
        Rig rig;
        rig.qualifyB();
        rig.host.epochValue = 1;
        rig.tick(500);
        rig.host.setPhase(Phase::Idle);
        check(rig.roaming.recoveryNotice() == notice, "notice: a session that did stop keeps it");
    }
}

/* #786 r6: every operator barrier in every stage of a switch. An operator command (pending or since applied) ends the
 * switch before the destination's start and nothing starts. A snapshot value alone (a held talkgroup, an operator tune,
 * trunking off, a cleared snapshot) ends it only in the call wait, where it is the running session's: once the stop is
 * issued it may be the teardown's, and the switch goes on. The destination's start is its own session's, which arms
 * only once the barrier is gone. */
void
check_executor_operator_barriers() {
    using Phase = dsd_qt::DecoderHost::SessionState;

    struct Barrier {
        const char* name;
        std::function<void(Rig&)> apply;
        SiteRoaming::Reason after; // what the destination shows after its edge
        bool command;              // an operator command; else a snapshot value alone
    };

    const QList<Barrier> barriers = {
        {"Hold TG pending", [](Rig& rig) { rig.roaming.noteOperatorIntent("hold"); }, SiteRoaming::PendingChange, true},
        {"Hold TG applied",
         [](Rig& rig) {
             rig.roaming.noteOperatorIntent("hold");
             rig.set("heldTg", 5);
         },
         SiteRoaming::TalkgroupHeld, true},
        {"manual tune pending", [](Rig& rig) { rig.roaming.noteOperatorIntent("manualTune"); },
         SiteRoaming::PendingChange, true},
        {"trunking off pending", [](Rig& rig) { rig.roaming.noteOperatorIntent("trunkingOff"); },
         SiteRoaming::PendingChange, true},
        {"a talkgroup held", [](Rig& rig) { rig.set("heldTg", 5); }, SiteRoaming::TalkgroupHeld, false},
        {"an operator tune", [](Rig& rig) { rig.set("operatorTuned", true); }, SiteRoaming::OperatorTune, false},
        {"trunking off", [](Rig& rig) { rig.set("trunking", false); }, SiteRoaming::NotTrunking, false},
        {"a cleared snapshot", [](Rig& rig) { rig.set("optionsKnown", false); }, SiteRoaming::SessionStarting, false},
    };
    const QString notice = QStringLiteral("Roaming did not switch to Lake. Listening stopped.");
    for (const Barrier& b : barriers) {
        const auto label = [&b](const char* what) {
            return QStringLiteral("barrier %1: %2").arg(QLatin1String(b.name), QLatin1String(what)).toUtf8();
        };
        {
            Rig rig; // waiting for the call to end
            rig.running();
            rig.set("callBusy", true);
            rig.nearB(0);
            rig.nearB(10000);
            rig.nearB(10000);
            b.apply(rig);
            rig.set("callBusy", false);
            check(rig.host.switchStops == 0 && !rig.roaming.transactionActive()
                      && rig.roaming.recoveryNotice().isEmpty(),
                  label("in the call wait, no stop").constData());
        }
        {
            Rig rig; // the switch's stop drains
            rig.qualifyB();
            b.apply(rig);
            rig.nearB(1000);
            rig.nearB(10000);
            rig.nearB(10000);
            if (b.command) {
                const bool ended =
                    !rig.roaming.transactionActive() && rig.ended == QStringList{QStringLiteral("cancelled")};
                rig.host.setPhase(Phase::Idle);
                check(ended && !rig.roaming.startPlan(1).value("ok").toBool() && rig.host.switchStops == 1
                          && rig.roaming.recoveryNotice() == notice,
                      label("in the stop, the switch ends and Idle starts nothing").constData());
            } else {
                // Teardown-like: not the operator's, so the switch goes on to its start.
                const bool going = rig.roaming.transactionActive() && rig.ended.isEmpty();
                rig.host.setPhase(Phase::Idle);
                check(going && rig.roaming.startPlan(rig.roamId()).value("ok").toBool(),
                      label("in the stop, a snapshot value alone does not end the switch").constData());
            }
        }
        {
            Rig rig; // the destination starting
            rig.qualifyB();
            rig.host.setPhase(Phase::Idle);
            rig.roaming.startPlan(rig.roamId());
            rig.roaming.noteStartResult(rig.roamId(), {{"ok", true}, {"started", true}});
            rig.host.setPhase(Phase::Starting);
            rig.roaming.noteSessionStart("saved", rig.uids[1], rig.roamId());
            rig.host.setPhase(Phase::Running);
            rig.set("savedUid", rig.uids[1]);
            b.apply(rig);
            rig.roaming.noteSessionInitialized();
            rig.nearC(1000);
            rig.nearC(10000);
            rig.nearC(10000);
            check(rig.ended == QStringList{QStringLiteral("done")} && rig.host.switchStops == 1
                      && rig.roaming.reason() == b.after,
                  label("the destination runs, and roams only once the barrier is gone").constData());
        }
    }
}

/* #786 r7: every value QML reads is part of the published view, so a change of a name alone (a site renamed by an RR
 * update, same coordinates) or of the unit shown is announced, and the recovery notice follows a rename too. */
void
check_executor_metadata_changes() {
    {
        Rig rig;
        rig.running();
        rig.nearB(0);
        check(rig.roaming.state() == SiteRoaming::Candidate
                  && rig.roaming.statusText() == QStringLiteral("Lake is closer. Checking before switching"),
              "metadata: Lake is the candidate");
        int changes = 0;
        QObject::connect(&rig.roaming, &SiteRoaming::changed, &rig.roaming, [&changes] { ++changes; });
        rig.systems.update(1, {{"siteName", "Lakeside"}});
        check(changes == 1 && rig.roaming.state() == SiteRoaming::Candidate
                  && rig.roaming.candidateName() == QStringLiteral("Lakeside")
                  && rig.roaming.statusText() == QStringLiteral("Lakeside is closer. Checking before switching"),
              "metadata: a candidate renamed alone is announced");
        rig.systems.update(0, {{"siteName", "Hilltop"}, {"name", "Upland — Hilltop"}});
        check(changes == 2 && rig.roaming.currentName() == QStringLiteral("Hilltop")
                  && rig.roaming.systemName() == QStringLiteral("Upland"),
              "metadata: the current site and its system renamed are announced");
    }
    {
        Rig rig;
        rig.running();
        rig.fix(30.0, 20.0, 1000);
        check(rig.roaming.statusText() == QStringLiteral("No saved site within 100 km"), "metadata: out of range");
        int changes = 0;
        QObject::connect(&rig.roaming, &SiteRoaming::changed, &rig.roaming, [&changes] { ++changes; });
        rig.prefs.setMetricUnits(false);
        // Unset, the range is the unit's own default (60 mi).
        check(changes == 1 && rig.roaming.statusText() == QStringLiteral("No saved site within 60 mi"),
              "metadata: the unit shown changes the distance and is announced");
    }
    {
        Rig rig;
        rig.qualifyB();
        rig.prefs.setRoamingEnabled(false); // during the stop: the notice
        check(rig.roaming.recoveryNotice() == QStringLiteral("Roaming did not switch to Lake. Listening stopped."),
              "metadata: the notice");
        int notices = 0;
        QObject::connect(&rig.roaming, &SiteRoaming::recoveryNoticeChanged, &rig.roaming, [&notices] { ++notices; });
        rig.systems.update(1, {{"siteName", "Lakeside"}});
        rig.systems.update(0, {{"siteName", "Hilltop"}});
        check(notices == 2
                  && rig.roaming.recoveryNotice()
                         == QStringLiteral("Roaming did not switch to Lakeside. Listening stopped.")
                  && rig.roaming.recoveryActionText() == QStringLiteral("Listen to Hilltop"),
              "metadata: the notice follows a rename");
    }
}

/* Asked for a start out of turn (the session it leaves still stopping), no one asks again: the roam ends, with the
 * notice, rather than sit in its stop. */
void
check_executor_plan_out_of_turn() {
    Rig rig;
    rig.qualifyB();
    check(!rig.roaming.startPlan(rig.roamId()).value("ok").toBool() && !rig.roaming.transactionActive()
              && rig.ended == QStringList{QStringLiteral("cancelled")} && !rig.roaming.recoveryNotice().isEmpty(),
          "plan: asked while the session still stops, the roam ends");
    rig.host.setPhase(dsd_qt::DecoderHost::Idle);
    check(!rig.roaming.startPlan(rig.roamId()).value("ok").toBool() && rig.ended.size() == 1,
          "plan: and stays ended at Idle");
}

/* A session the app took up without starting never arms, and says so; a manual tune while already tuned by hand shows
 * the operator hold, not a pending change. */
void
check_executor_adopted_and_retune() {
    Rig rig;
    rig.host.setPhase(dsd_qt::DecoderHost::Running);
    rig.roaming.noteSessionAdopted("saved");
    check(rig.roaming.state() == SiteRoaming::Idle && rig.roaming.reason() == SiteRoaming::SessionAdopted
              && rig.roaming.statusText() == QStringLiteral("Start listening again to use GPS roaming")
              && rig.host.streamStarts == 0,
          "adopted: idle, with its own reason, and no location");
    rig.fix(10.49, 20.0);
    check(rig.switches.isEmpty(), "adopted: never arms");
    rig.host.setPhase(dsd_qt::DecoderHost::Idle);
    check(rig.roaming.reason() == SiteRoaming::NoSession, "adopted: ended, nothing runs");
    rig.host.setPhase(dsd_qt::DecoderHost::Running);
    rig.roaming.noteSessionAdopted("scan");
    check(rig.roaming.reason() == SiteRoaming::NoSession, "adopted: only a saved session says so");
    rig.host.setPhase(dsd_qt::DecoderHost::Idle);

    rig.running();
    rig.set("operatorTuned", true);
    rig.roaming.noteOperatorIntent("manualTune");
    check(rig.roaming.reason() == SiteRoaming::OperatorTune
              && rig.roaming.statusText() == QStringLiteral("Paused while you tune by hand"),
          "retune: tuned by hand already, a further tune shows the operator hold");
}

/* The 1 s pass runs only while roaming judges a session or a switch is under way. */
void
check_executor_timer() {
    Rig rig;
    const QTimer* timer = rig.roaming.findChild<QTimer*>(QStringLiteral("siteRoamingTick"));
    check(timer != nullptr && !timer->isActive(), "timer: idle with nothing running");
    rig.running();
    check(timer->isActive(), "timer: armed (waiting for a fix)");
    rig.set("heldTg", 3);
    check(timer->isActive(), "timer: paused");
    rig.set("heldTg", 0);
    rig.host.setPhase(dsd_qt::DecoderHost::Idle);
    check(!timer->isActive(), "timer: stops once nothing runs");
    rig.qualifyB();
    rig.host.setPhase(dsd_qt::DecoderHost::Idle);
    check(timer->isActive() && rig.roaming.transactionActive(), "timer: runs through a switch");
    rig.prefs.setRoamingEnabled(false);
    check(!timer->isActive(), "timer: off");
}

/* The site the user switches to from a session roaming follows holds roaming until Resume roaming or another start; a
 * pick with nothing running, or one given up, holds nothing. */
void
check_executor_manual_hold() {
    Rig rig;
    rig.running();
    rig.roaming.noteSiteChoice(rig.uids[1]);
    rig.host.setPhase(dsd_qt::DecoderHost::Idle);
    rig.running(1);
    check(rig.roaming.state() == SiteRoaming::Holding && rig.roaming.action() == SiteRoaming::ResumeRoaming
              && rig.roaming.statusText() == QStringLiteral("Holding the site you chose"),
          "hold: the site chosen holds roaming");
    rig.fix(10.0, 20.0);
    rig.fix(10.0, 20.0, 10000);
    rig.fix(10.0, 20.0, 10000);
    check(rig.switches.isEmpty(), "hold: no switch while held");
    rig.roaming.performAction();
    check(rig.roaming.state() == SiteRoaming::Candidate || rig.roaming.state() == SiteRoaming::Paused,
          "hold: Resume roaming arms it");
    rig.roaming.noteSiteChoice(rig.uids[2]);
    rig.host.setPhase(dsd_qt::DecoderHost::Idle);
    rig.running(0);
    check(rig.roaming.state() != SiteRoaming::Holding, "hold: only the chosen site's own start holds");
    rig.host.setPhase(dsd_qt::DecoderHost::Idle);
    rig.roaming.noteSiteChoice(rig.uids[1]);
    rig.running(1);
    check(rig.roaming.state() != SiteRoaming::Holding, "hold: a pick with nothing running is a start like any other");
    // D3: a session roaming pauses on (tuned by hand) counts; one it idles on (a scan, an input it cannot retune) does
    // not.
    rig.host.setPhase(dsd_qt::DecoderHost::Idle);
    rig.running(0);
    rig.set("operatorTuned", true);
    check(rig.roaming.state() == SiteRoaming::Paused, "hold: hand-tuned is paused");
    rig.roaming.noteSiteChoice(rig.uids[1]);
    rig.set("operatorTuned", false);
    rig.host.setPhase(dsd_qt::DecoderHost::Idle);
    rig.running(1);
    check(rig.roaming.state() == SiteRoaming::Holding, "hold: a pick from a hand-tuned session holds");
    for (const char* key : {"scanKind", "radioInput"}) {
        rig.host.setPhase(dsd_qt::DecoderHost::Idle);
        rig.running(0);
        const bool scan = QLatin1String(key) == QLatin1String("scanKind");
        rig.set(key, scan ? QVariant(2) : QVariant(false));
        check(rig.roaming.state() == SiteRoaming::Idle, "hold: scanning or not tunable is idle");
        rig.roaming.noteSiteChoice(rig.uids[2]);
        rig.set(key, scan ? QVariant(0) : QVariant(true));
        rig.host.setPhase(dsd_qt::DecoderHost::Idle);
        rig.running(2);
        check(rig.roaming.state() != SiteRoaming::Holding,
              "hold: a pick from a session roaming idles on holds nothing");
    }
    rig.roaming.noteSiteChoice(rig.uids[2]);
    rig.roaming.forgetSiteChoice();
    rig.host.setPhase(dsd_qt::DecoderHost::Idle);
    rig.running(2);
    check(rig.roaming.state() != SiteRoaming::Holding, "hold: a pick whose start never happened holds nothing later");
}

/* The Monitor header's line: state first, the feature last, and the state alone for a narrow header; nothing while
 * Off or Idle. The system it follows is named as its Home card names it. */
void
check_executor_header_text() {
    Rig rig;
    const auto header = [&rig](const char* full, const char* shortForm) {
        return rig.roaming.headerText() == QString::fromUtf8(full)
               && rig.roaming.headerShortText() == QString::fromUtf8(shortForm);
    };
    check(header("", "") && rig.roaming.systemName().isEmpty(), "header: idle shows nothing");
    rig.running(0, false);
    check(header("", "") && rig.roaming.statusText() == QStringLiteral("GPS roaming starts once listening begins"),
          "header: a starting session is idle, with its sentence");
    rig.roaming.noteSessionInitialized();
    check(header("LOCATING · GPS ROAMING", "LOCATING") && rig.roaming.systemName() == QStringLiteral("Valley")
              && rig.roaming.statusText() == QStringLiteral("Waiting for a location fix"),
          "header: the session's first fix on its way, and the system by its card's name");
    rig.fix(10.0, 20.0);
    check(header("STAYING · GPS ROAMING", "STAYING"), "header: following");
    rig.set("callBusy", true);
    rig.nearB(0);
    check(header("CHECKING · GPS ROAMING", "CHECKING"), "header: a candidate");
    rig.nearB(10000);
    rig.nearB(10000);
    check(header("WAITING FOR CALL END · GPS ROAMING", "WAITING"), "header: waiting for the call");
    rig.set("heldTg", 5);
    check(header("PAUSED: TALKGROUP HELD · GPS ROAMING", "PAUSED"), "header: a held talkgroup");
    rig.set("heldTg", 0);
    rig.set("callBusy", false);
    rig.nearB(0);
    rig.nearB(10000);
    rig.nearB(10000);
    check(header("SWITCHING · GPS ROAMING", "SWITCHING"), "header: switching");
    rig.prefs.setRoamingEnabled(false);
    check(header("", "") && rig.roaming.systemName().isEmpty(), "header: off shows nothing");
}

/* A session's first wait for a fix reads as locating, not paused, until a fix comes or 60 s pass; a fix the session
 * had and lost reads paused. A new session (a start or a switch) locates again. */
void
check_executor_locating() {
    Rig rig;
    const auto header = [&rig](const char* full, const char* shortForm) {
        return rig.roaming.headerText() == QString::fromUtf8(full)
               && rig.roaming.headerShortText() == QString::fromUtf8(shortForm);
    };
    rig.running();
    rig.tick(59999);
    check(header("LOCATING · GPS ROAMING", "LOCATING"), "locating: just under a minute with no fix");
    rig.tick(1);
    check(header("PAUSED: NO FIX · GPS ROAMING", "PAUSED") && rig.roaming.reason() == SiteRoaming::NoFix,
          "locating: a minute with no fix is a pause");
    rig.host.setPhase(dsd_qt::DecoderHost::Idle);
    rig.running(1);
    check(header("LOCATING · GPS ROAMING", "LOCATING"), "locating: a new session locates again");
    rig.fix(10.5, 20.0, 1000);
    check(header("STAYING · GPS ROAMING", "STAYING"), "locating: its first fix");
    rig.tick(60001);
    check(header("PAUSED: NO FIX · GPS ROAMING", "PAUSED") && rig.roaming.reason() == SiteRoaming::NoFix,
          "locating: a fix had and lost is a pause");
    rig.host.epochValue = 1;
    rig.tick(500);
    check(header("PAUSED: NO FIX · GPS ROAMING", "PAUSED"),
          "locating: back in the foreground after a fix, still paused");
}

// ---- The feed shared with the map (#800) ----

using dsd_qt::LocationFeed;

/* The rig with roaming on a location feed it shares with the map, as qt_ui.cpp wires it. The feed outlives the rig. */
struct SharedRig {
    LocationFeed feed;
    Rig rig;

    SharedRig() {
        feed.setHost(&rig.host);
        rig.roaming.setLocationFeed(&feed);
    }

    void
    settle() {
        QCoreApplication::sendPostedEvents(&rig.roaming, QEvent::MetaCall);
    }
};

/* The map joins and leaves the feed roaming follows: no start or stop for either, and fixes keep reaching roaming. */
void
check_feed_map_joins_and_leaves() {
    SharedRig shared;
    Rig& rig = shared.rig;
    rig.running();
    rig.nearA(0);
    check(rig.roaming.state() == SiteRoaming::Following && rig.host.streamStarts == 1, "feed: roaming follows");
    shared.feed.acquire(LocationFeed::mapLease());
    rig.nearA(1000);
    check(rig.host.streamStarts == 1 && rig.host.streamStops == 0 && rig.roaming.state() == SiteRoaming::Following,
          "feed: the map joins roaming's feed");
    shared.feed.release(LocationFeed::mapLease());
    rig.nearA(1000);
    check(rig.host.streamStops == 0 && rig.roaming.state() == SiteRoaming::Following,
          "feed: the map leaving keeps roaming's feed");
    rig.prefs.setRoamingEnabled(false);
    check(rig.host.streamStops == 1 && !shared.feed.active(), "feed: roaming leaving last stops it");
}

/* A switch under way when the map takes the feed: the switch goes on untouched, and the map leaving again leaves
 * roaming's feed running for the checks before the destination starts. */
void
check_feed_pending_switch_while_map_acquires() {
    SharedRig shared;
    Rig& rig = shared.rig;
    rig.qualifyB();
    const qint64 id = rig.roamId();
    check(rig.host.switchStops == 1 && rig.roaming.state() == SiteRoaming::Switching, "feed: the switch is under way");
    shared.feed.acquire(LocationFeed::mapLease());
    rig.tick(1000);
    check(rig.host.streamStarts == 1 && rig.host.streamStops == 0 && rig.switches.size() == 1
              && rig.roaming.transactionActive() && rig.roaming.state() == SiteRoaming::Switching,
          "feed: the map's lease changes nothing for the switch");
    shared.feed.release(LocationFeed::mapLease());
    check(rig.host.streamStops == 0 && shared.feed.holds(LocationFeed::roamingLease()),
          "feed: roaming keeps the feed through its switch");
    rig.host.setPhase(dsd_qt::DecoderHost::Idle);
    const QVariantMap plan = rig.roaming.startPlan(id);
    check(plan.value("ok").toBool() && plan.value("uid").toString() == rig.uids[1],
          "feed: the switch starts its destination as before");
}

/* Roaming arming while the map already holds the feed joins it and reads where it stands at once. */
void
check_feed_roaming_joins_the_map() {
    SharedRig shared;
    Rig& rig = shared.rig;
    shared.feed.acquire(LocationFeed::mapLease());
    Q_EMIT rig.host.locationStreamUpdate({{"generation", 1}, {"state", "approximateOnly"}, {"hasFix", false}});
    rig.running();
    shared.settle();
    check(rig.host.streamStarts == 1 && rig.roaming.reason() == SiteRoaming::ApproximateOnly,
          "feed: roaming joins the map's feed and reads its state");
    rig.prefs.setRoamingEnabled(false);
    check(rig.host.streamStops == 0 && shared.feed.active(), "feed: roaming leaving keeps the map's feed");
}

/* The map's pass sees a foreground move first: roaming handles the move before the feed starts again, so the first
 * fix of the new epoch counts and roaming's own later look neither forgets it nor starts the feed a second time. */
void
check_feed_map_sees_the_foreground_move_first() {
    SharedRig shared;
    Rig& rig = shared.rig;
    rig.running();
    rig.nearB(0);
    rig.nearB(10000);
    shared.feed.acquire(LocationFeed::mapLease());
    check(rig.roaming.state() == SiteRoaming::Candidate, "feed: a candidate before the move");
    rig.host.epochValue = 1;
    shared.feed.acquire(LocationFeed::mapLease());
    check(rig.host.streamStarts == 2 && rig.roaming.reason() == SiteRoaming::NoFix,
          "feed: one start for the epoch, the candidate gone first");
    rig.nearA(1000);
    check(rig.roaming.state() == SiteRoaming::Following, "feed: the new epoch's first fix counts");
    rig.tick(500);
    check(rig.roaming.state() == SiteRoaming::Following && rig.host.streamStarts == 2 && rig.host.streamStops == 0,
          "feed: roaming's own look keeps that fix and starts nothing more");
}

/* The app goes to the background and back with roaming and the map on the feed: the feed's own return (Active) is the
 * first look, roaming handles the move before the restart, and the order of today holds: one start for the epoch, the
 * candidate gone, the new epoch's first fix counted and kept. */
void
check_feed_background_cycle() {
    SharedRig shared;
    Rig& rig = shared.rig;
    rig.running();
    rig.nearB(0);
    rig.nearB(10000);
    shared.feed.acquire(LocationFeed::mapLease());
    Q_EMIT qGuiApp->applicationStateChanged(Qt::ApplicationSuspended);
    check(shared.feed.record().isEmpty(), "feed: nothing from before the background reads as live");
    rig.host.epochValue = 1;
    Q_EMIT qGuiApp->applicationStateChanged(Qt::ApplicationActive);
    check(rig.host.streamStarts == 2 && rig.roaming.reason() == SiteRoaming::NoFix,
          "feed: back on screen, one start, roaming's candidate gone first");
    rig.nearA(1000);
    rig.tick(500);
    check(rig.roaming.state() == SiteRoaming::Following && rig.host.streamStarts == 2 && rig.host.streamStops == 0,
          "feed: the first fix after the return counts, and nothing starts twice");
}

/* A shared feed with no host refuses roaming's lease: roaming does not take itself for following one, and takes the
 * lease on a later pass once the feed has a host. */
void
check_feed_refused_lease() {
    SharedRig shared;
    Rig& rig = shared.rig;
    shared.feed.setHost(nullptr);
    rig.running();
    shared.settle();
    check(rig.host.streamStarts == 0 && !shared.feed.active(), "feed: no lease without a host");
    shared.feed.setHost(&rig.host);
    rig.tick(1000);
    check(rig.host.streamStarts == 1 && shared.feed.holds(LocationFeed::roamingLease()),
          "feed: the next pass takes the lease");
}

} // namespace

int
main(int argc, char** argv) {
    check_route();
    check_dwell_needs_span_and_count();
    check_cluster_then_silence();
    check_replayed_and_identical_fixes();
    check_boundary_jitter();
    check_winner_change();
    check_reset();
    check_ineligible_candidates();
    check_current_ineligible();
    check_out_of_range();
    check_fix_quality();
    check_accuracy_widens_margin();
    check_ties();
    check_settings_revoke();
    check_future_fix();
    check_invalid_settings();
    check_call_wait();

    QGuiApplication app(argc, argv); // the location feed follows the application state (#800)
    QCoreApplication::setOrganizationName(QStringLiteral("dsd-neo-test"));
    QCoreApplication::setApplicationName(QStringLiteral("site-roaming-%1").arg(QCoreApplication::applicationPid()));
    dsd_test_qt_isolate_paths();
    check_executor_arming();
    check_executor_route();
    check_executor_call_wait();
    check_executor_reset_drops_the_cached_fix();
    check_executor_barrier();
    check_executor_generation();
    check_executor_foreground();
    check_executor_starting();
    check_executor_manual_hold();
    check_executor_plan_out_of_turn();
    check_executor_stop_edges();
    check_executor_same_tick();
    check_executor_end_precedence();
    check_executor_notice_withdrawn();
    check_executor_operator_barriers();
    check_executor_metadata_changes();
    check_executor_adopted_and_retune();
    check_executor_timer();
    check_executor_header_text();
    check_executor_locating();
    check_feed_map_joins_and_leaves();
    check_feed_pending_switch_while_map_acquires();
    check_feed_roaming_joins_the_map();
    check_feed_map_sees_the_foreground_move_first();
    check_feed_background_cycle();
    check_feed_refused_lease();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    return 0;
}
