// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
#include "site_roaming.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include "site_groups.h"

namespace dsd_qt {
namespace {

// Settings are stored km values; a change smaller than this is the same setting read back.
constexpr double kSettingsToleranceKm = 1e-9;

bool
eligible(const SiteRoamingCandidate& c) {
    return !c.avoid && c.has_position && c.roamable && site_position_valid(c.lat, c.lon);
}

// Why @p m cannot be judged at @p now_ms; false when it can.
bool
fix_unusable(std::int64_t now_ms, const SiteRoamingMeasurement* m, SiteRoamingPause* reason) {
    if (m == nullptr || !site_position_valid(m->lat, m->lon)) {
        *reason = SiteRoamingPause::NoFix;
        return true;
    }
    // A fix stamped after now is on another clock: its age cannot be judged, so it is never fresh.
    if (m->fix_at_ms > now_ms || now_ms - m->fix_at_ms > kMaxFixAgeMs) {
        *reason = SiteRoamingPause::StaleFix;
        return true;
    }
    if (!m->has_accuracy || !(m->accuracy_m >= 0) || m->accuracy_m > kMaxAccuracyM) {
        *reason = SiteRoamingPause::ImpreciseFix;
        return true;
    }
    return false;
}

bool
settings_valid(const SiteRoamingSettings& s) {
    return std::isfinite(s.max_km) && s.max_km > 0 && std::isfinite(s.margin_km) && s.margin_km > 0;
}

// The nearest eligible candidate (the first in row order on a tie), -1 when none; its distance in @p best_km.
int
nearest_eligible(const std::vector<SiteRoamingCandidate>& candidates, double lat, double lon, double* best_km) {
    int best = -1;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const SiteRoamingCandidate& c = candidates[i];
        if (!eligible(c)) {
            continue;
        }
        const double d = site_distance_km(lat, lon, c.lat, c.lon);
        if (d >= 0 && (best < 0 || d < *best_km)) {
            best = static_cast<int>(i);
            *best_km = d;
        }
    }
    return best;
}

// The current site's distance when it is eligible and listed, else -1: an ineligible current site needs no margin.
double
current_distance_km(const std::vector<SiteRoamingCandidate>& candidates, const std::string& current_uid, double lat,
                    double lon) {
    const auto it = std::find_if(candidates.cbegin(), candidates.cend(),
                                 [&current_uid](const SiteRoamingCandidate& c) { return c.uid == current_uid; });
    return it != candidates.cend() && eligible(*it) ? site_distance_km(lat, lon, it->lat, it->lon) : -1;
}

SiteRoamingVerdict
verdict(SiteRoamingVerdict::Kind kind, const std::string& uid) {
    SiteRoamingVerdict v;
    v.kind = kind;
    v.uid = uid;
    return v;
}

SiteRoamingVerdict
paused(SiteRoamingPause reason) {
    SiteRoamingVerdict v;
    v.kind = SiteRoamingVerdict::Kind::Paused;
    v.reason = reason;
    return v;
}

} // namespace

SiteRoamingVerdict
SiteRoamingCore::evaluate(std::int64_t now_ms, const std::vector<SiteRoamingCandidate>& candidates,
                          const std::string& current_uid, const SiteRoamingMeasurement* measurement,
                          const SiteRoamingSettings& settings) {
    // Seen once, whatever the verdict: a fix that paused roaming never counts later either.
    const bool fresh = measurement != nullptr && consumeNew(measurement->id);
    // Invalid settings are never stored, so the change detector only ever compares finite values.
    if (!settings_valid(settings)) {
        return cleared(paused(SiteRoamingPause::InvalidSettings));
    }
    noteSettings(settings);
    SiteRoamingPause reason = SiteRoamingPause::NoFix;
    if (fix_unusable(now_ms, measurement, &reason)) {
        return cleared(paused(reason));
    }
    return judgeRanked(candidates, current_uid, *measurement, settings, fresh);
}

SiteRoamingVerdict
SiteRoamingCore::judgeRanked(const std::vector<SiteRoamingCandidate>& candidates, const std::string& current_uid,
                             const SiteRoamingMeasurement& m, const SiteRoamingSettings& settings, bool fresh) {
    using Kind = SiteRoamingVerdict::Kind;
    double best_km = 0;
    const int best = nearest_eligible(candidates, m.lat, m.lon, &best_km);
    if (best < 0) {
        return cleared(paused(SiteRoamingPause::NoRankableSite));
    }
    if (best_km > settings.max_km) {
        return cleared(paused(SiteRoamingPause::OutOfRange));
    }
    const std::string& uid = candidates[static_cast<std::size_t>(best)].uid;
    const double current_km = current_distance_km(candidates, current_uid, m.lat, m.lon);
    const bool worthy =
        uid != current_uid && (current_km < 0 || current_km - best_km >= settings.margin_km + 2 * m.accuracy_m / 1000);
    if (!worthy) {
        return cleared(verdict(Kind::Following, current_uid));
    }
    if (fresh && evidence_.count > 0 && evidence_.uid == uid) {
        evidence_.first_ms = std::min(evidence_.first_ms, m.fix_at_ms);
        evidence_.last_ms = std::max(evidence_.last_ms, m.fix_at_ms);
        ++evidence_.count;
    } else if (fresh) {
        evidence_ = SiteRoamingEvidence{uid, m.fix_at_ms, m.fix_at_ms, 1};
    } else if (evidence_.count == 0 || evidence_.uid != uid) {
        // A seen measurement, a timer tick or a list change never adds evidence; a new winner only revokes.
        return cleared(verdict(Kind::Following, current_uid));
    }
    const bool dwelled = evidence_.last_ms - evidence_.first_ms >= kDwellMs && evidence_.count >= kMinDwellSamples;
    return verdict(dwelled ? Kind::Switch : Kind::Candidate, uid);
}

SiteRoamingVerdict
SiteRoamingCore::cleared(const SiteRoamingVerdict& v) {
    evidence_ = SiteRoamingEvidence{};
    return v;
}

bool
SiteRoamingCore::consumeNew(const SiteRoamingMeasurementId& id) {
    const auto it = newest_ns_by_provider_.find(id.provider);
    if (it != newest_ns_by_provider_.end() && id.elapsed_realtime_ns <= it->second) {
        return false;
    }
    newest_ns_by_provider_[id.provider] = id.elapsed_realtime_ns;
    return true;
}

void
SiteRoamingCore::noteSettings(const SiteRoamingSettings& settings) {
    const bool changed = !has_settings_ || std::fabs(settings.max_km - settings_.max_km) > kSettingsToleranceKm
                         || std::fabs(settings.margin_km - settings_.margin_km) > kSettingsToleranceKm;
    if (changed && has_settings_) {
        evidence_ = SiteRoamingEvidence{};
    }
    has_settings_ = true;
    settings_ = settings;
}

void
SiteRoamingCore::reset() {
    newest_ns_by_provider_.clear();
    evidence_ = SiteRoamingEvidence{};
}

const SiteRoamingEvidence&
SiteRoamingCore::evidence() const {
    return evidence_;
}

CallWait
CallWaitClock::busy(std::int64_t now_ms, bool call_busy) {
    if (!call_busy) {
        waiting_ = false;
        return CallWait::Clear;
    }
    if (!waiting_) {
        waiting_ = true;
        since_ms_ = now_ms;
    }
    return now_ms - since_ms_ >= kMaxCallWaitMs ? CallWait::Expired : CallWait::Waiting;
}

void
CallWaitClock::reset() {
    waiting_ = false;
}

} // namespace dsd_qt
