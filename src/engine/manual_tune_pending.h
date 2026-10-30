// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Engine-private intake of manual-tune completions (issue #671): the RTL completion callback hands every request ID
 * with RTL_STREAM_TUNE_ID_MANUAL_BIT here, and every other ID to the trunk-tuning ledger. The record itself is
 * <dsd-neo/engine/manual_tune.h>.
 */

#ifndef DSD_NEO_SRC_ENGINE_MANUAL_TUNE_PENDING_H
#define DSD_NEO_SRC_ENGINE_MANUAL_TUNE_PENDING_H

#include <dsd-neo/engine/manual_tune.h>
#include <stdint.h>

/* How long a tune's failure toast stays up, as the frequency entry's own failure toast does: a manual tune's late
   failure, and a -Y resume's (issue #671). */
#define DSD_MANUAL_TUNE_FAILED_TOAST_S 4

/* What a noted tune was, as the poll that settled it took it. */
typedef struct {
    dsd_manual_tune_kind kind; /* 0 before any tune settled */
    uint32_t requested_hz;
    int row;
    uint32_t scan_session;
    uint64_t map_seq;
} dsd_manual_tune_settled;

/* The record the last dsd_engine_manual_tune_poll() that returned DSD_MANUAL_TUNE_LANDED, DSD_MANUAL_TUNE_FAILED or
   DSD_MANUAL_TUNE_DROPPED took, for the caller to settle what the tune was for (an untyped -Y step's row). Decoder
   thread. */
void dsd_engine_manual_tune_last_settled(dsd_manual_tune_settled* out);

/* Whether the noted tune's completion has come, and the next dsd_engine_manual_tune_poll() will act on it: a failure of
   any tune (the cached centre goes back, an entry's toast) or an untyped -Y step's landing (its row commits). The
   synced-frames loop ends a reception on it, so a frame decoded after it is never named after a frequency the receiver
   did not reach, nor after the row left. An entry's landing changes nothing the decoder names, so it ends nothing. A flag read
   and one atomic load while nothing is noted. Decoder thread. */
int dsd_engine_manual_tune_settled_ready(void);

/* Store the completion of the manual tune @p id (an rtl_stream_tune_result) for the decoder's next poll. Called on
   the RTL controller thread, or on the thread stopping the stream; never blocks. */
void dsd_engine_manual_tune_completion(uint64_t id, int result);

#endif /* DSD_NEO_SRC_ENGINE_MANUAL_TUNE_PENDING_H */
