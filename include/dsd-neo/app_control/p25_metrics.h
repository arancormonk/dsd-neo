// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_P25_METRICS_H_
#define DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_P25_METRICS_H_

#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** FEC successes as a percentage; 0/0 is unknown, not a perfect decode. */
typedef struct {
    uint64_t ok;
    uint64_t err;
    double ok_pct;
    int valid;
} dsd_app_fec_ratio;

/** Average mbe total_errors per voice frame. This is a count, never BER. */
typedef struct {
    double errs_per_frame;
    unsigned samples; /**< Populated frames, not the ring's capacity. */
    int valid;
} dsd_app_voice_errs;

typedef struct {
    int valid;
    int errs;
    int errs2;
} dsd_app_frame_errs;

typedef struct {
    int valid;
    dsd_app_fec_ratio cc_fec;
    dsd_app_fec_ratio voice_fec;
    dsd_app_fec_ratio rs;
    dsd_app_voice_errs p1_voice;
    dsd_app_voice_errs p2_voice[2];
    dsd_app_frame_errs last_frame[2]; /**< Non-P25 fallback, indexed by slot. */
} dsd_app_p25_quality;

dsd_app_fec_ratio dsd_app_fec_ratio_make(uint64_t ok, uint64_t err);
/** Raw ring arithmetic; NULL, empty or inconsistent rings yield an invalid zero value. */
dsd_app_voice_errs dsd_app_p25p1_voice_avg_errs(const dsd_state* state);
dsd_app_voice_errs dsd_app_p25p2_voice_avg_errs(const dsd_state* state, int slot);
/** Copy from the caller's held snapshot. Voice rings require an active matching
 * call with populated frames; last-frame errors require active non-P25 media.
 * FEC validity follows the engine's no-carrier counter reset, independent of
 * transient sync gaps. NULL state clears the output; NULL output is a no-op. */
void dsd_app_p25_quality_from_state(const dsd_state* state, dsd_app_p25_quality* out);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_P25_METRICS_H_ */
