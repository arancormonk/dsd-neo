// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The encrypted-audio mute a DMR-family slot verdict is read with.
 *
 * `dsd_opts::dmr_mute_encL/R` hold the user's "mute encrypted audio" choice and only user actions write them (the
 * menu toggle, a key load, a scan row's options). Decoding never does (issue #657): reverse mute (`-q`) and a key the
 * vocoder decrypts a frame with are applied to the frame's verdict, not to the options.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_DMR_ENC_MUTE_H
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_DMR_ENC_MUTE_H

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Whether a slot flagged encrypted is muted: 1 under reverse mute (`-q`), where the flipped slot flag alone
 * decides, else the user's encrypted-audio mute for slot @p slot (0 or 1).
 */
static inline int
dsd_dmr_slot_mute_enc(const dsd_opts* opts, int slot) {
    if (opts->reverse_mute == 1) {
        return 1;
    }
    return ((slot == 1) ? opts->dmr_mute_encR : opts->dmr_mute_encL) != 0 ? 1 : 0;
}

#ifdef __cplusplus
}
#endif

#endif // DSD_NEO_INCLUDE_DSD_NEO_CORE_DMR_ENC_MUTE_H
