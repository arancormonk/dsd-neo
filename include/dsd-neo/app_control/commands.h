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
#include <dsd-neo/app_control/rx_tone_view.h>
#include <dsd-neo/core/id_pattern.h>
#include <dsd-neo/core/receive_types.h>
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

    // Digital gain step, in whole units on the -g multiplier: from auto it steps from 1, below 0.001 is auto, the top
    // is 50 (<dsd-neo/core/gain_setting.h>).
    DSD_APP_CMD_GAIN_DELTA = 20,  // payload: int32_t delta (+1/-1)
    DSD_APP_CMD_AGAIN_DELTA = 21, // payload: int32_t delta (+1/-1) for analog gain; from auto it steps from 50, below 1
                                  // is auto, the top is 100

    // Trunking on/off. Refused while a --trunk-scan coordinator exists, running or paused: each target sets its own.
    DSD_APP_CMD_TRUNK_TOGGLE = 30,
    // `y`: a paused -Y scan or trunk scan resumes; a running trunk scan pauses where it is; a running -Y scan stops
    // and a stopped one starts.
    DSD_APP_CMD_SCANNER_TOGGLE = 31,

    DSD_APP_CMD_PAYLOAD_TOGGLE = 40,

    // UI/state toggles and actions
    DSD_APP_CMD_P25_GA_TOGGLE = 50,  // Toggle P25 Group Affiliation section
    DSD_APP_CMD_TG_HOLD_TOGGLE = 51, // payload: uint8_t slot (0 or 1)
    DSD_APP_CMD_LPF_TOGGLE = 52,
    DSD_APP_CMD_HPF_TOGGLE = 53,
    DSD_APP_CMD_PBF_TOGGLE = 54,
    DSD_APP_CMD_HPF_D_TOGGLE = 55,
    // Relaxed CRC: no payload flips it; an int32_t payload sets it (0 off, else on). The TCP API's
    // aggr_sync_toggle sends no payload and stays flip-only.
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
    // The gain settings: a value outside the range (NaN, infinite, negative, above the top, or positive under the
    // minimum) is refused and the setting kept (<dsd-neo/core/gain_setting.h>).
    DSD_APP_CMD_GAIN_SET = 104,          // payload: double (0 = auto, 0.001..50)
    DSD_APP_CMD_AGAIN_SET = 105,         // payload: double (0 = auto, 1..100)
    DSD_APP_CMD_INPUT_WARN_DB_SET = 106, // payload: double
    DSD_APP_CMD_INPUT_MONITOR_TOGGLE = 107,
    DSD_APP_CMD_COSINE_FILTER_TOGGLE = 108,

    // The input switches below (200, 206, 209, 444-447, 461, 463, 521) close the running input and open the new one
    // (dsd_audio_switch_input(), issue #634); one that cannot open its input keeps the running one and fails.
    DSD_APP_CMD_TCP_CONNECT_AUDIO = 200, // connect to opts->tcp_hostname/port and switch the input to it
    DSD_APP_CMD_RIGCTL_CONNECT = 201,    // uses opts->tcp_hostname/rigctlportno
    DSD_APP_CMD_RETURN_CC = 202,
    DSD_APP_CMD_CHANNEL_CYCLE = 203,
    DSD_APP_CMD_SYMCAP_SAVE = 204, // auto-name capture file and start
    DSD_APP_CMD_SYMCAP_STOP = 205,
    DSD_APP_CMD_REPLAY_LAST = 206, // replay opts->symbol_capture_last, the capture closed last
    DSD_APP_CMD_WAV_START = 207,
    DSD_APP_CMD_WAV_STOP = 208,
    DSD_APP_CMD_STOP_PLAYBACK = 209, // a file or symbol playback switches to the configured Pulse input

    // Trunk policy toggles
    DSD_APP_CMD_TRUNK_WLIST_TOGGLE = 210,
    DSD_APP_CMD_TRUNK_PRIV_TOGGLE = 211,
    DSD_APP_CMD_TRUNK_DATA_TOGGLE = 212,
    DSD_APP_CMD_TRUNK_ENC_TOGGLE = 213,
    DSD_APP_CMD_WAV_TOGGLE = 214,
    DSD_APP_CMD_ENC_LOCKOUT_CLEAR = 215, // forget all encrypted-target lockouts
    // On-the-fly scan controls (#380): -Y rows or --trunk-scan targets, whichever is running
    // (a paused scan refuses hold and avoid, CHANNEL_CYCLE's next included, until it resumes; clear is a flag edit it
    // takes, issue #672)
    DSD_APP_CMD_SCAN_HOLD_TOGGLE = 216, // hold/release rotation on the channel/target on air
    DSD_APP_CMD_SCAN_AVOID = 217,       // avoid the channel/target on air for the session and step on
    DSD_APP_CMD_SCAN_AVOID_CLEAR = 218, // put every avoided channel/target back into the rotation
    // Resume the paused -Y scan (the next row from its cursor) or trunk scan (the target it left); no payload. A
    // running scan is left as it is, and with none paused nothing starts.
    DSD_APP_CMD_SCAN_RESUME = 219,
    // Previous channel (issue #666): CHANNEL_CYCLE's step backward through the same list -- the previous trunk-scan
    // target, -Y row, P25 control-channel candidate (-^) or LCN -- refused as it is (a replay, Signal search, a paused
    // scan, no tuner); no payload. The rotation and Next carry on forward from where it lands.
    DSD_APP_CMD_CHANNEL_CYCLE_PREV = 220,
    // Put one avoided -Y row or --trunk-scan target back into the rotation by its identity (issue #672); payload
    // dsd_app_scan_avoid_remove_payload. Never retunes. Unlike hold and avoid, it and SCAN_AVOID_CLEAR are taken while a
    // scan is paused, and for -Y rows whenever no trunk scan runs (-Y stopped, -T): the avoids still filter the control
    // channel hunts there.
    DSD_APP_CMD_SCAN_AVOID_REMOVE = 221,
    // Switch one --trunk-scan target list on or off for the session, or every list on (issue #697); payload
    // dsd_app_scan_list_set_payload, submitted with dsd_app_command_scan_list_set(). Set semantics: the list ends in
    // the state asked for. Switching off a list holding the target on air leaves it at once, as `b` does, unless
    // another list that is on holds it too. A paused scan and Signal search refuse it. Every one gets a retained
    // result (dsd_app_scan_list_results_since()).
    DSD_APP_CMD_SCAN_LIST_SET = 222,

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
    // Per-call skip; unlike LOCKOUT_SLOT, never changes a policy row or saves a CSV.
    DSD_APP_CMD_SKIP_SLOT = 310, // payload: uint8_t slot (0=slot1, 1=slot2)

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
    DSD_APP_CMD_SYMBOL_IN_OPEN = 444,       // payload: char path[]; a symbol capture, replayed at symbol pace
    DSD_APP_CMD_INPUT_WAV_SET = 445,        // payload: char path[]; a file, opened as -i opens it
    DSD_APP_CMD_INPUT_SYM_STREAM_SET = 446, // payload: char path[]; a float symbol stream
    DSD_APP_CMD_INPUT_SET_PULSE = 447,      // the default Pulse input device
    // payload: int32_t 0/1; opts->record_unknown_only: record only calls no talkgroup row knows (issue #708). Waits
    // while a decoder holds a carrier, refused during Signal search; the recording view reports it.
    DSD_APP_CMD_RECORD_UNKNOWN_ONLY_SET = 448,
    // payload: int32_t ms 0..10000 (200 or less is off); opts->analog_record_max_gap_ms: the longest pause one analog
    // call and its recording survive (issue #756). Out of range fails and changes nothing. A held call takes a new value
    // at its next block or tick. Waits while a decoder holds a carrier, refused during Signal search; the recording view
    // reports it.
    DSD_APP_CMD_ANALOG_RECORD_MAX_GAP_SET = 449,
    // payload: int32_t ms 0..1000 (0 is off); opts->analog_record_preroll_ms: the audio heard just before an NFM call
    // that each new recording starts with (issue #668). Out of range fails and changes nothing. Applying it, even with
    // the value in force, drops the audio held so far. Waits while a decoder holds a carrier, refused during Signal
    // search; the recording view reports it.
    DSD_APP_CMD_ANALOG_RECORD_PREROLL_SET = 450,

    // Networking / device configs
    DSD_APP_CMD_UDP_OUT_CFG = 460,           // payload: struct { char host[256]; int32_t port; }
    DSD_APP_CMD_TCP_CONNECT_AUDIO_CFG = 461, // payload: struct { char host[256]; int32_t port; }
    DSD_APP_CMD_RIGCTL_CONNECT_CFG = 462,    // payload: struct { char host[256]; int32_t port; }
    DSD_APP_CMD_UDP_INPUT_CFG = 463,         // payload: struct { char bind[256]; int32_t port; }; binds now

    // RTL-SDR controls
    DSD_APP_CMD_RTL_ENABLE_INPUT = 480,
    DSD_APP_CMD_RTL_RESTART = 481,
    DSD_APP_CMD_RTL_SET_DEV = 482, // payload: int32_t index
    // payload: uint32_t Hz; selects the CC during single-system P25 trunking. Otherwise a tune the operator asked
    // for: accepted (applied or pending), it pauses a running -Y scan or trunk scan, which keeps its list, cursor and
    // session edits for SCAN_RESUME or `y`; refused or deferred, it changes nothing. A paused scan stays paused.
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
    // Hand the tuner back to the operator, idempotent: trunking off, and a running -Y scan or trunk
    // scan paused where it is (resumed by SCAN_RESUME or `y`); a paused scan stays paused. Distinct
    // from TRUNK_TOGGLE/SCANNER_TOGGLE, which cannot express "off" from a frontend that only sees
    // whether *something* owns the tuner.
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
    // already applies in the other direction — both cannot own the tuner (a paused -Y scan
    // ends). Turning it off is deliberately narrow; TUNER_RELEASE remains the way to clear both
    // at once. Refused while a --trunk-scan coordinator exists, running or paused.
    DSD_APP_CMD_TRUNK_SET = 496,  // payload: int32_t on(0/1)
    DSD_APP_CMD_AIRSPY_SET = 497, // payload: dsd_app_airspy_setting_payload
    DSD_APP_CMD_AIRSPY_ENABLE_INPUT = 498,
    // The whole squelch setting (issue #518 follow-up): a level, the auto squelch with its margin over the floor the
    // demodulator learns, or the noise squelch with its FM quieting. Edits the configured default, as RTL_SET_SQL_DB
    // does.
    DSD_APP_CMD_RTL_SET_SQL_SETTING = 499, // payload: dsd_app_squelch_setting_payload

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
    // Configured NFM channel-filter width (issue #525): the full RF passband in Hz, 8000..25000, or 0 for the
    // 16000 default. Refused with a toast, changing nothing, when out of range or when the front end the analog
    // preset runs on cannot filter it at its DSP rate; applied live to a running analog monitor.
    DSD_APP_CMD_NFM_BANDWIDTH_SET = 508, // payload: int32_t Hz (0 = default)
    // Configured AM channel-filter width (issue #524): the full RF passband in Hz, 5000..20000, or 0 for the 6000
    // default. Refused with a toast, changing nothing, when out of range or when a running AM front end cannot
    // filter it at its DSP rate; applied live while AM is on air, otherwise kept for the next switch to AM.
    DSD_APP_CMD_AM_BANDWIDTH_SET = 509, // payload: int32_t Hz (0 = default)
    // Configured CTCSS/DCS receive policy (issue #527, the live tone-filter editor): the mode and its '/'-separated
    // list, checked by the list parser (dsd_tone_filter_check()), or the mode alone with the configured list kept.
    // Refused with a toast, changing nothing, when invalid. It edits the configured policy: while a scan row with its
    // own tone options is on air the row keeps its own, and the toast says the row overrides the edit. Applied from the
    // monitor's next read of audio. Not coalescible: each queued edit is judged, and applied or refused, on its own.
    DSD_APP_CMD_TONE_FILTER_SET = 510, // payload: dsd_app_tone_filter_payload
    // Session edit of one field of the scan row on air ("this channel", issue #518): the squelch, channel width, tone
    // policy, receive filter or service names (issue #637) or (a --trunk-scan target's) tuner gain the row runs for the
    // rest of the session, over what its list sets; the configured defaults are not touched and nothing is saved.
    // Names the row the editor opened on (scanner, session, row, target id), so an edit that outlives the visit still
    // reaches that row, and one from an earlier scan is refused. Not coalescible, and no scoped (suspend/resume)
    // command: it changes the row's options itself.
    DSD_APP_CMD_SCAN_ROW_EDIT = 511, // payload: dsd_app_scan_row_edit_payload
    // Configured receive policy (issue #637): one slot/DCC/RAN/NAC filter (kind 0..3) or the whole service-name table
    // (kind 4), checked by dsd_receive_edit_parse() and refused with a toast, changing nothing, when invalid. While a
    // scan row with its own receive options is on air the row keeps them. Applied at once in memory (a config save
    // writes it); takes the P25 SM tick guard because a changed effective policy ends calls and rotates per-call WAVs.
    DSD_APP_CMD_RECEIVE_SET = 512, // payload: dsd_app_receive_payload

    // Pulse audio device selection
    DSD_APP_CMD_PULSE_OUT_SET = 520, // payload: char name[]
    DSD_APP_CMD_PULSE_IN_SET = 521,  // payload: char name[]; switches the input to that Pulse device

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

    // Re-read the group list file the session uses, on the decoder thread after a scan row's own list is set aside, so
    // a -Y row's or trunk-scan target's file never becomes the global list. No payload (or an empty string): the
    // configured group file, failing when there is none. A path: only when it is the configured group file (an exact
    // match), else nothing happens beyond a log line, so a frontend that replaced a file can post it without knowing
    // whether the session still uses it. Applies as DSD_APP_CMD_IMPORT_GROUP_LIST does, refusals included.
    DSD_APP_CMD_IMPORT_GROUP_LIST_RELOAD = 569, // payload: none, or char path[] (the file expected in use)

    // RadioReference import (docs/radioreference-import.md)
    DSD_APP_CMD_RR_APPLY_IMPORT = 570, // payload: dsd_app_rr_apply_payload
    DSD_APP_CMD_RR_ACCOUNT_SET = 571,  // payload: dsd_app_rr_account_payload

    // Source radio ID aliases (labels only)
    DSD_APP_CMD_IMPORT_SRC_LIST = 572,       // payload: char path[]
    DSD_APP_CMD_IMPORT_SRC_LIST_CLEAR = 573, // no payload

    // Re-read the channel map, only when the path is the session's channel map file (an exact match); else nothing
    // happens beyond a log line. Applies as DSD_APP_CMD_IMPORT_CHANNEL_MAP does, refusals included.
    DSD_APP_CMD_IMPORT_CHANNEL_MAP_RELOAD = 574, // payload: char path[] (the file expected in use)

    // P25 helpers
    DSD_APP_CMD_P25_P2_PARAMS_SET = 580, // payload: struct { uint64_t wacn, sysid, cc; }

    // Edit the row the payload's row token names (or, bounds-only, the first effective row
    // with these bounds), preserving its metadata. Missing exact IDs are added. listen=1
    // selects A; listen=0 selects B and releases blocked active group calls. Persist to
    // group_in_file unless a scan row owns the list.
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
    DSD_APP_CMD_MOUSE_WHEEL_TOGGLE = 628,

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
    DSD_APP_CMD_DSP_OP = 700,                // payload: dsd_app_dsp_payload
    DSD_APP_CMD_CONFIG_APPLY = 710,          // payload: dsdneoUserConfig, or dsd_app_config_load_payload
    DSD_APP_CMD_CONFIG_METADATA_SET = 711,   // payload: dsd_app_config_metadata_payload
    DSD_APP_CMD_SIGNAL_SEARCH_START = 712,   // payload: dsd_signal_search_config
    DSD_APP_CMD_SIGNAL_SEARCH_CONTROL = 713, // payload: dsd_signal_search_control
    // Internal (not in the control API): the terminal menu re-parsed the DSD_NEO_* environment on its own thread, and
    // the decoder now applies the runtime config's options fields (dsd_apply_runtime_config_to_opts()) to its options.
    DSD_APP_CMD_RUNTIME_CONFIG_APPLY = 714 // no payload
};

/* DSD_APP_CMD_RTL_SET_SQL_SETTING: mode is dsd_squelch_mode (core/power.h); level the LEVEL threshold in mean-power
   units (0 = off), margin_db the AUTO margin or the NOISE quieting in whole dB (3..30). */
typedef struct {
    int32_t mode;
    int32_t margin_db;
    double level;
} dsd_app_squelch_setting_payload;

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

/*
 * Row token fields shared by the single-row talkgroup payloads (TG_LISTEN_SET, TG_ROW_SET, TG_ROW_REMOVE), as
 * dsd_tg_policy_row_key in <dsd-neo/core/talkgroup_policy.h>:
 *   table_index  the listed row's index in the effective table; -1 for a heard-only exact ID or a legacy bounds-only
 *                edit, which addresses the first talkgroup row with id_start..id_end (a missing exact ID is added).
 *                A zeroed payload is index 0, so a bounds-only caller sets -1 explicitly.
 *   kind         DSD_APP_TG_ROW_* (dsd_tg_policy_row_kind) of the listed row; ignored with index -1.
 *   pattern      a pattern row's text (id_start = id_end = 0); a pattern row always needs its index.
 * A token edit is refused, changing nothing, when the captured context/generation is stale or the row at the index
 * no longer has that kind and key.
 */
enum { DSD_APP_TG_ROW_EXACT = 0, DSD_APP_TG_ROW_RANGE = 1, DSD_APP_TG_ROW_PATTERN = 2 };

typedef struct {
    uint32_t id_start;
    uint32_t id_end; /* Equal to id_start for an exact talkgroup. */
    int32_t listen;  /* 1 = listen (A), 0 = do not tune (B). */
    int32_t table_index;
    uint8_t kind;
    char pattern[DSD_ID_PATTERN_TEXT_MAX];
    /* The captured list version. Checked like TG_ROW_SET's whenever the payload carries a token (index >= 0) or a
     * non-zero pair; a legacy bounds-only payload (index -1, both 0) is applied unchecked. */
    uint64_t policy_context;
    unsigned int policy_generation;
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
    DSD_APP_TG_FIELD_TAGS = 1U << 4,
    DSD_APP_TG_FIELD_RECORD = 1U << 5 /* applied after LISTEN in the same edit; a row not listening records nothing */
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
    uint8_t record; /* 1 = record the talkgroup's calls, 0 = do not (the CSV's record column) */
    uint64_t policy_context;
    unsigned int policy_generation;
    int32_t table_index; /* Row token: see dsd_app_tg_listen_payload. */
    uint8_t kind;
    char pattern[DSD_ID_PATTERN_TEXT_MAX];
} dsd_app_tg_row_payload;

typedef struct {
    uint32_t id_start;
    uint32_t id_end;
    uint64_t policy_context;
    unsigned int policy_generation;
    int32_t table_index; /* Row token: see dsd_app_tg_listen_payload. */
    uint8_t kind;
    char pattern[DSD_ID_PATTERN_TEXT_MAX];
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
    char selection_path[1024]; /**< Private frontend-owned rows: table_index,kind,start,end,pattern (pattern empty
                                    except on a pattern row); an old table_index,start,end row is exact or range. */
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

/**
 * DSD_APP_CMD_CONFIG_APPLY of a loaded file (terminal Config > Load, a profile load): the config, and the autosave
 * setting the session adopts only when the config stays applied (issue #671). Applied ("Config applied; ..." included),
 * the session saves to @c autosave as CONFIG_METADATA_SET would set it, and the status line says what loaded unless the
 * apply said something else; not applied ("Config not applied: ..."), the autosave stays as it was, so the exit
 * autosave never writes the session into the file that did not load.
 */
typedef struct {
    dsdneoUserConfig config; /* first: a CONFIG_APPLY handler reads the payload as the config */
    dsd_app_config_metadata_payload autosave;
    char profile[64]; /* the profile loaded from @c autosave.path, "" for the whole file */
} dsd_app_config_load_payload;

/** DSD_APP_CMD_TONE_FILTER_SET: the whole configured tone policy. Allow and block need a list; off keeps the one it is
 * given (or none). The list is the text dsd_tone_set_parse() reads, as typed; the command validates it. With
 * @c keep_list the edit is the mode alone: @c list must be "", and the list is the configured one as the decoder holds
 * it when the edit runs, after every command queued before it (an edit, a loaded config), never a frontend's copy. */
typedef struct {
    int32_t mode;                             /* dsd_tone_filter_mode: 0 off, 1 allow, 2 block */
    int32_t keep_list;                        /* 1 = keep the configured list; 0 = @c list is the list */
    char list[DSD_APP_TONE_FILTER_LIST_SIZE]; /* NUL-terminated; "" for none */
} dsd_app_tone_filter_payload;

/** One receive filter (kind 0..3) or the complete naming table (kind 4). */
typedef struct {
    int32_t kind;
    int32_t enabled;
    char allow[DSD_RECEIVE_LIST_MAX + 1];
    char block[DSD_RECEIVE_LIST_MAX + 1];
    char names[DSD_RECEIVE_NAMES_TEXT_MAX + 1];
} dsd_app_receive_payload;

/** DSD_APP_CMD_SCAN_ROW_EDIT ("this channel", issue #518). The row as the frontends saw it: @c scanner, @c session and
 * @c row are dsd_state::scan_row_scanner, scan_row_session and scan_row_index, and @c target_id a --trunk-scan target's
 * id (trunk_scan_active_id; "" for a -Y row). @c field is one DSD_SCAN_ROW_FIELD_* and @c action a
 * dsd_scan_row_edit_action (runtime/scan_row_edit.h); for SET the field's value: @c squelch_db (whole dB, -100..0,
 * 0 = off), @c width_hz, @c tone_mode with @c tone_list as typed (checked by dsd_tone_filter_check()), or @c gain_db
 * (whole dB, 0 = AGC). @c mode is the row's dsd_scan_mode as the editor saw it, for the notice only (the engine checks the
 * edit against the row's own). */
typedef struct {
    uint32_t session;
    int32_t scanner;
    int32_t row;
    int32_t mode;
    int32_t field;
    int32_t action;
    int32_t squelch_db;
    int32_t width_hz;
    int32_t tone_mode;
    int32_t gain_db;
    int32_t squelch_mode;      /* dsd_squelch_mode: LEVEL (squelch_db), AUTO (squelch_margin_db, nfm and am rows) or
                                  NOISE (squelch_margin_db, nfm rows) */
    int32_t squelch_margin_db; /* AUTO: 3..30 dB over the learned floor; NOISE: 3..30 dB of quieting */
    char target_id[64];
    char tone_list[DSD_APP_TONE_FILTER_LIST_SIZE];
    dsd_app_receive_payload receive;
} dsd_app_scan_row_edit_payload;

/** Which store an avoided entry lives in (issue #672): a -Y channel map row, or a --trunk-scan target. */
typedef enum {
    DSD_APP_SCAN_AVOID_KIND_NONE = 0,
    DSD_APP_SCAN_AVOID_KIND_LCN = 1,
    DSD_APP_SCAN_AVOID_KIND_TRUNK_SCAN = 2,
} dsd_app_scan_avoid_kind;

/** DSD_APP_CMD_SCAN_AVOID_REMOVE (issue #672): the identity of one avoided entry, as the scan avoid view
 * (<dsd-neo/app_control/scan_avoid_view.h>) lists it, never a list position. A -Y row (KIND_LCN) is
 * (@c instance, @c list_rev, @c row, @c frequency_hz): dsd_state::state_instance and lcn_list_rev, the row and its
 * frequency, so it names nothing after a restart, a new channel map or a write to that row. A --trunk-scan target
 * (KIND_TRUNK_SCAN) is (@c session, @c target_id): dsd_state::trunk_scan_session (nonzero) and the target's id. The
 * fields the other kind uses are ignored. */
typedef struct {
    uint32_t kind; /* dsd_app_scan_avoid_kind */
    uint32_t session;
    uint64_t instance;
    uint64_t list_rev;
    uint64_t frequency_hz;
    int32_t row;
    uint32_t pad; /* explicit, so the payload carries no indeterminate padding */
    char target_id[64];
} dsd_app_scan_avoid_remove_payload;

/** The @c list of dsd_app_scan_list_set_payload that names every list: switched on, all of them at once. */
enum { DSD_APP_SCAN_LIST_ALL = -1 };

/** DSD_APP_CMD_SCAN_LIST_SET (issue #697): switch trunk-scan target list @c list (1-based, CLI/INI order) on or off,
 * or every list on (DSD_APP_SCAN_LIST_ALL with @c enabled 1). @c session is dsd_state::trunk_scan_session as the
 * frontend saw it (nonzero), so a request made of one scan never lands on the next. @c request is the caller's id from
 * dsd_app_scan_list_request_next(), which the retained result carries back. @c enabled is 0 or 1. */
typedef struct {
    uint32_t session;
    uint32_t request;
    int16_t list;
    uint8_t enabled;
    uint8_t pad; /* explicit, so the payload carries no indeterminate padding */
} dsd_app_scan_list_set_payload;

/** What one DSD_APP_CMD_SCAN_LIST_SET did. 0 to -5 are the coordinator's answers (the same values as
 * dsd_trunk_scan_list_set_result); the rest are app-control's, for a request the coordinator never saw. */
typedef enum {
    DSD_APP_SCAN_LIST_SET_OK = 0,            /**< The list is switched as asked */
    DSD_APP_SCAN_LIST_SET_UNCHANGED = 1,     /**< It already was (every list, for DSD_APP_SCAN_LIST_ALL) */
    DSD_APP_SCAN_LIST_SET_REFUSED_LAST = -1, /**< Switching it off would leave nothing to scan */
    DSD_APP_SCAN_LIST_SET_BUSY = -2,         /**< The coordinator was busy; try again */
    DSD_APP_SCAN_LIST_SET_UNAVAILABLE = -3,  /**< No trunk scan runs */
    DSD_APP_SCAN_LIST_SET_PAUSED = -4,       /**< The scan is paused: resume it first */
    DSD_APP_SCAN_LIST_SET_STALE = -5,        /**< The session is not this scan's, or it has no such list */
    DSD_APP_SCAN_LIST_SET_INVALID = -6,      /**< The decoder could not take the payload */
    DSD_APP_SCAN_LIST_SET_SEARCH = -7,       /**< Signal search runs: refused with every receiver command */
    DSD_APP_SCAN_LIST_SET_DROPPED = -8,      /**< Never applied: the session stopped or the queue overflowed first */
} dsd_app_scan_list_set_code;

/** The retained outcome of one DSD_APP_CMD_SCAN_LIST_SET: the request as submitted and what it did. @c sequence is
 * nonzero, process-wide, and advances per result. @c lists_seq is dsd_state::trunk_scan_lists_seq once the request was
 * handled (for one that never reached the decoder, the value the decoder last published): a snapshot whose
 * trunk_scan_lists_seq is behind it does not show this answer yet, and one past it shows a later change of the lists,
 * which a refusal no longer describes. A request dropped before the decoder saw it can carry lists older than the ones
 * the frontend showed when it asked, so a frontend keys its note on the later of the two. */
typedef struct {
    uint64_t sequence;
    uint32_t request;
    uint32_t session;
    int16_t list;
    uint8_t enabled;
    uint8_t pad;
    int32_t code;       /**< dsd_app_scan_list_set_code */
    uint32_t lists_seq; /**< dsd_state::trunk_scan_lists_seq after the request */
    uint32_t pad2;      /* explicit, so a copy carries no indeterminate padding */
} dsd_app_scan_list_result;

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

/** How many of the most recent export and decryption results stay readable through the *_results_since() getters. */
enum { DSD_APP_RESULT_HISTORY = 64 };

/** Copy, oldest first, up to @p cap retained export results whose sequence is above @p after_sequence; returns the
 * count. Only the last DSD_APP_RESULT_HISTORY are kept, so a reader that falls further behind sees a gap in the
 * sequence numbers. For a consumer that must see every result, not just the latest (the control API). */
int dsd_app_tg_export_results_since(uint64_t after_sequence, dsd_app_tg_export_result* out, int cap);
uint64_t dsd_app_command_session_generation(void);
int dsd_app_decryption_result_get(dsd_app_decryption_result* out);
/** dsd_app_tg_export_results_since() for decryption results. */
int dsd_app_decryption_results_since(uint64_t after_sequence, dsd_app_decryption_result* out, int cap);
int dsd_app_command_set_hytera_key(const dsd_app_hytera_key_payload* payload);
int dsd_app_command_set_aes_key(const dsd_app_aes_key_payload* payload);
int dsd_app_command_dsp_op(const dsd_app_dsp_payload* payload);
int dsd_app_command_apply_config(const dsdneoUserConfig* config);
int dsd_app_command_set_config_metadata(const dsd_app_config_metadata_payload* payload);
/** Submit DSD_APP_CMD_CONFIG_APPLY with the loaded config and the autosave it brings (dsd_app_config_load_payload). */
int dsd_app_command_load_config(const dsd_app_config_load_payload* payload);
/** Submit DSD_APP_CMD_TONE_FILTER_SET with @p mode and @p list (NULL for none) as typed. A list longer than the payload
 * holds is rejected here (DSD_APP_COMMAND_SUBMIT_REJECTED), never cut to a list nobody typed. */
int dsd_app_command_set_tone_filter(int32_t mode, const char* list);
/** Submit DSD_APP_CMD_TONE_FILTER_SET that sets @p mode and keeps the configured list (@c keep_list), read on the
 * decoder thread when the edit runs: the terminal's Off, which keeps the list the way --no-tone-filter keeps its own.
 * Allow and block are refused there when no list is configured. */
int dsd_app_command_set_tone_filter_mode(int32_t mode);
/** Submit DSD_APP_CMD_SCAN_ROW_EDIT. A payload whose strings are not NUL-terminated is rejected here. */
int dsd_app_command_scan_row_edit(const dsd_app_scan_row_edit_payload* payload);
/** Submit DSD_APP_CMD_SCAN_AVOID_REMOVE. A NULL payload or a @c target_id that is not NUL-terminated is rejected here;
 * the decoder checks the rest. */
int dsd_app_command_scan_avoid_remove(const dsd_app_scan_avoid_remove_payload* payload);
/** Submit DSD_APP_CMD_SCAN_LIST_SET. A NULL payload is rejected here, and so is any submission while the queue is
 * full: unlike other commands it never evicts the oldest, so a rejected request is never pending and gets no
 * retained result. The decoder checks the rest. */
int dsd_app_command_scan_list_set(const dsd_app_scan_list_set_payload* payload);
/** A fresh request id for dsd_app_scan_list_set_payload::request: process-wide, never 0, so the terminal, Qt and the
 * control API never take each other's results. Any thread. */
uint32_t dsd_app_scan_list_request_next(void);
/** Copy the newest retained scan-list result. Returns 1 if one exists, 0 otherwise (out is zeroed) or for NULL out. */
int dsd_app_scan_list_result_get(dsd_app_scan_list_result* out);
/** dsd_app_tg_export_results_since() for scan-list results: every DSD_APP_CMD_SCAN_LIST_SET the queue took gets one,
 * a refusal, an invalid payload, a Signal search refusal and a request dropped unapplied included. A result is
 * authoritative for what the request did and the toast it showed; dsd_state::trunk_scan_lists_seq in the snapshot is
 * authoritative for the lists to show. The result can arrive before the snapshot that shows its switch (a watchdog
 * tick can put that snapshot off), so a frontend clears its pending row on the result and, for OK (and UNCHANGED),
 * until the snapshot's trunk_scan_lists_seq reaches the result's @c lists_seq, shows the state the switch asked for;
 * for a refusal it shows the snapshot's lists, and once the snapshot moves past the result's @c lists_seq the refusal
 * no longer describes them. The code is the coordinator's answer even when a later step of the same command (a rigctl
 * passband follow-up the peer refused) fails the command: the list did switch. */
int dsd_app_scan_list_results_since(uint64_t after_sequence, dsd_app_scan_list_result* out, int cap);
/** dsd_app_scan_list_results_since() without the results of an earlier decoder run: only those published since the
 * command session last opened, so a reader that outlives the decoder, such as the control API, never serves a previous
 * run's answer as this one's. The DROPPED results of requests a stop discarded stay readable until the next open. */
int dsd_app_scan_list_session_results_since(uint64_t after_sequence, dsd_app_scan_list_result* out, int cap);
int dsd_app_command_set_rr_apply(const dsd_app_rr_apply_payload* payload);
int dsd_app_command_set_rr_account(const dsd_app_rr_account_payload* payload);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_COMMANDS_H_ */
