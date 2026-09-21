// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Live-scanner slicer threshold refresh.
 *
 * After every getFrameSync() return the engine derives the symbol slicer
 * thresholds (center, umid, lmid) from the level extremes the sync search left
 * in state->max / state->min. The derivation is stepped: it runs only when an
 * extreme, truncated to a whole level unit, differs from the value seen at the
 * previous refresh. See dsd_engine_slicer_thresholds_refresh() for what that
 * does and does not guarantee.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_ENGINE_SLICER_THRESHOLDS_H
#define DSD_NEO_INCLUDE_DSD_NEO_ENGINE_SLICER_THRESHOLDS_H

#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Extremes seen at the previous refresh, truncated to whole level units.
 * Zero-initialise or call dsd_engine_slicer_threshold_cache_init() before
 * first use. */
typedef struct {
    int last_max;
    int last_min;
    int primed;
} dsd_engine_slicer_threshold_cache;

void dsd_engine_slicer_threshold_cache_init(dsd_engine_slicer_threshold_cache* cache);

/** Recompute state->center, state->umid and state->lmid from state->max and
 * state->min on the first call and whenever either extreme, truncated toward
 * zero to a whole level unit, differs from the value seen at the previous
 * refresh. Extremes that are NaN, infinite or outside the int range are
 * skipped and the previous thresholds kept.
 *
 * The truncation is the scanner loop's long-standing stepped refresh, kept
 * here unchanged: for every pair of extremes that is finite and fits an int
 * the refresh decision is the same as before, and the derived values are
 * bit-identical except that this unit is built without fast-math, so on
 * targets where fast-math contracted the 5/8 split into a fused multiply-add
 * (arm64 release builds) umid/lmid can carry a contraction rounding
 * difference: tiny in absolute terms, but after cancellation near zero it is
 * not bounded by one ulp of the result. The refresh decision reads only the
 * extremes, so it is unaffected. Extremes that are NaN, infinite or out of
 * int range used to be cast anyway (undefined behaviour); they are now
 * skipped. They are not produced by ordinary captures, but a .flt symbol
 * replay or a runaway level estimate can produce them.
 *
 * It is not a one-unit
 * hysteresis: a move refreshes when it leaves its integer bucket, however
 * small (10000.9 -> 10001.0 refreshes), and a move inside a bucket never does,
 * however large (-0.9 -> 0.9 spans two units and does not, because truncation
 * is toward zero). A rounding difference of one ulp between builds can thus
 * decide whether a refresh happens. Replacing this with a refresh on any change,
 * or with a real one-unit band, changes the live slicer path for every protocol
 * whose dibit reader does not maintain the thresholds itself; the fixture
 * replay matrix has never shown a difference from it, but it has not been
 * validated either way with the paired real-capture A/B docs/testing.md asks
 * for, so it stays as it is until that is done. Note that the extremes are
 * not always on the +-10000 scale: the RTL symbol-rate fast path installs
 * +-1 or +-3 and the NXDN CAC-fail reset +-4, where one truncation unit is a
 * large fraction of full scale. The RTL fast path re-installs its own
 * umid/lmid on every symbol read, so this refresh does not reach the slicer
 * there; the NXDN reset is a one-off and this refresh does apply after it.
 *
 * Returns 1 when the thresholds were recomputed, 0 when they were left alone,
 * and -1 on a NULL argument. */
int dsd_engine_slicer_thresholds_refresh(dsd_state* state, dsd_engine_slicer_threshold_cache* cache);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_ENGINE_SLICER_THRESHOLDS_H */
