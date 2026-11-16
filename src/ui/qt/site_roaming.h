// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Which site of a saved system to follow from a location fix: the decision core of site roaming.
 *
 * Plain C++ with no Qt types and no clock reads, so every timing rule can be driven by a test. The caller supplies
 * every time on one monotonic clock in milliseconds: the evaluation time and each fix's time. It evaluates on each new
 * measurement, on a timer, on candidate or settings changes and at every transaction boundary; only a new measurement
 * can add evidence for a switch, and everything else can only take a verdict away.
 */

#ifndef DSD_NEO_SRC_UI_QT_SITE_ROAMING_H_
#define DSD_NEO_SRC_UI_QT_SITE_ROAMING_H_

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dsd_qt {

/** @brief The shortest span of fix times a switch needs, from its first to its latest qualifying measurement. */
constexpr std::int64_t kDwellMs = 20000;
/** @brief The fewest qualifying measurements a switch needs. */
constexpr int kMinDwellSamples = 3;
/** @brief A fix older than this pauses roaming, as does one stamped after the evaluation time. */
constexpr std::int64_t kMaxFixAgeMs = 60000;
/** @brief A fix with a larger accuracy radius, or none, pauses roaming. */
constexpr double kMaxAccuracyM = 1000;
/** @brief The longest a switch waits for a followed call before it goes ahead mid-call. */
constexpr std::int64_t kMaxCallWaitMs = 90000;
/**
 * @brief Two fixes whose elapsed-realtime stamps are at most this far apart (5 ms) are one physical fix: the fused
 * provider re-reports a GPS fix under its own name with the GPS stamp.
 */
constexpr std::int64_t kSameFixNs = 5000000;

/** @brief Whether elapsed-realtime stamps @p a_ns and @p b_ns are one physical fix (at most kSameFixNs apart). */
inline bool
site_roaming_same_fix(std::int64_t a_ns, std::int64_t b_ns) {
    return (a_ns > b_ns ? a_ns - b_ns : b_ns - a_ns) <= kSameFixNs;
}

/** @brief One site of the current system, in row order. */
struct SiteRoamingCandidate {
    std::string uid;
    bool avoid = false;
    bool has_position = false;
    double lat = 0;
    double lon = 0;
    /** Whether a roam may start this row (see the roamable rule in site_groups). */
    bool roamable = false;
};

/**
 * @brief A fix's identity: a replayed cached fix keeps it, a new fix at identical coordinates gets a new one. A fix is
 * new evidence only when its stamp is newer than the newest seen from its provider and more than kSameFixNs away from
 * the newest seen from each other provider, so one fix relayed by another provider counts once.
 */
struct SiteRoamingMeasurementId {
    std::string provider;
    std::int64_t elapsed_realtime_ns = 0;
};

struct SiteRoamingMeasurement {
    SiteRoamingMeasurementId id;
    double lat = 0;
    double lon = 0;
    bool has_accuracy = false;
    double accuracy_m = 0;
    /** When the fix was taken, on the caller's monotonic clock. */
    std::int64_t fix_at_ms = 0;
};

struct SiteRoamingSettings {
    double max_km = 0;
    double margin_km = 0;
};

enum class SiteRoamingPause {
    NoFix,
    StaleFix,
    ImpreciseFix,
    /** No candidate is unavoided, positioned and roamable. */
    NoRankableSite,
    /** The nearest rankable site is farther than the maximum distance. */
    OutOfRange,
    /** The maximum distance or the margin is not a finite number above zero. */
    InvalidSettings,
};

struct SiteRoamingVerdict {
    enum class Kind {
        /** Stay on the current site: it is the nearest, or no other site has earned a switch. */
        Following,
        /** @c uid is nearer by enough, but its evidence does not yet span the dwell. */
        Candidate,
        /** Switch to @c uid now. Valid only while each later evaluation derives it again. */
        Switch,
        /** Roaming cannot judge; @c reason says why. */
        Paused,
    };
    Kind kind = Kind::Following;
    /** Following: the current uid. Candidate and Switch: the destination. Paused: empty. */
    std::string uid;
    SiteRoamingPause reason = SiteRoamingPause::NoFix;
};

/** @brief The measurements that back a candidate: fix times of the first and latest, and how many. */
struct SiteRoamingEvidence {
    std::string uid;
    std::int64_t first_ms = 0;
    std::int64_t last_ms = 0;
    int count = 0;
};

class SiteRoamingCore {
  public:
    /**
     * @brief Judge the latest measurement against the candidates.
     *
     * @param now_ms       The evaluation time, on the same clock as each fix's @c fix_at_ms.
     * @param candidates   The current system's sites in row order. The current site is looked up among them; one
     *                     that is not listed is treated like an avoided or unpositioned one (a switch needs no margin).
     * @param current_uid  The site running now.
     * @param measurement  The latest measurement, or null when there is none.
     * @param settings     Maximum distance and margin, in km, each finite and above zero (else InvalidSettings).
     *                     A change clears the evidence.
     */
    SiteRoamingVerdict evaluate(std::int64_t now_ms, const std::vector<SiteRoamingCandidate>& candidates,
                                const std::string& current_uid, const SiteRoamingMeasurement* measurement,
                                const SiteRoamingSettings& settings);
    /** @brief Forget all evidence and every measurement seen. */
    void reset();
    /** @brief The evidence for the current candidate; a count of 0 means there is none. */
    const SiteRoamingEvidence& evidence() const;

  private:
    bool consumeNew(const SiteRoamingMeasurementId& id);
    void noteSettings(const SiteRoamingSettings& settings);
    SiteRoamingVerdict judgeRanked(const std::vector<SiteRoamingCandidate>& candidates, const std::string& current_uid,
                                   const SiteRoamingMeasurement& m, const SiteRoamingSettings& settings, bool fresh);
    SiteRoamingVerdict cleared(const SiteRoamingVerdict& v);

    // The newest elapsed-realtime stamp seen from each provider: a stamp at or below its own provider's, or within
    // kSameFixNs of another provider's, has been seen.
    std::map<std::string, std::int64_t> newest_ns_by_provider_;
    SiteRoamingEvidence evidence_;
    bool has_settings_ = false;
    SiteRoamingSettings settings_;
};

enum class CallWait {
    /** No call is followed: go ahead. */
    Clear,
    /** A call is followed: wait. */
    Waiting,
    /** A call has been followed for kMaxCallWaitMs: go ahead mid-call. */
    Expired,
};

/**
 * @brief The bounded wait of a switch for a followed call.
 *
 * The wait starts at the first busy observation and runs while the call stays busy; a free observation ends it. The
 * caller resets it whenever the verdict it waits for changes.
 */
class CallWaitClock {
  public:
    CallWait busy(std::int64_t now_ms, bool call_busy);
    void reset();

  private:
    bool waiting_ = false;
    std::int64_t since_ms_ = 0;
};

} // namespace dsd_qt

#endif
