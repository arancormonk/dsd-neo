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

#include <stdint.h>

/* Store the completion of the manual tune @p id (an rtl_stream_tune_result) for the decoder's next poll. Called on
   the RTL controller thread, or on the thread stopping the stream; never blocks. */
void dsd_engine_manual_tune_completion(uint64_t id, int result);

#endif /* DSD_NEO_SRC_ENGINE_MANUAL_TUNE_PENDING_H */
