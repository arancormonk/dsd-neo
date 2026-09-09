// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Folds the service state machine and the engine atomic into one UI phase.
 *
 * Two sources disagree about what "running" means. DecoderService owns the
 * IDLE/STARTING/RUNNING/STOPPING transitions, but only the native @c g_running atomic
 * knows whether the engine loop actually turns; a service that says RUNNING may still
 * be inside nativeConfigure. Neither alone can tell a start that failed from a start
 * that never happened, because DecoderService.failStart() drops straight back to IDLE.
 *
 * Deliberately free of Qt and JNI so the host test can drive it directly — the whole
 * point of pulling it out of DecoderHostAndroid::refresh().
 */

#ifndef DSD_NEO_ANDROID_SESSION_STATE_MAP_H_
#define DSD_NEO_ANDROID_SESSION_STATE_MAP_H_

#include <string.h>
#include "run_status.h"

namespace dsd_android {

/**
 * @brief What the UI should show. Mirrors dsd_qt::DecoderHost::SessionState.
 *
 * The values are pinned because decoder_host_android.cpp static_asserts them against
 * the Q_ENUM the QML binds to.
 */
enum SessionPhase {
    kSessionIdle = 0,
    kSessionStarting = 1,
    kSessionRunning = 2,
    kSessionStopping = 3,
    kSessionFailed = 4
};

/**
 * @brief How many IDLE polls to tolerate between asking for a start and seeing it.
 *
 * startForegroundService() only queues an intent, so the service is still IDLE for a
 * tick or two afterwards. At the UI's 250 ms poll this is a two-second grace period,
 * after which an undelivered start is reported as a failure rather than silently
 * dropping the user back to the setup screen.
 */
constexpr int kStartGraceTicks = 8;

/**
 * @brief Sticky-failure state machine over successive polls.
 *
 * Not thread-safe: like everything else the UI touches, it belongs to the Qt main
 * thread and is driven from the single poll tick.
 */
class SessionPhaseTracker {
  public:
    /**
     * @brief Note that a start was just asked for.
     *
     * Clears any previous failure and holds the phase at Starting through the gap
     * before the service picks the intent up.
     */
    void
    note_start_requested() {
        m_waiting_for_session = false;
        m_failed = false;
        m_attempting = true;
        m_saw_running = false;
        m_grace = kStartGraceTicks;
        m_phase = kSessionStarting;
    }

    /** Ignore the previous session's retained result while an intent is in flight. */
    void
    note_start_requested(uint64_t last_session) {
        note_start_requested();
        m_last_session = last_session;
        m_waiting_for_session = true;
    }

    /** A native return does not release the service's wake lock or worker slot.
     * The host must check a fresh service record before submitting a retry, even
     * if its last published UI phase still allowed starting. A rejected retry
     * leaves the failure/session latch and start grace period untouched. */
    bool
    note_start_requested(uint64_t last_session, const char* service_state) {
        if (!service_state || strcmp(service_state, "IDLE") != 0) {
            return false;
        }
        note_start_requested(last_session);
        return true;
    }

    SessionPhase
    update(const char* service_state, bool engine_running, uint64_t session_id, RunReason reason) {
        if (m_waiting_for_session && session_id <= m_last_session) {
            m_phase = idle_phase();
            return m_phase;
        }
        if (session_id < m_last_session) {
            return m_phase;
        }
        if (session_id > m_last_session) {
            m_last_session = session_id;
            m_waiting_for_session = false;
            m_failed = false;
        }
        // Retain the terminal outcome immediately, but only service IDLE means
        // the wake lock and worker slot have been released. Publishing Failed or
        // Idle earlier enables a retry the service would reject, followed by a
        // misleading start timeout. Stopping keeps restart disabled in that gap.
        if (reason == kRunFailed || reason == kRunCompleted || reason == kRunCancelled) {
            if (reason == kRunFailed) {
                (void)latch_failure();
            } else {
                m_attempting = false;
                m_saw_running = false;
                m_grace = 0;
            }
            const bool service_idle = service_state && strcmp(service_state, "IDLE") == 0;
            m_phase = service_idle ? (m_failed ? kSessionFailed : kSessionIdle) : kSessionStopping;
            return m_phase;
        }
        return update(service_state, engine_running);
    }

    /**
     * @brief Note that the start could not even be handed to the service.
     *
     * The host calls this when it has no Android context or cannot marshal the
     * arguments — failures that never reach DecoderService at all.
     */
    void
    note_start_failed() {
        m_failed = true;
        m_attempting = false;
        m_saw_running = false;
        m_grace = 0;
        m_phase = kSessionFailed;
    }

    /**
     * @brief Fold one poll into the phase.
     * @param service_state DecoderService.stateName(), or null when the call failed.
     * @param engine_running The native g_running atomic.
     */
    SessionPhase
    update(const char* service_state, bool engine_running) {
        const char* name = (service_state != nullptr) ? service_state : "IDLE";

        if (strcmp(name, "STARTING") == 0) {
            m_attempting = true;
            m_grace = 0;
            m_phase = kSessionStarting;
        } else if (strcmp(name, "RUNNING") == 0) {
            m_attempting = true;
            m_grace = 0;
            // The service flips to RUNNING before the engine thread has entered the
            // loop, so the atomic is what promotes Starting to Running.
            if (engine_running) {
                m_saw_running = true;
                m_phase = kSessionRunning;
            } else {
                m_phase = m_saw_running ? kSessionStopping : kSessionStarting;
            }
        } else if (strcmp(name, "STOPPING") == 0) {
            m_grace = 0;
            m_phase = kSessionStopping;
        } else {
            m_phase = idle_phase();
        }
        return m_phase;
    }

    SessionPhase
    phase() const {
        return m_phase;
    }

    /** @brief Whether the current session has a latched failure, including during teardown. */
    bool
    failed() const {
        return m_failed;
    }

  private:
    /**
     * @brief Resolve an IDLE poll, which is three different things.
     *
     * Before the service has seen the intent it means "not yet"; after a start that
     * reached STARTING but never ran it means the start failed; otherwise it is a
     * genuine idle. A failure latches until the next start request so the reason
     * stays on screen instead of flashing past in one 250 ms tick.
     */
    SessionPhase
    idle_phase() {
        if (m_grace > 0) {
            m_grace--;
            return (m_grace > 0) ? kSessionStarting : latch_failure();
        }
        if (m_attempting && !m_saw_running) {
            return latch_failure();
        }
        m_attempting = false;
        m_saw_running = false;
        return m_failed ? kSessionFailed : kSessionIdle;
    }

    SessionPhase
    latch_failure() {
        m_failed = true;
        m_attempting = false;
        m_saw_running = false;
        m_grace = 0;
        return kSessionFailed;
    }

    SessionPhase m_phase = kSessionIdle;
    uint64_t m_last_session = 0;
    bool m_waiting_for_session = false;
    bool m_failed = false;
    bool m_attempting = false;
    bool m_saw_running = false;
    int m_grace = 0;
};

} // namespace dsd_android

#endif /* DSD_NEO_ANDROID_SESSION_STATE_MAP_H_ */
