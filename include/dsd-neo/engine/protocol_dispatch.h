// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Protocol dispatch interface for mapping synctypes to handlers.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_ENGINE_PROTOCOL_DISPATCH_H_
#define DSD_NEO_INCLUDE_DSD_NEO_ENGINE_PROTOCOL_DISPATCH_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief What a frame handler made of the symbols it consumed.
 *
 * The SPS hunt pays a profile for the symbols its handlers consume, and cannot tell a
 * decoded frame from a block skipped on a sync no CRC would accept by size alone. This
 * is how a handler that knows says so.
 *
 * PRODUCTIVE is zero and therefore the default: a handler with no verdict to give, a
 * synctype with no handler at all, and a zeroed dsd_state all read as "decoded a frame".
 * That is deliberate. A site that decodes successfully but fails to report it would make
 * the hunt rotate off live traffic, which is the one failure this must not have; a site
 * that consumes nothing and claims productivity costs at most the symbols it took. Report
 * UNPRODUCTIVE only from a check the protocol actually ran and actually failed -- never
 * from a threshold that guesses.
 */
typedef enum {
    DSD_FRAME_VERDICT_PRODUCTIVE = 0,   /**< default: assume the handler decoded a frame */
    DSD_FRAME_VERDICT_UNPRODUCTIVE = 1, /**< consumed symbols, validated nothing */
} dsd_frame_verdict;

typedef struct dsd_protocol_handler {
    const char* name;
    int (*matches_synctype)(int synctype);
    dsd_frame_verdict (*handle_frame)(dsd_opts* opts, dsd_state* state);
    void (*on_reset)(dsd_opts* opts, dsd_state* state);
} dsd_protocol_handler;

extern const dsd_protocol_handler dsd_protocol_handlers[];

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_ENGINE_PROTOCOL_DISPATCH_H_ */
