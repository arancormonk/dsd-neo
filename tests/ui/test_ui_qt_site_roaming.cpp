// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/*
 * The site roaming decision core and the call wait cap, driven on a scripted clock. Distances are made up and
 * generous; where a case needs a distance near the margin it measures it with site_distance_km itself, so the cases
 * hold for any accurate distance formula.
 */
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include "site_groups.h"
#include "site_roaming.h"

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
    check(core.evidence().count == step + 1 && core.evidence().last_ms - core.evidence().first_ms == 10000 * step,
          "route: evidence counts the qualifying fixes and spans their fix times");
}

// A 1 s timer for the 9 s after fix @p m: each tick derives the same verdict and adds no evidence.
void
check_route_ticks(SiteRoamingCore& core, const Sites& sites, const std::string& current,
                  const SiteRoamingMeasurement& m, const SiteRoamingVerdict& v) {
    for (int tick = 1; tick < 10; ++tick) {
        const int count = core.evidence().count;
        const SiteRoamingVerdict ticked = core.evaluate(m.fix_at_ms + 1000 * tick, sites, current, &m, kSettings);
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

/* A replayed cached fix (same provider and stamp) adds nothing, however often or late it arrives, even restamped;
 * a new fix at identical coordinates counts. */
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
    SiteRoamingMeasurement other = nearB(42000);
    other.id.provider = "network";
    other.id.elapsed_realtime_ns = first.id.elapsed_realtime_ns;
    core.evaluate(42000, sites, "A", &other, kSettings);
    check(core.evidence().count == 2, "replay: the same stamp from another provider is new");
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
            const SiteRoamingVerdict v = core.evaluate(t + 1000 * tick, sites, current, &m, kSettings);
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

} // namespace

int
main() {
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
    check_call_wait();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    return 0;
}
