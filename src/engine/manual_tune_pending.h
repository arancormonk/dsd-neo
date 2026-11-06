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

#include <dsd-neo/core/state_fwd.h>
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
    uint64_t list_rev;
    int dir;        /* a walk's direction (DSD_MANUAL_TUNE_LCN_CYCLE): 1 Next, -1 Previous; 0 for any other kind */
    int scan_owned; /* a walk the running -Y scan owns (DSD_MANUAL_TUNE_LCN_CYCLE) */
    uint64_t id;    /* the tune's own ID, which an entry waiting on its row names (issue #644) */
} dsd_manual_tune_settled;

/* The record the last dsd_engine_manual_tune_poll() that returned DSD_MANUAL_TUNE_LANDED, DSD_MANUAL_TUNE_FAILED or
   DSD_MANUAL_TUNE_DROPPED took, for the caller to settle what the tune was for (an untyped -Y step's row). Decoder
   thread. */
void dsd_engine_manual_tune_last_settled(dsd_manual_tune_settled* out);

/* Whether the noted tune's completion has come, and the next dsd_engine_manual_tune_poll() will act on it: a failure of
   any tune (the cached centre goes back, an entry's toast), an untyped -Y step's or a Next or Previous walk's landing
   (its row commits; issue #644) or a RadioReference import's landing (its toast, issue #803). The synced-frames loop
   ends a reception on it, so a frame decoded after it is never named after a frequency the receiver did not reach, nor
   after the row left, and the outcome is posted while the carrier keeps sync. An entry's landing changes nothing the
   decoder names, so it ends nothing. A flag read and one atomic load while nothing is noted. Decoder thread. */
int dsd_engine_manual_tune_settled_ready(void);

/* Whether a manual tune of any kind is noted and its completion still awaited: it decides where the receiver goes, so
   no state machine moves the tuner meanwhile (dsd_engine_tuner_free_for_protocol()). Decoder thread. */
int dsd_engine_manual_tune_noted(void);

/* The ID of the tune noted, until a poll takes it, whatever its completion already said or its stream: which request
   the record tracks, for what follows its own outcome (an entry bound to it, a resume rebound to it, issue #644);
   whether a new request may join it is dsd_engine_manual_tune_pending_read()'s. 0 with none. Decoder thread. */
uint64_t dsd_engine_manual_tune_noted_id(void);

/* The tune noted, read once for a join (issue #644): its completion can come on the controller's thread between two
   separate queries, so a caller that checks and then names the tune reads both from one view. */
typedef struct {
    int row;       /* its -Y row on @p state's current list and scan session (a walk's or the step's), else -1 */
    int scan_walk; /* a walk the running -Y scan owns */
    int step;      /* the untyped -Y step's own tune (a timed step's or a resume's) */
    int joinable;  /* whether a new request may join it: live, and no failure or drop came for it */
    uint64_t id;   /* which request it is (dsd_engine_manual_tune_noted_id()), 0 with none */
} dsd_engine_manual_tune_pending_view;

/* Fill @p out with one read of the tune noted. Decoder thread. */
void dsd_engine_manual_tune_pending_read(const dsd_state* state, dsd_engine_manual_tune_pending_view* out);

/* Whether a Next or Previous walk off any scan (DSD_MANUAL_TUNE_LCN_CYCLE, not the -Y scan's own) is noted and its
   completion still awaited, or its landing or late failure was taken and its settling has not finished: the
   operator's move (not one asked of a stream that has since stopped), which owns the tuner until it settles, so no
   trunking state machine retunes meanwhile (dsd_engine_trunk_tune_to_freq_request() and its siblings answer DEFERRED;
   the no-carrier control-channel return waits). Atomic loads; any thread (the P25 watchdog's tick included). */
int dsd_engine_manual_tune_operator_walk_noted(void);

/* The settling of a walk whose completion a poll took (its row, its handoffs) is done: the walk no longer owns the
   tuner. Decoder thread, after dsd_engine_manual_tune_poll() returned for it. */
void dsd_engine_manual_tune_walk_settled(void);

/* The tune noted as @p id, still awaited, now belongs to -Y scan session @p session: a resume-at that replaced a
   resume still pending moved the session after its own tune was noted, so the resume it replaced, whatever its tune
   comes to, is no longer the scan's (issue #644). Nothing for any other tune. Decoder thread. */
void dsd_engine_manual_tune_rebind_session(uint64_t id, uint32_t session);

/* Store the completion of the manual tune @p id (an rtl_stream_tune_result) for the decoder's next poll. Called on
   the RTL controller thread, or on the thread stopping the stream; never blocks. */
void dsd_engine_manual_tune_completion(uint64_t id, int result);

#endif /* DSD_NEO_SRC_ENGINE_MANUAL_TUNE_PENDING_H */
