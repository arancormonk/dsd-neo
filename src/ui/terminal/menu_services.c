// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/ui/menu_services.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

int
svc_toggle_all_mutes(dsd_opts* opts) {
    if (!opts) {
        return -1;
    }
    opts->unmute_encrypted_p25 = !opts->unmute_encrypted_p25;
    opts->dmr_mute_encL = !opts->dmr_mute_encL;
    opts->dmr_mute_encR = !opts->dmr_mute_encR;
    return 0;
}

int
svc_toggle_call_alert(dsd_opts* opts) {
    if (!opts) {
        return -1;
    }
    opts->call_alert = !opts->call_alert;
    return 0;
}

int
svc_enable_per_call_wav(dsd_opts* opts, dsd_state* state) {
    (void)state;
    if (!opts) {
        return -1;
    }
    char wav_file_directory[1024];
    snprintf(wav_file_directory, sizeof wav_file_directory, "%s", opts->wav_out_dir);
    struct stat st;
    if (stat(wav_file_directory, &st) == -1) {
        fprintf(stderr, "%s wav file directory does not exist\n", wav_file_directory);
        fprintf(stderr, "Creating directory %s to save decoded wav files\n", wav_file_directory);
        mkdir(wav_file_directory, 0700);
    }
    fprintf(stderr, "\n Per Call Wav File Enabled to Directory: %s;.\n", opts->wav_out_dir);
    srand((unsigned)time(NULL));
    opts->wav_out_f = open_wav_file(opts->wav_out_dir, opts->wav_out_file, 8000, 0);
    opts->wav_out_fR = open_wav_file(opts->wav_out_dir, opts->wav_out_fileR, 8000, 0);
    opts->dmr_stereo_wav = 1;
    return (opts->wav_out_f && opts->wav_out_fR) ? 0 : -1;
}

int
svc_open_symbol_out(dsd_opts* opts, dsd_state* state, const char* filename) {
    if (!opts || !state || !filename || !*filename) {
        return -1;
    }
    snprintf(opts->symbol_out_file, sizeof opts->symbol_out_file, "%s", filename);
    openSymbolOutFile(opts, state);
    return 0;
}

int
svc_open_symbol_in(dsd_opts* opts, dsd_state* state, const char* filename) {
    (void)state;
    if (!opts || !filename || !*filename) {
        return -1;
    }
    struct stat sb;
    if (stat(filename, &sb) != 0) {
        fprintf(stderr, "Error, couldn't open %s\n", filename);
        return -1;
    }
    if (S_ISREG(sb.st_mode)) {
        opts->symbolfile = fopen(filename, "r");
        if (!opts->symbolfile) {
            return -1;
        }
        snprintf(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", filename);
        opts->audio_in_type = 4; // symbol capture bin
        return 0;
    }
    return -1;
}

int
svc_replay_last_symbol(dsd_opts* opts, dsd_state* state) {
    (void)state;
    if (!opts) {
        return -1;
    }
    struct stat sb;
    if (stat(opts->audio_in_dev, &sb) != 0) {
        fprintf(stderr, "Error, couldn't open %s\n", opts->audio_in_dev);
        return -1;
    }
    if (S_ISREG(sb.st_mode)) {
        opts->symbolfile = fopen(opts->audio_in_dev, "r");
        if (!opts->symbolfile) {
            return -1;
        }
        opts->audio_in_type = 4; // symbol capture bin
        return 0;
    }
    return -1;
}

void
svc_stop_symbol_playback(dsd_opts* opts) {
    if (!opts) {
        return;
    }
    if (opts->symbolfile != NULL) {
        if (opts->audio_in_type == 4) {
            fclose(opts->symbolfile);
        }
        opts->symbolfile = NULL;
    }
    if (opts->audio_out_type == 0) {
        opts->audio_in_type = 0; // Pulse input
    } else {
        opts->audio_in_type = 5; // STDIN/raw
    }
}

void
svc_stop_symbol_saving(dsd_opts* opts, dsd_state* state) {
    if (!opts) {
        return;
    }
    if (opts->symbol_out_f) {
        closeSymbolOutFile(opts, state);
        snprintf(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", opts->symbol_out_file);
    }
}

int
svc_tcp_connect_audio(dsd_opts* opts, const char* host, int port) {
    if (!opts || !host || port <= 0) {
        return -1;
    }
    snprintf(opts->tcp_hostname, sizeof opts->tcp_hostname, "%s", host);
    opts->tcp_portno = port;
    opts->tcp_sockfd = Connect(opts->tcp_hostname, opts->tcp_portno);
    if (opts->tcp_sockfd == 0) {
        return -1;
    }
    // Setup libsndfile RAW stream on the socket
    opts->audio_in_type = 8;
    opts->audio_in_file_info = calloc(1, sizeof(SF_INFO));
    if (!opts->audio_in_file_info) {
        return -1;
    }
    opts->audio_in_file_info->samplerate = opts->wav_sample_rate;
    opts->audio_in_file_info->channels = 1;
    opts->audio_in_file_info->seekable = 0;
    opts->audio_in_file_info->format = SF_FORMAT_RAW | SF_FORMAT_PCM_16 | SF_ENDIAN_LITTLE;
    opts->tcp_file_in = sf_open_fd(opts->tcp_sockfd, SFM_READ, opts->audio_in_file_info, 0);
    if (opts->tcp_file_in == NULL) {
        fprintf(stderr, "Error, couldn't open TCP with libsndfile: %s\n", sf_strerror(NULL));
        if (opts->audio_out_type == 0) {
            snprintf(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "pulse");
            opts->audio_in_type = 0;
        } else {
            opts->audio_in_type = 5;
        }
        return -1;
    }
    return 0;
}

int
svc_rigctl_connect(dsd_opts* opts, const char* host, int port) {
    if (!opts || !host || port <= 0) {
        return -1;
    }
    snprintf(opts->rigctlhostname, sizeof opts->rigctlhostname, "%s", host);
    opts->rigctlportno = port;
    opts->rigctl_sockfd = Connect(opts->rigctlhostname, opts->rigctlportno);
    if (opts->rigctl_sockfd != 0) {
        opts->use_rigctl = 1;
        return 0;
    }
    opts->use_rigctl = 0;
    return -1;
}

int
svc_lrrp_set_home(dsd_opts* opts) {
    if (!opts) {
        return -1;
    }
    const char* home = getenv("HOME");
    if (!home || !*home) {
        return -1;
    }
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", home, "lrrp.txt");
    snprintf(opts->lrrp_out_file, sizeof opts->lrrp_out_file, "%s", path);
    opts->lrrp_file_output = 1;
    return 0;
}

int
svc_lrrp_set_dsdp(dsd_opts* opts) {
    if (!opts) {
        return -1;
    }
    snprintf(opts->lrrp_out_file, sizeof opts->lrrp_out_file, "%s", "DSDPlus.LRRP");
    opts->lrrp_file_output = 1;
    return 0;
}

int
svc_lrrp_set_custom(dsd_opts* opts, const char* filename) {
    if (!opts || !filename || !*filename) {
        return -1;
    }
    snprintf(opts->lrrp_out_file, sizeof opts->lrrp_out_file, "%s", filename);
    opts->lrrp_file_output = 1;
    return 0;
}

void
svc_lrrp_disable(dsd_opts* opts) {
    if (!opts) {
        return;
    }
    opts->lrrp_file_output = 0;
    opts->lrrp_out_file[0] = 0;
}

// ---- Decode mode presets ----

static void
svc_common_init(dsd_opts* opts, dsd_state* state, int sps, int center, const char* name, int out_rate, int out_ch) {
    state->samplesPerSymbol = sps;
    state->symbolCenter = center;
    snprintf(opts->output_name, sizeof opts->output_name, "%s", name);
    opts->dmr_mono = 0;
    state->dmr_stereo = 0;
    opts->pulse_digi_rate_out = out_rate;
    opts->pulse_digi_out_channels = out_ch;
    opts->mod_c4fm = 1;
    opts->mod_qpsk = 0;
    opts->mod_gfsk = 0;
    state->rf_mod = 0;
}

int
svc_mode_auto(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return -1;
    }
    // AUTO: P25p1, P25p2, DMR, YSF
    svc_common_init(opts, state, 10, 4, "AUTO", 8000, 2);
    if (opts->use_heuristics == 1) {
        initialize_p25_heuristics(&state->p25_heuristics);
        initialize_p25_heuristics(&state->inv_p25_heuristics);
    }
    opts->frame_dstar = 0;
    opts->frame_x2tdma = 0;
    opts->frame_p25p1 = 1;
    opts->frame_p25p2 = 1;
    opts->frame_nxdn48 = 0;
    opts->frame_nxdn96 = 0;
    opts->frame_dmr = 1;
    opts->frame_dpmr = 0;
    opts->frame_provoice = 0;
    opts->frame_ysf = 1;
    opts->frame_m17 = 0;
    opts->dmr_stereo = 1; // end-user option
    return 0;
}

int
svc_mode_tdma(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return -1;
    }
    // TDMA: P25p1, P25p2, DMR
    if (opts->use_heuristics == 1) {
        initialize_p25_heuristics(&state->p25_heuristics);
        initialize_p25_heuristics(&state->inv_p25_heuristics);
    }
    opts->frame_p25p1 = 1;
    opts->frame_p25p2 = 1;
    opts->frame_dmr = 1;
    svc_common_init(opts, state, 10, 4, "TDMA", 8000, 2);
    opts->frame_dstar = 0;
    opts->frame_x2tdma = 0;
    opts->frame_nxdn48 = 0;
    opts->frame_nxdn96 = 0;
    opts->frame_dpmr = 0;
    opts->frame_provoice = 0;
    opts->frame_ysf = 0;
    opts->frame_m17 = 0;
    opts->dmr_stereo = 1;
    return 0;
}

int
svc_mode_dstar(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return -1;
    }
    svc_common_init(opts, state, 10, 4, "DSTAR", 8000, 1);
    opts->frame_dstar = 1;
    opts->frame_x2tdma = 0;
    opts->frame_p25p1 = 0;
    opts->frame_p25p2 = 0;
    opts->frame_nxdn48 = 0;
    opts->frame_nxdn96 = 0;
    opts->frame_dmr = 0;
    opts->frame_dpmr = 0;
    opts->frame_provoice = 0;
    opts->frame_ysf = 0;
    opts->frame_m17 = 0;
    return 0;
}

int
svc_mode_m17(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return -1;
    }
    svc_common_init(opts, state, 10, 4, "M17", 8000, 1);
    opts->frame_dstar = 0;
    opts->frame_x2tdma = 0;
    opts->frame_p25p1 = 0;
    opts->frame_p25p2 = 0;
    opts->frame_nxdn48 = 0;
    opts->frame_nxdn96 = 0;
    opts->frame_dmr = 0;
    opts->frame_dpmr = 0;
    opts->frame_provoice = 0;
    opts->frame_ysf = 0;
    opts->frame_m17 = 1;
    return 0;
}

int
svc_mode_edacs(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return -1;
    }
    // EDACS/ProVoice (GFSK)
    state->samplesPerSymbol = 5;
    state->symbolCenter = 2;
    snprintf(opts->output_name, sizeof opts->output_name, "%s", "EDACS/PV");
    opts->dmr_mono = 0;
    opts->dmr_stereo = 0;
    state->dmr_stereo = 0;
    opts->pulse_digi_rate_out = 8000;
    opts->pulse_digi_out_channels = 1;
    opts->frame_dstar = 0;
    opts->frame_x2tdma = 0;
    opts->frame_p25p1 = 0;
    opts->frame_p25p2 = 0;
    opts->frame_nxdn48 = 0;
    opts->frame_nxdn96 = 0;
    opts->frame_dmr = 0;
    opts->frame_dpmr = 0;
    opts->frame_provoice = 1;
    opts->frame_ysf = 0;
    opts->frame_m17 = 0;
    opts->mod_c4fm = 0;
    opts->mod_qpsk = 0;
    opts->mod_gfsk = 1;
    state->rf_mod = 2;
    return 0;
}

int
svc_mode_p25p2(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return -1;
    }
    // P25 Phase 2 control or single voice frequency
    svc_common_init(opts, state, 10, 4, "P25p2", 8000, 1);
    opts->frame_dstar = 0;
    opts->frame_x2tdma = 0;
    opts->frame_p25p1 = 0;
    opts->frame_p25p2 = 1;
    opts->frame_nxdn48 = 0;
    opts->frame_nxdn96 = 0;
    opts->frame_dmr = 0;
    opts->frame_dpmr = 0;
    opts->frame_provoice = 0;
    opts->frame_ysf = 0;
    opts->frame_m17 = 0;
    opts->mod_c4fm = 1;
    opts->mod_qpsk = 0;
    opts->mod_gfsk = 0;
    state->rf_mod = 0;
    return 0;
}

int
svc_mode_dpmr(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return -1;
    }
    svc_common_init(opts, state, 10, 4, "dPMR", 8000, 1);
    opts->frame_dstar = 0;
    opts->frame_x2tdma = 0;
    opts->frame_p25p1 = 0;
    opts->frame_p25p2 = 0;
    opts->frame_nxdn48 = 0;
    opts->frame_nxdn96 = 0;
    opts->frame_dmr = 0;
    opts->frame_dpmr = 1;
    opts->frame_provoice = 0;
    opts->frame_ysf = 0;
    opts->frame_m17 = 0;
    return 0;
}

int
svc_mode_nxdn48(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return -1;
    }
    svc_common_init(opts, state, 20, 10, "NXDN48", 8000, 1);
    opts->frame_dstar = 0;
    opts->frame_x2tdma = 0;
    opts->frame_p25p1 = 0;
    opts->frame_p25p2 = 0;
    opts->frame_nxdn48 = 1;
    opts->frame_nxdn96 = 0;
    opts->frame_dmr = 0;
    opts->frame_dpmr = 0;
    opts->frame_provoice = 0;
    opts->frame_ysf = 0;
    opts->frame_m17 = 0;
    return 0;
}

int
svc_mode_nxdn96(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return -1;
    }
    svc_common_init(opts, state, 10, 5, "NXDN96", 8000, 1);
    opts->frame_dstar = 0;
    opts->frame_x2tdma = 0;
    opts->frame_p25p1 = 0;
    opts->frame_p25p2 = 0;
    opts->frame_nxdn48 = 0;
    opts->frame_nxdn96 = 1;
    opts->frame_dmr = 0;
    opts->frame_dpmr = 0;
    opts->frame_provoice = 0;
    opts->frame_ysf = 0;
    opts->frame_m17 = 0;
    return 0;
}

int
svc_mode_dmr(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return -1;
    }
    state->samplesPerSymbol = 10;
    state->symbolCenter = 4;
    snprintf(opts->output_name, sizeof opts->output_name, "%s", "DMR");
    opts->dmr_mono = 0;
    opts->dmr_stereo = 1;
    state->dmr_stereo = 0;
    opts->pulse_digi_rate_out = 8000;
    opts->pulse_digi_out_channels = 2;
    opts->frame_dstar = 0;
    opts->frame_x2tdma = 0;
    opts->frame_p25p1 = 0;
    opts->frame_nxdn48 = 0;
    opts->frame_nxdn96 = 0;
    opts->frame_dmr = 1;
    opts->frame_dpmr = 0;
    opts->frame_provoice = 0;
    opts->frame_ysf = 0;
    opts->frame_m17 = 0;
    opts->mod_c4fm = 1;
    opts->mod_qpsk = 0;
    opts->mod_gfsk = 0;
    state->rf_mod = 0;
    return 0;
}

int
svc_mode_ysf(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return -1;
    }
    svc_common_init(opts, state, 10, 4, "YSF", 8000, 1);
    opts->frame_dstar = 0;
    opts->frame_x2tdma = 0;
    opts->frame_p25p1 = 0;
    opts->frame_p25p2 = 0;
    opts->frame_nxdn48 = 0;
    opts->frame_nxdn96 = 0;
    opts->frame_dmr = 0;
    opts->frame_dpmr = 0;
    opts->frame_provoice = 0;
    opts->frame_ysf = 1;
    opts->frame_m17 = 0;
    return 0;
}

void
svc_toggle_inversion(dsd_opts* opts) {
    if (!opts) {
        return;
    }
    int inv = opts->inverted_dmr ? 0 : 1;
    opts->inverted_dmr = inv;
    opts->inverted_dpmr = inv;
    opts->inverted_x2tdma = inv;
    opts->inverted_ysf = inv;
    opts->inverted_m17 = inv;
}

void
svc_reset_event_history(dsd_state* state) {
    if (!state || !state->event_history_s) {
        return;
    }
    for (uint8_t i = 0; i < 2; i++) {
        init_event_history(&state->event_history_s[i], 0, 255);
    }
}

void
svc_toggle_payload(dsd_opts* opts) {
    if (!opts) {
        return;
    }
    opts->payload = !opts->payload;
    fprintf(stderr, opts->payload ? "Payload on\n" : "Payload Off\n");
}

void
svc_set_p2_params(dsd_state* state, unsigned long long wacn, unsigned long long sysid, unsigned long long cc) {
    if (!state) {
        return;
    }
    state->p2_wacn = (wacn > 0xFFFFF) ? 0xFFFFF : wacn;
    state->p2_sysid = (sysid > 0xFFF) ? 0xFFF : sysid;
    state->p2_cc = (cc > 0xFFF) ? 0xFFF : cc;
    state->p2_hardset = (state->p2_wacn != 0 && state->p2_sysid != 0 && state->p2_cc != 0) ? 1 : 0;
}
