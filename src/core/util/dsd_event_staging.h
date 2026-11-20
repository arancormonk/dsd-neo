// SPDX-License-Identifier: ISC
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

#ifndef DSD_NEO_CORE_UTIL_DSD_EVENT_STAGING_H_
#define DSD_NEO_CORE_UTIL_DSD_EVENT_STAGING_H_

#include <dsd-neo/core/position.h>
#include <dsd-neo/core/state.h>

/* Caller already holds the history transaction; this helper never acquires the lock. */
void dsd_event_staging_clear_locked(Event_History_I* history);

/* 1 when @p fix is a record dsd_position_admit() admitted (admission stamped its rev and process instance). */
int dsd_event_pos_fix_admitted(const dsd_position_fix* fix);
/* A row position from an admitted record: valid, its fix, its sender scope and identity discriminator. */
void dsd_event_pos_set_from_fix(dsd_event_pos* pos, const dsd_position_fix* fix);
/* Forget the position (valid 0, every fix field 0) and keep the row's sender identity (scope, discriminator). */
void dsd_event_pos_drop_position(dsd_event_pos* pos);

#endif
