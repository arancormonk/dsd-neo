// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_P25_NETWORK_H_
#define DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_P25_NETWORK_H_
#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>
#include <time.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int64_t freq_hz;
    uint32_t wacn;
    uint16_t sysid;
    uint8_t rfss, site, lra;
    uint8_t wacn_valid, lra_valid;
    uint8_t is_current_cc, is_candidate;
    char cfva_text[48];
    time_t last_seen;
} dsd_app_p25_neighbor;

typedef struct {
    uint16_t sgid;
    uint8_t is_patch;
    uint8_t group_count, radio_count;
    uint16_t groups[8];
    uint32_t radios[8];
    time_t last_seen;
} dsd_app_p25_patch;

typedef struct {
    uint32_t rid;
    uint16_t tg; /**< Zero for the affiliated-RID view. */
    time_t last_seen;
} dsd_app_p25_affiliation;

/** Copy from a held snapshot; never mutate or retain it. Return rows written,
 * at most capacity. NULL inputs or nonpositive capacity return zero. Lists are
 * recent-first (stable on ties), filtered before truncation. Patches are active
 * announcements within the protocol's 20-second lifetime (zero timestamp is unknown); no encryption fields are exposed. */
int dsd_app_p25_neighbors(const dsd_state* state, dsd_app_p25_neighbor* out, int capacity);
int dsd_app_p25_patches(const dsd_state* state, dsd_app_p25_patch* out, int capacity);
int dsd_app_p25_group_affiliations(const dsd_state* state, dsd_app_p25_affiliation* out, int capacity);
int dsd_app_p25_affiliated_rids(const dsd_state* state, dsd_app_p25_affiliation* out, int capacity);
#ifdef __cplusplus
}
#endif
#endif
