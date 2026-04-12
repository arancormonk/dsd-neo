// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Talkgroup/private policy evaluation and shared mutation helpers.
 */

#pragma once

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DSD_TG_POLICY_MATCH_NONE = 0,
    DSD_TG_POLICY_MATCH_RANGE = 1,
    DSD_TG_POLICY_MATCH_EXACT = 2,
} dsd_tg_policy_match_type;

typedef enum {
    DSD_TG_POLICY_PRIVATE_ALLOWLIST_UNKNOWN_ALLOW = 0,
    DSD_TG_POLICY_PRIVATE_ALLOWLIST_UNKNOWN_BLOCK = 1,
} dsd_tg_policy_private_allowlist_mode;

typedef enum {
    DSD_TG_POLICY_HOLD_COMPAT_GRANT = 0,
    DSD_TG_POLICY_HOLD_FORCE_MEDIA_ONLY = 1,
    DSD_TG_POLICY_HOLD_FORCE_TUNE_AND_MEDIA = 2,
} dsd_tg_policy_hold_behavior;

typedef enum {
    DSD_TG_POLICY_SOURCE_IMPORTED = 0,
    DSD_TG_POLICY_SOURCE_RUNTIME_ALIAS = 1,
    DSD_TG_POLICY_SOURCE_USER_LOCKOUT = 2,
    DSD_TG_POLICY_SOURCE_ENC_LOCKOUT = 3,
    DSD_TG_POLICY_SOURCE_LEGACY_UNKNOWN = 4,
} dsd_tg_policy_entry_source;

typedef enum {
    DSD_TG_POLICY_UPSERT_ADD_IF_MISSING = 0,
    DSD_TG_POLICY_UPSERT_REPLACE_FIRST = 1,
    DSD_TG_POLICY_UPSERT_REPLACE_LEARNED_ONLY = 2,
} dsd_tg_policy_upsert_mode;

typedef enum {
    DSD_TG_POLICY_BLOCK_NONE = 0u,
    DSD_TG_POLICY_BLOCK_GROUP_DISABLED = 1u << 0,
    DSD_TG_POLICY_BLOCK_PRIVATE_DISABLED = 1u << 1,
    DSD_TG_POLICY_BLOCK_DATA_DISABLED = 1u << 2,
    DSD_TG_POLICY_BLOCK_ENCRYPTED_DISABLED = 1u << 3,
    DSD_TG_POLICY_BLOCK_ALLOWLIST = 1u << 4,
    DSD_TG_POLICY_BLOCK_MODE = 1u << 5,
    DSD_TG_POLICY_BLOCK_HOLD = 1u << 6,
    DSD_TG_POLICY_BLOCK_AUDIO = 1u << 7,
    DSD_TG_POLICY_BLOCK_RECORD = 1u << 8,
    DSD_TG_POLICY_BLOCK_STREAM = 1u << 9,
} dsd_tg_policy_block_reason;

typedef struct {
    uint32_t id_start;
    uint32_t id_end;
    char mode[8];
    char name[50];
    int priority;
    uint8_t preempt;
    uint8_t audio;
    uint8_t record;
    uint8_t stream;
    uint8_t is_range;
    uint8_t source;
    unsigned int row;
} dsd_tg_policy_entry;

typedef struct {
    dsd_tg_policy_match_type match;
    dsd_tg_policy_entry entry;
} dsd_tg_policy_lookup;

typedef struct {
    uint32_t target_id;
    uint32_t source_id;
    int encrypted;
    int data_call;
    int tune_allowed;
    int audio_allowed;
    int record_allowed;
    int stream_allowed;
    int priority;
    int preempt_requested;
    uint32_t block_reasons;
    int tg_hold_active;
    int tg_hold_match;
    char mode[8];
    char name[50];
    dsd_tg_policy_match_type match;
} dsd_tg_policy_decision;

typedef struct {
    uint32_t target_id;
    uint32_t source_id;
    long freq_hz;
    int channel;
    int slot;
    int requires_tuner_retune;
} dsd_tg_policy_call_route;

int dsd_tg_policy_make_legacy_exact_entry(uint32_t id, const char* mode, const char* name,
                                          dsd_tg_policy_entry_source source, dsd_tg_policy_entry* out);
int dsd_tg_policy_add_range_entry(dsd_state* state, const dsd_tg_policy_entry* entry);
int dsd_tg_policy_lookup_id(const dsd_state* state, uint32_t id, dsd_tg_policy_lookup* out);
int dsd_tg_policy_evaluate_group_call(const dsd_opts* opts, const dsd_state* state, uint32_t tg, uint32_t src,
                                      int encrypted, int data_call, dsd_tg_policy_hold_behavior hold_behavior,
                                      dsd_tg_policy_decision* out);
int dsd_tg_policy_evaluate_private_call(const dsd_opts* opts, const dsd_state* state, uint32_t src, uint32_t dst,
                                        int encrypted, int data_call,
                                        dsd_tg_policy_private_allowlist_mode allowlist_mode,
                                        dsd_tg_policy_hold_behavior hold_behavior, dsd_tg_policy_decision* out);
int dsd_tg_policy_append_legacy_exact(dsd_state* state, const dsd_tg_policy_entry* entry);
int dsd_tg_policy_upsert_legacy_exact(dsd_state* state, const dsd_tg_policy_entry* entry,
                                      dsd_tg_policy_upsert_mode mode);
int dsd_tg_policy_append_group_file_row(const dsd_opts* opts, const dsd_tg_policy_entry* entry,
                                        const char* legacy_metadata);

int dsd_tg_policy_should_preempt(const dsd_opts* opts, const dsd_state* state,
                                 const dsd_tg_policy_call_route* candidate_route,
                                 const dsd_tg_policy_decision* candidate, double now_mono_s);
int dsd_tg_policy_note_active_call(dsd_state* state, const dsd_tg_policy_call_route* route,
                                   const dsd_tg_policy_decision* decision, double now_mono_s);
int dsd_tg_policy_clear_active_call(dsd_state* state, int slot);
int dsd_tg_policy_clear_active_call_route(dsd_state* state, const dsd_tg_policy_call_route* route);

int dsd_tg_policy_reload_group_file(dsd_opts* opts, dsd_state* state);

#ifdef __cplusplus
}
#endif
