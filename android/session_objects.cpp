// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief SessionObjects: the decoder options/state pair, reset in place before each configure (issue #786 B1).
 */

#include "session_objects.h"

#include <cstdlib>

#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/engine/engine.h>

namespace dsd_android {

bool
SessionObjects::create() {
    if (opts_ != nullptr || state_ != nullptr) {
        return false;
    }
    dsd_opts* new_opts = static_cast<dsd_opts*>(calloc(1, sizeof(dsd_opts)));
    dsd_state* new_state = static_cast<dsd_state*>(calloc(1, sizeof(dsd_state)));
    if (new_opts == nullptr || new_state == nullptr) {
        free(new_opts);
        free(new_state);
        return false;
    }
    initOpts(new_opts);
    initState(new_state);
    if (new_state->init_failed != 0U) {
        /* initState() logged which buffer it could not allocate; a state missing one cannot decode. */
        freeState(new_state);
        free(new_opts);
        free(new_state);
        return false;
    }
    opts_ = new_opts;
    state_ = new_state;
    used_ = false;
    return true;
}

void
SessionObjects::destroy() {
    if (opts_ != nullptr && state_ != nullptr) {
        /* A session configured and never run still holds the output files its bootstrap opened (#782). */
        dsd_engine_release_outputs(opts_, state_);
    }
    if (state_ != nullptr) {
        freeState(state_);
    }
    free(opts_);
    free(state_);
    opts_ = nullptr;
    state_ = nullptr;
    used_ = false;
}

void
SessionObjects::release_outputs() {
    if (exists()) {
        dsd_engine_release_outputs(opts_, state_);
    }
}

bool
SessionObjects::reset() {
    /* Before the erase, which would otherwise drop the open streams' handles. */
    dsd_engine_release_outputs(opts_, state_);
    freeState(state_);
    /* Erased, not just cleared: the last session's keys must not outlive it. Both start zeroed, as create()'s calloc. */
    DSD_SECURE_ZERO(opts_, sizeof(dsd_opts));
    DSD_SECURE_ZERO(state_, sizeof(dsd_state));
    initOpts(opts_);
    initState(state_);
    return state_->init_failed == 0U;
}

bool
SessionObjects::prepare_for_configure() {
    if (!exists()) {
        return false;
    }
    if (used_ && !reset()) {
        return false;
    }
    used_ = true;
    return true;
}

} // namespace dsd_android
