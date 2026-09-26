// SPDX-License-Identifier: ISC
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

#ifndef DSD_NEO_CORE_UTIL_DSD_EVENT_STAGING_H_
#define DSD_NEO_CORE_UTIL_DSD_EVENT_STAGING_H_

#include <dsd-neo/core/state.h>

/* Caller already holds the history transaction; this helper never acquires the lock. */
void dsd_event_staging_clear_locked(Event_History_I* history);

#endif
