// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/*
 * Copyright (C) 2010 DSD Author
 * GPG Key ID: 0x3F1D7FD0 (74EF 430D F7F2 0A48 FCE6  F630 FAA2 635D 3F1D 7FD0)
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#define _MAIN

#include <dsd-neo/core/dsd.h>
#include <dsd-neo/protocol/dmr/dmr_const.h>
#include <dsd-neo/protocol/dstar/dstar_const.h>
#include <dsd-neo/protocol/nxdn/nxdn_const.h>
#include <dsd-neo/protocol/p25/p25p1_const.h>
#include <dsd-neo/protocol/provoice/provoice_const.h>
#include <dsd-neo/protocol/x2tdma/x2tdma_const.h>
#include <dsd-neo/runtime/git_ver.h>

#include <limits.h>
#include <signal.h>
#include <stdlib.h>

#ifdef USE_RTLSDR
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/io/udp_input.h>
#include <rtl-sdr.h>
#endif

volatile uint8_t exitflag; //fix for issue #136
#ifdef USE_RTLSDR
struct RtlSdrContext* g_rtl_ctx = NULL; /* global orchestrator context */
#endif

// Local caches to avoid redundant device I/O in hot paths
static long int s_last_rigctl_freq = -1;
static int s_last_rigctl_bw = -12345;
#ifdef USE_RTLSDR
static uint32_t s_last_rtl_freq = 0;
#endif
static int s_last_ted_sps = 0;

// Forward declaration for env-triggered LCN calculator
static int run_t3_lcn_calc_from_csv(const char* path);

void
handler(int sgnl) {
    UNUSED(sgnl);

    exitflag = 1;
}

int
pretty_colors() {
    fprintf(stderr, "%sred\n", KRED);
    fprintf(stderr, "%sgreen\n", KGRN);
    fprintf(stderr, "%syellow\n", KYEL);
    fprintf(stderr, "%sblue\n", KBLU);
    fprintf(stderr, "%smagenta\n", KMAG);
    fprintf(stderr, "%scyan\n", KCYN);
    fprintf(stderr, "%swhite\n", KWHT);
    fprintf(stderr, "%snormal\n", KNRM);

    return 0;
}

#include <dsd-neo/io/pa_devs.h>
#include <dsd-neo/protocol/p25/p25p1_heuristics.h>

inline int
comp(const void* a, const void* b) {
    if (*((const int*)a) == *((const int*)b)) {
        return 0;
    } else if (*((const int*)a) < *((const int*)b)) {
        return -1;
    } else {
        return 1;
    }
}

// Small helpers to efficiently set fixed-width strings
static inline void
set_spaces(char* buf, size_t count) {
    memset(buf, ' ', count);
    buf[count] = '\0';
}

static inline void
set_underscores(char* buf, size_t count) {
    memset(buf, '_', count);
    buf[count] = '\0';
}

//struct for checking existence of directory to write to
struct stat st = {0};
char wav_file_directory[1024] = {0};
char dsp_filename[1024] = {0};
unsigned long long int p2vars = 0;

char* pEnd; //bugfix

int slotson = 3;

void
noCarrier(dsd_opts* opts, dsd_state* state) {
    const time_t now = time(NULL);

    //when no carrier sync, rotate the symbol out file every hour, if enabled
    if (opts->symbol_out_f && opts->symbol_out_file_is_auto == 1) {
        rotate_symbol_out_file(opts, state);
    }

    if (opts->floating_point == 1) {
        state->aout_gain = opts->audio_gain;
        state->aout_gainR = opts->audio_gain;
    }

    //clear heuristics from last carrier signal
    if (opts->frame_p25p1 == 1 && opts->use_heuristics == 1) {
        initialize_p25_heuristics(&state->p25_heuristics);
        initialize_p25_heuristics(&state->inv_p25_heuristics);
    }

//only do it here on the tweaks
#ifdef LIMAZULUTWEAKS
    state->nxdn_last_ran = -1;
    state->nxdn_last_rid = 0;
    state->nxdn_last_tg = 0;
#endif

    //experimental conventional frequency scanner mode
    if (opts->scanner_mode == 1 && ((now - state->last_cc_sync_time) > opts->trunk_hangtime)) {

        //always do these -- makes sense during scanning
        state->nxdn_last_ran = -1; //
        state->nxdn_last_rid = 0;
        state->nxdn_last_tg = 0;

        if (state->lcn_freq_roll >= state->lcn_freq_count) {
            state->lcn_freq_roll = 0; //reset to zero
        }
        //check that we have a non zero value first, then tune next frequency
        if (state->trunk_lcn_freq[state->lcn_freq_roll] != 0) {
            //rigctl
            if (opts->use_rigctl == 1) {
                if (opts->setmod_bw != 0 && opts->setmod_bw != s_last_rigctl_bw) {
                    SetModulation(opts->rigctl_sockfd, opts->setmod_bw);
                    s_last_rigctl_bw = opts->setmod_bw;
                }
                long int f = state->trunk_lcn_freq[state->lcn_freq_roll];
                if (f != s_last_rigctl_freq) {
                    SetFreq(opts->rigctl_sockfd, f);
                    s_last_rigctl_freq = f;
                }
            }
            //rtl
            if (opts->audio_in_type == 3) {
#ifdef USE_RTLSDR
                if (g_rtl_ctx) {
                    uint32_t rf = (uint32_t)state->trunk_lcn_freq[state->lcn_freq_roll];
                    if (rf != s_last_rtl_freq) {
                        rtl_stream_tune(g_rtl_ctx, rf);
                        s_last_rtl_freq = rf;
                    }
                }
#endif
            }
        }
        state->lcn_freq_roll++;
        state->last_cc_sync_time = now;
    }
    //end experimental conventional frequency scanner mode

    //tune back to last known CC when using trunking after x second hangtime
    if (opts->p25_trunk == 1 && opts->p25_is_tuned == 1 && ((now - state->last_cc_sync_time) > opts->trunk_hangtime)) {
        if (state->p25_cc_freq != 0) {

            //cap+ rest channel - redundant?
            if (state->dmr_rest_channel != -1) {
                if (state->trunk_chan_map[state->dmr_rest_channel] != 0) {
                    state->p25_cc_freq = state->trunk_chan_map[state->dmr_rest_channel];
                }
            }

            if (opts->use_rigctl == 1) //rigctl tuning
            {
                if (opts->setmod_bw != 0 && opts->setmod_bw != s_last_rigctl_bw) {
                    SetModulation(opts->rigctl_sockfd, opts->setmod_bw);
                    s_last_rigctl_bw = opts->setmod_bw;
                }
                if (state->p25_cc_freq != s_last_rigctl_freq) {
                    SetFreq(opts->rigctl_sockfd, state->p25_cc_freq);
                    s_last_rigctl_freq = state->p25_cc_freq;
                }
                state->dmr_rest_channel = -1; //maybe?
            }
            //rtl
            else if (opts->audio_in_type == 3) {
#ifdef USE_RTLSDR
                if (g_rtl_ctx) {
                    uint32_t rf = (uint32_t)state->p25_cc_freq;
                    if (rf != s_last_rtl_freq) {
                        rtl_stream_tune(g_rtl_ctx, rf);
                        s_last_rtl_freq = rf;
                    }
                }
                state->dmr_rest_channel = -1;
#endif
            }

            opts->p25_is_tuned = 0;
            state->edacs_tuned_lcn = -1;

            state->last_cc_sync_time = now;
            //test to switch back to 10/4 P1 QPSK for P25 FDMA CC

            //if P25p2 VCH and going back to P25p1 CC, flip symbolrate
            if (state->p25_cc_is_tdma == 0) //is set on signal from P25 TSBK or MAC_SIGNAL
            {
                state->samplesPerSymbol = 10;
                state->symbolCenter = 4;
#ifdef USE_RTLSDR
                if (s_last_ted_sps != 10) {
                    rtl_stream_set_ted_sps(10);
                    s_last_ted_sps = 10;
                }
#endif
                //re-enable both slots
                opts->slot1_on = 1;
                opts->slot2_on = 1;
            }
            //if P25p1 SNDCP channel (or revert) and going to a P25 TDMA CC
            else if (state->p25_cc_is_tdma == 1) {
                state->samplesPerSymbol = 8;
                state->symbolCenter = 3;
#ifdef USE_RTLSDR
                if (s_last_ted_sps != 8) {
                    rtl_stream_set_ted_sps(8);
                    s_last_ted_sps = 8;
                }
#endif
                //re-enable both slots (in case of late entry voice, MAC_SIGNAL can turn them back off)
                opts->slot1_on = 1;
                opts->slot2_on = 1;
            }
        }
        //zero out vc frequencies?
        state->p25_vc_freq[0] = 0;
        state->p25_vc_freq[1] = 0;

        memset(state->active_channel, 0, sizeof(state->active_channel));

        state->is_con_plus = 0; //flag off
    }

    state->dibit_buf_p = state->dibit_buf + 200;
    memset(state->dibit_buf, 0, sizeof(int) * 200);
    //dmr buffer
    state->dmr_payload_p = state->dibit_buf + 200;
    memset(state->dmr_payload_buf, 0, sizeof(int) * 200);
    memset(state->dmr_stereo_payload, 1, sizeof(int) * 144);
    //dmr buffer end

    //close MBE out files
    if (opts->mbe_out_f != NULL) {
        closeMbeOutFile(opts, state);
    }
    if (opts->mbe_out_fR != NULL) {
        closeMbeOutFileR(opts, state);
    }

    state->jitter = -1;
    state->lastsynctype = -1;
    state->carrier = 0;
    state->max = 15000;
    state->min = -15000;
    state->center = 0;
    state->err_str[0] = '\0';
    state->err_strR[0] = '\0';
    set_spaces(state->fsubtype, 14);
    set_spaces(state->ftype, 13);
    state->errs = 0;
    state->errs2 = 0;

    //zero out right away if not trunking
    if (opts->p25_trunk == 0) {
        state->lasttg = 0;
        state->lastsrc = 0;
        state->lasttgR = 0;
        state->lastsrcR = 0;
        state->gi[0] = -1;
        state->gi[1] = -1;

        //zero out vc frequencies?
        state->p25_vc_freq[0] = 0;
        state->p25_vc_freq[1] = 0;

        //only reset cap+ rest channel if not trunking
        state->dmr_rest_channel = -1;

        //DMR Color Code
        //  state->dmr_color_code = 16; //disabled

        //zero out nxdn site/srv/cch info if not trunking
        state->nxdn_location_site_code = 0;
        state->nxdn_location_sys_code = 0;
        set_spaces(state->nxdn_location_category, 1);

        //channel access information
        state->nxdn_rcn = 0;
        state->nxdn_base_freq = 0;
        state->nxdn_step = 0;
        state->nxdn_bw = 0;

        //dmr mfid branding and site parms
        state->dmr_branding_sub[0] = '\0';
        state->dmr_branding[0] = '\0';
        state->dmr_site_parms[0] = '\0';
    }

    //The new event history should not require this, but revert if other random issues suddenly come up
    //this was mainly for preventling numbers blipping out on signal fade, but also leaves stale values
    //on occassion when carrier drops and return to control channel, doesn't close wav files in that instance
    //  if (opts->p25_trunk == 1 && opts->p25_is_tuned == 1 && time(NULL) - state->last_cc_sync_time > opts->trunk_hangtime)
    {
        state->lasttg = 0;
        state->lastsrc = 0;
        state->lasttgR = 0;
        state->lastsrcR = 0;
        state->gi[0] = -1;
        state->gi[1] = -1;
        //  state->nxdn_last_ran = -1; //
        state->nxdn_last_rid = 0;
        state->nxdn_last_tg = 0;
    }

    state->lastp25type = 0;
    state->repeat = 0;
    state->nac = 0;
    state->numtdulc = 0;
    state->slot1light[0] = '\0';
    state->slot2light[0] = '\0';
    state->firstframe = 0;
    memset(state->aout_max_buf, 0, sizeof(float) * 200);
    state->aout_max_buf_p = state->aout_max_buf;
    state->aout_max_buf_idx = 0;

    memset(state->aout_max_bufR, 0, sizeof(float) * 200);
    state->aout_max_buf_pR = state->aout_max_bufR;
    state->aout_max_buf_idxR = 0;

    set_underscores(state->algid, 8);
    set_underscores(state->keyid, 16);
    mbe_initMbeParms(state->cur_mp, state->prev_mp, state->prev_mp_enhanced);
    mbe_initMbeParms(state->cur_mp2, state->prev_mp2, state->prev_mp_enhanced2);

    state->dmr_ms_mode = 0;

    //not sure if desirable here or not just yet, may need to disable a few of these
    state->payload_mi = 0;
    state->payload_miR = 0;
    state->payload_mfid = 0;
    state->payload_mfidR = 0;
    state->payload_algid = 0;
    state->payload_algidR = 0;
    state->payload_keyid = 0;
    state->payload_keyidR = 0;

    state->HYTL = 0;
    state->HYTR = 0;
    state->DMRvcL = 0;
    state->DMRvcR = 0;
    state->dropL = 256;
    state->dropR = 256;

    state->payload_miN = 0;
    state->p25vc = 0;
    state->payload_miP = 0;

    //ks array storage and counters
    memset(state->ks_octetL, 0, sizeof(state->ks_octetL));
    memset(state->ks_octetR, 0, sizeof(state->ks_octetR));
    memset(state->ks_bitstreamL, 0, sizeof(state->ks_bitstreamL));
    memset(state->ks_bitstreamR, 0, sizeof(state->ks_bitstreamR));
    state->octet_counter = 0;
    state->bit_counterL = 0;
    state->bit_counterR = 0;

    //xl specific, we need to know if the ESS is from HDU, or from LDU2
    state->xl_is_hdu = 0;

    //NXDN, when a new IV has arrived
    state->nxdn_new_iv = 0;

    //initialize dmr data header source
    state->dmr_lrrp_source[0] = 0;
    state->dmr_lrrp_source[1] = 0;
    state->dmr_lrrp_target[0] = 0;
    state->dmr_lrrp_target[1] = 0;

    //initialize data header bits
    state->data_header_blocks[0] = 1; //initialize with 1, otherwise we may end up segfaulting when no/bad data header
    state->data_header_blocks[1] = 1; //when trying to fill the superframe and 0-1 blocks give us an overflow
    state->data_header_padding[0] = 0;
    state->data_header_padding[1] = 0;
    state->data_header_format[0] = 7;
    state->data_header_format[1] = 7;
    state->data_header_sap[0] = 0;
    state->data_header_sap[1] = 0;
    state->data_block_counter[0] = 1;
    state->data_block_counter[1] = 1;
    state->data_p_head[0] = 0;
    state->data_p_head[1] = 0;
    state->data_block_poc[0] = 0;
    state->data_block_poc[1] = 0;
    state->data_byte_ctr[0] = 0;
    state->data_byte_ctr[1] = 0;
    state->data_ks_start[0] = 0;
    state->data_ks_start[1] = 0;

    state->dmr_encL = 0;
    state->dmr_encR = 0;

    state->dmrburstL = 17;
    state->dmrburstR = 17;

    //reset P2 ESS_B fragments and 4V counter
    for (short i = 0; i < 4; i++) {
        state->ess_b[0][i] = 0;
        state->ess_b[1][i] = 0;
    }
    state->fourv_counter[0] = 0;
    state->fourv_counter[1] = 0;
    state->voice_counter[0] = 0;
    state->voice_counter[1] = 0;

    //values displayed in ncurses terminal
    // state->p25_vc_freq[0] = 0;
    // state->p25_vc_freq[1] = 0;

    //new nxdn stuff
    state->nxdn_part_of_frame = 0;
    state->nxdn_ran = 0;
    state->nxdn_sf = 0;
    memset(state->nxdn_sacch_frame_segcrc, 1, sizeof(state->nxdn_sacch_frame_segcrc)); //init on 1, bad CRC all
    state->nxdn_sacch_non_superframe = TRUE;
    memset(state->nxdn_sacch_frame_segment, 1, sizeof(state->nxdn_sacch_frame_segment));
    state->nxdn_alias_block_number = 0;
    memset(state->nxdn_alias_block_segment, 0, sizeof(state->nxdn_alias_block_segment));
    state->nxdn_call_type[0] = '\0';

    //unload keys when using keylaoder
    if (state->keyloader == 1) {
        state->R = 0;  //NXDN, or RC4 (slot 1)
        state->RR = 0; //RC4 (slot 2)
        state->K = 0;  //BP
        state->K1 = 0; //tera 10/32/64 char BP
        state->K2 = 0;
        state->K3 = 0;
        state->K4 = 0;
        memset(state->A1, 0, sizeof(state->A1));
        memset(state->A2, 0, sizeof(state->A2));
        memset(state->A3, 0, sizeof(state->A3));
        memset(state->A4, 0, sizeof(state->A4));
        memset(state->aes_key_loaded, 0, sizeof(state->aes_key_loaded));
        state->H = 0; //shim for above
    }

    //forcing key application will re-enable this at the time of voice tx
    state->nxdn_cipher_type = 0;

    //dmr overaching manufacturer in use for a particular system or radio
    // state->dmr_mfid = -1;

    //dmr slco stuff
    memset(state->dmr_cach_fragment, 1, sizeof(state->dmr_cach_fragment));
    state->dmr_cach_counter = 0;

    //initialize unified dmr pdu 'superframe'
    memset(state->dmr_pdu_sf, 0, sizeof(state->dmr_pdu_sf));
    memset(state->data_header_valid, 0, sizeof(state->data_header_valid));

    //initialize cap+ bits and block num storage
    memset(state->cap_plus_csbk_bits, 0, sizeof(state->cap_plus_csbk_bits));
    memset(state->cap_plus_block_num, 0, sizeof(state->cap_plus_block_num));

    //init confirmed data individual block crc as invalid
    memset(state->data_block_crc_valid, 0, sizeof(state->data_block_crc_valid));

    //embedded signalling
    memset(state->dmr_embedded_signalling, 0, sizeof(state->dmr_embedded_signalling));

    //late entry mi fragments
    memset(state->late_entry_mi_fragment, 0, sizeof(state->late_entry_mi_fragment));

    //dmr talker alias new/fixed stuff
    memset(state->dmr_alias_format, 0, sizeof(state->dmr_alias_format));
    memset(state->dmr_alias_block_len, 0, sizeof(state->dmr_alias_block_len));
    memset(state->dmr_alias_char_size, 0, sizeof(state->dmr_alias_char_size));
    memset(state->dmr_alias_block_segment, 0, sizeof(state->dmr_alias_block_segment));
    memset(state->dmr_embedded_gps, 0, sizeof(state->dmr_embedded_gps));
    memset(state->dmr_lrrp_gps, 0, sizeof(state->dmr_lrrp_gps));

    //Generic Talker Alias String
    memset(state->generic_talker_alias, 0, sizeof(state->generic_talker_alias));
    state->generic_talker_alias_src[0] = 0;
    state->generic_talker_alias_src[1] = 0;

    /* Initialize P25 metrics counters used by ncurses BER display */
    state->p25_p1_fec_ok = 0;
    state->p25_p1_fec_err = 0;
    state->p25_p2_rs_facch_ok = 0;
    state->p25_p2_rs_facch_err = 0;
    state->p25_p2_rs_facch_corr = 0;
    state->p25_p2_rs_sacch_ok = 0;
    state->p25_p2_rs_sacch_err = 0;
    state->p25_p2_rs_sacch_corr = 0;
    state->p25_p2_rs_ess_ok = 0;
    state->p25_p2_rs_ess_err = 0;
    state->p25_p2_rs_ess_corr = 0;

    // Initialize P25 SM candidate cache bookkeeping
    state->p25_cc_cand_count = 0;
    state->p25_cc_cand_idx = 0;
    state->p25_cc_cache_loaded = 0;

    // memset(state->active_channel, 0, sizeof(state->active_channel));

    //REMUS! multi-purpose call_string
    set_spaces(state->call_string[0], 21);
    set_spaces(state->call_string[1], 21);

    if (now - state->last_cc_sync_time > 10) //ten seconds of no carrier
    {
        state->dmr_rest_channel = -1;
        state->p25_vc_freq[0] = 0;
        state->p25_vc_freq[1] = 0;
        state->dmr_mfid = -1;
        state->dmr_branding_sub[0] = '\0';
        state->dmr_branding[0] = '\0';
        state->dmr_site_parms[0] = '\0';
        opts->p25_is_tuned = 0;
        memset(state->active_channel, 0, sizeof(state->active_channel));
    }

    opts->dPMR_next_part_of_superframe = 0;

    state->dPMRVoiceFS2Frame.CalledIDOk = 0;
    state->dPMRVoiceFS2Frame.CallingIDOk = 0;
    memset(state->dPMRVoiceFS2Frame.CalledID, 0, 8);
    memset(state->dPMRVoiceFS2Frame.CallingID, 0, 8);
    memset(state->dPMRVoiceFS2Frame.Version, 0, 8);

    set_spaces(state->dpmr_caller_id, 6);
    set_spaces(state->dpmr_target_id, 6);

    //YSF Fusion Call Strings
    set_spaces(state->ysf_tgt, 10);
    set_spaces(state->ysf_src, 10);
    set_spaces(state->ysf_upl, 10);
    set_spaces(state->ysf_dnl, 10);
    set_spaces(state->ysf_rm1, 5);
    set_spaces(state->ysf_rm2, 5);
    set_spaces(state->ysf_rm3, 5);
    set_spaces(state->ysf_rm4, 5);
    memset(state->ysf_txt, 0, sizeof(state->ysf_txt));
    state->ysf_dt = 9;
    state->ysf_fi = 9;
    state->ysf_cm = 9;

    //DSTAR Call Strings
    set_spaces(state->dstar_rpt1, 8);
    set_spaces(state->dstar_rpt2, 8);
    set_spaces(state->dstar_dst, 8);
    set_spaces(state->dstar_src, 8);
    set_spaces(state->dstar_txt, 8);
    set_spaces(state->dstar_gps, 8);

    //M17 Storage
    memset(state->m17_lsf, 0, sizeof(state->m17_lsf));
    memset(state->m17_pkt, 0, sizeof(state->m17_pkt));
    state->m17_pbc_ct = 0;
    state->m17_str_dt = 9;

    state->m17_dst = 0;
    state->m17_src = 0;
    state->m17_can = 0;
    memset(state->m17_dst_csd, 0, sizeof(state->m17_dst_csd));
    memset(state->m17_src_csd, 0, sizeof(state->m17_src_csd));
    state->m17_dst_str[0] = '\0';
    state->m17_src_str[0] = '\0';

    state->m17_enc = 0;
    state->m17_enc_st = 0;
    memset(state->m17_meta, 0, sizeof(state->m17_meta));

    //misc str storage
    //  sprintf (state->str50a, "%s", "");
    // memset (state->str50b, 0, 50*sizeof(char));
    // memset (state->str50c, 0, 50*sizeof(char));
    // memset (state->m17sms, 0, 800*sizeof(char));
    // sprintf (state->m17dat, "%s", "");

    //set float temp buffer to baseline
    memset(state->audio_out_temp_buf, 0.0f, sizeof(state->audio_out_temp_buf));
    memset(state->audio_out_temp_bufR, 0.0f, sizeof(state->audio_out_temp_bufR));

    //set float temp buffer to baseline
    memset(state->f_l, 0.0f, sizeof(state->f_l));
    memset(state->f_r, 0.0f, sizeof(state->f_r));

    //set float temp buffer to baseline
    memset(state->f_l4, 0.0f, sizeof(state->f_l4));
    memset(state->f_r4, 0.0f, sizeof(state->f_r4));

    //zero out the short sample storage buffers
    memset(state->s_l, 0, sizeof(state->s_l));
    memset(state->s_r, 0, sizeof(state->s_r));
    memset(state->s_l4, 0, sizeof(state->s_l4));
    memset(state->s_r4, 0, sizeof(state->s_r4));

    memset(state->s_lu, 0, sizeof(state->s_lu));
    memset(state->s_ru, 0, sizeof(state->s_ru));
    memset(state->s_l4u, 0, sizeof(state->s_l4u));
    memset(state->s_r4u, 0, sizeof(state->s_r4u));

    //we do reset the counter, but not the static_ks_bits
    memset(state->static_ks_counter, 0, sizeof(state->static_ks_counter));

} //nocarrier

void
initOpts(dsd_opts* opts) {
    opts->floating_point = 0; //use floating point audio output
    opts->onesymbol = 10;
    opts->mbe_in_file[0] = 0;
    opts->mbe_in_f = NULL;
    opts->errorbars = 1;
    opts->datascope = 0;
    opts->constellation = 0;
    opts->const_gate_qpsk = 0.25f;
    opts->const_gate_other = 0.05f;
    opts->const_norm_mode = 0; // default: radial percentile normalization
    opts->eye_view = 0;
    opts->fsk_hist_view = 0;
    opts->eye_unicode = 1;    //default On for clearer rendering
    opts->eye_color = 1;      //default On when terminal supports color
    opts->show_dsp_panel = 0; // hide compact DSP panel by default
    opts->symboltiming = 0;
    opts->verbose = 2;
    opts->p25enc = 0;
    opts->p25lc = 0;
    opts->p25status = 0;
    opts->p25tg = 0;
    opts->scoperate = 15;
#ifdef __CYGWIN__
    sprintf(opts->audio_in_dev, "/dev/dsp");
    sprintf(opts->audio_out_dev, "/dev/dsp");
#else
    sprintf(opts->audio_in_dev, "pulse");
    sprintf(opts->audio_out_dev, "pulse");
#endif
    opts->audio_in_fd = -1;
    opts->audio_out_fd = -1;
    opts->audio_out_fdR = -1;

    opts->split = 0;
    opts->playoffset = 0;
    opts->playoffsetR = 0;
    sprintf(opts->wav_out_dir, "%s", "./WAV");
    opts->mbe_out_dir[0] = 0;
    opts->mbe_out_file[0] = 0;
    opts->mbe_out_fileR[0] = 0; //second slot on a TDMA system
    opts->mbe_out_path[0] = 0;
    opts->mbe_out_f = NULL;
    opts->mbe_out_fR = NULL; //second slot on a TDMA system
    opts->audio_gain = 0;
    opts->audio_gainR = 0;
    opts->audio_gainA = 50.0f; //scale of 1 - 100
    opts->audio_out = 1;
    opts->wav_out_file[0] = 0;
    opts->wav_out_fileR[0] = 0;
    opts->wav_out_file_raw[0] = 0;
    opts->symbol_out_file[0] = 0;
    opts->lrrp_out_file[0] = 0;
    opts->event_out_file[0] = 0;
    //csv import filenames
    opts->group_in_file[0] = 0;
    opts->lcn_in_file[0] = 0;
    opts->chan_in_file[0] = 0;
    opts->key_in_file[0] = 0;
    //end import filenames
    opts->szNumbers[0] = 0;
    opts->symbol_out_f = NULL;
    opts->symbol_out_file_creation_time = time(NULL);
    opts->symbol_out_file_is_auto = 0;
    opts->mbe_out = 0;
    opts->mbe_outR = 0; //second slot on a TDMA system
    opts->wav_out_f = NULL;
    opts->wav_out_fR = NULL;
    opts->wav_out_raw = NULL;

    opts->dmr_stereo_wav = 0;  //flag for per call dmr stereo wav recordings
    opts->static_wav_file = 0; //single static wav file for decoding duration
    //opts->wav_out_fd = -1;
    opts->serial_baud = 115200;
    sprintf(opts->serial_dev, "/dev/ttyUSB0");
    opts->resume = 0;
    opts->frame_dstar = 1;
    opts->frame_x2tdma = 1;
    opts->frame_p25p1 = 1;
    opts->frame_p25p2 = 1;
    opts->frame_nxdn48 = 0;
    opts->frame_nxdn96 = 0;
    opts->frame_dmr = 1;
    opts->frame_dpmr = 0;
    opts->frame_provoice = 0;
    opts->frame_ysf = 1;
    opts->frame_m17 = 0;
    opts->mod_c4fm = 1;
    opts->mod_qpsk = 0;
    opts->mod_gfsk = 0;
    opts->uvquality = 3;
    opts->inverted_x2tdma = 1; // most transmitter + scanner + sound card combinations show inverted signals for this
    opts->inverted_dmr = 0; // most transmitter + scanner + sound card combinations show non-inverted signals for this
    opts->inverted_m17 = 0; //samples from M17_Education seem to all be positive polarity (same from m17-tools programs)
    opts->mod_threshold = 26;
    opts->ssize = 128;  //36 default, max is 128, much cleaner data decodes on Phase 2 cqpsk at max
    opts->msize = 1024; //15 default, max is 1024, much cleaner data decodes on Phase 2 cqpsk at max
    opts->playfiles = 0;
    opts->m17encoder = 0;
    opts->m17encoderbrt = 0;
    opts->m17encoderpkt = 0;
    opts->m17decoderip = 0;
    opts->delay = 0;
    opts->use_cosine_filter = 1;
    opts->unmute_encrypted_p25 = 0;
    //all RTL user options -- enabled AGC by default due to weak signal related issues
    opts->rtl_dev_index = 0;  //choose which device we want by index number
    opts->rtl_gain_value = 0; //mid value, 0 - AGC - 0 to 49 acceptable values
    opts->rtl_squelch_level = dB_to_pwr(-110);
    opts->rtl_volume_multiplier =
        2; //sample multiplier; This multiplies the sample value to produce a higher 'inlvl' for the demodulator
    opts->rtl_udp_port =
        0; //set UDP port for RTL remote -- 0 by default, will be making this optional for some external/legacy use cases (edacs-fm, etc)
    opts->rtl_bandwidth = 12;   //default is 12, reverted back to normal on this (no inherent benefit)
    opts->rtlsdr_ppm_error = 0; //initialize ppm with 0 value;
    opts->rtlsdr_center_freq =
        850000000; //set to an initial value (if user is using a channel map, then they won't need to specify anything other than -i rtl if desired)
    opts->rtl_started = 0;
    opts->rtl_pwr = 0;      // mean power approximation level on rtl input signal
    opts->rtl_bias_tee = 0; // bias tee disabled by default
    //end RTL user options
    opts->pulse_raw_rate_in = 48000;
    opts->pulse_raw_rate_out = 48000; //
    opts->pulse_digi_rate_in = 48000;
    opts->pulse_digi_rate_out = 8000; //
    opts->pulse_raw_in_channels = 1;
    opts->pulse_raw_out_channels = 1;
    opts->pulse_digi_in_channels = 1;  //2
    opts->pulse_digi_out_channels = 2; //new default for AUTO
    memset(opts->pa_input_idx, 0, 100 * sizeof(char));
    memset(opts->pa_output_idx, 0, 100 * sizeof(char));

    opts->wav_sample_rate = 48000; //default value (DSDPlus uses 96000 on raw signal wav files)
    opts->wav_interpolator = 1;    //default factor of 1 on 48000; 2 on 96000; sample rate / decimator
    opts->wav_decimator = 48000;   //maybe for future use?

    sprintf(opts->output_name, "AUTO");
    opts->pulse_flush = 1; //set 0 to flush, 1 for flushed
    opts->use_ncurses_terminal = 0;
    opts->ncurses_compact = 0;
    opts->ncurses_history = 1;
#ifdef LIMAZULUTWEAKS
    opts->ncurses_compact = 1;
#endif
    opts->payload = 0;
    opts->inverted_dpmr = 0;
    opts->dmr_mono = 0;
    opts->dmr_stereo = 1;
    opts->aggressive_framesync = 1;

//this may not matter so much, since its already checked later on
//but better safe than sorry I guess
#ifdef __CYGWIN__
    opts->audio_in_type = 9;  //only assign when configured
    opts->audio_out_type = 9; //only assign when configured
#else
    opts->audio_in_type = 0;
    opts->audio_out_type = 0;
#endif

    opts->lrrp_file_output = 0;

    opts->dmr_mute_encL = 1;
    opts->dmr_mute_encR = 1;

    opts->monitor_input_audio = 0; //enable with -8
    opts->analog_only = 0;         //only turned on with -fA

    opts->inverted_p2 = 0;
    opts->p2counter = 0;

    opts->call_alert = 0; //call alert beeper for ncurses

    //rigctl options
    opts->use_rigctl = 0;
    opts->rigctl_sockfd = 0;
    opts->rigctlportno = 4532; //TCP Port Number; GQRX - 7356; SDR++ - 4532
    sprintf(opts->rigctlhostname, "%s", "localhost");

    //UDP Socket Blaster Audio
    opts->udp_sockfd = 0;
    opts->udp_sockfdA = 0;
    opts->udp_portno = 23456; //default port, same os OP25's sockaudio.py
    sprintf(opts->udp_hostname, "%s", "127.0.0.1");

    //M17 UDP Port and hostname
    opts->m17_use_ip = 0;     //if enabled, open UDP and broadcast IP frame
    opts->m17_portno = 17000; //default is 17000
    opts->m17_udp_sock = 0;   //actual UDP socket for M17 to send to
    sprintf(opts->m17_hostname, "%s", "127.0.0.1");

    //tcp input options
    opts->tcp_sockfd = 0;
    opts->tcp_portno = 7355; //default favored by SDR++
    sprintf(opts->tcp_hostname, "%s", "localhost");

    // rtl_tcp defaults
    opts->rtltcp_enabled = 0;
    opts->rtltcp_portno = 1234;
    sprintf(opts->rtltcp_hostname, "%s", "127.0.0.1");

    // UDP direct input defaults
    opts->udp_in_sockfd = 0;
    opts->udp_in_portno = 7355;
    opts->udp_in_bindaddr[0] = '\0';
    opts->udp_in_ctx = NULL;
    opts->udp_in_packets = 0ULL;
    opts->udp_in_bytes = 0ULL;
    opts->udp_in_drops = 0ULL;

    opts->p25_trunk = 0;    //0 disabled, 1 is enabled
    opts->p25_is_tuned = 0; //set to 1 if currently on VC, set back to 0 on carrier drop
    opts->trunk_hangtime =
        1; //1 second hangtime by default before tuning back to CC, going sub 1 sec causes issues with cc slip

    opts->scanner_mode = 0; //0 disabled, 1 is enabled

    //reverse mute
    opts->reverse_mute = 0;

    //setmod bandwidth
    opts->setmod_bw = 0; //default to 0 - off

    //DMR Location Area - DMRLA B***S***
    opts->dmr_dmrla_is_set = 0;
    opts->dmr_dmrla_n = 0;

    //DMR Late Entry
    opts->dmr_le = 1; //re-enabled again

    //Trunking - Use Group List as Allow List
    opts->trunk_use_allow_list = 0; //disabled by default

    //Trunking - Tune Group Calls
    opts->trunk_tune_group_calls = 1; //enabled by default

    //Trunking - Tune Private Calls
    opts->trunk_tune_private_calls = 1; //enabled by default

    //Trunking - Tune Data Calls
    opts->trunk_tune_data_calls = 0; //disabled by default

    //Trunking - Tune Encrypted Calls (P25 only on applicable grants with svc opts)
    opts->trunk_tune_enc_calls = 1; //enabled by default

    //P25 LCW explicit retune (format 0x44)
    opts->p25_lcw_retune = 0; //disabled by default

    opts->dPMR_next_part_of_superframe = 0;

//OSS audio - Slot Preference
//slot preference is used during OSS audio playback to
//prefer one tdma voice slot over another when both are playing back
//this is a fix to OSS 48k/1 output
#ifdef __CYGWIN__
    opts->slot_preference = 0; //default prefer slot 1 -- state->currentslot = 0;
#else
    opts->slot_preference = 2; //use 2 since integrating the Stereo Channel Patch;
#endif
    //hardset slots to synthesize
    opts->slot1_on = 1;
    opts->slot2_on = 1;

    //enable filter options
    opts->use_lpf = 0;
    opts->use_hpf = 1;
    opts->use_pbf = 1;
    opts->use_hpf_d = 1;

//this is a quick bugfix for issues with OSS and TDMA slot 1/2 audio level mismatch
#ifdef __CYGWIN__
    opts->use_hpf_d = 0;
#endif

    //dsp structured file
    opts->dsp_out_file[0] = 0;
    opts->use_dsp_output = 0;

    //Use P25p1 heuristics
    opts->use_heuristics = 0;

    //DMR TIII heuristic LCN fill (opt-in)
    opts->dmr_t3_heuristic_fill = 0;
    {
        const char* env = getenv("DSD_NEO_DMR_T3_HEUR");
        if (env && (env[0] == '1' || env[0] == 't' || env[0] == 'T' || env[0] == 'y' || env[0] == 'Y')) {
            opts->dmr_t3_heuristic_fill = 1;
            fprintf(stderr, "DMR TIII: Heuristic LCN fill enabled via DSD_NEO_DMR_T3_HEUR.\n");
        }
    }

} //initopts

static void*
aligned_alloc_64(size_t size) {
    void* p = NULL;
#if defined(_ISOC11_SOURCE)
    // aligned_alloc requires size to be multiple of alignment; we pass 64 and large sizes so OK
    p = aligned_alloc(64, (size + 63) & ~((size_t)63));
    if (!p) {
        return malloc(size);
    }
    return p;
#else
    if (posix_memalign(&p, 64, size) != 0 || !p) {
        return malloc(size);
    }
    return p;
#endif
}

void
initState(dsd_state* state) {

    int i, j;
    // state->testcounter = 0;
    state->last_dibit = 0;
    state->dibit_buf = aligned_alloc_64(sizeof(int) * 1000000);
    state->dibit_buf_p = state->dibit_buf + 200;
    memset(state->dibit_buf, 0, sizeof(int) * 200);
    //dmr buffer -- double check this set up
    state->dmr_payload_buf = aligned_alloc_64(sizeof(int) * 1000000);
    state->dmr_payload_p = state->dmr_payload_buf + 200;
    memset(state->dmr_payload_buf, 0, sizeof(int) * 200);
    memset(state->dmr_stereo_payload, 1, sizeof(int) * 144);
    //dmr buffer end
    state->repeat = 0;

    //Bitmap Filtering Options
    state->audio_smoothing = 0;

    memset(state->audio_out_temp_buf, 0.0f, sizeof(state->audio_out_temp_buf));
    memset(state->audio_out_temp_bufR, 0.0f, sizeof(state->audio_out_temp_bufR));

    //set float temp buffer to baseline
    memset(state->f_l, 0.0f, sizeof(state->f_l));
    memset(state->f_r, 0.0f, sizeof(state->f_r));

    //set float temp buffer to baseline
    memset(state->f_l4, 0.0f, sizeof(state->f_l4));
    memset(state->f_r4, 0.0f, sizeof(state->f_r4));

    //zero out the short sample storage buffers
    memset(state->s_l, 0, sizeof(state->s_l));
    memset(state->s_r, 0, sizeof(state->s_r));
    memset(state->s_l4, 0, sizeof(state->s_l4));
    memset(state->s_r4, 0, sizeof(state->s_r4));

    memset(state->s_lu, 0, sizeof(state->s_lu));
    memset(state->s_ru, 0, sizeof(state->s_ru));
    memset(state->s_l4u, 0, sizeof(state->s_l4u));
    memset(state->s_r4u, 0, sizeof(state->s_r4u));

    state->audio_out_buf = aligned_alloc_64(sizeof(short) * 1000000);
    state->audio_out_bufR = aligned_alloc_64(sizeof(short) * 1000000);
    memset(state->audio_out_buf, 0, 100 * sizeof(short));
    memset(state->audio_out_bufR, 0, 100 * sizeof(short));
    //analog/raw signal audio buffers
    state->analog_sample_counter = 0; //when it reaches 960, then dump the raw/analog audio signal and reset
    memset(state->analog_out, 0, sizeof(state->analog_out));
    //
    state->audio_out_buf_p = state->audio_out_buf + 100;
    state->audio_out_buf_pR = state->audio_out_bufR + 100;
    state->audio_out_float_buf = aligned_alloc_64(sizeof(float) * 1000000);
    state->audio_out_float_bufR = aligned_alloc_64(sizeof(float) * 1000000);
    memset(state->audio_out_float_buf, 0, 100 * sizeof(float));
    memset(state->audio_out_float_bufR, 0, 100 * sizeof(float));
    state->audio_out_float_buf_p = state->audio_out_float_buf + 100;
    state->audio_out_float_buf_pR = state->audio_out_float_bufR + 100;
    state->audio_out_idx = 0;
    state->audio_out_idx2 = 0;
    state->audio_out_idxR = 0;
    state->audio_out_idx2R = 0;
    state->audio_out_temp_buf_p = state->audio_out_temp_buf;
    state->audio_out_temp_buf_pR = state->audio_out_temp_bufR;
    //state->wav_out_bytes = 0;
    state->center = 0;
    state->jitter = -1;
    state->synctype = -1;
    state->min = -15000;
    state->max = 15000;
    state->lmid = 0;
    state->umid = 0;
    state->minref = -12000;
    state->maxref = 12000;
    state->lastsample = 0;
    for (i = 0; i < 128; i++) {
        state->sbuf[i] = 0;
    }
    state->sidx = 0;
    for (i = 0; i < 1024; i++) {
        state->maxbuf[i] = 15000;
    }
    for (i = 0; i < 1024; i++) {
        state->minbuf[i] = -15000;
    }
    state->midx = 0;
    state->err_str[0] = '\0';
    state->err_strR[0] = '\0';
    set_spaces(state->fsubtype, 14);
    set_spaces(state->ftype, 13);
    state->symbolcnt = 0;
    state->symbolc = 0; //
    state->rf_mod = 0;
    state->numflips = 0;
    state->lastsynctype = -1;
    state->lastp25type = 0;
    state->offset = 0;
    state->carrier = 0;
    for (i = 0; i < 25; i++) {
        for (j = 0; j < 16; j++) {
            state->tg[i][j] = 48;
        }
    }
    state->tgcount = 0;
    state->lasttg = 0;
    state->lastsrc = 0;
    state->lasttgR = 0;
    state->lastsrcR = 0;
    state->gi[0] = -1;
    state->gi[1] = -1;
    state->eh_index = 0;
    state->eh_slot = 0;
    state->nac = 0;
    state->errs = 0;
    state->errs2 = 0;
    state->mbe_file_type = -1;
    state->optind = 0;
    state->numtdulc = 0;
    state->firstframe = 0;
    state->slot1light[0] = '\0';
    state->slot2light[0] = '\0';
    state->aout_gain = 25.0f;
    state->aout_gainR = 25.0f;
    state->aout_gainA = 0.0f; //use purely as a display or internal value, no user setting
    memset(state->aout_max_buf, 0, sizeof(float) * 200);
    state->aout_max_buf_p = state->aout_max_buf;
    state->aout_max_buf_idx = 0;

    memset(state->aout_max_bufR, 0, sizeof(float) * 200);
    state->aout_max_buf_pR = state->aout_max_bufR;
    state->aout_max_buf_idxR = 0;

    state->samplesPerSymbol = 10;
    state->symbolCenter = 4;
    set_underscores(state->algid, 8);
    set_underscores(state->keyid, 16);
    state->currentslot = 0;
    state->cur_mp = malloc(sizeof(mbe_parms));
    state->prev_mp = malloc(sizeof(mbe_parms));
    state->prev_mp_enhanced = malloc(sizeof(mbe_parms));

    state->cur_mp2 = malloc(sizeof(mbe_parms));
    state->prev_mp2 = malloc(sizeof(mbe_parms));
    state->prev_mp_enhanced2 = malloc(sizeof(mbe_parms));

    mbe_initMbeParms(state->cur_mp, state->prev_mp, state->prev_mp_enhanced);
    mbe_initMbeParms(state->cur_mp2, state->prev_mp2, state->prev_mp_enhanced2);
    state->p25kid = 0;

    state->debug_audio_errors = 0;
    state->debug_audio_errorsR = 0;
    state->debug_header_errors = 0;
    state->debug_header_critical_errors = 0;
    state->debug_mode = 0;

    state->nxdn_last_ran = -1;
    state->nxdn_last_rid = 0;
    state->nxdn_last_tg = 0;
    state->nxdn_cipher_type = 0;
    state->nxdn_key = 0;
    state->nxdn_call_type[0] = '\0';
    state->payload_miN = 0;

    state->dpmr_color_code = -1;

    state->payload_mi = 0;
    state->payload_miR = 0;
    state->payload_mfid = 0;
    state->payload_mfidR = 0;
    state->payload_algid = 0;
    state->payload_algidR = 0;
    state->payload_keyid = 0;
    state->payload_keyidR = 0;

    //init P2 ESS_B fragments and 4V counter
    for (short i = 0; i < 4; i++) {
        state->ess_b[0][i] = 0;
        state->ess_b[1][i] = 0;
    }
    state->fourv_counter[0] = 0;
    state->fourv_counter[1] = 0;
    state->voice_counter[0] = 0;
    state->voice_counter[1] = 0;

    state->K = 0;
    state->R = 0;
    state->RR = 0;
    state->H = 0;
    state->K1 = 0;
    state->K2 = 0;
    state->K3 = 0;
    state->K4 = 0;
    state->M = 0; //force key priority over settings from fid/so

    state->dmr_stereo = 0; //1, or 0?
    state->dmrburstL = 17; //initialize at higher value than possible
    state->dmrburstR = 17; //17 in char array is set for ERR
    state->dmr_so = 0;
    state->dmr_soR = 0;
    state->dmr_fid = 0;
    state->dmr_fidR = 0;
    state->dmr_flco = 0;
    state->dmr_flcoR = 0;
    state->dmr_ms_mode = 0;

    state->HYTL = 0;
    state->HYTR = 0;
    state->DMRvcL = 0;
    state->DMRvcR = 0;
    state->dropL = 256;
    state->dropR = 256;

    state->tyt_ap = 0;
    state->tyt_bp = 0;
    state->tyt_ep = 0;
    state->retevis_ap = 0;

    state->ken_sc = 0;
    state->any_bp = 0;
    state->straight_ks = 0;
    state->straight_mod = 0;

    //ks array storage and counters
    memset(state->ks_octetL, 0, sizeof(state->ks_octetL));
    memset(state->ks_octetR, 0, sizeof(state->ks_octetR));
    memset(state->ks_bitstreamL, 0, sizeof(state->ks_bitstreamL));
    memset(state->ks_bitstreamR, 0, sizeof(state->ks_bitstreamR));
    state->octet_counter = 0;
    state->bit_counterL = 0;
    state->bit_counterR = 0;

    memset(state->static_ks_bits, 0, sizeof(state->static_ks_bits));
    memset(state->static_ks_counter, 0, sizeof(state->static_ks_counter));

    //AES Specific Variables
    memset(state->aes_key, 0, sizeof(state->aes_key));
    memset(state->aes_iv, 0, sizeof(state->aes_iv));
    memset(state->aes_ivR, 0, sizeof(state->aes_ivR));
    memset(state->A1, 0, sizeof(state->A1));
    memset(state->A2, 0, sizeof(state->A2));
    memset(state->A3, 0, sizeof(state->A3));
    memset(state->A4, 0, sizeof(state->A4));
    memset(state->aes_key_loaded, 0, sizeof(state->aes_key_loaded));

    //xl specific, we need to know if the ESS is from HDU, or from LDU2
    state->xl_is_hdu = 0;

    //NXDN, when a new IV has arrived
    state->nxdn_new_iv = 0;

    state->p25vc = 0;
    state->payload_miP = 0;
    state->payload_miN = 0;

    //initialize dmr data header source
    state->dmr_lrrp_source[0] = 0;
    state->dmr_lrrp_source[1] = 0;
    state->dmr_lrrp_target[0] = 0;
    state->dmr_lrrp_target[1] = 0;

    //initialize data header bits
    state->data_header_blocks[0] = 1; //initialize with 1, otherwise we may end up segfaulting when no/bad data header
    state->data_header_blocks[1] = 1; //when trying to fill the superframe and 0-1 blocks give us an overflow
    state->data_header_padding[0] = 0;
    state->data_header_padding[1] = 0;
    state->data_header_format[0] = 7;
    state->data_header_format[1] = 7;
    state->data_header_sap[0] = 0;
    state->data_header_sap[1] = 0;
    state->data_block_counter[0] = 1;
    state->data_block_counter[1] = 1;
    state->data_p_head[0] = 0;
    state->data_p_head[1] = 0;
    state->data_block_poc[0] = 0;
    state->data_block_poc[1] = 0;
    state->data_byte_ctr[0] = 0;
    state->data_byte_ctr[1] = 0;
    state->data_ks_start[0] = 0;
    state->data_ks_start[1] = 0;

    state->menuopen = 0; //is the ncurses menu open, if so, don't process frame sync

    state->dmr_encL = 0;
    state->dmr_encR = 0;

    //P2 variables
    state->p2_wacn = 0;
    state->p2_sysid = 0;
    state->p2_cc = 0;
    state->p2_siteid = 0;
    state->p2_rfssid = 0;
    state->p2_hardset = 0;
    state->p2_is_lcch = 0;
    // P25p2 RS metrics
    state->p25_p2_rs_facch_ok = 0;
    state->p25_p2_rs_facch_err = 0;
    state->p25_p2_rs_facch_corr = 0;
    state->p25_p2_rs_sacch_ok = 0;
    state->p25_p2_rs_sacch_err = 0;
    state->p25_p2_rs_sacch_corr = 0;
    state->p25_p2_rs_ess_ok = 0;
    state->p25_p2_rs_ess_err = 0;
    state->p25_p2_rs_ess_corr = 0;
    state->p25_p2_enc_lo_early = 0;
    state->p25_p2_enc_pending[0] = 0;
    state->p25_p2_enc_pending[1] = 0;
    state->p25_p2_enc_pending_ttg[0] = 0;
    state->p25_p2_enc_pending_ttg[1] = 0;
    state->p25_cc_is_tdma =
        2; //init on 2, TSBK NET_STS will set 0, TDMA NET_STS will set 1. //used to determine if we need to change symbol rate when cc hunting

    //experimental symbol file capture read throttle
    state->symbol_throttle = 100; //throttle speed
    state->use_throttle = 0;      //only use throttle if set to 1

    state->p2_scramble_offset = 0;
    state->p2_vch_chan_num = 0;

    //p25 iden_up values
    state->p25_chan_iden = 0;
    for (int i = 0; i < 16; i++) {
        state->p25_chan_type[i] = 0;
        state->p25_trans_off[i] = 0;
        state->p25_chan_spac[i] = 0;
        state->p25_base_freq[i] = 0;
    }

    //values displayed in ncurses terminal
    state->p25_cc_freq = 0;
    state->p25_vc_freq[0] = 0;
    state->p25_vc_freq[1] = 0;

    // Initialize P25 regroup/patch tracking
    state->p25_patch_count = 0;
    for (int p = 0; p < 8; p++) {
        state->p25_patch_sgid[p] = 0;
        state->p25_patch_is_patch[p] = 0;
        state->p25_patch_active[p] = 0;
        state->p25_patch_last_update[p] = 0;
        state->p25_patch_wgid_count[p] = 0;
        state->p25_patch_wuid_count[p] = 0;
        for (int q = 0; q < 8; q++) {
            state->p25_patch_wgid[p][q] = 0;
            state->p25_patch_wuid[p][q] = 0;
        }
        state->p25_patch_key[p] = 0;
        state->p25_patch_alg[p] = 0;
        state->p25_patch_ssn[p] = 0;
    }

    //edacs - may need to make these user configurable instead for stability on non-ea systems
    state->ea_mode = -1; //init on -1, 0 is standard, 1 is ea
    state->edacs_vc_call_type = 0;
    state->esk_mask = 0x0; //esk mask value
    state->edacs_site_id = 0;
    state->edacs_sys_id = 0;
    state->edacs_area_code = 0;
    state->edacs_lcn_count = 0;
    state->edacs_cc_lcn = 0;
    state->edacs_vc_lcn = 0;
    state->edacs_tuned_lcn = -1;
    state->edacs_a_bits = 4;   //  Agency Significant Bits
    state->edacs_f_bits = 4;   //   Fleet Significant Bits
    state->edacs_s_bits = 3;   //Subfleet Significant Bits
    state->edacs_a_shift = 7;  //Calculated Shift for A Bits
    state->edacs_f_shift = 3;  //Calculated Shift for F Bits
    state->edacs_a_mask = 0xF; //Calculated Mask for A Bits
    state->edacs_f_mask = 0xF; //Calculated Mask for F Bits
    state->edacs_s_mask = 0x7; //Calculated Mask for S Bits

    //trunking
    memset(state->trunk_lcn_freq, 0, sizeof(state->trunk_lcn_freq));
    memset(state->trunk_chan_map, 0, sizeof(state->trunk_chan_map));
    state->group_tally = 0;
    state->lcn_freq_count = 0; //number of frequncies imported as an enumerated lcn list
    state->lcn_freq_roll = 0;  //needs reset if sync is found?
    state->last_cc_sync_time = time(NULL);
    state->last_vc_sync_time = time(NULL);
    state->last_active_time = time(NULL);
    state->last_t3_tune_time = time(NULL);
    state->is_con_plus = 0;

    //dmr trunking/ncurses stuff
    state->dmr_rest_channel = -1; //init on -1
    state->dmr_mfid = -1;         //
    state->dmr_cc_lpcn = 0;
    state->tg_hold = 0;

    //new nxdn stuff
    state->nxdn_part_of_frame = 0;
    state->nxdn_ran = 0;
    state->nxdn_sf = 0;
    memset(state->nxdn_sacch_frame_segcrc, 1, sizeof(state->nxdn_sacch_frame_segcrc)); //init on 1, bad CRC all
    state->nxdn_sacch_non_superframe = TRUE;
    memset(state->nxdn_sacch_frame_segment, 1, sizeof(state->nxdn_sacch_frame_segment));
    state->nxdn_alias_block_number = 0;
    memset(state->nxdn_alias_block_segment, 0, sizeof(state->nxdn_alias_block_segment));

    //site/srv/cch info
    state->nxdn_location_site_code = 0;
    state->nxdn_location_sys_code = 0;
    set_spaces(state->nxdn_location_category, 1);

    //channel access information
    state->nxdn_rcn = 0;
    state->nxdn_base_freq = 0;
    state->nxdn_step = 0;
    state->nxdn_bw = 0;

    //multi-key array
    memset(state->rkey_array, 0, sizeof(state->rkey_array));
    state->keyloader = 0; //keyloader off

    //Remus DMR End Call Alert Beep
    state->dmr_end_alert[0] = 0;
    state->dmr_end_alert[1] = 0;

    state->dmr_branding[0] = '\0';
    state->dmr_branding_sub[0] = '\0';
    state->dmr_site_parms[0] = '\0';

    //initialize unified dmr pdu 'superframe'
    memset(state->dmr_pdu_sf, 0, sizeof(state->dmr_pdu_sf));
    memset(state->data_header_valid, 0, sizeof(state->data_header_valid));

    //initialize cap+ bits and block num storage
    memset(state->cap_plus_csbk_bits, 0, sizeof(state->cap_plus_csbk_bits));
    memset(state->cap_plus_block_num, 0, sizeof(state->cap_plus_block_num));

    //init confirmed data individual block crc as invalid
    memset(state->data_block_crc_valid, 0, sizeof(state->data_block_crc_valid));

    //dmr slco stuff
    memset(state->dmr_cach_fragment, 1, sizeof(state->dmr_cach_fragment));
    state->dmr_cach_counter = 0;

    //embedded signalling
    memset(state->dmr_embedded_signalling, 0, sizeof(state->dmr_embedded_signalling));

    //dmr talker alias new/fixed stuff
    memset(state->dmr_alias_format, 0, sizeof(state->dmr_alias_format));
    memset(state->dmr_alias_block_len, 0, sizeof(state->dmr_alias_block_len));
    memset(state->dmr_alias_char_size, 0, sizeof(state->dmr_alias_char_size));
    memset(state->dmr_alias_block_segment, 0, sizeof(state->dmr_alias_block_segment));
    memset(state->dmr_embedded_gps, 0, sizeof(state->dmr_embedded_gps));
    memset(state->dmr_lrrp_gps, 0, sizeof(state->dmr_lrrp_gps));
    memset(state->active_channel, 0, sizeof(state->active_channel));

    //Generic Talker Alias String
    memset(state->generic_talker_alias, 0, sizeof(state->generic_talker_alias));
    state->generic_talker_alias_src[0] = 0;
    state->generic_talker_alias_src[1] = 0;

    //REMUS! multi-purpose call_string
    set_spaces(state->call_string[0], 21);
    set_spaces(state->call_string[1], 21);

    //late entry mi fragments
    memset(state->late_entry_mi_fragment, 0, sizeof(state->late_entry_mi_fragment));

    initialize_p25_heuristics(&state->p25_heuristics);
    initialize_p25_heuristics(&state->inv_p25_heuristics);

    state->dPMRVoiceFS2Frame.CalledIDOk = 0;
    state->dPMRVoiceFS2Frame.CallingIDOk = 0;
    memset(state->dPMRVoiceFS2Frame.CalledID, 0, 8);
    memset(state->dPMRVoiceFS2Frame.CallingID, 0, 8);
    memset(state->dPMRVoiceFS2Frame.Version, 0, 8);

    set_spaces(state->dpmr_caller_id, 6);
    set_spaces(state->dpmr_target_id, 6);

    //YSF Fusion Call Strings
    set_spaces(state->ysf_tgt, 10); //10 spaces
    set_spaces(state->ysf_src, 10); //10 spaces
    set_spaces(state->ysf_upl, 10); //10 spaces
    set_spaces(state->ysf_dnl, 10); //10 spaces
    set_spaces(state->ysf_rm1, 5);  //5 spaces
    set_spaces(state->ysf_rm2, 5);  //5 spaces
    set_spaces(state->ysf_rm3, 5);  //5 spaces
    set_spaces(state->ysf_rm4, 5);  //5 spaces
    memset(state->ysf_txt, 0, sizeof(state->ysf_txt));
    state->ysf_dt = 9;
    state->ysf_fi = 9;
    state->ysf_cm = 9;

    //DSTAR Call Strings
    set_spaces(state->dstar_rpt1, 8); //8 spaces
    set_spaces(state->dstar_rpt2, 8); //8 spaces
    set_spaces(state->dstar_dst, 8);  //8 spaces
    set_spaces(state->dstar_src, 8);  //8 spaces
    set_spaces(state->dstar_txt, 8);  //8 spaces
    set_spaces(state->dstar_gps, 8);  //8 spaces

    //M17 Storage
    memset(state->m17_lsf, 0, sizeof(state->m17_lsf));
    memset(state->m17_pkt, 0, sizeof(state->m17_pkt));
    state->m17_pbc_ct = 0;
    state->m17_str_dt = 9;

    //misc str storage
    //  sprintf (state->str50a, "%s", "");
    memset(state->str50b, 0, 50 * sizeof(char));
    memset(state->str50c, 0, 50 * sizeof(char));
    memset(state->m17sms, 0, 800 * sizeof(char));
    state->m17dat[0] = '\0';

    state->m17_dst = 0;
    state->m17_src = 0;
    state->m17_can = 0;      //can value that was decoded from signal
    state->m17_can_en = -1;  //can value supplied to the encoding side
    state->m17_rate = 48000; //sampling rate for audio input
    state->m17_vox = 0;      //vox mode enabled on M17 encoder
    memset(state->m17_dst_csd, 0, sizeof(state->m17_dst_csd));
    memset(state->m17_src_csd, 0, sizeof(state->m17_src_csd));
    state->m17_dst_str[0] = '\0';
    state->m17_src_str[0] = '\0';

    state->m17_enc = 0;
    state->m17_enc_st = 0;
    state->m17encoder_tx = 0;
    state->m17encoder_eot = 0;
    memset(state->m17_meta, 0, sizeof(state->m17_meta));

#ifdef USE_CODEC2
    state->codec2_3200 = codec2_create(CODEC2_MODE_3200);
    state->codec2_1600 = codec2_create(CODEC2_MODE_1600);
#endif

    state->dmr_color_code = 16;
    state->dmr_t3_syscode = 0;

    // Allocate per-slot event history (2 slots)
    state->event_history_s = calloc(2, sizeof(Event_History_I));

    //debug
    //  fprintf (stderr, "allocated size of event history struct: %ld bytes; \n", 600 * sizeof(Event_History));

    if (state->event_history_s == NULL) {
        fprintf(stderr, "memory allocation failure! \n");
    }

    //initialize event history items (0 to 255)
    for (uint8_t i = 0; i < 2; i++) {
        init_event_history(&state->event_history_s[i], 0, 255);
    }

    // Initialize transient UI toast message state
    state->ui_msg[0] = '\0';
    state->ui_msg_expire = 0;

} //init_state

void
usage() {

    printf("\n");
    printf("Usage: dsd-neo [options]            Decoder/Trunking Mode\n");
    printf("  or:  dsd-neo [options] -r <files> Read/Play saved mbe data from file(s)\n");
    printf("  or:  dsd-neo -h                   Show help\n");
    printf("\n");
    printf("Display Options:\n");
    printf("  -N            Use NCurses Terminal\n");
    printf("                 dsd-neo -N 2> console_log.txt \n");
    printf("  -Z            Log MBE/PDU Payloads to console\n");
    printf("  -j            Enable P25 LCW explicit retune (format 0x44)\n");
    printf("  -^            Prefer P25 CC candidates (RFSS/Adjacent/Network) during hunt\n");
    printf("\n");
    printf("Device Options:\n");
    printf("  -O            List All Pulse Audio Input Sources and Output Sinks (devices).\n");
    printf("\n");
    printf("Input/Output options:\n");
#ifdef __CYGWIN__
    printf("  -i <device>   Audio input device (default is /dev/dsp)\n");
    printf("                pulse for pulse audio (will require pactl running in Cygwin)\n");
#else
    printf("  -i <device>   Audio input device (default is pulse)\n");
    printf("                /dev/dsp for OSS audio (Depreciated: Will require padsp wrapper in Linux) \n");
#endif
    printf("                pulse for pulse audio signal input \n");
    printf("                pulse:6 or pulse:virtual_sink2.monitor for pulse audio signal input on virtual_sink2 (see "
           "-O) \n");
    printf("                rtl for rtl dongle (Default Values -- see below)\n");
    printf("                rtl:dev:freq:gain:ppm:bw:sql:vol for rtl dongle (see below)\n");
    printf("                rtltcp for rtl_tcp (default 127.0.0.1:1234)\n");
    printf("                rtltcp:host:port for rtl_tcp server address\n");
    printf("                tcp for tcp client SDR++/GNURadio Companion/Other (Port 7355)\n");
    printf("                tcp:192.168.7.5:7355 for custom address and port \n");
    printf("                udp for UDP direct audio input (default host 127.0.0.1; default port 7355)\n");
    printf("                udp:0.0.0.0:7355 to bind all interfaces for UDP input\n");
    printf("                m17udp for M17 UDP/IP socket bind input (default host 127.0.0.1; default port 17000)\n");
    printf("                m17udp:192.168.7.8:17001 for M17 UDP/IP bind input (Binding Address and Port\n");
    printf("                filename.bin for OP25/FME capture bin files\n");
    printf("                filename.wav for 48K/1 wav files (SDR++, GQRX)\n");
    printf("                filename.wav -s 96000 for 96K/1 wav files (DSDPlus)\n");
#ifdef __CYGWIN__
    printf("                (Use single quotes '\\directory\\audio file.wav' when directories/spaces are present)\n");
#else
    printf("                (Use single quotes '/directory/audio file.wav' when directories/spaces are present)\n");
#endif
    // printf ("                (Windows - '\directory\audio file.wav' backslash, not forward slash)\n");
    printf("  -s <rate>     Sample Rate of wav input files (48000 or 96000) Mono only!\n");
#ifdef __CYGWIN__
    printf("  -o <device>   Audio output device (default is /dev/dsp)\n");
    printf("                pulse for pulse audio (will require pactl running in Cygwin)\n");
#else
    printf("  -o <device>   Audio output device (default is pulse)\n");
    printf("                /dev/dsp for OSS audio (Depreciated: Will require padsp wrapper in Linux) \n");
#endif
    printf("                pulse for pulse audio decoded voice or analog output\n");
    printf("                pulse:1 or pulse:alsa_output.pci-0000_0d_00.3.analog-stereo for pulse audio decoded voice "
           "or analog output on device (see -O) \n");
    printf("                null for no audio output\n");
    printf("                udp for UDP socket blaster output (default host 127.0.0.1; default port 23456)\n");
    printf("                udp:192.168.7.8:23470 for UDP socket blaster output (Target Address and Port\n");
    printf(
        "                m17udp for M17 UDP/IP socket blaster output (default host 127.0.0.1; default port 17000)\n");
    printf("                m17udp:192.168.7.8:17001 for M17 UDP/IP blaster output (Target Address and Port\n");
    printf("  -d <dir>      Create mbe data files, use this directory (TDMA version is experimental)\n");
    printf("  -r <files>    Read/Play saved mbe data from file(s)\n");
    printf("  -g <float>    Audio Digital Output Gain  (Default: 0 = Auto;        )\n");
    printf("                                           (Manual:  1 = 2%%; 50 = 100%%)\n");
    printf("  -n <float>    Audio Analog  Output Gain  (Default: 0 = Auto; 0-100%%  )\n");
    printf("  -6 <file>     Output raw audio .wav file (48K/1). (WARNING! Large File Sizes 1 Hour ~= 360 MB)\n");
    printf("  -7 <dir>      Create/Use Custom directory for Per Call decoded .wav file saving.\n");
    printf("                 (Use ./folder for Nested Directory!)\n");
    printf("                 (Use /path/to/folder for hard coded directory!)\n");
    printf("                 (Use Before the -P option!)\n");
    printf("  -8            Enable Source Audio Monitor\n");
    printf("                 (Set Squelch in RTL, SDR++ or GQRX, etc, if monitoring mixed analog/digital)\n");
    printf("  -w <file>     Output synthesized speech to a single static .wav file. (Do not use with -P Per Call "
           "Switch)\n");
    printf(
        "  -P            Enable Per Call WAV file saving. (Do not use with -w filename.wav single wav file switch)\n");
    printf("                 (Per Call works with everything now and doesn't require ncurses terminal!)\n");
    printf("  -a            Enable Call Alert Beep\n");
    printf("                 (Warning! Might be annoying.)\n");
    printf("  -J <file>     Specify Filename for Event Log Output.\n");
    printf("  -L <file>     Specify Filename for LRRP Data Output.\n");
    printf("  -Q <file>     Specify Filename for OK-DMRlib Structured File Output. (placed in DSP folder)\n");
    printf("  -Q <file>     Specify Filename for M17 Float Stream Output. (placed in DSP folder)\n");
    printf("  -c <file>     Output symbol capture to .bin file\n");
    printf(
        "  -q            Reverse Mute - Mute Unencrypted Voice and Unmute Encrypted Voice\n"); //does this still work correctly?
    printf("  -V <num>      Enable TDMA Voice Synthesis on Slot 1 (1), Slot 2 (2), or Both (3); Default is 3; \n");
#ifdef __CYGWIN__
    printf("                If using /dev/dsp input and output at 48k1, launch two instances of DSD-neo w -V 1 and -V "
           "2 if needed\n");
#endif
    printf("  -z            Set TDMA Voice Slot Preference when using /dev/dsp audio output (prevent lag and "
           "stuttering)\n");
    printf("  -y            Enable Experimental Pulse Audio Float Audio Output\n");
    printf("  -v <hex>      Set Filtering Bitmap Options (Advanced Option)\n");
    printf("                1 1 1 1 (0xF): PBF/LPF/HPF/HPFD on\n");
    printf("\n");
    printf("RTL-SDR options:\n");
    printf(" Usage: rtl:dev:freq:gain:ppm:bw:sql:vol[:bias[=on|off]]\n");
    printf("  NOTE: all arguments after rtl are optional now for trunking, but user configuration is recommended\n");
    printf("  dev  <num>    RTL-SDR Device Index Number or 8 Digit Serial Number, no strings! (default 0)\n");
    printf("  freq <num>    RTL-SDR Frequency (851800000 or 851.8M) \n");
    printf("  gain <num>    RTL-SDR Device Gain (0-49)(default = 0; Hardware AGC recommended)\n");
    printf("  ppm  <num>    RTL-SDR PPM Error (default = 0)\n");
    printf("  bw   <num>    RTL-SDR Bandwidth kHz (default = 12)(4, 6, 8, 12, 16, 24)  \n");
    printf("  sq   <val>    RTL-SDR Squelch Threshold (Optional)\n");
    printf("                 (Negative = dB; Positive/Zero = linear mean power)\n");
    // printf ("  udp  <num>    RTL-SDR Legacy UDP Remote Port (Optional -- External Use Only)\n"); //NOTE: This is still available as an option in the ncurses menu
    printf("  vol  <num>    RTL-SDR Sample 'Volume' Multiplier (default = 2)(1,2,3)\n");
    printf("  bias [on|off] Enable 5V bias tee on compatible dongles (default off)\n");
    printf(" Example: dsd-neo -fs -i rtl -C cap_plus_channel.csv -T\n");
    printf(" Example: dsd-neo -fp -i rtl:0:851.375M:22:-2:24:0:2\n");
    printf("\n");
    printf("RTL-TCP options:\n");
    printf(" Usage: rtltcp[:host:port[:freq:gain:ppm:bw:sql:vol[:bias[=on|off]]]]\n");
    printf("  host: default 127.0.0.1; port: default 1234\n");
    printf("  Remaining fields mirror rtl: string semantics.\n");
    printf(" Example: dsd-neo -i rtltcp:192.168.1.10:1234:851.375M:22:-2:24:0:2 -N\n");
    printf("\n");
    printf("UDP examples:\n");
    printf(" Example: dsd-neo -i udp -o pulse -N\n");
    printf("   Listen for UDP audio on 127.0.0.1:7355 and play to PulseAudio.\n");
    printf(" Example: dsd-neo -i udp:0.0.0.0:7355 -o pulse -N\n");
    printf("   Bind all interfaces; point GQRX/SDR++ UDP audio to this host:port.\n");
    printf("\n");
    printf("Encoder options:\n");
    printf("  -fZ           M17 Stream Voice Encoder\n");
    printf(" Example: dsd-neo -fZ -M M17:9:DSD-neo:arancormonk -i pulse -6 m17signal.wav -8 -N 2> m17encoderlog.txt\n");
    printf("   Run M17 Encoding, listening to pulse audio server, with internal decode/playback and output to 48k/1 "
           "wav file\n");
    printf("\n");
    printf(" Example: dsd-neo -fZ -M M17:9:DSD-neo:arancormonk -i tcp -o pulse -8 -N 2> m17encoderlog.txt\n");
    printf("   Run M17 Encoding, listening to default tcp input, without internal decode/playback and output to 48k/1 "
           "analog output device\n");
    printf("\n");
    printf("  -fP           M17 Packet Encoder\n");
    printf(" Example: dsd-neo -fP -M M17:9:DSD-neo:arancormonk -6 m17pkt.wav -8 -S 'Hello World'\n");
    printf("\n");
    printf("  -fB           M17 BERT Encoder\n");
    printf(" Example: dsd-neo -fB -M M17:9:DSD-neo:arancormonk -6 m17bert.wav -8\n");
    printf("\n");
    printf("  -M            M17 Encoding User Configuration String: M17:CAN:SRC:DST:INPUT_RATE:VOX (see examples "
           "above).\n");
    printf("                  CAN 1-15; SRC and DST have to be no more than 9 UPPER base40 characters.\n");
    printf("                  BASE40: '  ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-/.'\n");
    printf("                  Input Rate Default is 48000; Use Multiples of 8000 up to 48000.\n");
    printf("                  VOX Enabled on 1; (Default = 0)\n");
    printf("                  Values not entered into the M17: string are set to default values.\n");
    //  printf ("  -S            M17 Packet Encoder SMS String: No more than 772 chars, use single quotations (see example above).\n");
    //  printf ("  -S            M17 Stream Encoder SMS String: No more than  48 chars, activates 1600 voice  (best if broken into six 8 char chunks).\n");
    printf("Decoder options:\n");
    printf("  -fa           Auto Detection\n");
    printf("  -fA           Passive Analog Audio Monitor\n");
    printf("  -ft           TDMA Trunking P25p1 Control and Voice, P25p2 Trunked Channels, and DMR\n");
    printf("  -fs           DMR TDMA BS and MS Simplex\n");
    printf("  -f1           Decode only P25 Phase 1\n");
    printf("  -f2           Decode only P25 Phase 2 (6000 sps) **\n");
    printf("  -fd           Decode only DSTAR\n");
    printf("  -fx           Decode only X2-TDMA\n");
    printf("  -fy           Decode only YSF\n");
    printf("  -fz             Decode only M17*\n");
    printf("  -fU             Decode only M17 UDP/IP Frame***\n");
    printf("  -fi             Decode only NXDN48* (6.25 kHz) / IDAS*\n");
    printf("  -fn             Decode only NXDN96* (12.5 kHz)\n");
    printf("  -fp             Decode only ProVoice*\n");
    printf("  -fh             Decode only EDACS Standard/ProVoice*\n");
    printf("  -fH             Decode only EDACS Standard/ProVoice with ESK 0xA0*\n");
    printf(
        "  -fh344          Decode only EDACS Standard/ProVoice and set AFS to 344 or similar custom 11-bit scheme*\n");
    printf(
        "  -fH434          Decode only EDACS Standard/ProVoice and set AFS to custom 11-bit scheme with ESK 0xA0*\n");
    printf("  -fe             Decode only EDACS EA/ProVoice*\n");
    printf("  -fE             Decode only EDACS EA/ProVoice with ESK 0xA0*\n");
    printf("  -fm             Decode only dPMR*\n");
    printf("  -l            Disable DMR, dPMR, NXDN, M17 input filtering\n");
    printf("  -u <num>      Unvoiced speech quality (default=3)\n");
    printf("  -xx           Expect non-inverted X2-TDMA signal\n");
    printf("  -xr           Expect inverted DMR signal\n");
    printf("  -xd           Expect inverted ICOM dPMR signal\n");
    printf("  -xz           Expect inverted M17 signal\n");
    printf("\n");
    printf("  * denotes frame types that cannot be auto-detected.\n");
    printf("  ** Phase 2 Single Frequency may require user to manually set WACN/SYSID/CC parameters if MAC_SIGNAL not "
           "present.\n");
    printf("  *** configure UDP Input with -i m17:127.0.0.1:17000 \n");
    printf("\n");
    printf("Advanced Decoder options:\n");
    printf("  -X <hex>      Manually Set P2 Parameters (WACN, SYSID, CC/NAC)\n");
    printf("                 (-X BEE00ABC123)\n");
    printf("  -D <dec>      Manually Set TIII DMR Location Area n bit len (0-10)(10 max)\n");
    printf("                 (Value defaults to max n bit value for site model size)\n");
    printf("                 (Setting 0 will show full Site ID, no area/subarea)\n");
    printf("\n");
    printf("  -ma           Auto-select modulation optimizations\n");
    printf("  -mc           Use only C4FM modulation optimizations (default)\n");
    printf("  -mg           Use only GFSK modulation optimizations\n");
    printf("  -mq           Use only QPSK modulation optimizations\n");
    printf("  -m2           Use P25p2 6000 sps QPSK modulation optimizations\n");
    printf("  -mL           Enable CQPSK LMS (experimental)\n");
    //printf ("                 (4 Level, not 8 Level LSM) (this is honestly unknown since I can't verify what local systems are using)\n");
    printf("  -F            Relax P25 Phase 2 MAC_SIGNAL CRC Checksum Pass/Fail\n");
    printf("                 Use this feature to allow MAC_SIGNAL even if CRC errors.\n");
    printf("  -F            Relax DMR RAS/CRC CSBK/DATA Pass/Fail\n");
    printf("                 Enabling on some systems could lead to bad channel assignments/site data decoding if bad "
           "or marginal signal\n");
    printf("  -F            Relax M17 LSF/PKT CRC Error Checking\n");
    printf("\n");
    printf("  -b <dec>      Manually Enter Basic Privacy Key (Decimal Value of Key Number)\n");
    printf("                 (NOTE: This used to be the 'K' option! \n");
    printf("\n");
    printf("  -H <hex>      Manually Enter Hytera 10/32/64 Char Basic Privacy Hex Key (see example below)\n");
    printf("                 Encapulate in Single Quotation Marks; Space every 16 chars.\n");
    printf("                 -H 0B57935150 \n");
    printf("                 -H '736B9A9C5645288B 243AD5CB8701EF8A' \n");
    printf("                 -H '20029736A5D91042 C923EB0697484433 005EFC58A1905195 E28E9C7836AA2DB8' \n");
    printf("\n"); //may move to using the rkey_array with an offset for additional key values
    printf("  -H <hex>      Manually Enter AES-128 or AES-256 Hex Key (see example below)\n");
    printf("                 Encapulate in Single Quotation Marks; Space every 16 chars.\n");
    printf("                 -H '736B9A9C5645288B 243AD5CB8701EF8A' \n");
    printf("                 -H '20029736A5D91042 C923EB0697484433 005EFC58A1905195 E28E9C7836AA2DB8' \n");
    printf("\n");
    printf("  -R <dec>      Manually Enter dPMR or NXDN EHR Scrambler Key Value (Decimal Value)\n");
    printf("                 \n");
    printf("  -1 <hex>      Manually Enter RC4 or DES Key Value (DMR, P25, NXDN) (Hex Value) \n");
    printf("                 \n");
    printf("  -2 <hex>      Manually Enter and Enforce TYT 16-bit BP Key Value (DMR) (Hex Value) \n");
    printf("                 \n");
    printf("  -! <hex>      Manually Enter and Enforce TYT Advanced Privacy (PC4) AP Hex Key (see example below)\n");
    printf("                 Encapulate in Single Quotation Marks; Space every 16 chars.\n");
    printf("                 -! '736B9A9C5645288B 243AD5CB8701EF8A' \n");
    printf("                 \n");
    printf(
        "  -@ <hex>      Manually Enter and Enforce Retevis Advanced Privacy (RC2) AP Hex Key (see example below)\n");
    printf("                 Encapulate in Single Quotation Marks; Space every 16 chars.\n");
    printf("                 -@ '736B9A9C5645288B 243AD5CB8701EF8A' \n");
    printf("                 \n");
    printf(
        "  -5 <hex>      Manually Enter and Enforce TYT Enhanced Privacy (AES-128) EP Hex Key (see example below)\n");
    printf("                 Encapulate in Single Quotation Marks; Space every 16 chars.\n");
    printf("                 -5 '736B9A9C5645288B 243AD5CB8701EF8A' \n");
    printf("                 \n");
    printf("  -9 <dec>      Manually Enter and Enforce Kenwood 15-bit Scrambler Key Value (DMR) (Dec Value) \n");
    printf("                 \n");
    printf("  -A <hex>      Manually Enter and Enforce Anytone 16-bit BP Key Value (DMR) (Hex Value) \n");
    printf("                 \n");
    printf("  -S <str>      Manually Enter and Enforce Generic Static Keystream -> Length and BYTE PACKED / ALIGNED "
           "String for AMBE (up to 882 bits)\n");
    printf("                  For Example, enter 16-bit Keystream 0909 as:\n");
    printf("                    -S 16:0909\n");
    printf("                  For Example, enter 49-bit Keystream as:\n");
    printf("                    -S 49:123456789ABC80\n");
    printf("                  For Example, enter 49-bit Keystream (MBP 70) as:\n");
    printf("                    -S 49:ED0AED4AED4AED4A\n");
    printf("                 \n");
    printf("  -k <file>     Import Key List from csv file (Decimal Format) -- Lower Case 'k'.\n");
    printf("                  Only supports NXDN, DMR Basic Privacy (decimal value). \n");
    printf("                  (dPMR and Hytera 32/64 char not supported, DMR uses TG value as key id -- "
           "EXPERIMENTAL!!). \n");
    printf("                 \n");
    printf("  -K <file>     Import Key List from csv file (Hexidecimal Format) -- Capital 'K'.\n");
    printf("                  Use for Hex Value Hytera 10-char BP keys, RC4 10-char, DES 16-char Hex Keys, and "
           "AES128/256 32/64-char keys. \n");
    printf("                 \n");
    printf("  -4            Force Privacy Key over Encryption Identifiers (DMR MBP/HBP and NXDN Scrambler) \n");
    printf("                 \n");
    printf("  -0            Force RC4 Key over Missing PI header/LE Encryption Identifiers (DMR) \n");
    printf("                 \n");
    printf("  -3            Disable DMR Late Entry Encryption Identifiers (VC6 Single Burst) \n");
    printf("                  Note: Disable this if false positives on Voice ENC occur. \n");
    printf("\n");
    printf(" Trunking Options:\n");
    printf("  -C <file>     Import Channel to Frequency Map (channum, freq) from csv file. (Capital C)                 "
           "  \n");
    printf("                 (See channel_map.csv for example)\n");
    printf("  -G <file>     Import Group List Allow/Block and Label from csv file.\n");
    printf("                 (See group.csv for example)\n");
    printf("  -T            Enable Trunking Features (NXDN/P25/EDACS/DMR) with RIGCTL/TCP or RTL Input\n");
    printf("  -Y            Enable Scanning Mode with RIGCTL/TCP or RTL Input \n");
    printf(
        "                 Experimental -- Can only scan for sync with enabled decoders, don't mix NXDN and DMR/P25!\n");
    printf("                 This is not a Trunking Feature, just scans through conventional frequencies fast!\n");
    printf("  -W            Use Imported Group List as a Trunking Allow/White List -- Only Tune with Mode A\n");
    printf("  -p            Disable Tune to Private Calls (DMR TIII, P25, NXDN Type-C and Type-D)\n");
    printf("  -E            Disable Tune to Group Calls (DMR TIII, Con+, Cap+, P25, NXDN Type-C, and Type-D)\n");
    printf("  -e            Enable Tune to Data Calls (DMR TIII, Cap+, NXDN Type-C)\n");
    printf("                 (NOTE: No Clear Distinction between Cap+ Private Voice Calls and Data Calls -- Both "
           "enabled with Data Calls \n");
    printf("  -I <dec>      Specify TG to Hold During Trunking (DMR, P25, NXDN Type-C Trunking)\n");
    printf("  -U <port>     Enable RIGCTL/TCP; Set TCP Port for RIGCTL. (4532 on SDR++)\n");
    printf("  -B <Hertz>    Set RIGCTL Setmod Bandwidth in Hertz (0 - default - Off)\n");
    printf(
        "                 P25 - 12000; NXDN48 - 7000; NXDN96: 12000; DMR - 7000-12000; EDACS/PV - 12000-24000;\n"); //redo this, or check work, or whatever
    printf("                 May vary based on system stregnth, etc.\n");
    printf("  -t <secs>     Set Trunking or Scan Speed VC/sync loss hangtime in seconds. (default = 1 second)\n");
    printf("\n");
    printf(" Trunking Example TCP: dsd-neo -fs -i tcp -U 4532 -T -C dmr_t3_chan.csv -G group.csv -N 2> log.txt\n");
    printf(" Trunking Example RTL: dsd-neo -fs -i rtl:0:450M:26:-2:8 -T -C connect_plus_chan.csv -G group.csv -N 2> "
           "log.txt\n");
    printf("\n");

    printf("DMR TIII Tools:\n");
    printf("  --calc-lcn <file>           Calculate LCNs from a CSV of frequencies (Hz or MHz).\n");
    printf("  --calc-cc-freq <freq>       Anchor CC frequency (Hz or MHz).\n");
    printf("  --calc-cc-lcn <int>         Anchor CC LCN number.\n");
    printf("  --calc-step <hz>            Override inferred channel step (Hz, e.g., 12500).\n");
    printf("  --calc-start-lcn <int>      Starting LCN when no anchor (default 1).\n");
    printf(" Example: dsd-neo --calc-lcn freqs.csv --calc-cc-freq 451.2375 --calc-cc-lcn 50\n");
    printf("\n");
    exit(0);
}

void
liveScanner(dsd_opts* opts, dsd_state* state) {
    // Cache previous thresholds to avoid redundant recalculation
    static int last_max = INT_MIN;
    static int last_min = INT_MAX;

    if (opts->floating_point == 1) {

        if (opts->audio_gain > 50.0f) {
            opts->audio_gain = 50.0f;
        }
        if (opts->audio_gain < 0.0f) {
            opts->audio_gain = 0.0f;
        }
    } else if (opts->audio_gain == 0) {
        state->aout_gain = 15.0f;
        state->aout_gainR = 15.0f;
    }

#ifdef USE_RTLSDR
    if (opts->audio_in_type == 3) {
        if (g_rtl_ctx == NULL) {
            if (rtl_stream_create(opts, &g_rtl_ctx) < 0) {
                fprintf(stderr, "Failed to create RTL stream.\n");
            }
        }
        if (g_rtl_ctx && rtl_stream_start(g_rtl_ctx) < 0) {
            fprintf(stderr, "Failed to open RTL-SDR stream.\n");
        }
        opts->rtl_started = 1;
    }
#endif

    if (opts->use_ncurses_terminal == 1) {
        ncursesOpen(opts, state);
    }

    if (opts->audio_in_type == 0) {
        openPulseInput(opts);
    }

    if (opts->audio_out_type == 0) {
        openPulseOutput(opts);
    }

    //push a DSD-neo started event so users can see what this section does, and also gives users an idea of when context started
    state->event_history_s[0].Event_History_Items[0].color_pair = 4;
    watchdog_event_datacall(opts, state, 0, 0, "Any decoded voice calls or data calls display here;", 0);
    push_event_history(&state->event_history_s[0]);
    init_event_history(&state->event_history_s[0], 0, 1);
    state->event_history_s[0].Event_History_Items[0].color_pair = 4;
    watchdog_event_datacall(opts, state, 0, 0, "DSD-neo Started and Event History Initialized;", 0);
    push_event_history(&state->event_history_s[0]);
    init_event_history(&state->event_history_s[0], 0, 1);

    if (opts->event_out_file[0] != 0) {
        char* timestr = getTimeN(time(NULL));
        char* datestr = getDateN(time(NULL));
        char event_string[2000];
        memset(event_string, 0, sizeof(event_string));
        sprintf(event_string, "%s %s DSD-neo Started and Event History Initialized;", datestr, timestr);
        write_event_to_log_file(opts, state, 0, 0, event_string);
        memset(event_string, 0, sizeof(event_string));
        sprintf(event_string, "%s %s Any decoded voice calls or data calls display here;", datestr, timestr);
        write_event_to_log_file(opts, state, 0, 0, event_string);

        if (timestr != NULL) {
            free(timestr);
            timestr = NULL;
        }
        if (datestr != NULL) {
            free(datestr);
            datestr = NULL;
        }
    }

    //test P25 moto alias by loading in test vectors captured from a system and dumped on forum (see dsd_gps.c)
    // apx_embedded_alias_test_phase1(opts, state); //enable this to run test

    while (!exitflag) {

        noCarrier(opts, state);
        if (state->menuopen == 0) {
            state->synctype = getFrameSync(opts, state);
            // Recompute thresholds only when extrema change
            if (state->max != last_max || state->min != last_min) {
                state->center = ((state->max) + (state->min)) / 2;
                state->umid = (((state->max) - state->center) * 5 / 8) + state->center;
                state->lmid = (((state->min) - state->center) * 5 / 8) + state->center;
                last_max = state->max;
                last_min = state->min;
            }
        }

        while (state->synctype != -1) {
            processFrame(opts, state);

#ifdef TRACE_DSD
            state->debug_prefix = 'S';
#endif

            // state->synctype = getFrameSync (opts, state);

#ifdef TRACE_DSD
            state->debug_prefix = '\0';
#endif

            // // recalibrate center/umid/lmid
            // state->center = ((state->max) + (state->min)) / 2;
            // state->umid = (((state->max) - state->center) * 5 / 8) + state->center;
            // state->lmid = (((state->min) - state->center) * 5 / 8) + state->center;
            if (state->menuopen == 0) {
                state->synctype = getFrameSync(opts, state);
                // Recompute thresholds only when extrema change
                if (state->max != last_max || state->min != last_min) {
                    state->center = ((state->max) + (state->min)) / 2;
                    state->umid = (((state->max) - state->center) * 5 / 8) + state->center;
                    state->lmid = (((state->min) - state->center) * 5 / 8) + state->center;
                    last_max = state->max;
                    last_min = state->min;
                }
            }
        }
    }
}

void
cleanupAndExit(dsd_opts* opts, dsd_state* state) {
    // Signal that everything should shutdown.
    exitflag = 1;

    // Close ncurses early so subsequent logs (e.g., ring stats) print to TTY
    if (opts->use_ncurses_terminal == 1) {
        ncursesClose();
    }

#ifdef USE_CODEC2
    codec2_destroy(state->codec2_1600);
    codec2_destroy(state->codec2_3200);
#endif

    //watchdog event at this point
    watchdog_event_history(opts, state, 0);
    watchdog_event_current(opts, state, 0);
    watchdog_event_history(opts, state, 1);
    watchdog_event_current(opts, state, 1);

    noCarrier(opts, state);

    //watchdog event at this point
    watchdog_event_history(opts, state, 0);
    watchdog_event_current(opts, state, 0);
    watchdog_event_history(opts, state, 1);
    watchdog_event_current(opts, state, 1);

    if (opts->static_wav_file == 0) {
        if (opts->wav_out_f != NULL) {
            opts->wav_out_f = close_and_rename_wav_file(opts->wav_out_f, opts->wav_out_file, opts->wav_out_dir,
                                                        &state->event_history_s[0]);
        }

        if (opts->wav_out_fR != NULL) {
            opts->wav_out_fR = close_and_rename_wav_file(opts->wav_out_fR, opts->wav_out_fileR, opts->wav_out_dir,
                                                         &state->event_history_s[1]);
        }
    }

    else if (opts->static_wav_file == 1) {

        if (opts->wav_out_f != NULL) {
            opts->wav_out_f = close_wav_file(opts->wav_out_f);
        }

        //this one needed?
        if (opts->wav_out_fR != NULL) {
            opts->wav_out_fR = close_wav_file(opts->wav_out_fR);
        }
    }

    if (opts->wav_out_raw != NULL) {
        opts->wav_out_raw = close_wav_file(opts->wav_out_raw);
    }

    //no if statement first?
    closeSymbolOutFile(opts, state);

#ifdef USE_RTLSDR
    if (opts->rtl_started == 1) {
        if (g_rtl_ctx) {
            rtl_stream_stop(g_rtl_ctx);
            rtl_stream_destroy(g_rtl_ctx);
            g_rtl_ctx = NULL;
        }
    }
#endif

    if (opts->udp_sockfd) {
        close(opts->udp_sockfd);
    }

    if (opts->udp_sockfdA) {
        close(opts->udp_sockfdA);
    }

    if (opts->m17_udp_sock) {
        close(opts->m17_udp_sock);
    }

    if (opts->udp_in_ctx) {
        udp_input_stop(opts);
    }

    //close MBE out files
    if (opts->mbe_out_f != NULL) {
        closeMbeOutFile(opts, state);
    }
    if (opts->mbe_out_fR != NULL) {
        closeMbeOutFileR(opts, state);
    }

    fprintf(stderr, "\n");
    fprintf(stderr, "Total audio errors: %i\n", state->debug_audio_errors);
    fprintf(stderr, "Total header errors: %i\n", state->debug_header_errors);
    fprintf(stderr, "Total irrecoverable header errors: %i\n", state->debug_header_critical_errors);
    fprintf(stderr, "Exiting.\n");
    exit(0);
}

double
atofs(char* s) {
    size_t len = strlen(s);
    if (len == 0) {
        return 0.0;
    }

    char last = s[len - 1];
    double factor = 1.0;

    switch (last) {
        case 'g':
        case 'G': factor = 1e9; break;
        case 'm':
        case 'M': factor = 1e6; break;
        case 'k':
        case 'K': factor = 1e3; break;
        default: return atof(s);
    }

    s[len - 1] = '\0';
    double val = atof(s);
    s[len - 1] = last;
    return val * factor;
}

int
main(int argc, char** argv) {
    int c;
//optarg and optind already defined when using Cygwin, no need to do so again
#ifdef __CYGWIN__
#else
    extern char* optarg;
    extern int optind;
#endif
    dsd_opts opts;
    dsd_state state;
    char versionstr[25];
    mbe_printVersion(versionstr);

// Simple banner with semantic version and short git hash
#if 0
    fprintf(stderr, "------------------------------------------------------------------------------\\n");
    fprintf(stderr, "| Digital Speech Decoder: DSD-neo %s (%s) \\n+", GIT_TAG, GIT_HASH);
    fprintf(stderr, "------------------------------------------------------------------------------\\n");

#endif
    fprintf(stderr, "------------------------------------------------------------------------------\n");
    fprintf(stderr, "| Digital Speech Decoder: DSD-neo %s (%s) \n", GIT_TAG, GIT_HASH);
    fprintf(stderr, "------------------------------------------------------------------------------\n");

#ifdef USE_MBELIB_NEO
    fprintf(stderr, "MBElib-neo Version: %s\n", versionstr);
#else
    fprintf(stderr, "MBElib Version: %s\n", versionstr);
#endif

#ifdef USE_CODEC2
    fprintf(stderr, "CODEC2 Support Enabled\n");
#endif

    initOpts(&opts);
    initState(&state);
    init_audio_filters(&state); //audio filters
    init_rrc_filter_memory();   //initialize input filtering
    InitAllFecFunction();
    CNXDNConvolution_init();

    exitflag = 0;

    // One-shot: DMR TIII LCN calculator via command line or environment
    {
        const char* calc_csv_env = getenv("DSD_NEO_DMR_T3_CALC_CSV");

        // Parse long-style CLI args before getopt
        const char* calc_csv_cli = NULL;
        const char* calc_step_cli = NULL;
        const char* calc_ccf_cli = NULL;
        const char* calc_ccl_cli = NULL;
        const char* calc_start_cli = NULL;

        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--calc-lcn") == 0 && i + 1 < argc) {
                calc_csv_cli = argv[++i];
                continue;
            }
            if (strcmp(argv[i], "--calc-step") == 0 && i + 1 < argc) {
                calc_step_cli = argv[++i];
                continue;
            }
            if (strcmp(argv[i], "--calc-cc-freq") == 0 && i + 1 < argc) {
                calc_ccf_cli = argv[++i];
                continue;
            }
            if (strcmp(argv[i], "--calc-cc-lcn") == 0 && i + 1 < argc) {
                calc_ccl_cli = argv[++i];
                continue;
            }
            if (strcmp(argv[i], "--calc-start-lcn") == 0 && i + 1 < argc) {
                calc_start_cli = argv[++i];
                continue;
            }
        }

        // If CLI present, set env vars and run calculator
        if (calc_csv_cli) {
            setenv("DSD_NEO_DMR_T3_CALC_CSV", calc_csv_cli, 1);
            if (calc_step_cli) {
                setenv("DSD_NEO_DMR_T3_STEP_HZ", calc_step_cli, 1);
            }
            if (calc_ccf_cli) {
                setenv("DSD_NEO_DMR_T3_CC_FREQ", calc_ccf_cli, 1);
            }
            if (calc_ccl_cli) {
                setenv("DSD_NEO_DMR_T3_CC_LCN", calc_ccl_cli, 1);
            }
            if (calc_start_cli) {
                setenv("DSD_NEO_DMR_T3_START_LCN", calc_start_cli, 1);
            }
            int rc = run_t3_lcn_calc_from_csv(calc_csv_cli);
            return rc;
        }

        // Environment fallback remains
        if (calc_csv_env && *calc_csv_env) {
            int rc = run_t3_lcn_calc_from_csv(calc_csv_env);
            return rc;
        }
    }

    while ((c = getopt(argc, argv,
                       "~yhaepPqs:t:v:z:i:o:d:c:g:n:w:B:C:R:f:m:u:x:A:S:M:G:D:L:V:U:YK:b:H:X:NQ:WrlZTF@:!:01:2:345:6:7:"
                       "89:Ek:I:J:Oj^"))
           != -1) {

        switch (c) {
            case 'h':
                usage();
                exit(0);
                break; //probably isn't required, but just making sure it doesn't do anything bizarre

            case 'a': opts.call_alert = 1; break;

            //Free'd up switches include: j, O,
            //

            //make sure to put a colon : after each if they need an argument
            //or remove colon if no argument required

            //NOTE: The 'K' option for single BP key has been swapped to 'b'
            //'K' is now used for hexidecimal key.csv imports

            //this is a debug option hidden from users, but use it to replay .bin files on loop
            case '~':
                state.debug_mode = 1;
                fprintf(stderr, "Debug Mode Enabled; \n");
                break;

            //List Pulse Audio Input and Output
            case 'O':
                pulse_list();
                exit(0);
                break;

            //Specify M17 encoder User Data (CAN, DST, SRC values)
            //NOTE: Disabled QPSK settings by borrowing these switches (nobody probalby used them anyways)
            case 'M':
                strncpy(state.m17dat, optarg, 49);
                state.m17dat[49] = '\0';
                break;

                //  //Specify M17 encoder SMS Message (truncates at 772)
                //  case 'S':
                //    strncpy(state.m17sms, optarg, 772);
                //    state.m17sms[772] = '\0';
                //    state.m17_str_dt = 3; //flip this so that STR encoder knows to use 1600 voice + data
                //    break;

            //specify TG Hold value
            case 'I':
                sscanf(optarg, "%u", &state.tg_hold);
                fprintf(stderr, "TG Hold set to %u \n", state.tg_hold);
                break;

            //experimental audio monitoring
            case '8':
                opts.monitor_input_audio = 1;
                fprintf(stderr, "Experimental Raw Analog Source Monitoring Enabled (Pulse Audio Only!)\n");
                break;

            //Enable optional retune from P25 LCW explicit updates (format 0x44)
            case 'j':
                opts.p25_lcw_retune = 1;
                fprintf(stderr, "P25: Enable LCW explicit retune (0x44).\n");
                break;
            case '^':
                opts.p25_prefer_candidates = 1;
                fprintf(stderr, "P25: Prefer CC candidates during hunt: On.\n");
                break;

            //rc4 enforcement on DMR (due to missing the PI header)
            case '0':
                state.M = 0x21;
                fprintf(stderr, "Force RC4 Key over Missing PI header/LE Encryption Identifiers (DMR)\n");
                break;

            //load single rc4/des key
            case '1':
                sscanf(optarg, "%llX", &state.R);
                state.RR = state.R; //put key on both sides
                fprintf(stderr, "RC4/DES Encryption Key Value set to 0x%llX \n", state.R);
                opts.unmute_encrypted_p25 = 0;
                state.keyloader = 0; //turn off keyloader
                break;

            //get user TYT BP Key and Force Its application
            case '2':
                state.tyt_bp = 1;
                sscanf(optarg, "%llX", &state.H);
                state.H = state.H & 0xFFFF; //truncate to 16-bits
                fprintf(stderr, "DMR TYT Basic 16-bit Key 0x%llX with Forced Application\n", state.H);
                break;

            //get user TYT AP Key and Force Its application
            case '!': tyt_ap_pc4_keystream_creation(&state, optarg); break;

            //get user Retevis AP Key and Force Its application
            case '@': retevis_rc2_keystream_creation(&state, optarg); break;

            //get user TYT EP Key and Force Its application
            case '5': tyt_ep_aes_keystream_creation(&state, optarg); break;

            //get user Kenwood DMR Scrambler Key and Force Its application
            case '9': ken_dmr_scrambler_keystream_creation(&state, optarg); break;

            //get user Anytone BP Key and Force Its application
            case 'A': anytone_bp_keystream_creation(&state, optarg); break;

            //Straight KS Generation
            case 'S': straight_mod_xor_keystream_creation(&state, optarg); break;

            case '3':
                opts.dmr_le = 0;
                fprintf(stderr, "DMRA Late Entry Encryption Identifiers Disabled\n");
                break;

            case 'y':                    //use experimental 'float' audio output
                opts.floating_point = 1; //enable floating point audio output
                fprintf(stderr, "Enabling Experimental Floating Point Audio Output\n");
                break;

            case 'Y':                  //conventional scanner mode
                opts.scanner_mode = 1; //enable scanner
                opts.p25_trunk = 0;    //turn off trunking mode if user enabled it
                break;

            case 'k': //multi-key loader (dec)
                strncpy(opts.key_in_file, optarg, 1023);
                opts.key_in_file[1023] = '\0';
                csvKeyImportDec(&opts, &state);
                state.keyloader = 1;
                break;

            case 'K': //multi-key loader (hex)
                strncpy(opts.key_in_file, optarg, 1023);
                opts.key_in_file[1023] = '\0';
                csvKeyImportHex(&opts, &state);
                state.keyloader = 1;
                break;

            case 'Q': //'DSP' Structured Output file for OKDMRlib
                sprintf(wav_file_directory, "./DSP");
                wav_file_directory[1023] = '\0';
                if (stat(wav_file_directory, &st) == -1) {
                    fprintf(stderr, "-Q %s DSP file directory does not exist\n", wav_file_directory);
                    fprintf(stderr, "Creating directory %s to save DSP Structured or M17 Binary Stream files\n",
                            wav_file_directory);
                    mkdir(wav_file_directory,
                          0700); //user read write execute, needs execute for some reason or segfault
                }
                //read in filename
                strncpy(dsp_filename, optarg, 1023);
                sprintf(opts.dsp_out_file, "%s/%s", wav_file_directory, dsp_filename);
                fprintf(stderr, "Saving DSP Structured or M17 Float Stream Output to %s\n", opts.dsp_out_file);
                opts.use_dsp_output = 1;
                break;

            case 'z':
                sscanf(optarg, "%d", &opts.slot_preference);
                if (opts.slot_preference > 0) {
                    opts.slot_preference--; //user inputs 1 or 2, internally we want 0 and 1
                }
                if (opts.slot_preference > 1) {
                    opts.slot_preference = 1;
                }
                fprintf(stderr, "TDMA (DMR and P2) Slot Voice Preference is Slot %d. \n", opts.slot_preference + 1);
                break;

            case 'n': //manually set analog audio output gain
                sscanf(optarg, "%f", &opts.audio_gainA);
                if (opts.audio_gainA > 100.0f) {
                    opts.audio_gainA = 100.0f;
                } else if (opts.audio_gainA < 0.0f) {
                    opts.audio_gainA = 0.0f;
                }
                fprintf(stderr, "Analog Audio Out Gain set to %f;\n", opts.audio_gainA);
                break;

            case 'V':
                sscanf(optarg, "%d", &slotson);
                if (slotson > 3) {
                    slotson = 3;
                }
                opts.slot1_on = (slotson & 1) >> 0;
                opts.slot2_on = (slotson & 2) >> 1;
                fprintf(stderr, "TDMA Voice Synthesis ");
                if (opts.slot1_on == 1) {
                    fprintf(stderr, "on Slot 1");
                }
                if (slotson == 3) {
                    fprintf(stderr, " and ");
                }
                if (opts.slot2_on == 1) {
                    fprintf(stderr, "on Slot 2");
                }

                if (slotson == 0) {
                    fprintf(stderr, "Disabled");
                }
                //disable slot preference if not 1 or 2
                if (slotson == 1 || slotson == 2) {
                    opts.slot_preference = 3;
                }
                fprintf(stderr, "\n");
                break;

            //Trunking - Use Group List as Allow List
            case 'W':
                opts.trunk_use_allow_list = 1;
                fprintf(stderr, "Using Group List as Allow/White List. \n");
                break;

            //Trunking - Tune Group Calls
            case 'E':
                opts.trunk_tune_group_calls = 0; //disable
                fprintf(stderr, "Disable Tuning to Group Calls. \n");
                break;

            //Trunking - Tune Private Calls
            case 'p':
                opts.trunk_tune_private_calls = 0; //disable
                fprintf(stderr, "Disable Tuning to Private Calls. \n");
                break;

            //Trunking - Tune Data Calls
            case 'e':
                opts.trunk_tune_data_calls = 1; //enable
                fprintf(stderr, "Enable Tuning to Data Calls. \n");
                break;

            case 'D': //user set DMRLA n value
                sscanf(optarg, "%c", &opts.dmr_dmrla_n);
                if (opts.dmr_dmrla_n > 10) {
                    opts.dmr_dmrla_n = 10; //max out at 10;
                }
                // if (opts.dmr_dmrla_n != 0) opts.dmr_dmrla_is_set = 1; //zero will fix a capmax site id value...I think
                opts.dmr_dmrla_is_set = 1;
                fprintf(stderr, "DMRLA n value set to %d. \n", opts.dmr_dmrla_n);
                break;

            case 'C': //new letter assignment for Channel import, flow down to allow temp numbers
                strncpy(opts.chan_in_file, optarg, 1023);
                opts.chan_in_file[1023] = '\0';
                csvChanImport(&opts, &state);
                break;

            case 'G': //new letter assignment for group import, flow down to allow temp numbers
                strncpy(opts.group_in_file, optarg, 1023);
                opts.group_in_file[1023] = '\0';
                csvGroupImport(&opts, &state);
                break;

            case 'T': //new letter assignment for trunking, flow down to allow temp numbers
                opts.p25_trunk = 1;
                opts.scanner_mode = 0; //turn off scanner mode if user enabled it
                break;

            case 'U': //New letter assignment for RIGCTL TCP port, flow down to allow temp numbers
                sscanf(optarg, "%d", &opts.rigctlportno);
                if (opts.rigctlportno != 0) {
                    opts.use_rigctl = 1;
                }
                break;

                //NOTE: I changed trunk_hangtime to a float, BUT! time(NULL) returns in second whole numbers
                //so using anything but whole numbers won't affect the outcome (rounded up?), in the future though,
                //may change to a different return on sync times and this will matter!

            case 't': //New letter assignment for Trunk Hangtime, flow down to allow temp numbers
                sscanf(optarg, "%f", &opts.trunk_hangtime); //updated for float/decimal values
                fprintf(stderr, "Trunking or Scanner Speed/Hang Time set to: %.02f sec\n", opts.trunk_hangtime);
                break;

            case 'q': //New letter assignment for Reverse Mute, flow down to allow temp numbers
                opts.reverse_mute = 1;
                fprintf(stderr, "Reverse Mute\n");
                break;

            case 'B': //New letter assignment for RIGCTL SetMod BW, flow down to allow temp numbers
                sscanf(optarg, "%d", &opts.setmod_bw);
                if (opts.setmod_bw > 25000) {
                    opts.setmod_bw = 25000; //not too high
                }
                break;

            case 's':
                sscanf(optarg, "%d", &opts.wav_sample_rate);
                opts.wav_interpolator = opts.wav_sample_rate / opts.wav_decimator;
                state.samplesPerSymbol = state.samplesPerSymbol * opts.wav_interpolator;
                state.symbolCenter = state.symbolCenter * opts.wav_interpolator;
                break;

            case 'v': //set filters via bitmap values 0b1111
            {
                unsigned int _audio_smoothing_tmp = 0;
                sscanf(optarg, "%x", &_audio_smoothing_tmp);
                state.audio_smoothing = (int)_audio_smoothing_tmp;
            }
                if (state.audio_smoothing & 1) {
                    opts.use_hpf_d = 1;
                } else {
                    opts.use_hpf_d = 0;
                }

                if (state.audio_smoothing & 2) {
                    opts.use_hpf = 1;
                } else {
                    opts.use_hpf = 0;
                }

                if (state.audio_smoothing & 4) {
                    opts.use_lpf = 1;
                } else {
                    opts.use_lpf = 0;
                }

                if (state.audio_smoothing & 8) {
                    opts.use_pbf = 1;
                } else {
                    opts.use_pbf = 0;
                }

                if (opts.use_hpf_d == 1) {
                    fprintf(stderr, "High Pass Filter on Digital Enabled\n");
                }
                if (opts.use_hpf == 1) {
                    fprintf(stderr, "High Pass Filter on Analog  Enabled\n");
                }
                if (opts.use_lpf == 1) {
                    fprintf(stderr, "Low  Pass Filter on Analog  Enabled\n");
                }
                if (opts.use_pbf == 1) {
                    fprintf(stderr, "Pass Band Filter on Analog  Enabled\n");
                }

                break;

            case 'b': //formerly Capital 'K'
                sscanf(optarg, "%llu", &state.K);
                if (state.K > 256) {
                    state.K = 256;
                }
                opts.dmr_mute_encL = 0;
                opts.dmr_mute_encR = 0;
                if (state.K == 0) {
                    opts.dmr_mute_encL = 1;
                    opts.dmr_mute_encR = 1;
                }
                break;

            case 'R':
                sscanf(optarg, "%llu", &state.R);
                if (state.R > 0x7FFF) {
                    state.R = 0x7FFF;
                }
                //disable keyloader in case user tries to use this and it at the same time
                state.keyloader = 0;
                break;

            case 'H':
                //new handling for 10/32/64 Char Key

                strncpy(opts.szNumbers, optarg, 1023);
                opts.szNumbers[1023] = '\0';
                state.K1 = strtoull(opts.szNumbers, &pEnd, 16);
                state.K2 = strtoull(pEnd, &pEnd, 16);
                state.K3 = strtoull(pEnd, &pEnd, 16);
                state.K4 = strtoull(pEnd, &pEnd, 16);
                fprintf(stderr, "Hytera40/128/256 BP or AES128/256 Key = %016llX %016llX %016llX %016llX\n", state.K1,
                        state.K2, state.K3, state.K4);
                opts.dmr_mute_encL = 0;
                opts.dmr_mute_encR = 0;
                if (state.K1 == 0 && state.K2 == 0 && state.K3 == 0 && state.K4 == 0) {
                    opts.dmr_mute_encL = 1;
                    opts.dmr_mute_encR = 1;
                }
                state.H = state.K1; //shim still required?
                //load the AES keys into a seperate handler
                state.A1[0] = state.A1[1] = state.K1;
                state.A2[0] = state.A2[1] = state.K2;
                state.A3[0] = state.A3[1] = state.K3;
                state.A4[0] = state.A4[1] = state.K4;
                //disable keyloader function
                state.keyloader = 0;
                //signal an aes key is loaded into each slot -- if no value presented, then this won't matter anyways
                state.aes_key_loaded[0] = state.aes_key_loaded[1] = 1;
                break;

            case '4':
                state.M = 1;
                fprintf(stderr, "Force Privacy Key over Encryption Identifiers (DMR BP and NXDN Scrambler) \n");
                break;

            //manually set Phase 2 TDMA WACN/SYSID/CC
            case 'X':
                sscanf(optarg, "%llX", &p2vars);
                if (p2vars > 0) {
                    state.p2_wacn = p2vars >> 24;
                    state.p2_sysid = (p2vars >> 12) & 0xFFF;
                    state.p2_cc = p2vars & 0xFFF;
                }
                if (state.p2_wacn != 0 && state.p2_sysid != 0 && state.p2_cc != 0) {
                    state.p2_hardset = 1;
                }
                break;

            case 'N':
                opts.use_ncurses_terminal = 1;
                fprintf(stderr, "Enabling NCurses Terminal.\n");
                break;

            case 'Z':
                opts.payload = 1;
                fprintf(stderr, "Logging Frame Payload to console\n");
                break;

            case 'L': //LRRP output to file
                strncpy(opts.lrrp_out_file, optarg, 1023);
                opts.lrrp_out_file[1023] = '\0';
                opts.lrrp_file_output = 1;
                fprintf(stderr, "Writing + Appending LRRP data to file %s\n", opts.lrrp_out_file);
                break;

            case 'J': //Event output to file (J for Journal)
                strncpy(opts.event_out_file, optarg, 1023);
                opts.event_out_file[1023] = '\0';
                fprintf(stderr, "Writing + Appending Event History to file %s\n", opts.event_out_file);
                break;

            case '7': //make a custom wav file directory in the current working directory -- use this before -P
                strncpy(opts.wav_out_dir, optarg, 511);
                opts.wav_out_dir[511] = '\0';
                if (stat(opts.wav_out_dir, &st) == -1) {
                    fprintf(stderr, "%s directory does not exist\n", opts.wav_out_dir);
                    fprintf(stderr, "Creating directory %s to save wav files\n", opts.wav_out_dir);
                    mkdir(opts.wav_out_dir, 0700);
                } else {
                    fprintf(stderr, "Writing wav decoded audio files to directory %s\n", opts.wav_out_dir);
                }
                break;

            case 'P': //Per Call Wav Files
                sprintf(wav_file_directory, "%s", opts.wav_out_dir);
                wav_file_directory[1023] = '\0';
                if (stat(wav_file_directory, &st) == -1) {
                    fprintf(stderr, "-P %s WAV file directory does not exist\n", wav_file_directory);
                    fprintf(stderr, "Creating directory %s to save decoded wav files\n", wav_file_directory);
                    mkdir(wav_file_directory,
                          0700); //user read write execute, needs execute for some reason or segfault
                }
                fprintf(stderr, "Per Call Wav File Enabled.\n");
                srand(time(
                    NULL)); //seed random for filenames (so two filenames aren't the exact same datetime string on initailization)
                opts.wav_out_f = open_wav_file(opts.wav_out_dir, opts.wav_out_file, 8000, 0);
                opts.wav_out_fR = open_wav_file(opts.wav_out_dir, opts.wav_out_fileR, 8000, 0);
                opts.dmr_stereo_wav = 1;
                break;

            case 'F':
                opts.aggressive_framesync = 0;
                fprintf(stderr, "%s", KYEL);
                //fprintf (stderr,"DMR Stereo Aggressive Resync Disabled!\n");
                fprintf(stderr, "Relax P25 Phase 2 MAC_SIGNAL CRC Checksum Pass/Fail\n");
                fprintf(stderr, "Relax DMR RAS/CRC CSBK/DATA Pass/Fail\n");
                fprintf(stderr, "Relax NXDN SACCH/FACCH/CAC/F2U CRC Pass/Fail\n");
                fprintf(stderr, "Relax M17 LSF/PKT CRC Pass/Fail\n");
                fprintf(stderr, "%s", KNRM);
                break;

            case 'i':
                strncpy(opts.audio_in_dev, optarg, 2047);
                opts.audio_in_dev[2047] = '\0';
                break;

            case 'o':
                strncpy(opts.audio_out_dev, optarg, 1023);
                opts.audio_out_dev[1023] = '\0';
                break;

            case 'd':
                strncpy(opts.mbe_out_dir, optarg, 1023);
                opts.mbe_out_dir[1023] = '\0';
                if (stat(opts.mbe_out_dir, &st) == -1) {
                    fprintf(stderr, "-d %s directory does not exist\n", opts.mbe_out_dir);
                    fprintf(stderr, "Creating directory %s to save MBE files\n", opts.mbe_out_dir);
                    mkdir(opts.mbe_out_dir, 0700); //user read write execute, needs execute for some reason or segfault
                } else {
                    fprintf(stderr, "Writing mbe data files to directory %s\n", opts.mbe_out_dir);
                }
                break;

            case 'c':
                strncpy(opts.symbol_out_file, optarg, 1023);
                opts.symbol_out_file[1023] = '\0';
                fprintf(stderr, "Writing symbol capture to file %s\n", opts.symbol_out_file);
                openSymbolOutFile(&opts, &state);
                break;

            case 'g':
                sscanf(optarg, "%f", &opts.audio_gain);

                if (opts.audio_gain < (float)0) {
                    fprintf(stderr, "Disabling audio out gain setting\n");
                    opts.audio_gainR = opts.audio_gain;
                } else if (opts.audio_gain == (float)0) {
                    opts.audio_gain = (float)0;
                    opts.audio_gainR = opts.audio_gain;
                    fprintf(stderr, "Enabling audio out auto-gain\n");
                } else {
                    fprintf(stderr, "Setting audio out gain to %f\n", opts.audio_gain);
                    opts.audio_gainR = opts.audio_gain;
                    state.aout_gain = opts.audio_gain;
                    state.aout_gainR = opts.audio_gain;
                }
                break;

            case 'w': //experimental re-enabling (needs thorough testing)
                strncpy(opts.wav_out_file, optarg, 1023);
                opts.wav_out_file[1023] = '\0';
                fprintf(stderr, "Writing + Appending decoded audio to file %s\n", opts.wav_out_file);
                opts.dmr_stereo_wav = 0;
                opts.static_wav_file = 1;
                openWavOutFileLR(&opts, &state);
                break;

            case '6':
                strncpy(opts.wav_out_file_raw, optarg, 1023);
                opts.wav_out_file_raw[1023] = '\0';
                fprintf(stderr, "Writing raw audio to file %s\n", opts.wav_out_file_raw);
                openWavOutFileRaw(&opts, &state);
                break;

            case 'f':
                if (optarg[0] == 'a') //
                {
                    opts.frame_dstar = 1;
                    opts.frame_x2tdma = 1;
                    opts.frame_p25p1 = 1;
                    opts.frame_p25p2 = 1;
                    opts.inverted_p2 = 0;
                    opts.frame_nxdn48 = 0;
                    opts.frame_nxdn96 = 0;
                    opts.frame_dmr = 1;
                    opts.frame_dpmr = 0;
                    opts.frame_provoice = 0;
                    opts.frame_ysf = 1;
                    opts.frame_m17 = 0;
                    opts.mod_c4fm = 1;
                    opts.mod_qpsk = 0;
                    state.rf_mod = 0;
                    opts.dmr_stereo = 1;
                    opts.dmr_mono = 0;
                    // opts.setmod_bw = 12000; //safe default on both DMR and P25
                    opts.pulse_digi_rate_out = 8000;
                    opts.pulse_digi_out_channels = 2;
                    sprintf(opts.output_name, "AUTO");
                    fprintf(stderr, "Decoding AUTO P25, YSF, DSTAR, X2-TDMA, and DMR\n");
                } else if (optarg[0]
                           == 'A') //activate analog out and passively monitor it, useful for analog scanning, etc
                {
                    opts.frame_dstar = 0;
                    opts.frame_x2tdma = 0;
                    opts.frame_p25p1 = 0;
                    opts.frame_p25p2 = 0;
                    opts.frame_nxdn48 = 0;
                    opts.frame_nxdn96 = 0;
                    opts.frame_dmr = 0;
                    opts.frame_dpmr = 0;
                    opts.frame_provoice = 0;
                    opts.frame_ysf = 0;
                    opts.frame_m17 = 0;
                    opts.pulse_digi_rate_out = 8000;
                    opts.pulse_digi_out_channels = 1;
                    opts.dmr_stereo = 0;
                    state.dmr_stereo = 0;
                    opts.dmr_mono = 0;
                    state.rf_mod = 0;
                    opts.monitor_input_audio = 1;
                    opts.analog_only = 1;
                    sprintf(opts.output_name, "Analog Monitor");
                    fprintf(stderr, "Only Monitoring Passive Analog Signal\n");
                } else if (optarg[0] == 'd') {
                    opts.frame_dstar = 1;
                    opts.frame_x2tdma = 0;
                    opts.frame_p25p1 = 0;
                    opts.frame_p25p2 = 0;
                    opts.frame_nxdn48 = 0;
                    opts.frame_nxdn96 = 0;
                    opts.frame_dmr = 0;
                    opts.frame_dpmr = 0;
                    opts.frame_provoice = 0;
                    opts.frame_ysf = 0;
                    opts.frame_m17 = 0;
                    opts.pulse_digi_rate_out = 8000;
                    opts.pulse_digi_out_channels = 1;
                    opts.dmr_stereo = 0;
                    state.dmr_stereo = 0;
                    opts.dmr_mono = 0;
                    state.rf_mod = 0;
                    sprintf(opts.output_name, "DSTAR");
                    fprintf(stderr, "Decoding only DSTAR frames.\n");
                } else if (optarg[0] == 'x') {
                    opts.frame_dstar = 0;
                    opts.frame_x2tdma = 1;
                    opts.frame_p25p1 = 0;
                    opts.frame_p25p2 = 0;
                    opts.frame_nxdn48 = 0;
                    opts.frame_nxdn96 = 0;
                    opts.frame_dmr = 0;
                    opts.frame_dpmr = 0;
                    opts.frame_provoice = 0;
                    opts.frame_ysf = 0;
                    opts.frame_m17 = 0;
                    opts.pulse_digi_rate_out = 8000;
                    opts.pulse_digi_out_channels = 2;
                    opts.dmr_stereo = 0;
                    opts.dmr_mono = 0;
                    state.dmr_stereo = 0;
                    sprintf(opts.output_name, "X2-TDMA");
                    fprintf(stderr, "Decoding only X2-TDMA frames.\n");
                } else if (optarg[0] == 'p') {
                    opts.frame_dstar = 0;
                    opts.frame_x2tdma = 0;
                    opts.frame_p25p1 = 0;
                    opts.frame_p25p2 = 0;
                    opts.frame_nxdn48 = 0;
                    opts.frame_nxdn96 = 0;
                    opts.frame_dmr = 0;
                    opts.frame_dpmr = 0;
                    opts.frame_provoice = 1;
                    opts.frame_ysf = 0;
                    opts.frame_m17 = 0;
                    state.samplesPerSymbol = 5;
                    state.symbolCenter = 2;
                    opts.mod_c4fm = 0;
                    opts.mod_qpsk = 0;
                    opts.mod_gfsk = 1;
                    state.rf_mod = 2;
                    opts.pulse_digi_rate_out = 8000;
                    opts.pulse_digi_out_channels = 1;
                    opts.dmr_stereo = 0;
                    opts.dmr_mono = 0;
                    state.dmr_stereo = 0;
                    // opts.setmod_bw = 16000;
                    sprintf(opts.output_name, "EDACS/PV");
                    fprintf(stderr, "Setting symbol rate to 9600 / second\n");
                    fprintf(stderr, "Decoding only ProVoice frames.\n");
                    fprintf(stderr, "EDACS Analog Voice Channels are Experimental.\n");
                    //misc tweaks
                    opts.rtl_bandwidth = 24;
                } else if (optarg[0] == 'h') //standard / net w/o ESK
                {
                    // does it make sense to do it this way?
                    if (optarg[1] != 0) {
                        char abits[2];
                        abits[0] = optarg[1];
                        abits[1] = 0;
                        char fbits[2];
                        fbits[0] = optarg[2];
                        fbits[1] = 0;
                        char sbits[2];
                        sbits[0] = optarg[3];
                        sbits[1] = 0;
                        state.edacs_a_bits = atoi(&abits[0]);
                        state.edacs_f_bits = atoi(&fbits[0]);
                        state.edacs_s_bits = atoi(&sbits[0]);
                    }
                    opts.frame_dstar = 0;
                    opts.frame_x2tdma = 0;
                    opts.frame_p25p1 = 0;
                    opts.frame_p25p2 = 0;
                    opts.frame_nxdn48 = 0;
                    opts.frame_nxdn96 = 0;
                    opts.frame_dmr = 0;
                    opts.frame_dpmr = 0;
                    opts.frame_provoice = 1;
                    state.ea_mode = 0;
                    state.esk_mask = 0;
                    opts.frame_ysf = 0;
                    opts.frame_m17 = 0;
                    state.samplesPerSymbol = 5;
                    state.symbolCenter = 2;
                    opts.mod_c4fm = 0;
                    opts.mod_qpsk = 0;
                    opts.mod_gfsk = 1;
                    state.rf_mod = 2;
                    opts.pulse_digi_rate_out = 8000;
                    opts.pulse_digi_out_channels = 1;
                    opts.dmr_stereo = 0;
                    opts.dmr_mono = 0;
                    state.dmr_stereo = 0;
                    // opts.setmod_bw = 12500;
                    sprintf(opts.output_name, "EDACS/PV");
                    fprintf(stderr, "Setting symbol rate to 9600 / second\n");
                    fprintf(stderr, "Decoding EDACS STD/NET and ProVoice frames.\n");
                    fprintf(stderr, "EDACS Analog Voice Channels are Experimental.\n");
                    //sanity check, make sure we tally up to 11 bits, or set to default values
                    if (optarg[1] != 0) {
                        if ((state.edacs_a_bits + state.edacs_f_bits + state.edacs_s_bits) != 11) {
                            fprintf(stderr, "Invalid AFS Configuration: Reverting to Default.\n");
                            state.edacs_a_bits = 4;
                            state.edacs_f_bits = 4;
                            state.edacs_s_bits = 3;
                        }
                        fprintf(stderr, "AFS Setup in %d:%d:%d configuration.\n", state.edacs_a_bits,
                                state.edacs_f_bits, state.edacs_s_bits);
                    }
                    //rtl specific tweaks
                    opts.rtl_bandwidth = 24;
                    // opts.rtl_gain_value = 36;
                } else if (optarg[0] == 'H') //standard / net w/ ESK
                {
                    // does it make sense to do it this way?
                    if (optarg[1] != 0) {
                        char abits[2];
                        abits[0] = optarg[1];
                        abits[1] = 0;
                        char fbits[2];
                        fbits[0] = optarg[2];
                        fbits[1] = 0;
                        char sbits[2];
                        sbits[0] = optarg[3];
                        sbits[1] = 0;
                        state.edacs_a_bits = atoi(&abits[0]);
                        state.edacs_f_bits = atoi(&fbits[0]);
                        state.edacs_s_bits = atoi(&sbits[0]);
                    }
                    opts.frame_dstar = 0;
                    opts.frame_x2tdma = 0;
                    opts.frame_p25p1 = 0;
                    opts.frame_p25p2 = 0;
                    opts.frame_nxdn48 = 0;
                    opts.frame_nxdn96 = 0;
                    opts.frame_dmr = 0;
                    opts.frame_dpmr = 0;
                    opts.frame_provoice = 1;
                    state.ea_mode = 0;
                    state.esk_mask = 0xA0;
                    opts.frame_ysf = 0;
                    opts.frame_m17 = 0;
                    state.samplesPerSymbol = 5;
                    state.symbolCenter = 2;
                    opts.mod_c4fm = 0;
                    opts.mod_qpsk = 0;
                    opts.mod_gfsk = 1;
                    state.rf_mod = 2;
                    opts.pulse_digi_rate_out = 8000;
                    opts.pulse_digi_out_channels = 1;
                    opts.dmr_stereo = 0;
                    opts.dmr_mono = 0;
                    state.dmr_stereo = 0;
                    // opts.setmod_bw = 12500;
                    sprintf(opts.output_name, "EDACS/PV");
                    fprintf(stderr, "Setting symbol rate to 9600 / second\n");
                    fprintf(stderr, "Decoding EDACS STD/NET w/ ESK and ProVoice frames.\n");
                    fprintf(stderr, "EDACS Analog Voice Channels are Experimental.\n");
                    //sanity check, make sure we tally up to 11 bits, or set to default values
                    if (optarg[1] != 0) {
                        if ((state.edacs_a_bits + state.edacs_f_bits + state.edacs_s_bits) != 11) {
                            fprintf(stderr, "Invalid AFS Configuration: Reverting to Default.\n");
                            state.edacs_a_bits = 4;
                            state.edacs_f_bits = 4;
                            state.edacs_s_bits = 3;
                        }
                        fprintf(stderr, "AFS Setup in %d:%d:%d configuration.\n", state.edacs_a_bits,
                                state.edacs_f_bits, state.edacs_s_bits);
                    }
                    //rtl specific tweaks
                    opts.rtl_bandwidth = 24;
                    // opts.rtl_gain_value = 36;
                } else if (optarg[0] == 'e') //extended addressing w/o ESK
                {
                    opts.frame_dstar = 0;
                    opts.frame_x2tdma = 0;
                    opts.frame_p25p1 = 0;
                    opts.frame_p25p2 = 0;
                    opts.frame_nxdn48 = 0;
                    opts.frame_nxdn96 = 0;
                    opts.frame_dmr = 0;
                    opts.frame_dpmr = 0;
                    opts.frame_provoice = 1;
                    state.ea_mode = 1;
                    state.esk_mask = 0;
                    opts.frame_ysf = 0;
                    opts.frame_m17 = 0;
                    state.samplesPerSymbol = 5;
                    state.symbolCenter = 2;
                    opts.mod_c4fm = 0;
                    opts.mod_qpsk = 0;
                    opts.mod_gfsk = 1;
                    state.rf_mod = 2;
                    opts.pulse_digi_rate_out = 8000;
                    opts.pulse_digi_out_channels = 1;
                    opts.dmr_stereo = 0;
                    opts.dmr_mono = 0;
                    state.dmr_stereo = 0;
                    // opts.setmod_bw = 12500;
                    sprintf(opts.output_name, "EDACS/PV");
                    fprintf(stderr, "Setting symbol rate to 9600 / second\n");
                    fprintf(stderr, "Decoding EDACS Extended Addressing and ProVoice frames.\n");
                    fprintf(stderr, "EDACS Analog Voice Channels are Experimental.\n");
                    //rtl specific tweaks
                    opts.rtl_bandwidth = 24;
                    // opts.rtl_gain_value = 36;
                } else if (optarg[0] == 'E') //extended addressing w/ ESK
                {
                    opts.frame_dstar = 0;
                    opts.frame_x2tdma = 0;
                    opts.frame_p25p1 = 0;
                    opts.frame_p25p2 = 0;
                    opts.frame_nxdn48 = 0;
                    opts.frame_nxdn96 = 0;
                    opts.frame_dmr = 0;
                    opts.frame_dpmr = 0;
                    opts.frame_provoice = 1;
                    state.ea_mode = 1;
                    state.esk_mask = 0xA0;
                    opts.frame_ysf = 0;
                    opts.frame_m17 = 0;
                    state.samplesPerSymbol = 5;
                    state.symbolCenter = 2;
                    opts.mod_c4fm = 0;
                    opts.mod_qpsk = 0;
                    opts.mod_gfsk = 1;
                    state.rf_mod = 2;
                    opts.pulse_digi_rate_out = 8000;
                    opts.pulse_digi_out_channels = 1;
                    opts.dmr_stereo = 0;
                    opts.dmr_mono = 0;
                    state.dmr_stereo = 0;
                    // opts.setmod_bw = 12500;
                    sprintf(opts.output_name, "EDACS/PV");
                    fprintf(stderr, "Setting symbol rate to 9600 / second\n");
                    fprintf(stderr, "Decoding EDACS Extended Addressing w/ ESK and ProVoice frames.\n");
                    fprintf(stderr, "EDACS Analog Voice Channels are Experimental.\n");
                    //rtl specific tweaks
                    opts.rtl_bandwidth = 24;
                    // opts.rtl_gain_value = 36;
                } else if (optarg[0] == '1') {
                    opts.frame_dstar = 0;
                    opts.frame_x2tdma = 0;
                    opts.frame_p25p1 = 1;
                    opts.frame_p25p2 = 0;
                    opts.frame_nxdn48 = 0;
                    opts.frame_nxdn96 = 0;
                    opts.frame_dmr = 0;
                    opts.frame_dpmr = 0;
                    opts.frame_provoice = 0;
                    opts.frame_ysf = 0;
                    opts.frame_m17 = 0;
                    opts.dmr_stereo = 0;
                    state.dmr_stereo = 0;
                    opts.mod_c4fm = 1;
                    opts.mod_qpsk = 0;
                    opts.mod_gfsk = 0;
                    state.rf_mod = 0; //
                    opts.dmr_stereo = 0;
                    opts.dmr_mono = 0;
                    opts.pulse_digi_rate_out = 8000;
                    opts.pulse_digi_out_channels = 1;
                    // opts.setmod_bw = 12000;
                    opts.ssize = 36; //128 current default, fall back to old default on P1 only systems
                    opts.msize = 15; //1024 current default, fall back to old default on P1 only systems
                    // opts.use_heuristics = 1; //Causes issues with Voice Wide
                    sprintf(opts.output_name, "P25p1");
                    fprintf(stderr, "Decoding only P25 Phase 1 frames.\n");
                } else if (optarg[0] == 'i') {
                    opts.frame_dstar = 0;
                    opts.frame_x2tdma = 0;
                    opts.frame_p25p1 = 0;
                    opts.frame_p25p2 = 0;
                    opts.frame_nxdn48 = 1;
                    opts.frame_nxdn96 = 0;
                    opts.frame_dmr = 0;
                    opts.frame_dpmr = 0;
                    opts.frame_provoice = 0;
                    opts.frame_ysf = 0;
                    opts.frame_m17 = 0;
                    state.samplesPerSymbol = 20;
                    state.symbolCenter = 10;
                    opts.mod_c4fm = 1;
                    opts.mod_qpsk = 0;
                    opts.mod_gfsk = 0;
                    state.rf_mod = 0;
                    opts.pulse_digi_rate_out = 8000;
                    opts.pulse_digi_out_channels = 1;
                    opts.dmr_stereo = 0;
                    state.dmr_stereo = 0;
                    opts.dmr_mono = 0;
                    // opts.setmod_bw = 4000; //causing issues
                    sprintf(opts.output_name, "NXDN48");
                    fprintf(stderr, "Setting symbol rate to 2400 / second\n");
                    fprintf(stderr, "Decoding only NXDN 4800 baud frames.\n");
                } else if (optarg[0] == 'y') {
                    opts.frame_dstar = 0;
                    opts.frame_x2tdma = 0;
                    opts.frame_p25p1 = 0;
                    opts.frame_p25p2 = 0;
                    opts.frame_p25p2 = 0;
                    opts.frame_nxdn48 = 0;
                    opts.frame_nxdn96 = 0;
                    opts.frame_dmr = 0;
                    opts.frame_dpmr = 0;
                    opts.frame_provoice = 0;
                    opts.frame_ysf = 1;
                    opts.frame_m17 = 0;
                    opts.mod_c4fm = 1;
                    opts.mod_qpsk = 0;
                    opts.mod_gfsk = 0;
                    state.rf_mod = 0;
                    opts.pulse_digi_rate_out = 8000;
                    opts.pulse_digi_out_channels = 1;
                    opts.dmr_stereo = 0;
                    state.dmr_stereo = 0;
                    opts.dmr_mono = 0;
                    sprintf(opts.output_name, "YSF");
                    fprintf(stderr, "Decoding only YSF frames. \n");
                } else if (optarg[0] == '2') {
                    opts.frame_dstar = 0;
                    opts.frame_x2tdma = 0;
                    opts.frame_p25p1 = 0;
                    opts.frame_p25p2 = 1;
                    opts.frame_nxdn48 = 0;
                    opts.frame_nxdn96 = 0;
                    opts.frame_dmr = 0;
                    opts.frame_dpmr = 0;
                    opts.frame_provoice = 0;
                    opts.frame_ysf = 0;
                    opts.frame_m17 = 0;
                    state.samplesPerSymbol = 8;
                    state.symbolCenter = 3;
                    opts.mod_c4fm = 1;
                    opts.mod_qpsk = 0;
                    opts.mod_gfsk = 0;
                    state.rf_mod = 0;
                    opts.dmr_stereo = 1;
                    state.dmr_stereo = 0;
                    opts.dmr_mono = 0;
                    // opts.setmod_bw = 12000;
                    sprintf(opts.output_name, "P25p2");
                    fprintf(stderr, "Decoding 6000 sps P25p2 frames only!\n");
                } else if (optarg[0] == 's') {
                    opts.frame_dstar = 0;
                    opts.frame_x2tdma = 0;
                    opts.frame_p25p1 = 0;
                    opts.frame_p25p2 = 0;
                    opts.inverted_p2 = 0;
                    opts.frame_nxdn48 = 0;
                    opts.frame_nxdn96 = 0;
                    opts.frame_dmr = 1;
                    opts.frame_dpmr = 0;
                    opts.frame_provoice = 0;
                    opts.frame_ysf = 0;
                    opts.frame_m17 = 0;
                    opts.mod_c4fm = 1;
                    opts.mod_qpsk = 0;
                    opts.mod_gfsk = 0;
                    state.rf_mod = 0;
                    opts.dmr_stereo = 1;
                    opts.dmr_mono = 0;
                    // opts.setmod_bw = 7000;
                    opts.pulse_digi_rate_out = 8000;
                    opts.pulse_digi_out_channels = 2;
                    sprintf(opts.output_name, "DMR");

                    fprintf(stderr, "Decoding DMR BS/MS Simplex\n");
                }
                //change ft to only do P25 and DMR (TDMA trunking modes)
                else if (optarg[0] == 't') {
                    opts.frame_dstar = 0;
                    opts.frame_x2tdma = 0;
                    opts.frame_p25p1 = 1;
                    opts.frame_p25p2 = 1;
                    opts.inverted_p2 = 0;
                    opts.frame_nxdn48 = 0;
                    opts.frame_nxdn96 = 0;
                    opts.frame_dmr = 1;
                    opts.frame_dpmr = 0;
                    opts.frame_provoice = 0;
                    opts.frame_ysf = 0;
                    opts.frame_m17 = 0;
                    opts.mod_c4fm = 1;
                    opts.mod_qpsk = 0;
                    opts.mod_gfsk = 0;
                    //Need a new demodulator is needed to handle p2 cqpsk
                    //or consider using an external modulator (GNURadio?)
                    state.rf_mod = 0;
                    opts.dmr_stereo = 1;
                    opts.dmr_mono = 0;
                    // opts.setmod_bw = 12000; //safe default on both DMR and P25
                    opts.pulse_digi_rate_out = 8000;
                    opts.pulse_digi_out_channels = 2;
                    // opts.use_heuristics = 1; //Causes issues with Voice Wide
                    sprintf(opts.output_name, "TDMA");
                    fprintf(stderr, "Decoding P25 and DMR\n");
                } else if (optarg[0] == 'n') {
                    opts.frame_dstar = 0;
                    opts.frame_x2tdma = 0;
                    opts.frame_p25p1 = 0;
                    opts.frame_p25p2 = 0;
                    opts.frame_nxdn48 = 0;
                    opts.frame_nxdn96 = 1;
                    opts.frame_dmr = 0;
                    opts.frame_dpmr = 0;
                    opts.frame_provoice = 0;
                    opts.frame_ysf = 0;
                    opts.frame_m17 = 0;
                    opts.mod_c4fm = 1;
                    opts.mod_qpsk = 0;
                    opts.mod_gfsk = 0;
                    state.rf_mod = 0;
                    opts.pulse_digi_rate_out = 8000;
                    opts.pulse_digi_out_channels = 1;
                    opts.dmr_stereo = 0;
                    opts.dmr_mono = 0;
                    state.dmr_stereo = 0;
                    // opts.setmod_bw = 12000; //causing issues
                    sprintf(opts.output_name, "NXDN96");
                    fprintf(stderr, "Decoding only NXDN 9600 baud frames.\n");
                } else if (optarg[0] == 'r') {
                    opts.frame_dstar = 0;
                    opts.frame_x2tdma = 0;
                    opts.frame_p25p1 = 0;
                    opts.frame_p25p2 = 0;
                    opts.frame_nxdn48 = 0;
                    opts.frame_nxdn96 = 0;
                    opts.frame_dmr = 1;
                    opts.frame_dpmr = 0;
                    opts.frame_provoice = 0;
                    opts.frame_ysf = 0;
                    opts.frame_m17 = 0;
                    opts.mod_c4fm = 1;
                    opts.mod_qpsk = 0;
                    opts.mod_gfsk = 0; //
                    state.rf_mod = 0;  //
                    opts.pulse_digi_rate_out = 8000;
                    opts.pulse_digi_out_channels = 2;
                    opts.dmr_mono = 0;
                    opts.dmr_stereo = 1;
                    state.dmr_stereo = 0; //0
                    // opts.setmod_bw = 7000;
                    sprintf(opts.output_name, "DMR");
                    fprintf(stderr, "-fr / DMR Mono switch has been deprecated.\n");
                    fprintf(stderr, "Decoding DMR BS/MS Simplex\n");

                } else if (optarg[0] == 'm') {
                    opts.frame_dstar = 0;
                    opts.frame_x2tdma = 0;
                    opts.frame_p25p1 = 0;
                    opts.frame_p25p2 = 0;
                    opts.frame_nxdn48 = 0;
                    opts.frame_nxdn96 = 0;
                    opts.frame_dmr = 0;
                    opts.frame_provoice = 0;
                    opts.frame_dpmr = 1;
                    opts.frame_ysf = 0;
                    opts.frame_m17 = 0;
                    state.samplesPerSymbol = 20; //same as NXDN48 - 20
                    state.symbolCenter = 10;     //same as NXDN48 - 10
                    opts.mod_c4fm = 1;
                    opts.mod_qpsk = 0;
                    opts.mod_gfsk = 0;
                    state.rf_mod = 0;
                    opts.pulse_digi_rate_out = 8000;
                    opts.pulse_digi_out_channels = 1;
                    opts.dmr_stereo = 0;
                    opts.dmr_mono = 0;
                    state.dmr_stereo = 0;
                    sprintf(opts.output_name, "dPMR");
                    fprintf(
                        stderr,
                        "Notice: dPMR cannot autodetect polarity. \n Use -xd option if Inverted Signal expected.\n");
                    fprintf(stderr, "Decoding only dPMR frames.\n");
                } else if (optarg[0] == 'z') //placeholder letter
                {
                    opts.frame_dstar = 0;
                    opts.frame_x2tdma = 0;
                    opts.frame_p25p1 = 0;
                    opts.frame_p25p2 = 0;
                    opts.frame_nxdn48 = 0;
                    opts.frame_nxdn96 = 0;
                    opts.frame_dmr = 0;
                    opts.frame_provoice = 0;
                    opts.frame_dpmr = 0;
                    opts.frame_ysf = 0;
                    opts.frame_m17 = 1;
                    opts.mod_c4fm = 1;
                    opts.mod_qpsk = 0;
                    opts.mod_gfsk = 0;
                    state.rf_mod = 0;
                    opts.pulse_digi_rate_out = 8000;
                    opts.pulse_digi_out_channels = 1;
                    opts.dmr_stereo = 0;
                    opts.dmr_mono = 0;
                    state.dmr_stereo = 0;
                    sprintf(opts.output_name, "M17");
                    fprintf(stderr,
                            "Notice: M17 cannot autodetect polarity. \n Use -xz option if Inverted Signal expected.\n");
                    fprintf(stderr, "Decoding only M17 frames.\n");

                    //disable RRC filter for now
                    opts.use_cosine_filter = 0;
                } else if (optarg[0] == 'Z') //Captial Z to Run the M17 STR encoder
                {
                    opts.m17encoder = 1;
                    opts.pulse_digi_rate_out = 48000;
                    opts.pulse_digi_out_channels = 1;
                    //filters disabled by default, use ncurses VBN switches
                    opts.use_lpf = 0;
                    opts.use_hpf = 0;
                    opts.use_pbf = 0;
                    opts.dmr_stereo = 0;
                    sprintf(opts.output_name, "M17 Encoder");
                } else if (optarg[0] == 'B') //Captial B to Run the M17 BRT encoder
                {
                    opts.m17encoderbrt = 1;
                    opts.pulse_digi_rate_out = 48000;
                    opts.pulse_digi_out_channels = 1;
                    sprintf(opts.output_name, "M17 BERT");
                } else if (optarg[0] == 'P') //Captial P to Run the M17 PKT encoder
                {
                    opts.m17encoderpkt = 1;
                    opts.pulse_digi_rate_out = 48000;
                    opts.pulse_digi_out_channels = 1;
                    sprintf(opts.output_name, "M17 Packet");
                } else if (optarg[0] == 'U') //Captial U to Run the M17 UDP IPF decoder
                {
                    opts.m17decoderip = 1;
                    opts.pulse_digi_rate_out = 8000;
                    opts.pulse_digi_out_channels = 1;
                    sprintf(opts.output_name, "M17 IP Frame");
                    fprintf(stderr, "Decoding M17 UDP/IP Frames.\n");
                }
                break;
            //don't mess with the modulations unless you really need to
            case 'm':
                if (optarg[0] == 'a') {
                    opts.mod_c4fm = 1;
                    opts.mod_qpsk = 1;
                    opts.mod_gfsk = 1;
                    state.rf_mod = 0;
                    fprintf(stderr, "Don't use the -ma switch.\n");
                } else if (optarg[0] == 'c') {
                    opts.mod_c4fm = 1;
                    opts.mod_qpsk = 0;
                    opts.mod_gfsk = 0;
                    state.rf_mod = 0;
                    fprintf(stderr, "Enabling only C4FM modulation optimizations.\n");
                } else if (optarg[0] == 'g') {
                    opts.mod_c4fm = 0;
                    opts.mod_qpsk = 0;
                    opts.mod_gfsk = 1;
                    state.rf_mod = 2;
                    fprintf(stderr, "Enabling only GFSK modulation optimizations.\n");
                } else if (optarg[0] == 'q') {
                    opts.mod_c4fm = 0;
                    opts.mod_qpsk = 1;
                    opts.mod_gfsk = 0;
                    state.rf_mod = 1;
                    // opts.setmod_bw = 12000;
                    fprintf(stderr, "Enabling only QPSK modulation optimizations.\n");
                } else if (optarg[0] == '2') {
                    opts.mod_c4fm = 0;
                    opts.mod_qpsk = 1;
                    opts.mod_gfsk = 0;
                    state.rf_mod = 1;
                    state.samplesPerSymbol = 8;
                    state.symbolCenter = 3;
                    // opts.setmod_bw = 12000;
                    fprintf(stderr, "Enabling 6000 sps P25p2 QPSK.\n");
                } else if (optarg[0] == 'L') {
                    opts.cqpsk_lms = 1;
                    /* Tiny defaults unless overridden later */
                    opts.cqpsk_mu_q15 = 1;
                    opts.cqpsk_stride = 4;
                    fprintf(stderr, "Enabling CQPSK LMS (experimental).\n");
                }
                //test
                else if (optarg[0] == '3') {
                    opts.mod_c4fm = 1;
                    opts.mod_qpsk = 0;
                    opts.mod_gfsk = 0;
                    state.rf_mod = 0;
                    state.samplesPerSymbol = 10;
                    state.symbolCenter = 4;
                    // opts.setmod_bw = 12000;
                    fprintf(stderr, "Enabling 6000 sps P25p2 C4FM.\n");
                } else if (optarg[0] == '4') {
                    opts.mod_c4fm = 1;
                    opts.mod_qpsk = 1;
                    opts.mod_gfsk = 1;
                    state.rf_mod = 0;
                    state.samplesPerSymbol = 8;
                    state.symbolCenter = 3;
                    // opts.setmod_bw = 12000;
                    fprintf(stderr, "Enabling 6000 sps P25p2 all optimizations.\n");
                }
                break;
            case 'u':
                sscanf(optarg, "%i", &opts.uvquality);
                if (opts.uvquality < 1) {
                    opts.uvquality = 1;
                } else if (opts.uvquality > 64) {
                    opts.uvquality = 64;
                }
                fprintf(stderr, "Setting unvoice speech quality to %i waves per band.\n", opts.uvquality);
                break;
            case 'x':
                if (optarg[0] == 'x') {
                    opts.inverted_x2tdma = 0;
                    fprintf(stderr, "Expecting non-inverted X2-TDMA signals.\n");
                } else if (optarg[0] == 'r') {
                    opts.inverted_dmr = 1;
                    fprintf(stderr, "Expecting inverted DMR signals.\n");
                } else if (optarg[0] == 'd') {
                    opts.inverted_dpmr = 1;
                    fprintf(stderr, "Expecting inverted ICOM dPMR signals.\n");
                } else if (optarg[0] == 'z') {
                    opts.inverted_m17 = 1;
                    fprintf(stderr, "Expecting inverted M17 signals.\n");
                }
                break;

            case 'r':
                opts.playfiles = 1;
                opts.errorbars = 0;
                opts.datascope = 0;
                opts.pulse_digi_rate_out = 48000;
                opts.pulse_digi_out_channels = 1;
                opts.dmr_stereo = 0;
                state.dmr_stereo = 0;
                sprintf(opts.output_name, "MBE Playback");
                state.optind = optind;
                break;
            case 'l': opts.use_cosine_filter = 0; break;
            default: usage(); exit(0);
        }
    }

    if (opts.resume > 0) {
        openSerial(&opts, &state);
    }

    if ((strncmp(opts.audio_in_dev, "m17udp", 6) == 0)) //M17 UDP Socket Input
    {
        fprintf(stderr, "M17 UDP IP Frame Input: ");
        char* curr;
        char* saveptr = NULL;
        char inbuf[1024];
        strncpy(inbuf, opts.audio_in_dev, sizeof(inbuf) - 1);
        inbuf[sizeof(inbuf) - 1] = '\0';

        curr = strtok_r(inbuf, ":", &saveptr); //should be 'm17'
        if (curr == NULL) {
            goto M17ENDIN; //end early with preset values
        }

        curr = strtok_r(NULL, ":", &saveptr); //host address
        if (curr != NULL) {
            strncpy(opts.m17_hostname, curr, 1023);
        }

        curr = strtok_r(NULL, ":", &saveptr); //host port
        if (curr != NULL) {
            opts.m17_portno = atoi(curr);
        }

    M17ENDIN:
        fprintf(stderr, "%s:", opts.m17_hostname);
        fprintf(stderr, "%d \n", opts.m17_portno);
    }

    if ((strncmp(opts.audio_in_dev, "udp", 3) == 0)) // UDP Direct Audio Input
    {
        fprintf(stderr, "UDP Direct Input: ");
        char* curr;
        char* saveptr = NULL;
        char inbuf[1024];
        strncpy(inbuf, opts.audio_in_dev, sizeof(inbuf) - 1);
        inbuf[sizeof(inbuf) - 1] = '\0';

        curr = strtok_r(inbuf, ":", &saveptr); // 'udp'
        if (curr == NULL) {
            goto UDPINEND;
        }

        curr = strtok_r(NULL, ":", &saveptr); // bind address
        if (curr != NULL) {
            strncpy(opts.udp_in_bindaddr, curr, 1023);
            opts.udp_in_bindaddr[1023] = '\0';
        }

        curr = strtok_r(NULL, ":", &saveptr); // bind port
        if (curr != NULL) {
            opts.udp_in_portno = atoi(curr);
        }

    UDPINEND:
        if (opts.udp_in_portno == 0) {
            opts.udp_in_portno = 7355;
        }
        if (opts.udp_in_bindaddr[0] == '\0') {
            snprintf(opts.udp_in_bindaddr, sizeof(opts.udp_in_bindaddr), "%s", "127.0.0.1");
        }
        fprintf(stderr, "%s:%d\n", opts.udp_in_bindaddr, opts.udp_in_portno);
    }

    if ((strncmp(opts.audio_out_dev, "m17udp", 6) == 0)) //M17 UDP Socket Output
    {
        fprintf(stderr, "M17 UDP IP Frame Output: ");
        char* curr;
        char* saveptr = NULL;
        char outbuf[1024];
        strncpy(outbuf, opts.audio_out_dev, sizeof(outbuf) - 1);
        outbuf[sizeof(outbuf) - 1] = '\0';

        curr = strtok_r(outbuf, ":", &saveptr); //should be 'm17'
        if (curr == NULL) {
            goto M17ENDOUT; //end early with preset values
        }

        curr = strtok_r(NULL, ":", &saveptr); //host address
        if (curr != NULL) {
            strncpy(opts.m17_hostname, curr, 1023);
        }

        curr = strtok_r(NULL, ":", &saveptr); //host port
        if (curr != NULL) {
            opts.m17_portno = atoi(curr);
        }

    M17ENDOUT:
        fprintf(stderr, "%s:", opts.m17_hostname);
        fprintf(stderr, "%d \n", opts.m17_portno);
        opts.m17_use_ip = 1;     //tell the encoder to open the socket
        opts.audio_out_type = 9; //set to null device
    }

    if ((strncmp(opts.audio_in_dev, "tcp", 3) == 0)) //tcp socket input from SDR++ and others
    {
        fprintf(stderr, "TCP Direct Link: ");
        char* curr;
        char* saveptr = NULL;
        char inbuf[1024];
        strncpy(inbuf, opts.audio_in_dev, sizeof(inbuf) - 1);
        inbuf[sizeof(inbuf) - 1] = '\0';

        curr = strtok_r(inbuf, ":", &saveptr); //should be 'tcp'
        if (curr == NULL) {
            goto TCPEND; //end early with preset values
        }

        curr = strtok_r(NULL, ":", &saveptr); //host address
        if (curr != NULL) {
            strncpy(opts.tcp_hostname, curr, 1023);
            //shim to tie the hostname of the tcp input to the rigctl hostname (probably covers a vast majority of use cases)
            //in the future, I will rework part of this so that users can enter a hostname and port similar to how tcp and rtl strings work
            memcpy(opts.rigctlhostname, opts.tcp_hostname, sizeof(opts.rigctlhostname));
        }

        curr = strtok_r(NULL, ":", &saveptr); //host port
        if (curr != NULL) {
            opts.tcp_portno = atoi(curr);
        }

    TCPEND:
        if (exitflag == 1) {
            cleanupAndExit(&opts, &state); //needed to break the loop on ctrl+c
        }
        fprintf(stderr, "%s:", opts.tcp_hostname);
        fprintf(stderr, "%d \n", opts.tcp_portno);
        opts.tcp_sockfd = Connect(opts.tcp_hostname, opts.tcp_portno);
        if (opts.tcp_sockfd != 0) {
            opts.audio_in_type = 8;

            fprintf(stderr, "TCP Connection Success!\n");
            // openAudioInDevice(&opts); //do this to see if it makes it work correctly
        } else {
#ifdef __CYGWIN__
            sprintf(opts.audio_in_dev, "%s", "/dev/dsp");
            opts.audio_in_type = 5;
#else
            if (opts.frame_m17 == 1) {
                sleep(1);
                goto TCPEND; //try again if using M17 encoder / decoder over TCP
            }
            sprintf(opts.audio_in_dev, "%s", "pulse");
            fprintf(stderr, "TCP Connection Failure - Using %s Audio Input.\n", opts.audio_in_dev);
            opts.audio_in_type = 0;
#endif
        }
    }

    if (opts.use_rigctl == 1) {
        opts.rigctl_sockfd = Connect(opts.rigctlhostname, opts.rigctlportno);
        if (opts.rigctl_sockfd != 0) {
            opts.use_rigctl = 1;
        } else {
            fprintf(stderr, "RIGCTL Connection Failure - RIGCTL Features Disabled\n");
            opts.use_rigctl = 0;
        }
    }

    if ((strncmp(opts.audio_in_dev, "rtltcp", 6) == 0)) // rtl_tcp networked RTL-SDR
    {
        fprintf(stderr, "RTL_TCP Input: ");
        char* curr;
        char* saveptr = NULL;
        char inbuf[1024];
        strncpy(inbuf, opts.audio_in_dev, sizeof(inbuf) - 1);
        inbuf[sizeof(inbuf) - 1] = '\0';

        curr = strtok_r(inbuf, ":", &saveptr); // 'rtltcp'
        if (curr == NULL) {
            goto RTLTCPEND;
        }

        curr = strtok_r(NULL, ":", &saveptr); // host
        if (curr != NULL) {
            strncpy(opts.rtltcp_hostname, curr, 1023);
        }

        curr = strtok_r(NULL, ":", &saveptr); // port
        if (curr != NULL) {
            opts.rtltcp_portno = atoi(curr);
        }

        // Optional: freq:gain:ppm:bw:sql:vol (mirrors rtl: string semantics)
        curr = strtok_r(NULL, ":", &saveptr); // freq
        if (curr != NULL) {
            opts.rtlsdr_center_freq = (uint32_t)atofs(curr);
        } else {
            goto RTLTCPEND;
        }

        curr = strtok_r(NULL, ":", &saveptr); // gain
        if (curr != NULL) {
            opts.rtl_gain_value = atoi(curr);
        } else {
            goto RTLTCPEND;
        }

        curr = strtok_r(NULL, ":", &saveptr); // ppm
        if (curr != NULL) {
            opts.rtlsdr_ppm_error = atoi(curr);
        } else {
            goto RTLTCPEND;
        }

        curr = strtok_r(NULL, ":", &saveptr); // bw (kHz)
        if (curr != NULL) {
            int bw = atoi(curr);
            if (bw == 4 || bw == 6 || bw == 8 || bw == 12 || bw == 16 || bw == 24) {
                opts.rtl_bandwidth = bw;
            } else {
                opts.rtl_bandwidth = 12;
            }
        } else {
            goto RTLTCPEND;
        }

        curr = strtok_r(NULL, ":", &saveptr); // sql (dB if negative; else linear)
        if (curr != NULL) {
            double sq_val = atof(curr);
            if (sq_val < 0.0) {
                opts.rtl_squelch_level = (int)dB_to_pwr(sq_val);
            } else {
                opts.rtl_squelch_level = (int)sq_val;
            }
        } else {
            goto RTLTCPEND;
        }

        curr = strtok_r(NULL, ":", &saveptr); // vol (1..3)
        if (curr != NULL) {
            opts.rtl_volume_multiplier = atoi(curr);
        } else {
            goto RTLTCPEND;
        }

        // Optional trailing tokens: bias tee toggle
        while ((curr = strtok_r(NULL, ":", &saveptr)) != NULL) {
            if (strncmp(curr, "bias", 4) == 0 || strncmp(curr, "b", 1) == 0) {
                const char* val = strchr(curr, '=');
                int on = 1; // default enable if no explicit value
                if (val && *(val + 1)) {
                    val++; // move past '='
                    if (*val == '0' || *val == 'n' || *val == 'N' || *val == 'o' || *val == 'O' || *val == 'f'
                        || *val == 'F') {
                        on = 0;
                    }
                }
                opts.rtl_bias_tee = on;
            }
        }

    RTLTCPEND:
        if (opts.rtltcp_portno == 0) {
            opts.rtltcp_portno = 1234;
        }
        fprintf(stderr, "%s:%d", opts.rtltcp_hostname, opts.rtltcp_portno);
        if (opts.rtl_bias_tee) {
            fprintf(stderr, " (bias=on)\n");
        } else {
            fprintf(stderr, "\n");
        }
        opts.rtltcp_enabled = 1;
        opts.audio_in_type = 3; // use RTL pipeline
    }

    // NOTE: Guard against matching "rtltcp" here; it shares the "rtl" prefix
    // and opts.audio_in_dev has been tokenized by strtok above. Without this
    // guard, selecting rtltcp would also fall through to the local RTL path
    // and erroneously require a USB device, causing an early exit.
    if ((strncmp(opts.audio_in_dev, "rtl", 3) == 0)
        && (strncmp(opts.audio_in_dev, "rtltcp", 6) != 0)) //rtl dongle input
    {
        uint8_t rtl_ok = 0;
        //use to list out all detected RTL dongles
        char vendor[256], product[256], serial[256], userdev[256];
        int device_count = 0;

#ifdef USE_RTLSDR
        fprintf(stderr, "RTL Input: ");
        char* curr;
        char* saveptr = NULL;
        char inbuf[1024];
        strncpy(inbuf, opts.audio_in_dev, sizeof(inbuf) - 1);
        inbuf[sizeof(inbuf) - 1] = '\0';

        curr = strtok_r(inbuf, ":", &saveptr); //should be 'rtl'
        if (curr == NULL) {
            goto RTLEND; //end early with preset values
        }

        curr = strtok_r(NULL, ":", &saveptr); //rtl device number "-D"
        if (curr != NULL) {
            opts.rtl_dev_index = atoi(curr);
        } else {
            goto RTLEND;
        }

        curr = strtok_r(NULL, ":", &saveptr); //rtl freq "-c"
        if (curr != NULL) {
            opts.rtlsdr_center_freq = (uint32_t)atofs(curr);
        } else {
            goto RTLEND;
        }

        curr = strtok_r(NULL, ":", &saveptr); //rtl gain value "-G"
        if (curr != NULL) {
            opts.rtl_gain_value = atoi(curr);
        } else {
            goto RTLEND;
        }

        curr = strtok_r(NULL, ":", &saveptr); //rtl ppm err "-P"
        if (curr != NULL) {
            opts.rtlsdr_ppm_error = atoi(curr);
        } else {
            goto RTLEND;
        }

        curr = strtok_r(NULL, ":", &saveptr); //rtl bandwidth "-Y"
        if (curr != NULL) {
            int bw = 0;
            bw = atoi(curr);
            //check for proper values (6,8,12,24)
            if (bw == 4 || bw == 6 || bw == 8 || bw == 12 || bw == 16
                || bw == 24) //testing 4 and 16 as well for weak and/or nxdn48 systems
            {
                opts.rtl_bandwidth = bw;
            } else {
                opts.rtl_bandwidth = 12; //safe default -- provides best performance on most systems
            }
        } else {
            goto RTLEND;
        }

        curr = strtok_r(NULL, ":", &saveptr); //rtl squelch threshold (dB if negative; else linear)
        if (curr != NULL) {
            double sq_val = atof(curr);
            if (sq_val < 0.0) {
                opts.rtl_squelch_level = (int)dB_to_pwr(sq_val);
            } else {
                opts.rtl_squelch_level = (int)sq_val;
            }
        } else {
            goto RTLEND;
        }

        // curr = strtok_r(NULL, ":", &saveptr); //rtl udp port "-U"
        // if (curr != NULL) opts.rtl_udp_port = atoi (curr);
        // else goto RTLEND;

        curr = strtok_r(NULL, ":", &saveptr); //rtl sample / volume multiplier
        if (curr != NULL) {
            opts.rtl_volume_multiplier = atoi(curr);
        } else {
            goto RTLEND;
        }

        // Optional trailing tokens: bias tee toggle
        while ((curr = strtok_r(NULL, ":", &saveptr)) != NULL) {
            if (strncmp(curr, "bias", 4) == 0 || strncmp(curr, "b", 1) == 0) {
                const char* val = strchr(curr, '=');
                int on = 1; // default enable if no explicit value
                if (val && *(val + 1)) {
                    val++; // move past '='
                    if (*val == '0' || *val == 'n' || *val == 'N' || *val == 'o' || *val == 'O' || *val == 'f'
                        || *val == 'F') {
                        on = 0;
                    }
                }
                opts.rtl_bias_tee = on;
            }
        }

    RTLEND:

        device_count = rtlsdr_get_device_count();
        if (!device_count) {
            fprintf(stderr, "No supported devices found.\n");
            exitflag = 1;
        } else {
            fprintf(stderr, "Found %d device(s):\n", device_count);
        }
        for (int i = 0; i < device_count; i++) {
            rtlsdr_get_device_usb_strings(i, vendor, product, serial);
            fprintf(stderr, "  %d:  %s, %s, SN: %s\n", i, vendor, product, serial);

            sprintf(userdev, "%08d", opts.rtl_dev_index);

            //check by index first, then by serial
            if (opts.rtl_dev_index == i) {
                fprintf(stderr, "Selected Device #%d with Serial Number: %s \n", i, serial);
            } else if (strcmp(userdev, serial) == 0) {
                fprintf(stderr, "Selected Device #%d with Serial Number: %s \n", i, serial);
                opts.rtl_dev_index = i;
            }
        }

        if (opts.rtl_volume_multiplier > 3 || opts.rtl_volume_multiplier < 0) {
            opts.rtl_volume_multiplier = 1; //I wonder if you could flip polarity by using -1
        }

        fprintf(stderr, "Dev %d ", opts.rtl_dev_index);
        fprintf(stderr, "Freq %d ", opts.rtlsdr_center_freq);
        fprintf(stderr, "Gain %d ", opts.rtl_gain_value);
        fprintf(stderr, "PPM %d ", opts.rtlsdr_ppm_error);
        fprintf(stderr, "BW %d ", opts.rtl_bandwidth);
        fprintf(stderr, "SQ %.1f dB ", pwr_to_dB(opts.rtl_squelch_level));
        // fprintf (stderr, "UDP %d \n", opts.rtl_udp_port);
        fprintf(stderr, "VOL %d ", opts.rtl_volume_multiplier);
        if (opts.rtl_bias_tee) {
            fprintf(stderr, "BIAS on\n");
        } else {
            fprintf(stderr, "\n");
        }
        opts.audio_in_type = 3;

        rtl_ok = 1;
#endif

#ifdef __CYGWIN__
        if (rtl_ok == 0) //not set, means rtl support isn't compiled/available
        {
            fprintf(stderr, "RTL Support not enabled/compiled, falling back to OSS /dev/dsp Audio Input.\n");
            sprintf(opts.audio_in_dev, "%s", "/dev/dsp");
            opts.audio_in_type = 5;
        }
#else
        if (rtl_ok == 0) //not set, means rtl support isn't compiled/available
        {
            fprintf(stderr, "RTL Support not enabled/compiled, falling back to Pulse Audio Audio Input.\n");
            sprintf(opts.audio_in_dev, "%s", "pulse");
            opts.audio_in_type = 0;
        }
        UNUSED(vendor);
        UNUSED(product);
        UNUSED(serial);
        UNUSED(userdev);
        UNUSED(device_count);
#endif
    }

    //moved these to be checked prior to checking OSS for the split for 1-48k, or variable configurations
    if ((strncmp(opts.audio_in_dev, "pulse", 5) == 0)) {
        opts.audio_in_type = 0;

        //string yeet
        parse_pulse_input_string(&opts, opts.audio_in_dev + 5);
    }

    //UDP Socket Blaster Audio Output Setup
    if ((strncmp(opts.audio_out_dev, "udp", 3) == 0)) {

        //read in values
        fprintf(stderr, "UDP Blaster Output: ");
        char* curr;
        char* saveptr = NULL;
        char outbuf[1024];
        strncpy(outbuf, opts.audio_out_dev, sizeof(outbuf) - 1);
        outbuf[sizeof(outbuf) - 1] = '\0';

        curr = strtok_r(outbuf, ":", &saveptr); //should be 'udp'
        if (curr == NULL) {
            goto UDPEND; //end early with preset values
        }

        curr = strtok_r(NULL, ":", &saveptr); //udp blaster hostname
        if (curr != NULL) {
            strncpy(opts.udp_hostname, curr, 1023); //set address to blast to
        }

        curr = strtok_r(NULL, ":", &saveptr); //udp blaster port
        if (curr != NULL) {
            opts.udp_portno = atoi(curr);
        }

    UDPEND:
        fprintf(stderr, "%s:", opts.udp_hostname);
        fprintf(stderr, "%d \n", opts.udp_portno);

        int err = udp_socket_connect(&opts, &state);
        if (err < 0) {
            fprintf(stderr, "Error Configuring UDP Socket for UDP Blaster Audio :( \n");
#ifdef __CYGWIN__
            sprintf(opts.audio_in_dev, "%s", "/dev/dsp");
            opts.audio_in_type = 5;
            //since I can't determine what the configuration will be for 48k1 or 8k2 here(lazy), need to exit
            exitflag = 1;
#else
            sprintf(opts.audio_out_dev, "%s", "pulse");
            opts.audio_out_type = 0;
#endif
        }

        opts.audio_out_type = 8;

        if (opts.monitor_input_audio == 1 || opts.frame_provoice == 1) {
            err = udp_socket_connectA(&opts, &state);
            if (err < 0) {
                fprintf(stderr, "Error Configuring UDP Socket for UDP Blaster Audio Analog :( \n");
                opts.udp_sockfdA = 0;
                opts.monitor_input_audio = 0;
            } else {
                fprintf(stderr, "UDP Blaster Output (Analog): ");
                fprintf(stderr, "%s:", opts.udp_hostname);
                fprintf(stderr, "%d \n", opts.udp_portno + 2);
            }

            //this functionality is disabled when trunking EDACS, but we still use the behavior for analog channel monitoring
            if (opts.frame_provoice == 1 && opts.p25_trunk == 1) {
                opts.monitor_input_audio = 0;
            }
        }
    }

    if ((strncmp(opts.audio_out_dev, "pulse", 5) == 0)) {
        opts.audio_out_type = 0;

        //string yeet
        parse_pulse_output_string(&opts, opts.audio_out_dev + 5);
    }

    if ((strncmp(opts.audio_out_dev, "null", 4) == 0)) {
        opts.audio_out_type = 9; //9 for NULL, or mute output
        opts.audio_out = 0;      //turn off so we won't playSynthesized
    }

    if ((strncmp(opts.audio_out_dev, "-", 1) == 0)) {
        opts.audio_out_fd = fileno(stdout); //STDOUT_FILENO;
        opts.audio_out_type = 1;            //using 1 for stdout to match input stdin as 1
        fprintf(stderr, "Audio Out Device: -\n");
    }

    int fmt;
    int speed;

    //The long of the short is that PADSP can open multiple virtual /dev/dsp devices each with different sampling rates and channel configurations
    //but the instance inside of Cygwin is a single instance tied to one sample rate AND one channel configuration, so if you change it on the output, it
    //also changes on the input, and the way dsd handles this is to upsample output to make it work correctly, so in order to be able to change the output
    //to a variable config, it cannot be the input as well

    if ((strncmp(opts.audio_in_dev, "/dev/audio", 10) == 0)) {
        sprintf(opts.audio_in_dev, "%s", "/dev/dsp");
        fprintf(stderr, "Switching to /dev/dsp.\n");
    }

    if ((strncmp(opts.audio_in_dev, "pa", 2) == 0)) {
        sprintf(opts.audio_in_dev, "%s", "/dev/dsp");
        fprintf(stderr, "Switching to /dev/dsp.\n");
    }

    speed = 48000; //hardset to 48000
#if DSD_HAVE_OSS
    if ((strncmp(opts.audio_in_dev, "/dev/dsp", 8) == 0)) {
        fprintf(stderr, "OSS Input %s.\n", opts.audio_in_dev);
        opts.audio_in_fd = open(opts.audio_in_dev, O_RDWR);
        if (opts.audio_in_fd == -1) {
            fprintf(stderr, "Error, couldn't open %s\n", opts.audio_in_dev);
        }

        fmt = 0;
        if (ioctl(opts.audio_in_fd, SNDCTL_DSP_RESET) < 0) {
            fprintf(stderr, "ioctl reset error \n");
        }
        fmt = speed;
        if (ioctl(opts.audio_in_fd, SNDCTL_DSP_SPEED, &fmt) < 0) {
            fprintf(stderr, "ioctl speed error \n");
        }
        fmt = 0;
        if (ioctl(opts.audio_in_fd, SNDCTL_DSP_STEREO, &fmt) < 0) {
            fprintf(stderr, "ioctl stereo error \n");
        }
        fmt = AFMT_S16_LE;
        if (ioctl(opts.audio_in_fd, SNDCTL_DSP_SETFMT, &fmt) < 0) {
            fprintf(stderr, "ioctl setfmt error \n");
        }

        opts.audio_in_type = 5; //5 will become OSS input type
    }
#endif

    //check for OSS output
    if ((strncmp(opts.audio_out_dev, "/dev/audio", 10) == 0)) {
        sprintf(opts.audio_out_dev, "%s", "/dev/dsp");
        fprintf(stderr, "Switching to /dev/dsp.\n");
    }

    if ((strncmp(opts.audio_out_dev, "pa", 2) == 0)) {
        sprintf(opts.audio_out_dev, "%s", "/dev/dsp");
        fprintf(stderr, "Switching to /dev/dsp.\n");
    }

    //this will only open OSS output if its listed as a type
    //changed to this so I could call it freely inside of ncurses terminal
    if (opts.playfiles == 0) {
        openOSSOutput(&opts);
    }

    if (opts.playfiles == 1) {
        opts.split = 1;
        opts.playoffset = 0;
        opts.playoffsetR = 0;
        opts.delay = 0;
        opts.pulse_digi_rate_out = 8000;
        opts.pulse_digi_out_channels = 1;
        if (opts.audio_out_type == 0) {
            openPulseOutput(&opts);
        } else if ((strncmp(opts.audio_out_dev, "/dev/dsp", 8) == 0)) {
            openOSSOutput(&opts); //open after split == 1 so it will open and playback at the proper speed
        }
    }

    //this particular if-elseif-else could be rewritten to be a lot neater and simpler
    else if (strcmp(opts.audio_in_dev, opts.audio_out_dev) != 0) {
        opts.split = 1;
        opts.playoffset = 0;
        opts.playoffsetR = 0;
        opts.delay = 0;

        //open wav file should be handled directly by the -w switch now
        // if (strlen(opts.wav_out_file) > 0 && opts.dmr_stereo_wav == 0)
        //   openWavOutFile (&opts, &state);

        // else

        openAudioInDevice(&opts);

        // fprintf (stderr,"Press CTRL + C to close.\n");
    }

    else {
        opts.split = 0;
        opts.playoffset = 0;
        opts.playoffsetR = 0;
        opts.delay = 0;
        openAudioInDevice(&opts);
    }

    signal(SIGINT, handler);
    signal(SIGTERM, handler);

    //read in any user supplied M17 CAN and/or CSD data
    if ((strncmp(state.m17dat, "M17", 3) == 0)) {
        //read in values
        //string in format of M17:can:src_csd:dst_csd:input_rate

        //check and capatalize any letters in the CSD
        for (int i = 0; state.m17dat[i] != '\0'; i++) {
            if (state.m17dat[i] >= 'a' && state.m17dat[i] <= 'z') {
                state.m17dat[i] = state.m17dat[i] - 32;
            }
        }

        fprintf(stderr, "M17 User Data: ");
        char* curr;

        // if((strncmp(state.m17dat, "M17", 3) == 0))
        // goto M17END;

        curr = strtok(state.m17dat, ":"); //should be 'M17'
        if (curr != NULL)
            ; //continue
        else {
            goto M17END; //end early with preset values
        }

        curr = strtok(NULL, ":"); //m17 channel access number
        if (curr != NULL) {
            state.m17_can_en = atoi(curr);
        }

        curr = strtok(NULL, ":"); //m17 src address
        if (curr != NULL) {
            strncpy(state.str50c, curr, 9); //only read first 9
            state.str50c[9] = '\0';
        }

        curr = strtok(NULL, ":"); //m17 dst address
        if (curr != NULL) {
            strncpy(state.str50b, curr, 9); //only read first 9
            state.str50b[9] = '\0';
        }

        curr = strtok(NULL, ":"); //m17 input audio rate
        if (curr != NULL) {
            state.m17_rate = atoi(curr);
        }

        curr = strtok(NULL, ":"); //m17 vox enable
        if (curr != NULL) {
            state.m17_vox = atoi(curr);
        }

        // curr = strtok(NULL, ":"); //moved to in and out methods
        // if (curr != NULL)
        //   opts.m17_use_ip = atoi(curr);

    M17END:; //do nothing

        //check to make sure can value is no greater than 15 (4 bit value)
        if (state.m17_can_en > 15) {
            state.m17_can_en = 15;
        }

        //if vox is greater than 1, assume user meant 'yes' and set to one
        if (state.m17_vox > 1) {
            state.m17_vox = 1;
        }

        //debug print m17dat string
        // fprintf (stderr, " %s;", state.m17dat);

        fprintf(stderr, " M17:%d:%s:%s:%d;", state.m17_can_en, state.str50c, state.str50b, state.m17_rate);
        if (state.m17_vox == 1) {
            fprintf(stderr, "VOX;");
        }
        fprintf(stderr, "\n");
    }

    if (opts.playfiles == 1) {

        playMbeFiles(&opts, &state, argc, argv);
    }

    else if (opts.m17encoder == 1) {
        //disable RRC filter for now
        opts.use_cosine_filter = 0;

        opts.pulse_digi_rate_out = 8000;

        //open any inputs, if not alread opened, OSS input and output already handled
        if (opts.audio_in_type == 0) {
            openPulseInput(&opts);
        }

#ifdef USE_RTLSDR
        else if (opts.audio_in_type == 3) {
            if (g_rtl_ctx == NULL) {
                if (rtl_stream_create(&opts, &g_rtl_ctx) < 0) {
                    fprintf(stderr, "Failed to create RTL stream.\n");
                }
            }
            if (g_rtl_ctx && rtl_stream_start(g_rtl_ctx) < 0) {
                fprintf(stderr, "Failed to open RTL-SDR stream.\n");
            }
            opts.rtl_started = 1;
        }
#endif

        //open any outputs, if not already opened
        if (opts.audio_out_type == 0) {
            openPulseOutput(&opts);
        }
        //All input and output now opened and handled correctly, so let's not break things by tweaking
        encodeM17STR(&opts, &state);
    }

    else if (opts.m17encoderbrt == 1) {
        opts.pulse_digi_rate_out = 8000;
        //open any outputs, if not already opened
        if (opts.audio_out_type == 0) {
            openPulseOutput(&opts);
        }
        encodeM17BRT(&opts, &state);
    }

    else if (opts.m17encoderpkt == 1) {
        //disable RRC filter for now
        opts.use_cosine_filter = 0;

        opts.pulse_digi_rate_out = 8000;
        //open any outputs, if not already opened
        if (opts.audio_out_type == 0) {
            openPulseOutput(&opts);
        }
        encodeM17PKT(&opts, &state);
    }

    else if (opts.m17decoderip == 1) {
        opts.pulse_digi_rate_out = 8000;
        //open any outputs, if not already opened
        if (opts.audio_out_type == 0) {
            openPulseOutput(&opts);
        }
        processM17IPF(&opts, &state);
    }

    else {

        liveScanner(&opts, &state);
    }

    cleanupAndExit(&opts, &state);

    return (0);
}

// --- DMR TIII LCN calculator (env-triggered utility) ---
static int
cmp_long(const void* a, const void* b) {
    long aa = *(const long*)a;
    long bb = *(const long*)b;
    if (aa < bb) {
        return -1;
    }
    if (aa > bb) {
        return 1;
    }
    return 0;
}

static long
nearest_125(long hz) {
    // round to nearest 125 Hz
    long q = (hz + (hz >= 0 ? 62 : -62)) / 125;
    return q * 125;
}

static long
infer_step_125(const long* f, int n) {
    // Infer channel spacing, snapped to 125 Hz grid.
    // Use the smallest positive rounded diff as conservative step.
    long best = 0;
    for (int i = 1; i < n; i++) {
        long d = f[i] - f[i - 1];
        if (d <= 0) {
            continue;
        }
        long r = nearest_125(d);
        if (r <= 0) {
            continue;
        }
        if (best == 0 || r < best) {
            best = r;
        }
    }
    return best;
}

static int
run_t3_lcn_calc_from_csv(const char* path) {
    FILE* fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "LCN calc: unable to open '%s'\n", path);
        return 1;
    }
    // Parse first numeric field per line as frequency in Hz (accept raw or with separators)
    long freqs[4096];
    int nf = 0;
    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        // Skip empty/comment/header
        int has_digit = 0;
        for (char* p = line; *p; ++p) {
            if ((*p >= '0' && *p <= '9')) {
                has_digit = 1;
                break;
            }
        }
        if (!has_digit) {
            continue;
        }
        // Extract first field
        char* p = line;
        // Skip leading non-digit (allow +/-)
        while (*p && !((*p >= '0' && *p <= '9') || *p == '+' || *p == '-')) {
            p++;
        }
        if (!*p) {
            continue;
        }
        // Read as double to accept MHz; if < 1e6 assume MHz
        double val = 0.0;
        char* end = p;
        val = strtod(p, &end);
        if (end == p) {
            continue;
        }
        long hz = (long)llround(val);
        if (val < 1e5) {
            // probably MHz; convert
            hz = (long)llround(val * 1000000.0);
        }
        if (hz <= 0) {
            continue;
        }
        if (nf < (int)(sizeof(freqs) / sizeof(freqs[0]))) {
            freqs[nf++] = hz;
        }
    }
    fclose(fp);
    if (nf < 1) {
        fprintf(stderr, "LCN calc: no frequencies parsed from '%s'\n", path);
        return 2;
    }
    qsort(freqs, nf, sizeof(long), cmp_long);
    // Unique
    int m = 0;
    for (int i = 0; i < nf; i++) {
        if (m == 0 || freqs[i] != freqs[m - 1]) {
            freqs[m++] = freqs[i];
        }
    }
    nf = m;
    if (nf == 1) {
        long start_lcn = 1;
        const char* s = getenv("DSD_NEO_DMR_T3_START_LCN");
        if (s && *s) {
            start_lcn = strtol(s, NULL, 10);
        }
        printf("lcn,freq\n%ld,%ld\n", start_lcn, freqs[0]);
        return 0;
    }
    // Infer step or take from env
    long step = 0;
    const char* sstep = getenv("DSD_NEO_DMR_T3_STEP_HZ");
    if (sstep && *sstep) {
        step = strtol(sstep, NULL, 10);
    }
    if (step <= 0) {
        step = infer_step_125(freqs, nf);
    }
    if (step <= 0) {
        fprintf(stderr, "LCN calc: could not infer channel step. Provide DSD_NEO_DMR_T3_STEP_HZ.\n");
        return 3;
    }
    // Anchors
    long cc_freq = 0;
    long cc_lcn = 0;
    const char* sccf = getenv("DSD_NEO_DMR_T3_CC_FREQ");
    const char* sccl = getenv("DSD_NEO_DMR_T3_CC_LCN");
    if (sccf && *sccf) {
        double v = strtod(sccf, NULL);
        if (v < 1e5) {
            cc_freq = (long)llround(v * 1000000.0);
        } else {
            cc_freq = (long)llround(v);
        }
    }
    if (sccl && *sccl) {
        cc_lcn = strtol(sccl, NULL, 10);
    }

    long start_lcn = 1;
    const char* sstart = getenv("DSD_NEO_DMR_T3_START_LCN");
    if (sstart && *sstart) {
        start_lcn = strtol(sstart, NULL, 10);
    }

    long base_freq = freqs[0];
    long base_lcn = start_lcn;
    if (cc_freq > 0 && cc_lcn > 0) {
        // Align base so that cc maps to cc_lcn
        // base_lcn = cc_lcn - round((cc_freq - base_freq)/step)
        long delta = cc_freq - base_freq;
        long idx = (long)llround((double)delta / (double)step);
        base_lcn = cc_lcn - idx;
    }

    // Emit mapping sorted by LCN
    printf("lcn,freq\n");
    for (int i = 0; i < nf; i++) {
        long delta = freqs[i] - base_freq;
        long idx = (long)llround((double)delta / (double)step);
        long lcn = base_lcn + idx;
        printf("%ld,%ld\n", lcn, freqs[i]);
    }
    return 0;
}
