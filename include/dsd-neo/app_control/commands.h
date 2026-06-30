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

#include <dsd-neo/runtime/config.h>
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

    // Additional commands to preserve legacy hotkeys in async mode
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

    DSD_APP_CMD_CRC_RELAX_TOGGLE = 420,
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
    DSD_APP_CMD_RTL_SET_DEV = 482,         // payload: int32_t index
    DSD_APP_CMD_RTL_SET_FREQ = 483,        // payload: int32_t hz
    DSD_APP_CMD_RTL_SET_GAIN = 484,        // payload: int32_t gain
    DSD_APP_CMD_RTL_SET_PPM = 485,         // payload: int32_t ppm
    DSD_APP_CMD_RTL_SET_BW = 486,          // payload: int32_t khz
    DSD_APP_CMD_RTL_SET_SQL_DB = 487,      // payload: double dB
    DSD_APP_CMD_RTL_SET_VOL_MULT = 488,    // payload: int32_t mult
    DSD_APP_CMD_RTL_SET_BIAS_TEE = 489,    // payload: int32_t on(0/1)
    DSD_APP_CMD_RTLTCP_SET_AUTOTUNE = 490, // payload: int32_t on(0/1)
    DSD_APP_CMD_RTL_SET_AUTO_PPM = 491,    // payload: int32_t on(0/1)

    // Rigctl / tuning params
    DSD_APP_CMD_RIGCTL_SET_MOD_BW = 500, // payload: int32_t hz
    DSD_APP_CMD_TG_HOLD_SET = 501,       // payload: uint32_t tg
    DSD_APP_CMD_HANGTIME_SET = 502,      // payload: double seconds
    DSD_APP_CMD_SLOT_PREF_SET = 503,     // payload: int32_t pref01
    DSD_APP_CMD_SLOTS_ONOFF_SET = 504,   // payload: int32_t mask

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

    // P25 helpers
    DSD_APP_CMD_P25_P2_PARAMS_SET = 580, // payload: struct { uint64_t wacn, sysid, cc; }

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

    // DSP runtime (rtl_stream_*)
    DSD_APP_CMD_DSP_OP = 700,      // payload: dsd_app_dsp_payload (see ui_dsp_cmd.h)
    DSD_APP_CMD_CONFIG_APPLY = 710 // payload: dsdneoUserConfig (see runtime/config.h)
};

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

#ifdef __cplusplus
extern "C" {
#endif

int dsd_app_command_action(int cmd_id);
int dsd_app_command_set_i32(int cmd_id, int32_t value);
int dsd_app_command_set_u8(int cmd_id, uint8_t value);
int dsd_app_command_set_u32(int cmd_id, uint32_t value);
int dsd_app_command_set_u64(int cmd_id, uint64_t value);
int dsd_app_command_set_double(int cmd_id, double value);
int dsd_app_command_set_float(int cmd_id, float value);
int dsd_app_command_set_string(int cmd_id, const char* value);
int dsd_app_command_set_endpoint(int cmd_id, const char* host, int32_t port);
int dsd_app_command_set_udp_input(const char* bind, int32_t port);
int dsd_app_command_set_p25_p2_params(const dsd_app_p25_p2_params_payload* payload);
int dsd_app_command_set_hytera_key(const dsd_app_hytera_key_payload* payload);
int dsd_app_command_set_aes_key(const dsd_app_aes_key_payload* payload);
int dsd_app_command_dsp_op(const dsd_app_dsp_payload* payload);
int dsd_app_command_apply_config(const dsdneoUserConfig* config);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_COMMANDS_H_ */
