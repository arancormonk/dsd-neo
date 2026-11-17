// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The Android service's one decoder options/state pair, fresh for every configure (issue #786 B1).
 *
 * DecoderService can hand a new start to the instance that ran the last one, before its onDestroy(), so the process
 * keeps one dsd_opts/dsd_state pair from nativeInit() to nativeDestroy() and configures every session on it. The
 * bootstrap only adds to what it finds (a -C channel map appends at lcn_freq_count, an option sets its flag and nothing
 * clears the ones it does not name), and a run marks the state used (dsd_state::engine_fresh), so every configure
 * after the first starts from objects reset to what nativeInit() made. The reset keeps both allocations, so the
 * pointers stay valid for every reader that holds them.
 *
 * Free of JNI and Qt so the host build tests it (ANDROID_SESSION_OBJECTS). Not thread-safe: the JNI layer calls it under
 * its engine lock, never while a run uses the objects. It has no destructor that frees: the JNI layer's instance is a
 * global, and freeing it during static destruction at process exit could pull the objects from under a decoder thread
 * still running, so only destroy() (nativeDestroy()) frees them.
 */

#ifndef DSD_NEO_ANDROID_SESSION_OBJECTS_H_
#define DSD_NEO_ANDROID_SESSION_OBJECTS_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

namespace dsd_android {

class SessionObjects {
  public:
    SessionObjects() = default;
    SessionObjects(const SessionObjects&) = delete;
    SessionObjects& operator=(const SessionObjects&) = delete;

    /**
     * @brief Allocates and initialises the pair (initOpts(), initState()).
     * @return false when the objects already exist, an allocation failed or initState() set `init_failed`; nothing is
     *         kept on failure.
     */
    bool create();

    /** @brief Closes what a configured session left open, frees both objects and forgets them; safe when none exist. */
    void destroy();

    /**
     * @brief Readies the objects for one configure attempt; call it at the start of every attempt.
     *
     * When an earlier attempt touched the objects, closes the output files it left open
     * (dsd_engine_release_outputs()), then resets both in place: freeState(), both structures erased, initOpts(),
     * initState(). Marks the objects used either way, since the attempt's bootstrap writes to them.
     *
     * @return false when no objects exist, or initState() set `init_failed` during the reset. The objects then must
     *         not be configured or run; they stay marked used, so the next call resets them again.
     */
    bool prepare_for_configure();

    /** @brief Closes the output files a configured session that never ran left open (dsd_engine_release_outputs()). */
    void release_outputs();

    bool
    exists() const {
        return opts_ != nullptr && state_ != nullptr;
    }

    dsd_opts*
    opts() const {
        return opts_;
    }

    dsd_state*
    state() const {
        return state_;
    }

  private:
    bool reset();

    dsd_opts* opts_ = nullptr;
    dsd_state* state_ = nullptr;
    /* Set once a configure attempt may have written to the objects; cleared by create() and destroy(). */
    bool used_ = false;
};

} // namespace dsd_android

#endif /* DSD_NEO_ANDROID_SESSION_OBJECTS_H_ */
