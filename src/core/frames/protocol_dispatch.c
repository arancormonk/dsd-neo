// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/frame.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/protocol_dispatch.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/io/control.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dstar/dstar.h>
#include <dsd-neo/protocol/edacs/edacs.h>
#include <dsd-neo/protocol/m17/m17.h>
#include <dsd-neo/protocol/nxdn/nxdn.h>
#include <dsd-neo/protocol/p25/p25.h>
#include <dsd-neo/protocol/p25/p25p1_check_nid.h>
#include <dsd-neo/protocol/provoice/provoice.h>
#include <dsd-neo/protocol/x2tdma/x2tdma.h>
#include <dsd-neo/protocol/ysf/ysf.h>
#include <dsd-neo/runtime/colors.h>

#include <mbelib.h>

#include <stdio.h>
#include <string.h>

static int
matches_p25p1(int synctype) {
    return DSD_SYNC_IS_P25P1(synctype);
}

static int
matches_p25p2(int synctype) {
    return DSD_SYNC_IS_P25P2(synctype);
}

static int
matches_x2tdma(int synctype) {
    return DSD_SYNC_IS_X2TDMA(synctype);
}

static int
matches_dstar(int synctype) {
    return DSD_SYNC_IS_DSTAR(synctype);
}

static int
matches_dmr(int synctype) {
    return DSD_SYNC_IS_DMR(synctype);
}

static int
matches_provoice(int synctype) {
    return synctype == DSD_SYNC_PROVOICE_POS || synctype == DSD_SYNC_PROVOICE_NEG;
}

static int
matches_edacs(int synctype) {
    return synctype == DSD_SYNC_EDACS_POS || synctype == DSD_SYNC_EDACS_NEG;
}

static int
matches_ysf(int synctype) {
    return DSD_SYNC_IS_YSF(synctype);
}

static int
matches_m17(int synctype) {
    return DSD_SYNC_IS_M17(synctype);
}

static int
matches_nxdn(int synctype) {
    return DSD_SYNC_IS_NXDN(synctype);
}

static int
matches_dpmr(int synctype) {
    return DSD_SYNC_IS_DPMR(synctype);
}

static void
handle_dstar(dsd_opts* opts, dsd_state* state) {
    if ((opts->mbe_out_dir[0] != 0) && (opts->mbe_out_f == NULL)) {
        openMbeOutFile(opts, state);
    }

    if (state->synctype == DSD_SYNC_DSTAR_VOICE_POS || state->synctype == DSD_SYNC_DSTAR_VOICE_NEG) {
        sprintf(state->fsubtype, " VOICE        ");
        processDSTAR(opts, state);
        return;
    }

    sprintf(state->fsubtype, " DATA         ");
    processDSTAR_HD(opts, state);
}

static void
handle_x2tdma(dsd_opts* opts, dsd_state* state) {
    state->nac = 0;
    if (opts->errorbars == 1) {
        printFrameInfo(opts, state);
    }

    if (state->synctype == DSD_SYNC_X2TDMA_VOICE_NEG || state->synctype == DSD_SYNC_X2TDMA_VOICE_POS) {
        if ((opts->mbe_out_dir[0] != 0) && (opts->mbe_out_f == NULL)) {
            openMbeOutFile(opts, state);
        }
        sprintf(state->fsubtype, " VOICE        ");
        processX2TDMAvoice(opts, state);
        return;
    }

    if (opts->mbe_out_f != NULL) {
        closeMbeOutFile(opts, state);
    }
    state->err_str[0] = 0;
    processX2TDMAdata(opts, state);
}

static void
handle_provoice(dsd_opts* opts, dsd_state* state) {
    if ((opts->mbe_out_dir[0] != 0) && (opts->mbe_out_f == NULL)) {
        openMbeOutFile(opts, state);
    }
    sprintf(state->fsubtype, " VOICE        ");
    processProVoice(opts, state);
}

static void
handle_edacs(dsd_opts* opts, dsd_state* state) {
    if (opts->mbe_out_f != NULL) {
        closeMbeOutFile(opts, state);
    }
    edacs(opts, state);
}

static void
handle_ysf(dsd_opts* opts, dsd_state* state) {
    processYSF(opts, state);
}

static void
handle_m17(dsd_opts* opts, dsd_state* state) {
    if (state->synctype == DSD_SYNC_M17_PRE_POS || state->synctype == DSD_SYNC_M17_PRE_NEG) {
        skipDibit(opts, state, 8);
        return;
    }

    if (state->synctype == DSD_SYNC_M17_LSF_POS || state->synctype == DSD_SYNC_M17_LSF_NEG) {
        processM17LSF(opts, state);
        return;
    }

    if (state->synctype == DSD_SYNC_M17_BRT_POS || state->synctype == DSD_SYNC_M17_BRT_NEG) {
        return;
    }

    if (state->synctype == DSD_SYNC_M17_PKT_POS || state->synctype == DSD_SYNC_M17_PKT_NEG) {
        processM17PKT(opts, state);
        return;
    }

    processM17STR(opts, state);
}

static void
handle_p25p2(dsd_opts* opts, dsd_state* state) {
    processP2(opts, state);
}

static void
handle_nxdn(dsd_opts* opts, dsd_state* state) {
    nxdn_frame(opts, state);
}

static void
handle_dmr(dsd_opts* opts, dsd_state* state) {

    //Start DMR Types
    if (DSD_SYNC_IS_DMR(state->synctype)) //BS 10-13, MS voice/data 32-33, RC 34
    {

        //print manufacturer strings to branding, disabled 0x10 moto other systems can use that fid set
        //0x06 is trident, but when searching, apparently, they developed con+, but was bought by moto?
        if (state->dmr_mfid == 0x10)
            ; //sprintf (state->dmr_branding, "%s",  "Motorola");
        else if (state->dmr_mfid == 0x68) {
            sprintf(state->dmr_branding, "%s", "  Hytera");
        } else if (state->dmr_mfid == 0x58) {
            sprintf(state->dmr_branding, "%s", "    Tait");
        }

        //disabling these due to random data decodes setting an odd mfid, could be legit, but only for that one packet?
        //or, its just a decode error somewhere
        // else if (state->dmr_mfid == 0x20) sprintf (state->dmr_branding, "%s", "JVC Kenwood");
        // else if (state->dmr_mfid == 0x04) sprintf (state->dmr_branding, "%s", "Flyde Micro");
        // else if (state->dmr_mfid == 0x05) sprintf (state->dmr_branding, "%s", "PROD-EL SPA");
        // else if (state->dmr_mfid == 0x06) sprintf (state->dmr_branding, "%s", "Motorola"); //trident/moto con+
        // else if (state->dmr_mfid == 0x07) sprintf (state->dmr_branding, "%s", "RADIODATA");
        // else if (state->dmr_mfid == 0x08) sprintf (state->dmr_branding, "%s", "Hytera");
        // else if (state->dmr_mfid == 0x09) sprintf (state->dmr_branding, "%s", "ASELSAN");
        // else if (state->dmr_mfid == 0x0A) sprintf (state->dmr_branding, "%s", "Kirisun");
        // else if (state->dmr_mfid == 0x0B) sprintf (state->dmr_branding, "%s", "DMR Association");
        // else if (state->dmr_mfid == 0x13) sprintf (state->dmr_branding, "%s", "EMC S.P.A.");
        // else if (state->dmr_mfid == 0x1C) sprintf (state->dmr_branding, "%s", "EMC S.P.A.");
        // else if (state->dmr_mfid == 0x33) sprintf (state->dmr_branding, "%s", "Radio Activity");
        // else if (state->dmr_mfid == 0x3C) sprintf (state->dmr_branding, "%s", "Radio Activity");
        // else if (state->dmr_mfid == 0x77) sprintf (state->dmr_branding, "%s", "Vertex Standard");

        //disable so radio id doesn't blink in and out during ncurses and aggressive_framesync
        state->nac = 0;

        if (opts->errorbars == 1) {
            if (opts->verbose > 0) {
                //fprintf (stderr,"inlvl: %2i%% ", (int)state->max / 164);
            }
        }
        if ((state->synctype == DSD_SYNC_DMR_BS_VOICE_NEG) || (state->synctype == DSD_SYNC_DMR_BS_VOICE_POS)
            || (state->synctype == DSD_SYNC_DMR_MS_VOICE)) //DMR Voice Modes
        {

            sprintf(state->fsubtype, " VOICE        ");
            if (opts->dmr_stereo == 0 && state->synctype < DSD_SYNC_DMR_MS_VOICE) {
                sprintf(state->slot1light, " slot1 ");
                sprintf(state->slot2light, " slot2 ");
                //we can safely open MBE on any MS or mono handling
                if ((opts->mbe_out_dir[0] != 0) && (opts->mbe_out_f == NULL)) {
                    openMbeOutFile(opts, state);
                }
                if (opts->p25_trunk == 0) {
                    dmrMSBootstrap(opts, state);
                }
            }
            if (opts->dmr_mono == 1 && state->synctype == DSD_SYNC_DMR_MS_VOICE) {
                //we can safely open MBE on any MS or mono handling
                if ((opts->mbe_out_dir[0] != 0) && (opts->mbe_out_f == NULL)) {
                    openMbeOutFile(opts, state);
                }
                if (opts->p25_trunk == 0) {
                    dmrMSBootstrap(opts, state);
                }
            }
            if (opts->dmr_stereo == 1) //opts->dmr_stereo == 1
            {
                state->dmr_stereo = 1; //set the state to 1 when handling pure voice frames
                if (state->synctype >= DSD_SYNC_DMR_MS_VOICE) {
                    //we can safely open MBE on any MS or mono handling
                    if ((opts->mbe_out_dir[0] != 0) && (opts->mbe_out_f == NULL)) {
                        openMbeOutFile(opts, state);
                    }
                    if (opts->p25_trunk == 0) {
                        dmrMSBootstrap(opts, state); //bootstrap into MS Bootstrap (voice only)
                    }
                } else {
                    dmrBSBootstrap(opts, state); //bootstrap into BS Bootstrap
                }
            }
        } else if ((state->synctype == DSD_SYNC_DMR_MS_DATA)
                   || (state->synctype == DSD_SYNC_DMR_RC_DATA)) //MS Data and RC data
        {
            if (opts->mbe_out_f != NULL) {
                closeMbeOutFile(opts, state);
            }
            if (opts->mbe_out_fR != NULL) {
                closeMbeOutFileR(opts, state);
            }
            if (opts->p25_trunk == 0) {
                dmrMSData(opts, state);
            }
        } else {
            if (opts->dmr_stereo == 0) {
                if (opts->mbe_out_f != NULL) {
                    closeMbeOutFile(opts, state);
                }
                if (opts->mbe_out_fR != NULL) {
                    closeMbeOutFileR(opts, state);
                }

                state->err_str[0] = 0;
                sprintf(state->slot1light, " slot1 ");
                sprintf(state->slot2light, " slot2 ");
                dmr_data_sync(opts, state);
            }
            //switch dmr_stereo to 0 when handling BS data frame syncs with processDMRdata
            if (opts->dmr_stereo == 1) {
                if (opts->mbe_out_f != NULL) {
                    closeMbeOutFile(opts, state);
                }
                if (opts->mbe_out_fR != NULL) {
                    closeMbeOutFileR(opts, state);
                }

                state->dmr_stereo = 0; //set the state to zero for handling pure data frames
                sprintf(state->slot1light, " slot1 ");
                sprintf(state->slot2light, " slot2 ");
                dmr_data_sync(opts, state);
            }
        }
        return;
    }
}

static void
handle_dpmr(dsd_opts* opts, dsd_state* state) {

    //dPMR
    if ((state->synctype == DSD_SYNC_DPMR_FS1_POS) || (state->synctype == DSD_SYNC_DPMR_FS1_NEG)) {
        /* dPMR Frame Sync 1 */
        fprintf(stderr, "dPMR Frame Sync 1 ");
        if (opts->mbe_out_f != NULL) {
            closeMbeOutFile(opts, state);
        }
    } else if ((state->synctype == DSD_SYNC_DPMR_FS2_POS) || (state->synctype == DSD_SYNC_DPMR_FS2_NEG)) {
        /* dPMR Frame Sync 2 */
        fprintf(stderr, "dPMR Frame Sync 2 ");

        state->nac = 0;
        state->lastsrc = 0;
        state->lasttg = 0;
        state->nac = 0;

        if ((opts->mbe_out_dir[0] != 0) && (opts->mbe_out_f == NULL)) {
            openMbeOutFile(opts, state);
        }
        sprintf(state->fsubtype, " VOICE        ");
        processdPMRvoice(opts, state);

        return;

    } else if ((state->synctype == DSD_SYNC_DPMR_FS3_POS) || (state->synctype == DSD_SYNC_DPMR_FS3_NEG)) {
        /* dPMR Frame Sync 3 */
        fprintf(stderr, "dPMR Frame Sync 3 ");
        if (opts->mbe_out_f != NULL) {
            closeMbeOutFile(opts, state);
        }
    } else if ((state->synctype == DSD_SYNC_DPMR_FS4_POS) || (state->synctype == DSD_SYNC_DPMR_FS4_NEG)) {
        /* dPMR Frame Sync 4 */
        fprintf(stderr, "dPMR Frame Sync 4 ");
        if (opts->mbe_out_f != NULL) {
            closeMbeOutFile(opts, state);
        }
    }
    //dPMR
}

static void
handle_p25p1(dsd_opts* opts, dsd_state* state) {

    int i, j, dibit;
    char duid[3];
    char nac[13];
    UNUSED(nac);

    char bch_code[63];
    int index_bch_code;
    unsigned char parity;
    char v;
    int new_nac;
    char new_duid[3];
    int check_result;

    nac[12] = 0;
    duid[2] = 0;

    // Read the NAC, 12 bits
    j = 0;
    index_bch_code = 0;
    for (i = 0; i < 6; i++) {
        dibit = getDibit(opts, state);

        v = 1 & (dibit >> 1); // bit 1
        nac[j] = v + '0';
        j++;
        bch_code[index_bch_code] = v;
        index_bch_code++;

        v = 1 & dibit; // bit 0
        nac[j] = v + '0';
        j++;
        bch_code[index_bch_code] = v;
        index_bch_code++;
    }
    //this one setting bogus nac data
    // state->nac = strtol (nac, NULL, 2);

    // Read the DUID, 4 bits
    for (i = 0; i < 2; i++) {
        dibit = getDibit(opts, state);
        duid[i] = dibit + '0';

        bch_code[index_bch_code] = 1 & (dibit >> 1); // bit 1
        index_bch_code++;
        bch_code[index_bch_code] = 1 & dibit; // bit 0
        index_bch_code++;
    }

    // Read the BCH data for error correction of NAC and DUID
    for (i = 0; i < 3; i++) {
        dibit = getDibit(opts, state);

        bch_code[index_bch_code] = 1 & (dibit >> 1); // bit 1
        index_bch_code++;
        bch_code[index_bch_code] = 1 & dibit; // bit 0
        index_bch_code++;
    }
    // Intermission: read and discard the status dibit
    (void)getDibit(opts, state);
    // ... continue reading the BCH error correction data
    for (i = 0; i < 20; i++) {
        dibit = getDibit(opts, state);

        bch_code[index_bch_code] = 1 & (dibit >> 1); // bit 1
        index_bch_code++;
        bch_code[index_bch_code] = 1 & dibit; // bit 0
        index_bch_code++;
    }

    // Read the parity bit
    dibit = getDibit(opts, state);
    bch_code[index_bch_code] = 1 & (dibit >> 1); // bit 1
    parity = (1 & dibit);                        // bit 0

    // Check if the NID is correct
    check_result = check_NID(bch_code, &new_nac, new_duid, parity);
    if (check_result == 1) {
        if (new_nac != state->nac) {
            // NAC fixed by error correction
            state->nac = new_nac;
            //apparently, both 0 and 0xFFF can the BCH code on signal drop
            if (state->p2_hardset == 0 && new_nac != 0 && new_nac != 0xFFF) {
                state->p2_cc = new_nac;
            }
            state->debug_header_errors++;
        }
        if (strcmp(new_duid, duid) != 0) {
            // DUID fixed by error correction
            //fprintf (stderr,"Fixing DUID %s -> %s\n", duid, new_duid);
            duid[0] = new_duid[0];
            duid[1] = new_duid[1];
            state->debug_header_errors++;
        }
    } else {
        if (check_result == -1 && opts->verbose > 0) {
            fprintf(stderr, "%s", KRED);
            fprintf(stderr, " NID PARITY MISMATCH ");
            fprintf(stderr, "%s", KNRM);
        }
        // Check of NID failed and unable to recover its value
        //fprintf (stderr,"NID error\n");
        duid[0] = 'E';
        duid[1] = 'E';
        state->debug_header_critical_errors++;
    }

    if (strcmp(duid, "00") == 0) {
        // Header Data Unit
        if (opts->errorbars == 1) {
            printFrameInfo(opts, state);
            fprintf(stderr, " HDU\n");
        }
        if (opts->mbe_out_dir[0] != 0) {
            if (opts->mbe_out_f != NULL) {
                closeMbeOutFile(opts, state);
            }
            if (opts->mbe_out_f == NULL) {
                openMbeOutFile(opts, state);
            }
        }
        mbe_initMbeParms(state->cur_mp, state->prev_mp, state->prev_mp_enhanced);
        state->lastp25type = 2;
        state->dmrburstL = 25;
        state->currentslot = 0;
        sprintf(state->fsubtype, " HDU          ");
        processHDU(opts, state);
    } else if (strcmp(duid, "11") == 0) {
        // Logical Link Data Unit 1
        if (opts->errorbars == 1) {
            printFrameInfo(opts, state);
            fprintf(stderr, " LDU1  ");
        }
        if (opts->mbe_out_dir[0] != 0) {
            if (opts->mbe_out_f == NULL) {
                openMbeOutFile(opts, state);
            }
        }
        state->lastp25type = 1;
        state->dmrburstL = 26;
        state->currentslot = 0;
        sprintf(state->fsubtype, " LDU1         ");
        state->numtdulc = 0;

        processLDU1(opts, state);
    } else if (strcmp(duid, "22") == 0) {
        // Logical Link Data Unit 2
        state->dmrburstL = 27;
        state->currentslot = 0;
        if (opts->errorbars == 1) {
            printFrameInfo(opts, state);
            if (state->lastp25type != 1) {
                // Late entry: short calls or mid-call tuning can land on an
                // LDU2 first. Decode it anyway so voice isn't lost.
                fprintf(stderr, " LDU2 (late entry)  ");
            } else {
                fprintf(stderr, " LDU2  ");
            }
        }
        if (opts->mbe_out_dir[0] != 0) {
            if (opts->mbe_out_f == NULL) {
                openMbeOutFile(opts, state);
            }
        }
        state->lastp25type = 2;
        sprintf(state->fsubtype, " LDU2         ");
        state->numtdulc = 0;
        processLDU2(opts, state);
    } else if (strcmp(duid, "33") == 0) {
        // Terminator with subsequent Link Control
        state->dmrburstL = 28;
        if (opts->errorbars == 1) {
            printFrameInfo(opts, state);
            fprintf(stderr, " TDULC\n");
        }
        if (opts->mbe_out_dir[0] != 0) {
            if (opts->mbe_out_f != NULL) {
                closeMbeOutFile(opts, state);
            }
        }
        mbe_initMbeParms(state->cur_mp, state->prev_mp, state->prev_mp_enhanced);
        // state->lasttg = 0;
        // state->lastsrc = 0;
        state->lastp25type = 0;
        state->err_str[0] = 0;
        sprintf(state->fsubtype, " TDULC        ");
        // Clear GPS data on call termination
        state->dmr_embedded_gps[0][0] = '\0';
        state->dmr_lrrp_gps[0][0] = '\0';
        state->numtdulc++;
        if ((opts->resume > 0) && (state->numtdulc > opts->resume)) {
            resumeScan(opts, state);
        }
        processTDULC(opts, state);
        state->err_str[0] = 0;
    } else if (strcmp(duid, "03") == 0) {
        // Terminator without subsequent Link Control
        state->dmrburstL = 28;
        if (opts->errorbars == 1) {
            printFrameInfo(opts, state);
            fprintf(stderr, " TDU\n");
        }
        if (opts->mbe_out_dir[0] != 0) {
            if (opts->mbe_out_f != NULL) {
                closeMbeOutFile(opts, state);
            }
        }
        mbe_initMbeParms(state->cur_mp, state->prev_mp, state->prev_mp_enhanced);
        state->lasttg = 0;
        state->lastsrc = 0;
        state->lastp25type = 0;
        state->err_str[0] = 0;
        sprintf(state->fsubtype, " TDU          ");
        // Clear GPS data on call termination
        state->dmr_embedded_gps[0][0] = '\0';
        state->dmr_lrrp_gps[0][0] = '\0';

        processTDU(opts, state);
    } else if (strcmp(duid, "13") == 0) {
        state->dmrburstL = 29;
        if (opts->errorbars == 1) {
            printFrameInfo(opts, state);
            fprintf(stderr, " TSBK");
        }
        if (opts->mbe_out_dir[0] != 0) {
            if (opts->mbe_out_f != NULL) {
                closeMbeOutFile(opts, state);
            }
            if (opts->mbe_out_fR != NULL) {
                closeMbeOutFileR(opts, state);
            }
        }
        if (opts->resume > 0) {
            resumeScan(opts, state);
        }
        state->lasttg = 0;
        state->lastsrc = 0;
        state->lastp25type = 3;
        sprintf(state->fsubtype, " TSBK         ");

        processTSBK(opts, state);

    } else if (strcmp(duid, "30") == 0) {
        state->dmrburstL = 29;
        if (opts->errorbars == 1) {
            printFrameInfo(opts, state);
            fprintf(stderr, " MPDU\n"); //multi block format PDU
        }
        if (opts->mbe_out_dir[0] != 0) {
            if (opts->mbe_out_f != NULL) {
                closeMbeOutFile(opts, state);
            }
            if (opts->mbe_out_fR != NULL) {
                closeMbeOutFileR(opts, state);
            }
        }
        if (opts->resume > 0) {
            resumeScan(opts, state);
        }
        state->lastp25type = 4;
        sprintf(state->fsubtype, " MPDU         ");

        processMPDU(opts, state);
    }

    else {
        state->lastp25type = 0;
        sprintf(state->fsubtype, "              ");
        if (opts->errorbars == 1) {
            printFrameInfo(opts, state);
            // fprintf (stderr," duid:%s *Unknown DUID*\n", duid);
            fprintf(stderr, " duid:%s \n", duid); //DUID ERR
        }
    }
}

const dsd_protocol_handler dsd_protocol_handlers[] = {
    {"NXDN", matches_nxdn, handle_nxdn, NULL},
    {"D-STAR", matches_dstar, handle_dstar, NULL},
    {"DMR", matches_dmr, handle_dmr, NULL},
    {"X2-TDMA", matches_x2tdma, handle_x2tdma, NULL},
    {"ProVoice", matches_provoice, handle_provoice, NULL},
    {"EDACS", matches_edacs, handle_edacs, NULL},
    {"YSF", matches_ysf, handle_ysf, NULL},
    {"M17", matches_m17, handle_m17, NULL},
    {"P25P2", matches_p25p2, handle_p25p2, NULL},
    {"dPMR", matches_dpmr, handle_dpmr, NULL},
    {"P25P1", matches_p25p1, handle_p25p1, NULL},
    {0, 0, 0, 0},
};
