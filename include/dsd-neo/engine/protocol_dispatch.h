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
 *
 * PROFILE_PROVEN is the same rule in the other direction, and answers a question
 * PRODUCTIVE cannot: consumption credit is bounded by what the handler read, so a frame
 * that validates hard but reads a fraction of its slot still leaves the profile paying
 * for the rest of it. A control channel decoding a one-block TSDU reads 134 of ~180
 * symbols and can never get ahead of the failures between (#400). Report PROFILE_PROVEN
 * when a check the protocol ran says the signal on this profile is the protocol -- the
 * profile has re-earned its dwell, whatever the frame cost to read.
 *
 * WITHHELD answers a third question, and it is about the engine rather than the protocol:
 * the sync was real and the handler never got to see it. Trunked DMR skips the MS
 * bootstrap and MS data paths outright, and a frame the retune generation makes
 * undispatchable skips processFrame() entirely -- so the search that found the sync is
 * charged and nothing is ever consumed to pay it back, and the hunt rotates off a channel
 * the engine had just deliberately tuned (#392). A withheld frame makes its own search
 * cycle budget-neutral: the charge is refunded, no more. It is not credit, because the
 * engine's reason for declining says nothing about whether this is the right profile.
 *
 * That neutrality has a bounded cost in the other direction: a stream of false DMR MS
 * syncs under trunking accrues nothing, so it cannot reach a dwell either. It stays
 * bounded because every other protocol's false syncs still charge -- their handlers run
 * and report UNPRODUCTIVE -- and because the undispatchable case resolves with the retune
 * that caused it. Report WITHHELD only where the engine declined by policy, never where a
 * handler ran and found nothing.
 *
 * The numeric values are load-bearing. src/dsp reads the verdict out of
 * dsd_state::sps_hunt_last_frame_verdict without this header (the DSP layer includes no
 * engine headers), so frame_sync_sps_hunt_note_handler_consumption() compares against
 * the literals. A value it does not know is treated as UNPRODUCTIVE, which is the safe
 * direction for the budget but not for live traffic: a new verdict owes that function a
 * matching branch in the same change.
 */
typedef enum {
    DSD_FRAME_VERDICT_PRODUCTIVE = 0,     /**< default: assume the handler decoded a frame */
    DSD_FRAME_VERDICT_UNPRODUCTIVE = 1,   /**< consumed symbols, validated nothing */
    DSD_FRAME_VERDICT_PROFILE_PROVEN = 2, /**< a check passed: the profile re-earns its dwell */
    DSD_FRAME_VERDICT_WITHHELD = 3,       /**< the engine declined to dispatch: budget-neutral */
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
