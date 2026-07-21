// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Protocol-neutral per-slot call state and recent activity.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_CALL_STATE_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_CALL_STATE_H_H

#include <dsd-neo/core/state_fwd.h>

#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    DSD_CALL_STATE_SLOT_COUNT = 2,
    DSD_RECENT_ACTIVITY_COUNT = 31,
    DSD_RECENT_ACTIVITY_TEXT_SIZE = 200,
    DSD_RECENT_ACTIVITY_TTL_MS = 3000,
};

typedef enum {
    DSD_CALL_PHASE_IDLE = 0,
    DSD_CALL_PHASE_ACTIVE,
    DSD_CALL_PHASE_ENDED,
} dsd_call_phase;

typedef enum {
    DSD_CALL_KIND_UNKNOWN = 0,
    DSD_CALL_KIND_GROUP_VOICE,
    DSD_CALL_KIND_PRIVATE_VOICE,
    DSD_CALL_KIND_DATA,
} dsd_call_kind;

typedef enum {
    DSD_CALL_CRYPTO_UNKNOWN = 0,
    DSD_CALL_CRYPTO_CLEAR,
    DSD_CALL_CRYPTO_ENCRYPTED_PENDING,
    DSD_CALL_CRYPTO_ENCRYPTED,
    DSD_CALL_CRYPTO_DECRYPTABLE,
} dsd_call_crypto_state;

typedef enum {
    DSD_CALL_BOUNDARY_CONTINUE = 0,
    DSD_CALL_BOUNDARY_BEGIN,
} dsd_call_boundary;

typedef struct {
    int protocol; /**< DSD_SYNC_* value; use DSD_SYNC_NONE when unobserved. */
    uint8_t slot;
    dsd_call_kind kind;
    uint32_t ota_target_id;
    uint32_t policy_target_id;
    uint32_t source_id;
    uint32_t group_id;
    uint32_t private_id;
    uint32_t channel;
    int64_t frequency_hz;
    uint16_t service_options;
    uint8_t emergency;
    uint8_t priority;
    double observed_m;
} dsd_call_observation;

typedef struct {
    dsd_call_crypto_state classification;
    uint8_t algid;
    uint16_t kid;
    uint64_t mi;
    uint8_t audio_permitted;
    double observed_m;
} dsd_call_crypto_update;

typedef struct {
    uint64_t revision;
    uint64_t epoch;
    dsd_call_phase phase;
    int protocol; /**< DSD_SYNC_* value, or DSD_SYNC_NONE when unobserved. */
    uint8_t slot;
    dsd_call_kind kind;
    uint32_t ota_target_id;
    uint32_t policy_target_id;
    uint32_t source_id;
    uint32_t group_id;
    uint32_t private_id;
    uint32_t channel;
    int64_t frequency_hz;
    uint16_t service_options;
    uint8_t emergency;
    uint8_t priority;
    dsd_call_crypto_state crypto;
    uint8_t algid;
    uint16_t kid;
    uint64_t mi;
    uint8_t audio_permitted;
    uint8_t media_active;
    double started_m;
    double updated_m;
    double ended_m;
} dsd_call_snapshot;

typedef struct {
    uint64_t revision;
    dsd_call_snapshot slots[DSD_CALL_STATE_SLOT_COUNT];
} dsd_call_state_snapshot;

typedef struct {
    char text[DSD_RECENT_ACTIVITY_TEXT_SIZE];
    uint64_t updated_m_ms;
} dsd_recent_activity_entry;

typedef struct {
    uint64_t revision;
    dsd_recent_activity_entry entries[DSD_RECENT_ACTIVITY_COUNT];
} dsd_recent_activity_snapshot;

typedef struct {
    uint8_t valid;
    uint8_t index;
    time_t last_active_time;
    dsd_recent_activity_entry entry;
    char legacy_text[DSD_RECENT_ACTIVITY_TEXT_SIZE];
} dsd_recent_activity_transaction;

int dsd_call_state_observe(dsd_state* state, const dsd_call_observation* observation, dsd_call_boundary boundary);
int dsd_call_state_update_crypto(dsd_state* state, uint8_t slot, const dsd_call_crypto_update* update);
int dsd_call_state_update_media(dsd_state* state, uint8_t slot, int media_active, double observed_m);
int dsd_call_state_end(dsd_state* state, uint8_t slot, double observed_m);
int dsd_call_state_get(const dsd_state* state, uint8_t slot, dsd_call_snapshot* out);
int dsd_call_state_copy_snapshot(const dsd_state* state, dsd_call_state_snapshot* out);

int dsd_recent_activity_set(dsd_state* state, uint8_t index, const char* text);
int dsd_recent_activity_set_at(dsd_state* state, uint8_t index, const char* text, uint64_t observed_m_ms);
int dsd_recent_activity_append(dsd_state* state, uint8_t index, const char* text);
int dsd_recent_activity_append_at(dsd_state* state, uint8_t index, const char* text, uint64_t observed_m_ms);
int dsd_recent_activity_clear(dsd_state* state, uint8_t index);
int dsd_recent_activity_clear_all(dsd_state* state);
int dsd_recent_activity_copy_snapshot(const dsd_state* state, dsd_recent_activity_snapshot* out);
int dsd_recent_activity_sync_legacy_entry(dsd_state* state, uint8_t index);
int dsd_recent_activity_sync_legacy(dsd_state* state);
int dsd_recent_activity_expire(dsd_state* state, uint64_t now_m_ms, uint64_t ttl_ms);
int dsd_recent_activity_save(const dsd_state* state, uint8_t index, dsd_recent_activity_transaction* transaction);
int dsd_recent_activity_restore(dsd_state* state, const dsd_recent_activity_transaction* transaction);

/** Copy the canonical extension into another `dsd_state` snapshot. */
int dsd_call_state_copy_to_state(dsd_state* dst, const dsd_state* src);

#ifdef DSD_NEO_TEST_HOOKS
void dsd_call_state_test_alloc_reset(void);
void dsd_call_state_test_alloc_fail_after(long fail_after);
int dsd_call_state_test_set_epoch(dsd_state* state, uint8_t slot, uint64_t epoch);
#endif

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_CALL_STATE_H_H */
