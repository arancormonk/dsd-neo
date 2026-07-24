// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef DSD_NEO_SRC_CORE_UTIL_CALL_STATE_INTERNAL_H_
#define DSD_NEO_SRC_CORE_UTIL_CALL_STATE_INTERNAL_H_

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/platform/threading.h>
#include <stdint.h>
#include "dsd-neo/core/state_fwd.h"

typedef dsd_call_event_lifecycle_snapshot dsd_call_event_lifecycle;

typedef struct {
    dsd_mutex_t mutex;
    dsd_call_state_snapshot calls;
    dsd_recent_activity_snapshot recent;
    dsd_call_event_lifecycle events[DSD_CALL_STATE_SLOT_COUNT];
    uint64_t epoch_sequence[DSD_CALL_STATE_SLOT_COUNT];
} dsd_call_state_ext;

dsd_call_state_ext* dsd_call_state_ext_get(dsd_state* state, int create);
const dsd_call_state_ext* dsd_call_state_ext_peek(const dsd_state* state);
void dsd_call_state_ext_lock(const dsd_call_state_ext* ext);
void dsd_call_state_ext_unlock(const dsd_call_state_ext* ext);

#endif /* DSD_NEO_SRC_CORE_UTIL_CALL_STATE_INTERNAL_H_ */
