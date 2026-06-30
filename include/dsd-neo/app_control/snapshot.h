// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Decoder → frontend copied snapshot API for stable read-only state.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_SNAPSHOT_H_
#define DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_SNAPSHOT_H_

#include <dsd-neo/app_control/frontend.h>
#include <dsd-neo/core/input_level.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    DSD_FRONTEND_EVENT_HISTORY_SLOTS = 2,
    DSD_FRONTEND_EVENT_HISTORY_ITEMS = 255,
    DSD_FRONTEND_EVENT_PDU_BYTES = 128 * 24,
    DSD_FRONTEND_SHORT_TEXT = 200,
    DSD_FRONTEND_LONG_TEXT = 2000,
    DSD_FRONTEND_TRUNK_CHANNEL_MAX = 1024,
    DSD_FRONTEND_TRUNK_CC_CANDIDATES_MAX = 16,
};

typedef struct dsd_frontend_event_history_item {
    uint8_t write;
    uint8_t color_pair;
    int8_t systype;
    int8_t subtype;
    uint32_t sys_id1;
    uint32_t sys_id2;
    uint32_t sys_id3;
    uint32_t sys_id4;
    uint32_t sys_id5;
    int8_t gi;
    uint8_t enc;
    uint8_t enc_alg;
    uint16_t enc_key;
    uint64_t mi;
    uint16_t svc;
    uint32_t source_id;
    uint32_t target_id;
    char src_str[DSD_FRONTEND_SHORT_TEXT];
    char tgt_str[DSD_FRONTEND_SHORT_TEXT];
    char t_name[DSD_FRONTEND_SHORT_TEXT];
    char s_name[DSD_FRONTEND_SHORT_TEXT];
    char t_mode[DSD_FRONTEND_SHORT_TEXT];
    char s_mode[DSD_FRONTEND_SHORT_TEXT];
    uint32_t channel;
    int64_t event_time;
    uint8_t pdu[DSD_FRONTEND_EVENT_PDU_BYTES];
    char sysid_string[DSD_FRONTEND_SHORT_TEXT];
    char alias[DSD_FRONTEND_LONG_TEXT];
    char gps_s[DSD_FRONTEND_LONG_TEXT];
    char text_message[DSD_FRONTEND_LONG_TEXT];
    char event_string[DSD_FRONTEND_LONG_TEXT];
    char internal_str[DSD_FRONTEND_LONG_TEXT];
} dsd_frontend_event_history_item;

typedef struct dsd_frontend_event_history_slot {
    dsd_frontend_event_history_item items[DSD_FRONTEND_EVENT_HISTORY_ITEMS];
} dsd_frontend_event_history_slot;

typedef struct dsd_frontend_trunk_channel {
    uint16_t channel;
    long freq_hz;
} dsd_frontend_trunk_channel;

typedef struct dsd_frontend_trunk_cc_candidate {
    long freq_hz;
    uint8_t flags;
    double cool_until_monotonic_s;
} dsd_frontend_trunk_cc_candidate;

typedef struct dsd_frontend_trunk_cc_candidates {
    dsd_frontend_trunk_cc_candidate candidates[DSD_FRONTEND_TRUNK_CC_CANDIDATES_MAX];
    int count;
    int index;
    unsigned int added;
    unsigned int used;
} dsd_frontend_trunk_cc_candidates;

typedef struct dsd_frontend_ui_message {
    char text[1024];
    int64_t expire_unix_s;
} dsd_frontend_ui_message;

typedef struct dsd_frontend_active_slot_summary {
    uint32_t last_tg;
    uint32_t last_src;
    uint32_t payload_algid;
    uint32_t payload_keyid;
    int audio_allowed;
    int active_call;
    char call_string[64];
} dsd_frontend_active_slot_summary;

typedef struct dsd_frontend_p25_display {
    uint64_t p2_wacn;
    uint64_t p2_sysid;
    uint64_t p2_cc;
    long trunk_cc_freq;
    long trunk_vc_freq;
    long p25_cc_freq;
    long p25_vc_freq;
    int p25_cc_is_tdma;
    int p25_p2_active_slot;
    int p25_p2_audio_ring_count[2];
    int p25_p2_audio_allowed[2];
    unsigned int p25_p1_fec_ok;
    unsigned int p25_p1_fec_err;
    unsigned int p25_p2_facch_ok;
    unsigned int p25_p2_facch_err;
    unsigned int p25_p2_sacch_ok;
    unsigned int p25_p2_sacch_err;
    unsigned int p25_p2_voice_err;
} dsd_frontend_p25_display;

typedef struct dsd_frontend_snapshot {
    int has_options;
    int has_state;
    dsd_frontend_status status;
    dsd_frontend_metrics metrics;
    dsd_frontend_ui_message ui_message;
    dsd_input_level_snapshot input_level;
    int64_t input_level_last_toast_time;
    int input_level_last_toast_status;
    int input_level_last_toast_source;
    dsd_frontend_active_slot_summary slots[2];
    dsd_frontend_p25_display p25;
    dsd_frontend_trunk_channel trunk_channels[DSD_FRONTEND_TRUNK_CHANNEL_MAX];
    size_t trunk_channel_count;
    uint64_t trunk_channel_sequence;
    dsd_frontend_trunk_cc_candidates trunk_cc_candidates;
    dsd_frontend_event_history_slot event_history[DSD_FRONTEND_EVENT_HISTORY_SLOTS];
    int event_history_present;
} dsd_frontend_snapshot;

int dsd_app_frontend_snapshot_get(dsd_frontend_snapshot* out);
void dsd_app_frontend_redraw_request(void);
int dsd_app_frontend_redraw_consume(void);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_SNAPSHOT_H_ */
