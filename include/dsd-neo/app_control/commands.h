// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Frontend → decoder command queue API and command IDs.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_COMMANDS_H_
#define DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_COMMANDS_H_

#include <dsd-neo/app_control/rr_import_apply.h>
#include <dsd-neo/runtime/config.h>
#include <stddef.h>
#include <stdint.h>

/** Command identifiers used by the async UI command queue. */
enum dsd_app_command_id {
    DSD_APP_CMD_TOGGLE_MUTE = 1,
    DSD_APP_CMD_TOGGLE_COMPACT = 2,
    DSD_APP_CMD_HISTORY_CYCLE = 3,

    DSD_APP_CMD_SLOT1_TOGGLE = 10,
    DSD_APP_CMD_SLOT2_TOGGLE = 11,
    DSD_APP_CMD_SLOT_PREF_CYCLE = 12,

    DSD_APP_CMD_GAIN_DELTA = 20,  // payload: int32_t delta (+1/-1)
    DSD_APP_CMD_AGAIN_DELTA = 21, // payload: int32_t delta (+1/-1) for analog gain

    DSD_APP_CMD_TRUNK_TOGGLE = 30,
    DSD_APP_CMD_SCANNER_TOGGLE = 31,

    DSD_APP_CMD_PAYLOAD_TOGGLE = 40,

    // UI/state toggles and actions
    DSD_APP_CMD_P25_GA_TOGGLE = 50,  // Toggle P25 Group Affiliation section
    DSD_APP_CMD_TG_HOLD_TOGGLE = 51, // payload: uint8_t slot (0 or 1)
    DSD_APP_CMD_LPF_TOGGLE = 52,
    DSD_APP_CMD_HPF_TOGGLE = 53,
    DSD_APP_CMD_PBF_TOGGLE = 54,
    DSD_APP_CMD_HPF_D_TOGGLE = 55,
    DSD_APP_CMD_AGGR_SYNC_TOGGLE = 56,
    DSD_APP_CMD_CALL_ALERT_TOGGLE = 57,
    DSD_APP_CMD_CALL_ALERT_EVENTS_SET = 58, // payload: uint8_t event mask (0 disables master switch)

    // Views and visualization controls
    DSD_APP_CMD_CONST_TOGGLE = 70,
    DSD_APP_CMD_CONST_NORM_TOGGLE = 71,
    DSD_APP_CMD_CONST_GATE_DELTA = 72, // payload: float delta
    DSD_APP_CMD_EYE_TOGGLE = 73,
    DSD_APP_CMD_EYE_UNICODE_TOGGLE = 74,
    DSD_APP_CMD_EYE_COLOR_TOGGLE = 75,
    DSD_APP_CMD_FSK_HIST_TOGGLE = 76,
    DSD_APP_CMD_SPECTRUM_TOGGLE = 77,
    DSD_APP_CMD_SPEC_SIZE_DELTA = 78, // payload: int32_t (+/-)
    DSD_APP_CMD_INPUT_VOL_CYCLE = 79,

    // Event history keys
    DSD_APP_CMD_EH_NEXT = 90,
    DSD_APP_CMD_EH_PREV = 91,
    DSD_APP_CMD_EH_TOGGLE_SLOT = 92,

    // Device related
    DSD_APP_CMD_PPM_DELTA = 100, // payload: int32_t (+/-1)
    DSD_APP_CMD_INVERT_TOGGLE = 101,
    DSD_APP_CMD_MOD_TOGGLE = 102,
    DSD_APP_CMD_DMR_RESET = 103,
    DSD_APP_CMD_GAIN_SET = 104,          // payload: int32_t (0..50)
    DSD_APP_CMD_AGAIN_SET = 105,         // payload: int32_t (0..50)
    DSD_APP_CMD_INPUT_WARN_DB_SET = 106, // payload: double
    DSD_APP_CMD_INPUT_MONITOR_TOGGLE = 107,
    DSD_APP_CMD_COSINE_FILTER_TOGGLE = 108,

    DSD_APP_CMD_TCP_CONNECT_AUDIO = 200, // use opts->tcp_hostname/port; sets audio_in_type=AUDIO_IN_TCP
    DSD_APP_CMD_RIGCTL_CONNECT = 201,    // uses opts->tcp_hostname/rigctlportno
    DSD_APP_CMD_RETURN_CC = 202,
    DSD_APP_CMD_CHANNEL_CYCLE = 203,
    DSD_APP_CMD_SYMCAP_SAVE = 204, // auto-name capture file and start
    DSD_APP_CMD_SYMCAP_STOP = 205,
    DSD_APP_CMD_REPLAY_LAST = 206,
    DSD_APP_CMD_WAV_START = 207,
    DSD_APP_CMD_WAV_STOP = 208,
    DSD_APP_CMD_STOP_PLAYBACK = 209,

    // Trunk policy toggles
    DSD_APP_CMD_TRUNK_WLIST_TOGGLE = 210,
    DSD_APP_CMD_TRUNK_PRIV_TOGGLE = 211,
    DSD_APP_CMD_TRUNK_DATA_TOGGLE = 212,
    DSD_APP_CMD_TRUNK_ENC_TOGGLE = 213,
    DSD_APP_CMD_WAV_TOGGLE = 214,
    DSD_APP_CMD_ENC_LOCKOUT_CLEAR = 215, // forget all encrypted-target lockouts
    // On-the-fly scan controls (#380): -Y rows or --trunk-scan targets, whichever is running
    DSD_APP_CMD_SCAN_HOLD_TOGGLE = 216, // pause/resume rotation on the channel/target on air
    DSD_APP_CMD_SCAN_AVOID = 217,       // avoid the channel/target on air for the session and step on
    DSD_APP_CMD_SCAN_AVOID_CLEAR = 218, // put every avoided channel/target back into the rotation

    // Additional commands used by terminal hotkeys in async mode
    DSD_APP_CMD_QUIT = 300,
    DSD_APP_CMD_FORCE_PRIV_TOGGLE = 301,
    DSD_APP_CMD_FORCE_RC4_TOGGLE = 302,
    DSD_APP_CMD_TRUNK_GROUP_TOGGLE = 303,
    DSD_APP_CMD_SIM_NOCAR = 304,
    DSD_APP_CMD_MOD_P2_TOGGLE = 305,
    DSD_APP_CMD_LOCKOUT_SLOT = 306, // payload: uint8_t slot (0=slot1, 1=slot2)
    DSD_APP_CMD_M17_TX_TOGGLE = 307,

    // ProVoice debug toggles
    DSD_APP_CMD_PROVOICE_ESK_TOGGLE = 308,
    DSD_APP_CMD_PROVOICE_MODE_TOGGLE = 309,

    // UI utility
    DSD_APP_CMD_UI_MSG_CLEAR = 400, // clear transient toast message in canonical state
    // Logging and maintenance helpers
    DSD_APP_CMD_EH_RESET = 401,          // clear ring-buffered event history
    DSD_APP_CMD_EVENT_LOG_DISABLE = 402, // disable event log file output
    DSD_APP_CMD_EVENT_LOG_SET = 403,     // payload: char path[]

    DSD_APP_CMD_LCW_RETUNE_TOGGLE = 421,
    DSD_APP_CMD_P25_CC_CAND_TOGGLE = 423,
    DSD_APP_CMD_REVERSE_MUTE_TOGGLE = 424,
    DSD_APP_CMD_DMR_LE_TOGGLE = 425,
    DSD_APP_CMD_ALL_MUTES_TOGGLE = 426,
    DSD_APP_CMD_INV_X2_TOGGLE = 430,
    DSD_APP_CMD_INV_DMR_TOGGLE = 431,
    DSD_APP_CMD_INV_DPMR_TOGGLE = 432,
    DSD_APP_CMD_INV_M17_TOGGLE = 433,

    // File outputs / inputs
    DSD_APP_CMD_WAV_STATIC_OPEN = 440,      // payload: char path[]
    DSD_APP_CMD_WAV_RAW_OPEN = 441,         // payload: char path[]
    DSD_APP_CMD_DSP_OUT_SET = 442,          // payload: char filename[]
    DSD_APP_CMD_SYMCAP_OPEN = 443,          // payload: char path[]
    DSD_APP_CMD_SYMBOL_IN_OPEN = 444,       // payload: char path[]
    DSD_APP_CMD_INPUT_WAV_SET = 445,        // payload: char path[]; sets type=AUDIO_IN_WAV
    DSD_APP_CMD_INPUT_SYM_STREAM_SET = 446, // payload: char path[]; sets type=AUDIO_IN_SYMBOL_FLT
    DSD_APP_CMD_INPUT_SET_PULSE = 447,      // sets audio_in_dev="pulse", type=AUDIO_IN_PULSE

    // Networking / device configs
    DSD_APP_CMD_UDP_OUT_CFG = 460,           // payload: struct { char host[256]; int32_t port; }
    DSD_APP_CMD_TCP_CONNECT_AUDIO_CFG = 461, // payload: struct { char host[256]; int32_t port; }
    DSD_APP_CMD_RIGCTL_CONNECT_CFG = 462,    // payload: struct { char host[256]; int32_t port; }
    DSD_APP_CMD_UDP_INPUT_CFG = 463,         // payload: struct { char bind[256]; int32_t port; }

    // RTL-SDR controls
    DSD_APP_CMD_RTL_ENABLE_INPUT = 480,
    DSD_APP_CMD_RTL_RESTART = 481,
    DSD_APP_CMD_RTL_SET_DEV = 482, // payload: int32_t index
    // payload: uint32_t Hz; selects the CC during single-system P25 trunking.
    // Refused during trunk scan; conventional/scanner frequency semantics are unchanged.
    DSD_APP_CMD_RTL_SET_FREQ = 483,
    DSD_APP_CMD_RTL_SET_GAIN = 484,        // payload: int32_t gain
    DSD_APP_CMD_RTL_SET_PPM = 485,         // payload: int32_t ppm
    DSD_APP_CMD_RTL_SET_BW = 486,          // payload: int32_t khz
    DSD_APP_CMD_RTL_SET_SQL_DB = 487,      // payload: double dB
    DSD_APP_CMD_RTL_SET_VOL_MULT = 488,    // payload: int32_t mult
    DSD_APP_CMD_RTL_SET_BIAS_TEE = 489,    // payload: int32_t on(0/1)
    DSD_APP_CMD_RTLTCP_SET_AUTOTUNE = 490, // payload: int32_t on(0/1)
    DSD_APP_CMD_RTL_SET_AUTO_PPM = 491,    // payload: int32_t on(0/1)
    // Live retune from a spectrum tap. Separate from RTL_SET_FREQ so taps coalesce only
    // with taps, the trunking gate is enforced at drain time, and an accepted tune resets
    // the decoder's auto-modulation votes and call state for re-acquisition.
    DSD_APP_CMD_MANUAL_TUNE = 492, // payload: uint32_t hz
    // Hand the tuner back to the operator: trunking and conventional scanner mode both off,
    // idempotent. Distinct from TRUNK_TOGGLE/SCANNER_TOGGLE, which cannot express "off" from a
    // frontend that only sees whether *something* owns the tuner.
    DSD_APP_CMD_TUNER_RELEASE = 493, // no payload
    // Set the modulation outright rather than cycling it. A view showing C4FM and
    // QPSK as two choices has to be able to ask for the one it is not on; a toggle
    // makes that a guess about state that may already have moved.
    DSD_APP_CMD_MOD_SET = 494, // payload: int32_t (0 = C4FM, 1 = QPSK, 2 = GFSK), matching dsd_state::rf_mod
    // Switch which protocols are decoded, mid-session. Payload is a
    // dsdneoUserDecodeMode (<dsd-neo/runtime/config.h>), applied through the same
    // preset helper the CLI and config paths use.
    DSD_APP_CMD_DECODE_MODE_SET = 495, // payload: int32_t dsdneoUserDecodeMode
    // Hand the tuner to trunking, or take it back, from a frontend that knows which
    // of the two it is asking for. TRUNK_TOGGLE cannot say that: a view offering
    // "follow this system" as a choice has to be able to ask for the state it is not
    // in, and a flip makes that a guess about state that may already have moved.
    // Turning it on clears scanner mode, which is the same exclusion SCANNER_TOGGLE
    // already applies in the other direction — both cannot own the tuner. Turning it
    // off is deliberately narrow; TUNER_RELEASE remains the way to clear both at once.
    DSD_APP_CMD_TRUNK_SET = 496,  // payload: int32_t on(0/1)
    DSD_APP_CMD_AIRSPY_SET = 497, // payload: dsd_app_airspy_setting_payload
    DSD_APP_CMD_AIRSPY_ENABLE_INPUT = 498,

    // Rigctl / tuning params
    DSD_APP_CMD_RIGCTL_SET_MOD_BW = 500, // payload: int32_t hz
    DSD_APP_CMD_TG_HOLD_SET = 501,       // payload: uint32_t tg
    DSD_APP_CMD_HANGTIME_SET = 502,      // payload: double seconds
    DSD_APP_CMD_SLOT_PREF_SET = 503,     // payload: int32_t pref (0=slot 1, 1=slot 2, 2=auto)
    DSD_APP_CMD_SLOTS_ONOFF_SET = 504,   // payload: int32_t mask
    // Voice-gated scan (-Y and --trunk-scan): hold on a signal only while it carries voice.
    // The qualify window opens at sync; voice must appear before it lapses or
    // the scan moves on. The hold window restarts on every voice frame and the
    // scan leaves only after it lapses with no voice.
    DSD_APP_CMD_SCAN_VOICE_ONLY_SET = 505,       // payload: int32_t on(0/1)
    DSD_APP_CMD_SCAN_VOICE_QUALIFY_MS_SET = 506, // payload: int32_t ms (100..600000)
    DSD_APP_CMD_SCAN_VOICE_HOLD_MS_SET = 507,    // payload: int32_t ms (100..600000)

    // Pulse audio device selection
    DSD_APP_CMD_PULSE_OUT_SET = 520, // payload: char name[]
    DSD_APP_CMD_PULSE_IN_SET = 521,  // payload: char name[]

    // Input volume
    DSD_APP_CMD_INPUT_VOL_SET = 530, // payload: int32_t mult (1..16)

    // LRRP file output
    DSD_APP_CMD_LRRP_SET_HOME = 540,
    DSD_APP_CMD_LRRP_SET_DSDP = 541,
    DSD_APP_CMD_LRRP_SET_CUSTOM = 542, // payload: char path[]
    DSD_APP_CMD_LRRP_DISABLE = 543,

    // Import helpers
    DSD_APP_CMD_IMPORT_CHANNEL_MAP = 560, // payload: char path[]
    DSD_APP_CMD_IMPORT_GROUP_LIST = 561,  // payload: char path[]
    DSD_APP_CMD_IMPORT_KEYS_DEC = 562,    // payload: char path[]
    DSD_APP_CMD_IMPORT_KEYS_HEX = 563,    // payload: char path[]

    // Unload what those imported. A frontend that lets a system clear its CSV
    // selection needs to express "none", and re-importing cannot: every command
    // above takes a path, and the services reject an empty one. Without these,
    // clearing a picker left the previous file naming talkgroups and mapping
    // channels for the rest of the session.
    DSD_APP_CMD_IMPORT_CHANNEL_MAP_CLEAR = 564, // no payload
    DSD_APP_CMD_IMPORT_GROUP_LIST_CLEAR = 565,  // no payload
    DSD_APP_CMD_IMPORT_KEYS_CLEAR = 566,        // no payload; dec and hex share one keyring

    // P25 band plan (docs/csv-formats.md, "P25 Band Plan CSV"): load a user plan into the IDEN
    // tables, or write the learned tables (every trunk-scan target under trunk scan) to a file.
    DSD_APP_CMD_IMPORT_P25_BANDPLAN = 567, // payload: char path[]
    DSD_APP_CMD_EXPORT_P25_BANDPLAN = 568, // payload: char path[]

    // RadioReference import (docs/radioreference-import.md)
    DSD_APP_CMD_RR_APPLY_IMPORT = 570, // payload: dsd_app_rr_apply_payload
    DSD_APP_CMD_RR_ACCOUNT_SET = 571,  // payload: dsd_app_rr_account_payload

    // Source radio ID aliases (labels only)
    DSD_APP_CMD_IMPORT_SRC_LIST = 572,       // payload: char path[]
    DSD_APP_CMD_IMPORT_SRC_LIST_CLEAR = 573, // no payload

    // P25 helpers
    DSD_APP_CMD_P25_P2_PARAMS_SET = 580, // payload: struct { uint64_t wacn, sysid, cc; }

    // Edit the first effective policy row with these bounds, preserving its metadata.
    // Missing exact IDs are added. listen=1 selects A; listen=0 selects B and releases
    // blocked active group calls. Persist to group_in_file unless a scan row owns the list.
    DSD_APP_CMD_TG_LISTEN_SET = 590, // payload: dsd_app_tg_listen_payload
    // Edit all rows except radio-ID aliases; a nonempty tag selects exact tag matches.
    DSD_APP_CMD_TG_LISTEN_SET_ALL = 591,      // payload: dsd_app_tg_listen_all_payload
    DSD_APP_CMD_TG_ROW_SET = 592,             // payload: dsd_app_tg_row_payload
    DSD_APP_CMD_TG_ROW_REMOVE = 593,          // payload: dsd_app_tg_range_payload
    DSD_APP_CMD_TG_LIST_EXPORT = 594,         // payload: dsd_app_tg_export_payload
    DSD_APP_CMD_TG_SELECTION_SET = 595,       // payload: dsd_app_tg_selection_payload
    DSD_APP_CMD_TG_LOCKOUT_PERSIST_SET = 596, // payload: int32_t 0=session-only, 1=save (default)
    DSD_APP_CMD_TG_SESSION_AVOID_CLEAR = 597, // payload: uint64_t captured policy context; current list only

    // UI display toggles
    DSD_APP_CMD_UI_SHOW_DSP_PANEL_TOGGLE = 620,
    DSD_APP_CMD_UI_SHOW_P25_METRICS_TOGGLE = 621,
    DSD_APP_CMD_UI_SHOW_P25_AFFIL_TOGGLE = 622,
    DSD_APP_CMD_UI_SHOW_P25_NEIGHBORS_TOGGLE = 623,
    DSD_APP_CMD_UI_SHOW_P25_IDEN_TOGGLE = 624,
    DSD_APP_CMD_UI_SHOW_P25_CCC_TOGGLE = 625,
    DSD_APP_CMD_UI_SHOW_CHANNELS_TOGGLE = 626,
    DSD_APP_CMD_UI_SHOW_P25_CALLSIGN_TOGGLE = 627,

    // Key management
    DSD_APP_CMD_KEY_BASIC_SET = 640,     // payload: uint32_t
    DSD_APP_CMD_KEY_SCRAMBLER_SET = 641, // payload: uint32_t
    DSD_APP_CMD_KEY_RC4DES_SET = 642,    // payload: uint64_t
    DSD_APP_CMD_KEY_HYTERA_SET = 643,    // payload: struct { uint64_t H,K1,K2,K3,K4; }
    DSD_APP_CMD_KEY_AES_SET = 644,       // payload: struct { uint64_t K1,K2,K3,K4; }

    // Keystream creation (string payloads processed on demod thread)
    DSD_APP_CMD_KEY_TYT_AP_SET = 645,      // payload: char s[] (two 64-bit hex concatenated)
    DSD_APP_CMD_KEY_RETEVIS_RC2_SET = 646, // payload: char s[] (two 64-bit hex concatenated)
    DSD_APP_CMD_KEY_TYT_EP_SET = 647,      // payload: char s[] (two 64-bit hex concatenated)
    DSD_APP_CMD_KEY_KEN_SCR_SET = 648,     // payload: char s[] (decimal lfsr)
    DSD_APP_CMD_KEY_ANYTONE_BP_SET = 649,  // payload: char s[] (16-bit hex)
    DSD_APP_CMD_KEY_XOR_SET = 650,         // payload: char s[] ("len:hexbytes")

    // Encoders / protocol helpers
    DSD_APP_CMD_M17_USER_DATA_SET = 651, // payload: char s[] (<=49 chars)
    DSD_APP_CMD_KEY_DIRECT_SET = 652,    // sensitive: dsd_app_key_direct_payload; erase every owned copy
    DSD_APP_CMD_FORCE_KEY_SET = 653,     // payload: int32_t 0/1/2; configured scan-mode setting
    DSD_APP_CMD_DECRYPTION_APPLY = 654,  // sensitive: dsd_app_decryption_payload; retained result

    // DSP runtime (rtl_stream_*)
    DSD_APP_CMD_DSP_OP = 700,             // payload: dsd_app_dsp_payload
    DSD_APP_CMD_CONFIG_APPLY = 710,       // payload: dsdneoUserConfig (see runtime/config.h)
    DSD_APP_CMD_CONFIG_METADATA_SET = 711 // payload: dsd_app_config_metadata_payload
};

/* A single edit is merged into decoder-owned settings, avoiding stale snapshot writes. */
typedef struct {
    char key[40]; /* canonical airspy_ config key */
    char value[40];
} dsd_app_airspy_setting_payload;

/** DSP control opcodes understood by the decoder/control-pump thread. */
enum dsd_app_dsp_op {
    DSD_APP_DSP_OP_TOGGLE_CQ = 2,
    DSD_APP_DSP_OP_TOGGLE_IQBAL = 5,
    DSD_APP_DSP_OP_IQ_DC_TOGGLE = 6,
    DSD_APP_DSP_OP_IQ_DC_K_DELTA = 7, // a: delta (+/-)
    DSD_APP_DSP_OP_TED_GAIN_SET = 9,  // a: CQPSK timing gain
    DSD_APP_DSP_OP_TUNER_AUTOGAIN_TOGGLE = 18,
};

/**
 * @brief Payload wrapper for DSP opcodes (fields interpreted per opcode).
 */
typedef struct {
    int op;
    int a;
    int b;
    int c;
    int d;
} dsd_app_dsp_payload;

typedef struct {
    char host[256];
    int32_t port;
} dsd_app_endpoint_payload;

typedef struct {
    char bind[256];
    int32_t port;
} dsd_app_udp_input_payload;

typedef struct {
    uint64_t wacn;
    uint64_t sysid;
    uint64_t cc;
} dsd_app_p25_p2_params_payload;

typedef struct {
    uint32_t id_start;
    uint32_t id_end; /* Equal to id_start for an exact talkgroup. */
    int32_t listen;  /* 1 = listen (A), 0 = do not tune (B). */
} dsd_app_tg_listen_payload;

typedef struct {
    int32_t listen;
    char tags[50]; /* Empty = all rows; otherwise an exact category match. */
} dsd_app_tg_listen_all_payload;

enum {
    DSD_APP_TG_FIELD_LISTEN = 1U << 0,
    DSD_APP_TG_FIELD_PRIORITY = 1U << 1,
    DSD_APP_TG_FIELD_PREEMPT = 1U << 2,
    DSD_APP_TG_FIELD_NAME = 1U << 3,
    DSD_APP_TG_FIELD_TAGS = 1U << 4
};

/* Context and generation are both required: a row from a previous scan target
 * can have the same ids and generation while belonging to a different policy. */
typedef struct {
    uint32_t id_start;
    uint32_t id_end;
    uint32_t fields; /* Bitmask below; absent fields keep the existing row value. */
    int32_t listen;
    int32_t priority;
    int32_t preempt;
    char name[50];
    char tags[50];
    uint64_t policy_context;
    unsigned int policy_generation;
} dsd_app_tg_row_payload;

typedef struct {
    uint32_t id_start;
    uint32_t id_end;
    uint64_t policy_context;
    unsigned int policy_generation;
} dsd_app_tg_range_payload;

typedef struct {
    uint64_t policy_context;
    unsigned int policy_generation;
    char path[1]; /* NUL-terminated, bounded by the submitted payload size. */
} dsd_app_tg_export_payload;

typedef struct {
    uint64_t policy_context;
    uint32_t policy_generation;
    uint32_t count;
    int32_t listening;
    char selection_path[1024]; /**< Private frontend-owned rows: table_index,start,end. */
} dsd_app_tg_selection_payload;

/** Retained outcome of the most recently drained TG_LIST_EXPORT command.
 * The context/generation identify the submitted request, including on failure.
 * path is the requested destination (written only when success == 1), or empty
 * if the submitted path was invalid. sequence is nonzero and advances per result. */
typedef struct {
    uint64_t sequence;
    uint64_t policy_context;
    unsigned int policy_generation;
    int success;
    char path[1024];
} dsd_app_tg_export_result;

typedef enum {
    DSD_APP_KEY_TYPE_BASIC = 0,
    DSD_APP_KEY_TYPE_HEX = 1, /* Hytera/AES determined by width. */
    DSD_APP_KEY_TYPE_RC4 = 2,
    DSD_APP_KEY_TYPE_SCRAMBLER = 3,
    DSD_APP_KEY_TYPE_M17_SCRAMBLER = 4,
    DSD_APP_KEY_TYPE_M17_AES = 5
} dsd_app_key_type;

typedef struct {
    int32_t key_type; /* dsd_app_key_type; digits alone cannot distinguish BP/RC4/Hytera. */
    char value[72];   /* Bounded, NUL-terminated; never echo to logs, toasts or snapshots. */
} dsd_app_key_direct_payload;

enum { DSD_APP_DECRYPTION_MATERIAL = 1U << 0, DSD_APP_DECRYPTION_MAP = 1U << 1, DSD_APP_DECRYPTION_FORCE = 1U << 2 };

enum { DSD_APP_KEY_SCOPE_DEFAULTS = 0, DSD_APP_KEY_SCOPE_TARGET = 1 };

enum {
    DSD_APP_KEY_SOURCE_NONE = 0,
    DSD_APP_KEY_SOURCE_COLLECTION = 1,
    DSD_APP_KEY_SOURCE_DIRECT = 2,
    DSD_APP_KEY_SOURCE_DIRECT_OVERLAY = 3
};

enum {
    DSD_APP_KEY_APPLIED = 1,
    DSD_APP_KEY_INVALID = -1,
    DSD_APP_KEY_STALE = -2,
    DSD_APP_KEY_UNAVAILABLE = -3,
    DSD_APP_KEY_FILE_ERROR = -4,
    DSD_APP_KEY_BUSY = -5,
    DSD_APP_KEY_CANCELLED = -6
};

/** Mutations are applied together on the decoder thread. Missing field bits
 * preserve current settings. Target requests include the context captured when
 * opening the editor; default edits never replace an explicit scan target. */
typedef struct {
    uint64_t request_id;
    uint64_t session_generation;
    uint64_t tune_generation;
    uint64_t key_epoch;
    uint32_t fields;
    int32_t scope;
    int32_t source;
    int32_t key_type;
    int32_t force;
    char target_id[64];
    char profile_ref[64];
    char value[72];
    char keys_hex[1024];
    char keys_dec[1024];
    char map_file[2048];
} dsd_app_decryption_payload;

/** Contains only request identity and outcome, never material or paths. */
typedef struct {
    uint64_t sequence;
    uint64_t request_id;
    uint64_t session_generation;
    int status;
    int scope;
} dsd_app_decryption_result;

typedef struct {
    uint64_t H;
    uint64_t K1;
    uint64_t K2;
    uint64_t K3;
    uint64_t K4;
} dsd_app_hytera_key_payload;

typedef struct {
    uint64_t K1;
    uint64_t K2;
    uint64_t K3;
    uint64_t K4;
} dsd_app_aes_key_payload;

typedef struct {
    int32_t autosave_enabled;
    char path[1024];
} dsd_app_config_metadata_payload;

typedef enum {
    DSD_APP_COMMAND_SUBMIT_REJECTED = -1,
    DSD_APP_COMMAND_SUBMIT_QUEUED = 1,
    DSD_APP_COMMAND_SUBMIT_COALESCED = 2
} dsd_app_command_submit_status;

#ifdef __cplusplus
extern "C" {
#endif

/* The queue erases its owned payload copies on every disposal path. The caller
 * still owns payload (including on rejection) and must erase its secret storage. */
/* Submission is rejected outside a frontend runtime session. Pending commands
 * are securely discarded at stop, and exports receive a failed completion. */
int dsd_app_command_submit(int cmd_id, const void* payload, size_t payload_sz);
int dsd_app_command_action(int cmd_id);
int dsd_app_command_set_i32(int cmd_id, int32_t value);
int dsd_app_command_set_u8(int cmd_id, uint8_t value);
int dsd_app_command_set_u32(int cmd_id, uint32_t value);
int dsd_app_command_set_u64(int cmd_id, uint64_t value);
int dsd_app_command_set_double(int cmd_id, double value);
int dsd_app_command_set_float(int cmd_id, float value);
int dsd_app_command_set_string(int cmd_id, const char* value);
int dsd_app_command_set_endpoint(int cmd_id, const char* host, int32_t port);
int dsd_app_command_set_p25_p2_params(const dsd_app_p25_p2_params_payload* payload);
int dsd_app_command_set_tg_listen(const dsd_app_tg_listen_payload* payload);
int dsd_app_command_set_tg_listen_all(const dsd_app_tg_listen_all_payload* payload);
/** Copy the last export result under a mutex without consuming a decoder snapshot.
 * Returns 1 if a result exists, 0 otherwise (out is zeroed), or 0 for NULL out.
 * Results are retained across unrelated commands, toasts, reads and session stops;
 * only another drained export replaces them. Sequence numbers are process-wide.
 * The UI should retain the pre-submit sequence, allow one outstanding export, and
 * accept a newer result matching its context/generation/path before registering
 * the file. Submission rejection must still be handled from the submit return. */
int dsd_app_tg_export_result_get(dsd_app_tg_export_result* out);
uint64_t dsd_app_command_session_generation(void);
int dsd_app_decryption_result_get(dsd_app_decryption_result* out);
int dsd_app_command_set_hytera_key(const dsd_app_hytera_key_payload* payload);
int dsd_app_command_set_aes_key(const dsd_app_aes_key_payload* payload);
int dsd_app_command_dsp_op(const dsd_app_dsp_payload* payload);
int dsd_app_command_apply_config(const dsdneoUserConfig* config);
int dsd_app_command_set_config_metadata(const dsd_app_config_metadata_payload* payload);
int dsd_app_command_set_rr_apply(const dsd_app_rr_apply_payload* payload);
int dsd_app_command_set_rr_account(const dsd_app_rr_account_payload* payload);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_COMMANDS_H_ */
