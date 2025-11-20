// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Core decoder options structure (dsd_opts).
 *
 * This header hosts the full dsd_opts definition so modules that need
 * configuration fields can include it directly instead of pulling in
 * the full dsd.h umbrella.
 */

#pragma once

#include <dsd-neo/core/opts_fwd.h>

#include <pulse/simple.h>
#include <sndfile.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

struct dsd_opts {
    // Pointers and wide-aligned members first (minimize padding)
    FILE* mbe_in_f;
    SNDFILE* audio_in_file;
    SF_INFO* audio_in_file_info;
    SNDFILE* audio_out_file;
    SF_INFO* audio_out_file_info;
    FILE* mbe_out_f;
    FILE* mbe_out_fR; //second slot on a TDMA system
    FILE* symbol_out_f;
    time_t symbol_out_file_creation_time; //time the symbol out file was created
    SNDFILE* wav_out_f;
    SNDFILE* wav_out_fR;
    SNDFILE* wav_out_raw;
    long int rtl_pwr;
    pa_simple* pulse_raw_dev_in;
    pa_simple* pulse_raw_dev_out;
    pa_simple* pulse_digi_dev_in;
    pa_simple* pulse_digi_dev_out;
    pa_simple* pulse_digi_dev_outR;
    FILE* symbolfile;
    void* udp_in_ctx;                  // opaque UDP input context
    unsigned long long udp_in_packets; // received datagrams
    unsigned long long udp_in_bytes;   // received bytes
    unsigned long long udp_in_drops;   // dropped samples due to ring overflow
    SNDFILE* tcp_file_in;

    // Scalars and smaller integers
    int onesymbol;
    int errorbars;
    int datascope;
    int constellation;      //ncurses ASCII constellation view (0=off, 1=on)
    float const_gate_qpsk;  //constellation magnitude gate for QPSK
    float const_gate_other; //constellation gate for non-QPSK (FSK)
    int symboltiming;
    int verbose;
    int p25enc;
    int p25lc;
    int p25status;
    int p25tg;
    int scoperate;
    int audio_in_fd;
    uint32_t rtlsdr_center_freq;
    int rtlsdr_ppm_error;
    int audio_in_type;
    int audio_out_fd;
    int audio_out_fdR;  //right channel audio for OSS hack
    int audio_out_type; // 0 for device, 1 for file,
    int split;
    int playoffset;
    int playoffsetR;
    float audio_gain;
    float audio_gainR;
    float audio_gainA;
    int audio_out;
    int dmr_stereo_wav;  //per-call wav file use (rename later)
    int static_wav_file; //single static wav file for decoding duration
    int serial_baud;
    int serial_fd;
    int resume;
    int frame_dstar;
    int frame_x2tdma;
    int frame_p25p1;
    int frame_p25p2;
    int inverted_p2;
    int p2counter;
    int frame_nxdn48;
    int frame_nxdn96;
    int frame_dmr;
    int frame_provoice;
    int mod_c4fm;
    int mod_qpsk;
    int mod_gfsk;
    /* When set by CLI (-mc/-mg/-mq/-m2), pin demod path and disable
       auto modulation switching/overrides. 0=auto (default), 1=locked. */
    int mod_cli_lock;
    int uvquality;
    int inverted_x2tdma;
    int inverted_dmr;
    int mod_threshold;
    int ssize;
    int msize;
    int playfiles;
    int m17encoder;
    int m17encoderbrt;
    int m17encoderpkt;
    int m17decoderip;
    int delay;
    int use_cosine_filter;
    int p25_c4fm_rrc_fixed; // 0: dynamic RRC(alpha≈0.2); 1: fixed RRC(alpha=0.5) for P25p1 C4FM
    int p25_p2_rrc_fixed;   // 0: dynamic RRC(alpha≈0.2); 1: fixed RRC(alpha=0.5) for P25p2 CQPSK
    int unmute_encrypted_p25;
    int rtl_dev_index;
    int rtl_gain_value;
    int rtl_squelch_level;
    int rtl_volume_multiplier;
    /* Generic input volume multiplier for non-RTL inputs (Pulse/WAV/TCP/UDP). */
    int input_volume_multiplier;
    int rtl_udp_port;
    /* Base DSP bandwidth for RTL path in kHz (4,6,8,12,16,24). Influences capture rate planning.
       Not the hardware tuner IF bandwidth. */
    int rtl_dsp_bw_khz;
    int rtl_bias_tee; /* 1 to enable RTL-SDR bias tee (if supported) */
    int rtl_started;
    /* Mark when RTL-SDR stream must be destroyed/recreated to apply changes
       that cannot be updated live (e.g., device index, bandwidth, manual gain). */
    int rtl_needs_restart;
    /* Spectrum-based RTL auto-PPM enable (0=off, 1=on). Mirrors DSD_NEO_AUTO_PPM. */
    int rtl_auto_ppm;
    /* Spectrum-based RTL auto-PPM SNR threshold in dB; <=0 means default. */
    float rtl_auto_ppm_snr_db;
    int monitor_input_audio;
    /* Warn when input level is below this dBFS threshold (e.g., -40). */
    double input_warn_db;
    /* Minimum seconds between repeated low-level warnings. */
    int input_warn_cooldown_sec;
    /* Last time a low-level input warning was emitted. */
    time_t last_input_warn_time;
    int analog_only;
    int pulse_raw_rate_in;
    int pulse_raw_rate_out;
    int pulse_digi_rate_in;
    int pulse_digi_rate_out;
    int pulse_raw_in_channels;
    int pulse_raw_out_channels;
    int pulse_digi_in_channels;
    int pulse_digi_out_channels;
    int pulse_flush;
    uint8_t use_ncurses_terminal;
    uint8_t ncurses_compact;
    uint8_t ncurses_history;
    uint8_t eye_view;                    //ncurses timing/eye diagram for C4FM/FSK (0=off)
    uint8_t fsk_hist_view;               //ncurses 4-level histogram for C4FM/FSK (0=off)
    uint8_t spectrum_view;               //ncurses spectrum analyzer for complex baseband (0=off)
    uint8_t eye_unicode;                 //use Unicode block glyphs in eye diagram (0=ASCII)
    uint8_t eye_color;                   //use colorized density in eye diagram (0=mono)
    uint8_t show_dsp_panel;              //show compact DSP status panel (0=hidden)
    uint8_t show_p25_metrics;            //show P25 Metrics section (0=hidden)
    uint8_t show_p25_neighbors;          //show P25 Neighbors (freq list) (0=hidden)
    uint8_t show_p25_iden_plan;          //show P25 IDEN Plan table (0=hidden)
    uint8_t show_p25_cc_candidates;      //show P25 CC Candidates (0=hidden)
    uint8_t show_channels;               //show Channels section (0=hidden)
    uint8_t show_p25_affiliations;       //show P25 Affiliations (RID list) (0=hidden)
    uint8_t show_p25_group_affiliations; //show P25 Group Affiliation (RID↔TG) (0=hidden)
    // P25 SM unified follower configuration (CLI-mirrored; env fallback retained)
    // Values <= 0 mean "unset" and will defer to environment or defaults.
    double p25_vc_grace_s;             // seconds after tune before eligible for VC->CC return
    double p25_min_follow_dwell_s;     // minimum seconds to dwell after first voice
    double p25_grant_voice_to_s;       // max seconds to wait from grant until voice before returning
    double p25_retune_backoff_s;       // seconds to block immediate retune to same VC after return
    double p25_force_release_extra_s;  // safety-net extra seconds beyond hangtime
    double p25_force_release_margin_s; // safety-net hard margin seconds beyond extra
    double p25_p1_err_hold_pct;        // P25p1 IMBE error average threshold (percent) to extend hang
    double p25_p1_err_hold_s;          // additional seconds to hold when threshold exceeded
    int reset_state;
    int payload;
    unsigned int dPMR_curr_frame_is_encrypted;
    int dPMR_next_part_of_superframe;
    int inverted_dpmr;
    int frame_dpmr;
    short int mbe_out;  //flag for mbe out, don't attempt fclose more than once
    short int mbe_outR; //flag for mbe out, don't attempt fclose more than once
    short int dmr_mono;
    short int dmr_stereo;
    short int lrrp_file_output;
    short int dmr_mute_encL;
    short int dmr_mute_encR;
    /* DMR: relax CRC gating by default (ignore final CRC when no irrecoverable errors).
       This improves continuity on RAS/marginal signals without affecting other protocols. */
    uint8_t dmr_crc_relaxed_default;
    int frame_ysf;
    int inverted_ysf;
    short int aggressive_framesync;
    int frame_m17;
    int inverted_m17;
    int call_alert;

    // rigctl / sockets / streaming
    int rigctl_sockfd;
    int use_rigctl;
    int rigctlportno;
    int udp_sockfd;  //digital
    int udp_sockfdA; //analog 48k1
    int udp_portno;
    int udp_in_sockfd; // bound UDP socket for input
    int udp_in_portno; // bind port (default 7355)
    int m17_use_ip;    //if enabled, open UDP and broadcast IP frame
    int m17_portno;    //default is 17000
    int m17_udp_sock;  //actual UDP socket for M17 to send to
    int tcp_sockfd;
    int tcp_portno;
    int rtltcp_enabled;  // 1 when using rtl_tcp backend
    int rtltcp_portno;   // default 1234
    int rtltcp_autotune; // 1 to enable rtl_tcp network auto-tuning (adaptive buffering)
    int wav_sample_rate;
    int wav_interpolator;
    int wav_decimator;
    int p25_trunk;        // legacy flag name used across protocols
    int trunk_enable;     // protocol-agnostic alias for trunking enable (kept in sync with p25_trunk)
    int p25_is_tuned;     //set to 1 if currently on VC, set back to 0 if on CC
    int trunk_is_tuned;   //protocol-agnostic alias (kept in sync with p25_is_tuned)
    float trunk_hangtime; //hangtime in seconds before tuning back to CC
    int scanner_mode;     //experimental -- use the channel map as a conventional scanner, quicker tuning, but no CC
    int setmod_bw;
    int slot_preference;
    int slot1_on;
    int slot2_on;
    int use_lpf;
    int use_hpf;
    int use_pbf;
    int use_hpf_d;
    int floating_point;
    int cqpsk_lms;    // 0 off, 1 on
    int cqpsk_mu_q15; // small step size (1..64)
    int cqpsk_stride; // update stride (1..32)

    // Small flags and bytes
    uint8_t const_norm_mode;         //0=radial (percentile) norm, 1=unit-circle norm
    uint8_t symbol_out_file_is_auto; //if the user hit the R key
    uint8_t reverse_mute;
    uint8_t dmr_dmrla_is_set; //flag to tell us dmrla is set by the user
    uint8_t dmr_dmrla_n;      //n value for dmrla
    uint8_t dmr_le;           //late entry
    uint8_t trunk_use_allow_list;
    uint8_t trunk_tune_group_calls;
    uint8_t trunk_tune_private_calls;
    uint8_t trunk_tune_data_calls;
    uint8_t trunk_tune_enc_calls;
    /* Flag set when any CLI explicitly enables or disables trunking (e.g., -T, -Y). */
    uint8_t trunk_cli_seen;
    uint8_t p25_lcw_retune;
    uint8_t p25_prefer_candidates;
    uint8_t use_dsp_output;
    uint8_t use_heuristics;
    uint8_t dmr_t3_heuristic_fill;

    // Strings and paths (large trailing arrays)
    char pa_input_idx[100];
    char pa_output_idx[100];
    char wav_out_dir[512];
    char mbe_in_file[1024];
    char audio_out_dev[1024];
    char mbe_out_dir[1024];
    char mbe_out_file[1024];
    char mbe_out_fileR[1024]; //second slot on a TDMA system
    char wav_out_file[1024];
    char wav_out_fileR[1024];
    char wav_out_file_raw[1024];
    char symbol_out_file[1024];
    char lrrp_out_file[1024];
    char event_out_file[1024];
    char szNumbers[1024]; //**tera 10/32/64 char str
    char serial_dev[1024];
    char output_name[1024];
    char rigctlhostname[1024];
    char udp_hostname[1024];
    char udp_in_bindaddr[1024];
    char m17_hostname[1024];
    char tcp_hostname[1024];
    char rtltcp_hostname[1024];
    char group_in_file[1024];
    char lcn_in_file[1024];
    char chan_in_file[1024];
    char key_in_file[1024];
    char audio_in_dev[2048]; //increase size for super long directory/file names
    char mbe_out_path[2048]; //1024
    char dsp_out_file[2048];
};
