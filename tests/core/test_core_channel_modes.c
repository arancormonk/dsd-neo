// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

#include <assert.h>
#include <dsd-neo/core/channel_mode.h>
#include <dsd-neo/core/csv_import.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/protocol/nxdn/nxdn_lfsr.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void
LFSRN(const char* in, char* out, dsd_state* state) {
    (void)in;
    (void)out;
    (void)state;
}

static void
write_csv(const char* path, const char* content) {
    FILE* fp = dsd_fopen_private(path, "w");
    assert(fp);
    assert(fputs(content, fp) >= 0);
    assert(fclose(fp) == 0);
}

static void
clear(dsd_state* s) {
    dsd_state_trunk_lcn_free(s);
    dsd_state_ext_free_all(s);
    DSD_MEMSET(s, 0, sizeof(*s));
}

int
main(void) {
    dsd_opts* o = (dsd_opts*)calloc(1, sizeof(*o));
    dsd_state* s = (dsd_state*)calloc(1, sizeof(*s));
    dsd_state* dst = (dsd_state*)calloc(1, sizeof(*dst));
    assert(o && s && dst);
    DSD_SNPRINTF(o->chan_in_file, sizeof(o->chan_in_file), "%s", "test-channel-modes.csv");
    write_csv(o->chan_in_file,
              "chan,freq,notes,MODE,name,name\n1,150000000,x, NXDN48 ,First,Other\n"
              "1,150000000,x,P25,Second,Other\n3,0,x,dmr,Placeholder,Other\n4,150000002,x,,Blank,Other\n");
    assert(csvChanImport(o, s) == 0 && s->lcn_freq_count == 4);
    assert(dsd_channel_mode_get(s, 0) == DSD_SCAN_MODE_NXDN48);
    assert(dsd_channel_mode_get(s, 1) == DSD_SCAN_MODE_P25);
    assert(dsd_channel_mode_get(s, 2) == DSD_SCAN_MODE_DMR);
    assert(dsd_channel_mode_get(s, 3) == DSD_SCAN_MODE_INHERIT);
    assert(strcmp(dsd_state_trunk_lcn_name_get(s, 0), "First") == 0);
    assert(*dsd_state_trunk_lcn_slot(s, 0) == *dsd_state_trunk_lcn_slot(s, 1));
    assert(*dsd_state_trunk_lcn_slot(s, 2) == 0);
    assert(dsd_channel_mode_set(dst, 0, DSD_SCAN_MODE_M17) == 0);
    dsd_channel_modes_move(dst, s);
    assert(!s->state_ext[DSD_STATE_EXT_CORE_CHANNEL_MODES]);
    assert(dsd_channel_mode_get(dst, 1) == DSD_SCAN_MODE_P25);
    clear(s);
    write_csv(o->chan_in_file, "chan,freq,a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p,mode,name,single_key_dec\n"
                               "1,150000000,,,,,,,,,,,,,,,,,nxdn96,Late,123\n");
    assert(csvChanImport(o, s) == 0);
    assert(dsd_channel_mode_get(s, 0) == DSD_SCAN_MODE_NXDN96);
    assert(strcmp(dsd_state_trunk_lcn_name_get(s, 0), "Late") == 0);
    clear(s);
    write_csv(o->chan_in_file, "chan,freq,mode\ninvalid,150000000,typo\n");
    assert(csvChanImport(o, s) == -1);
    clear(s);
    write_csv(o->chan_in_file, "chan,freq,mode,MoDe\n1,150000000,dmr,dmr\n");
    assert(csvChanImport(o, s) == -1);
    clear(s);
    write_csv(o->chan_in_file, "chan,freq,mode\ninvalid,150000000,dmr\n1,150000000,m17\n");
    assert(csvChanImport(o, s) == 0 && s->lcn_freq_count == 1);
    assert(dsd_channel_mode_get(s, 0) == DSD_SCAN_MODE_M17);
    clear(s);
    FILE* fp = dsd_fopen_private(o->chan_in_file, "w");
    assert(fp);
    DSD_FPRINTF(fp, "chan,freq,mode\n");
    for (int i = 0; i < 128; i++) {
        DSD_FPRINTF(fp, "%d,150000000,%s\n", i, i % 2 ? "dmr" : "nxdn48");
    }
    assert(fclose(fp) == 0);
    assert(csvChanImport(o, s) == 0 && s->lcn_freq_count == 128);
    assert(dsd_channel_mode_get(s, 127) == DSD_SCAN_MODE_DMR);
    clear(s);
    write_csv(o->chan_in_file, "chan,freq,notes\n1,150000000,dmr\n");
    assert(csvChanImport(o, s) == 0 && !dsd_channel_modes_present(s));
    assert(dsd_channel_mode_set(s, 0, DSD_SCAN_MODE_DMR) == 0);
    assert(dsd_channel_modes_present(s));
    assert(dsd_channel_mode_set(s, 0, DSD_SCAN_MODE_P25) == 0);
    assert(dsd_channel_modes_present(s));
    assert(dsd_channel_mode_set(s, 0, DSD_SCAN_MODE_INHERIT) == 0);
    assert(!dsd_channel_modes_present(s));
    clear(s);
    fp = dsd_fopen_private(o->chan_in_file, "w");
    assert(fp);
    DSD_FPRINTF(fp, "chan,freq,notes,mode\n1,150000000,");
    for (int i = 0; i < 2048; i++) {
        assert(fputc('x', fp) != EOF);
    }
    DSD_FPRINTF(fp, ",nxdn48\n2,150000001,,dmr\n");
    assert(fclose(fp) == 0);
    assert(csvChanImport(o, s) == 0 && s->lcn_freq_count == 2);
    assert(dsd_channel_mode_get(s, 0) == DSD_SCAN_MODE_NXDN48);
    assert(dsd_channel_mode_get(s, 1) == DSD_SCAN_MODE_DMR);
    clear(s);
    fp = dsd_fopen_private(o->chan_in_file, "w");
    assert(fp);
    DSD_FPRINTF(fp, "chan,freq");
    for (int i = 0; i < 400; i++) {
        DSD_FPRINTF(fp, ",ignored");
    }
    DSD_FPRINTF(fp, ",mode\n1,150000000");
    for (int i = 0; i < 400; i++) {
        assert(fputc(',', fp) != EOF);
    }
    DSD_FPRINTF(fp, ",p25"); /* EOF without a newline is still one complete row. */
    assert(fclose(fp) == 0);
    assert(csvChanImport(o, s) == 0 && s->lcn_freq_count == 1);
    assert(dsd_channel_mode_get(s, 0) == DSD_SCAN_MODE_P25);
    clear(s);
    fp = dsd_fopen_private(o->chan_in_file, "w");
    assert(fp);
    DSD_FPRINTF(fp, "chan,freq,notes,mode\n1,150000000,");
    for (int i = 0; i < 1024 * 1024; i++) {
        assert(fputc('x', fp) != EOF);
    }
    DSD_FPRINTF(fp, ",dmr\n");
    assert(fclose(fp) == 0);
    assert(csvChanImport(o, s) == -1 && s->lcn_freq_count == 0);
    assert(remove(o->chan_in_file) == 0);
    clear(s);
    clear(dst);
    free(dst);
    free(s);
    free(o);
    return 0;
}
