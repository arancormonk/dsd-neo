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
    DSD_FRONTEND_P25_NEIGHBOR_MAX = 32,
    DSD_FRONTEND_P25_IDEN_PLAN_MAX = 32,
    DSD_FRONTEND_ACTIVE_CHANNEL_MAX = 2,
};

typedef enum {
    DSD_FRONTEND_EVENT_SEVERITY_UNKNOWN = 0,
    DSD_FRONTEND_EVENT_SEVERITY_DEBUG = 1,
    DSD_FRONTEND_EVENT_SEVERITY_INFO = 2,
    DSD_FRONTEND_EVENT_SEVERITY_WARNING = 3,
    DSD_FRONTEND_EVENT_SEVERITY_ERROR = 4
} dsd_frontend_event_severity;

typedef enum {
    DSD_FRONTEND_EVENT_CATEGORY_UNKNOWN = 0,
    DSD_FRONTEND_EVENT_CATEGORY_STATUS = 1,
    DSD_FRONTEND_EVENT_CATEGORY_VOICE = 2,
    DSD_FRONTEND_EVENT_CATEGORY_DATA = 3,
    DSD_FRONTEND_EVENT_CATEGORY_CONTROL = 4,
    DSD_FRONTEND_EVENT_CATEGORY_SYSTEM = 5
} dsd_frontend_event_category;

typedef enum {
    DSD_FRONTEND_PROTOCOL_UNKNOWN = 0,
    DSD_FRONTEND_PROTOCOL_P25 = 1,
    DSD_FRONTEND_PROTOCOL_DMR = 2,
    DSD_FRONTEND_PROTOCOL_NXDN = 3,
    DSD_FRONTEND_PROTOCOL_YSF = 4,
    DSD_FRONTEND_PROTOCOL_M17 = 5,
    DSD_FRONTEND_PROTOCOL_DSTAR = 6,
    DSD_FRONTEND_PROTOCOL_DPMR = 7,
    DSD_FRONTEND_PROTOCOL_EDACS = 8,
    DSD_FRONTEND_PROTOCOL_PROVOICE = 9,
    DSD_FRONTEND_PROTOCOL_X2TDMA = 10,
    DSD_FRONTEND_PROTOCOL_ANALOG = 11,
    DSD_FRONTEND_PROTOCOL_DIGITAL = 12
} dsd_frontend_protocol;

typedef enum {
    DSD_FRONTEND_ENCRYPTION_UNKNOWN = 0,
    DSD_FRONTEND_ENCRYPTION_CLEAR = 1,
    DSD_FRONTEND_ENCRYPTION_ENCRYPTED = 2
} dsd_frontend_encryption_state;

typedef struct dsd_frontend_event_history_item {
    uint8_t present;
    uint8_t write_pending;
    uint8_t slot;
    dsd_frontend_event_severity severity;
    dsd_frontend_event_category category;
    dsd_frontend_protocol protocol;
    int16_t subtype;
    dsd_frontend_encryption_state encryption_state;
    uint8_t encryption_alg_id;
    uint16_t encryption_key_id;
    uint64_t encryption_message_indicator;
    uint16_t service_options;
    int8_t group_individual;
    uint32_t system_id[5];
    uint32_t source_id;
    uint32_t target_id;
    uint32_t channel;
    int64_t timestamp_unix_s;
    char source_text[DSD_FRONTEND_SHORT_TEXT];
    char target_text[DSD_FRONTEND_SHORT_TEXT];
    char source_label[DSD_FRONTEND_SHORT_TEXT];
    char target_label[DSD_FRONTEND_SHORT_TEXT];
    char source_mode[DSD_FRONTEND_SHORT_TEXT];
    char target_mode[DSD_FRONTEND_SHORT_TEXT];
    char system_label[DSD_FRONTEND_SHORT_TEXT];
    uint8_t pdu[DSD_FRONTEND_EVENT_PDU_BYTES];
    size_t pdu_len;
    char summary_text[DSD_FRONTEND_LONG_TEXT];
    char detail_text[DSD_FRONTEND_LONG_TEXT];
    char gps_text[DSD_FRONTEND_LONG_TEXT];
    char text_message[DSD_FRONTEND_LONG_TEXT];
    char alias[DSD_FRONTEND_LONG_TEXT];
} dsd_frontend_event_history_item;

typedef struct dsd_frontend_event_history_slot {
    dsd_frontend_event_history_item items[DSD_FRONTEND_EVENT_HISTORY_ITEMS];
} dsd_frontend_event_history_slot;

typedef struct dsd_frontend_event_history_summary {
    uint8_t present;
    uint8_t write_pending;
    uint8_t slot;
    dsd_frontend_event_severity severity;
    dsd_frontend_event_category category;
    dsd_frontend_protocol protocol;
    dsd_frontend_encryption_state encryption_state;
    uint32_t source_id;
    uint32_t target_id;
    uint32_t channel;
    int64_t timestamp_unix_s;
    char source_text[DSD_FRONTEND_SHORT_TEXT];
    char target_text[DSD_FRONTEND_SHORT_TEXT];
    char source_label[DSD_FRONTEND_SHORT_TEXT];
    char target_label[DSD_FRONTEND_SHORT_TEXT];
    char system_label[DSD_FRONTEND_SHORT_TEXT];
    char summary_text[DSD_FRONTEND_SHORT_TEXT];
    char detail_text[DSD_FRONTEND_SHORT_TEXT];
} dsd_frontend_event_history_summary;

typedef struct dsd_frontend_event_history_query {
    uint8_t slot;
    size_t offset;
    size_t limit;
    uint64_t known_sequence;
} dsd_frontend_event_history_query;

typedef struct dsd_frontend_event_history_page_info {
    uint64_t sequence;
    size_t total_items;
    size_t returned_items;
    int present;
    int unchanged;
} dsd_frontend_event_history_page_info;

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
    uint8_t present;
    dsd_frontend_event_severity severity;
    dsd_frontend_event_category category;
    char source[64];
    int8_t slot;
    int64_t created_unix_s;
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

typedef struct dsd_frontend_active_channel_summary {
    uint8_t present;
    uint8_t slot;
    dsd_frontend_protocol protocol;
    uint32_t target_id;
    uint32_t source_id;
    uint32_t payload_algid;
    uint32_t payload_keyid;
    long p25_vc_freq;
    long trunk_vc_freq;
    int audio_allowed;
} dsd_frontend_active_channel_summary;

typedef struct dsd_frontend_p25_neighbor_summary {
    uint8_t present;
    uint16_t sysid;
    uint8_t rfss;
    uint8_t site;
    uint8_t cfva;
    long freq_hz;
    int64_t last_seen_unix_s;
} dsd_frontend_p25_neighbor_summary;

typedef struct dsd_frontend_p25_iden_entry_summary {
    uint8_t present;
    uint8_t id;
    uint8_t tdma;
    uint8_t trust;
    uint8_t channel_type;
    uint8_t bandwidth;
    long base_freq_hz;
    int spacing_hz;
    int transmit_offset;
    uint64_t wacn;
    uint64_t sysid;
    uint64_t rfss;
    uint64_t site;
} dsd_frontend_p25_iden_entry_summary;

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
    dsd_frontend_p25_neighbor_summary neighbors[DSD_FRONTEND_P25_NEIGHBOR_MAX];
    size_t neighbor_count;
    dsd_frontend_p25_iden_entry_summary iden_plan[DSD_FRONTEND_P25_IDEN_PLAN_MAX];
    size_t iden_plan_count;
    size_t iden_plan_confirmed_count;
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
    dsd_frontend_active_channel_summary active_channels[DSD_FRONTEND_ACTIVE_CHANNEL_MAX];
    size_t active_channel_count;
    dsd_frontend_p25_display p25;
    dsd_frontend_trunk_channel trunk_channels[DSD_FRONTEND_TRUNK_CHANNEL_MAX];
    size_t trunk_channel_count;
    uint64_t trunk_channel_sequence;
    dsd_frontend_trunk_cc_candidates trunk_cc_candidates;
    uint64_t event_history_sequence;
    size_t event_history_slot_count;
    size_t event_history_items_per_slot;
    int event_history_present;
} dsd_frontend_snapshot;

int dsd_app_frontend_snapshot_get(dsd_frontend_snapshot* out);
int dsd_app_frontend_event_history_page_get(const dsd_frontend_event_history_query* query,
                                            dsd_frontend_event_history_summary* out_items, size_t max_items,
                                            dsd_frontend_event_history_page_info* out_info);
int dsd_app_frontend_event_history_item_get(uint8_t slot, size_t index, dsd_frontend_event_history_item* out,
                                            uint64_t* out_sequence);
void dsd_app_frontend_redraw_request(void);
int dsd_app_frontend_redraw_consume(void);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_SNAPSHOT_H_ */
