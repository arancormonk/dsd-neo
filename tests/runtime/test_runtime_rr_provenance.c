// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/string_utils.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/radioreference_generate.h>
#include <dsd-neo/runtime/radioreference_import.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "dsd-neo/platform/platform.h"
#include "test_support.h"

#if DSD_PLATFORM_WIN_NATIVE
#include <direct.h>
#define DSD_TEST_RMDIR _rmdir
#else
#include <unistd.h>
#define DSD_TEST_RMDIR rmdir
#endif

static int
make_paths(char* scratch, size_t scratch_sz, char* csv, size_t csv_sz, char* rr, size_t rr_sz, const char* prefix) {
    if (dsd_test_mkdtemp(scratch, scratch_sz, prefix) == NULL) {
        DSD_FPRINTF(stderr, "dsd_test_mkdtemp failed: %s\n", strerror(errno));
        return 1;
    }
    if (dsd_test_path_join(csv, csv_sz, scratch, "SARA System group.csv") != 0) {
        return 1;
    }
    int n = DSD_SNPRINTF(rr, rr_sz, "%s.rr", csv);
    return n > 0 && (size_t)n < rr_sz ? 0 : 1;
}

static void
fill_sample(dsd_rr_provenance* p) {
    DSD_MEMSET(p, 0, sizeof *p);
    DSD_STRNCPY(p->kind, "group", sizeof p->kind - 1);
    p->sid = 12059;
    DSD_STRNCPY(p->site_ids, "4181,4182", sizeof p->site_ids - 1);
    p->partial_enc_as_de = 1;
    DSD_STRNCPY(p->system_name, "SARA System", sizeof p->system_name - 1);
    p->imported_at = 1755500000LL;
}

static int
write_sidecar_text(const char* rr_path, const char* body) {
    FILE* fp = dsd_fopen_private(rr_path, "w");
    if (!fp) {
        return 1;
    }
    int rc = fputs(body, fp) < 0 ? 1 : 0;
    rc |= fclose(fp) != 0 ? 1 : 0;
    return rc;
}

static int
expect_round_trip(void) {
    char scratch[DSD_TEST_PATH_MAX];
    char csv[DSD_TEST_PATH_MAX];
    char rr[DSD_TEST_PATH_MAX];
    if (make_paths(scratch, sizeof scratch, csv, sizeof csv, rr, sizeof rr, "dsd_neo_rr_prov") != 0) {
        return 1;
    }

    dsd_rr_provenance p;
    fill_sample(&p);

    int rc = dsd_rr_provenance_write(csv, &p) == 0 ? 0 : 1;

    dsd_rr_provenance got;
    DSD_MEMSET(&got, 0, sizeof got);
    rc |= dsd_rr_provenance_read(csv, &got) == 0 ? 0 : 1;
    rc |= strcmp(got.kind, "group") == 0 ? 0 : 1;
    rc |= got.sid == 12059 ? 0 : 1;
    rc |= strcmp(got.site_ids, "4181,4182") == 0 ? 0 : 1;
    rc |= got.partial_enc_as_de == 1 ? 0 : 1;
    rc |= strcmp(got.system_name, "SARA System") == 0 ? 0 : 1;
    rc |= got.imported_at == 1755500000LL ? 0 : 1;

    /* Overwrite an existing sidecar: this is the Windows replace-existing path. */
    p.sid = 9340;
    rc |= dsd_rr_provenance_write(csv, &p) == 0 ? 0 : 1;
    DSD_MEMSET(&got, 0, sizeof got);
    rc |= dsd_rr_provenance_read(csv, &got) == 0 ? 0 : 1;
    rc |= got.sid == 9340 ? 0 : 1;

    /* No leftover temp file. */
    dsd_stat_t st;
    char tmp_glob[DSD_TEST_PATH_MAX];
    rc |= DSD_SNPRINTF(tmp_glob, sizeof tmp_glob, "%s.tmp.XXXXXX", rr) > 0 ? 0 : 1;
    rc |= dsd_stat_path(tmp_glob, &st) != 0 ? 0 : 1;

    (void)remove(rr);
    (void)DSD_TEST_RMDIR(scratch);
    return rc;
}

/*
 * A site_ids value that fills the field is the one case that couples this
 * struct to rr_provenance_parse()'s line buffer: the writer emits
 * "site_ids = " + the value + "\n", and a reader whose fgets() buffer is
 * shorter silently keeps the head and drops the tail (the tail carries no
 * '=' and is skipped as an unknown line). Sized off the field so it tracks
 * any future widening automatically.
 */
static int
expect_full_width_site_ids_round_trip(void) {
    char scratch[DSD_TEST_PATH_MAX];
    char csv[DSD_TEST_PATH_MAX];
    char rr[DSD_TEST_PATH_MAX];
    if (make_paths(scratch, sizeof scratch, csv, sizeof csv, rr, sizeof rr, "dsd_neo_rr_prov_wide") != 0) {
        return 1;
    }

    dsd_rr_provenance p;
    fill_sample(&p);

    /* Five-digit ids, the width RadioReference actually issues, packed until
     * one more would not fit. */
    char ids[sizeof p.site_ids];
    size_t len = 0;
    int next = 10000;
    for (;;) {
        const int n = DSD_SNPRINTF(ids + len, sizeof ids - len, "%s%d", (len == 0U) ? "" : ",", next);
        if (n <= 0 || (size_t)n >= sizeof ids - len) {
            ids[len] = '\0';
            break;
        }
        len += (size_t)n;
        next++;
    }
    DSD_STRNCPY(p.site_ids, ids, sizeof p.site_ids - 1);

    int rc = len > sizeof p.site_ids / 2 ? 0 : 1; /* the value really is near-full */
    rc |= dsd_rr_provenance_write(csv, &p) == 0 ? 0 : 1;

    dsd_rr_provenance got;
    DSD_MEMSET(&got, 0, sizeof got);
    rc |= dsd_rr_provenance_read(csv, &got) == 0 ? 0 : 1;
    /* Byte for byte: a split line would truncate this and nothing else. */
    rc |= strcmp(got.site_ids, p.site_ids) == 0 ? 0 : 1;
    /* Keys written after site_ids must still arrive - a split line's tail
     * being skipped would not stop the parse, it would just lose them. */
    rc |= strcmp(got.system_name, "SARA System") == 0 ? 0 : 1;
    rc |= got.imported_at == 1755500000LL ? 0 : 1;

    remove(rr);
    remove(csv);
    DSD_TEST_RMDIR(scratch);
    return rc;
}

static int
expect_zero_timestamp_is_stamped(void) {
    char scratch[DSD_TEST_PATH_MAX];
    char csv[DSD_TEST_PATH_MAX];
    char rr[DSD_TEST_PATH_MAX];
    if (make_paths(scratch, sizeof scratch, csv, sizeof csv, rr, sizeof rr, "dsd_neo_rr_stamp") != 0) {
        return 1;
    }

    dsd_rr_provenance p;
    fill_sample(&p);
    p.imported_at = 0;

    int rc = dsd_rr_provenance_write(csv, &p) == 0 ? 0 : 1;

    dsd_rr_provenance got;
    DSD_MEMSET(&got, 0, sizeof got);
    rc |= dsd_rr_provenance_read(csv, &got) == 0 ? 0 : 1;
    rc |= got.imported_at > 1700000000LL ? 0 : 1;

    (void)remove(rr);
    (void)DSD_TEST_RMDIR(scratch);
    return rc;
}

static int
expect_missing_sidecar_fails(void) {
    char scratch[DSD_TEST_PATH_MAX];
    char csv[DSD_TEST_PATH_MAX];
    char rr[DSD_TEST_PATH_MAX];
    if (make_paths(scratch, sizeof scratch, csv, sizeof csv, rr, sizeof rr, "dsd_neo_rr_missing") != 0) {
        return 1;
    }

    dsd_rr_provenance got;
    fill_sample(&got);
    int rc = dsd_rr_provenance_read(csv, &got) == -1 ? 0 : 1;
    /* A failed read must not clobber the caller's struct. */
    rc |= got.sid == 12059 ? 0 : 1;

    rc |= dsd_rr_provenance_read(NULL, &got) == -1 ? 0 : 1;
    rc |= dsd_rr_provenance_read(csv, NULL) == -1 ? 0 : 1;
    rc |= dsd_rr_provenance_write(csv, NULL) == -1 ? 0 : 1;

    (void)DSD_TEST_RMDIR(scratch);
    return rc;
}

static int
expect_unknown_keys_are_ignored(void) {
    char scratch[DSD_TEST_PATH_MAX];
    char csv[DSD_TEST_PATH_MAX];
    char rr[DSD_TEST_PATH_MAX];
    if (make_paths(scratch, sizeof scratch, csv, sizeof csv, rr, sizeof rr, "dsd_neo_rr_unknown") != 0) {
        return 1;
    }

    int rc = write_sidecar_text(rr, "# dsd-neo RadioReference provenance. Regenerated on refresh; do not edit.\n"
                                    "kind = chan\n"
                                    "sid = 6673\n"
                                    "site_ids = 4181\n"
                                    "partial_enc_as_de = 0\n"
                                    "system_name = Example\n"
                                    "imported_at = 1755500000\n"
                                    "future_key = whatever\n"
                                    "\n"
                                    "a line with no separator\n");

    dsd_rr_provenance got;
    DSD_MEMSET(&got, 0, sizeof got);
    rc |= dsd_rr_provenance_read(csv, &got) == 0 ? 0 : 1;
    rc |= strcmp(got.kind, "chan") == 0 ? 0 : 1;
    rc |= got.sid == 6673 ? 0 : 1;
    rc |= strcmp(got.site_ids, "4181") == 0 ? 0 : 1;
    rc |= got.partial_enc_as_de == 0 ? 0 : 1;

    (void)remove(rr);
    (void)DSD_TEST_RMDIR(scratch);
    return rc;
}

static int
expect_malformed_values_fail(void) {
    char scratch[DSD_TEST_PATH_MAX];
    char csv[DSD_TEST_PATH_MAX];
    char rr[DSD_TEST_PATH_MAX];
    if (make_paths(scratch, sizeof scratch, csv, sizeof csv, rr, sizeof rr, "dsd_neo_rr_bad") != 0) {
        return 1;
    }

    dsd_rr_provenance got;
    int rc = write_sidecar_text(rr, "kind = group\nsid = junk\n");
    DSD_MEMSET(&got, 0, sizeof got);
    rc |= dsd_rr_provenance_read(csv, &got) == -1 ? 0 : 1;

    rc |= write_sidecar_text(rr, "kind = group\nsid = 1\npartial_enc_as_de = 2\n");
    DSD_MEMSET(&got, 0, sizeof got);
    rc |= dsd_rr_provenance_read(csv, &got) == -1 ? 0 : 1;

    rc |= write_sidecar_text(rr, "kind = group\nimported_at = -1\n");
    DSD_MEMSET(&got, 0, sizeof got);
    rc |= dsd_rr_provenance_read(csv, &got) == -1 ? 0 : 1;

    (void)remove(rr);
    (void)DSD_TEST_RMDIR(scratch);
    return rc;
}

/* ---- Recipe: how the import was applied ---------------------------------- */

static void
fill_recipe(dsd_rr_recipe* r) {
    DSD_MEMSET(r, 0, sizeof *r);
    r->present = 1;
    r->protocol = DSD_RR_PROTO_P25;
    r->tune_hz = 769768750LL;
    r->trunking = 1;
    r->scan_list = 0;
    r->simulcast = 1;
    r->esk = 0;
}

static int
expect_recipe_round_trip(void) {
    char scratch[DSD_TEST_PATH_MAX];
    char csv[DSD_TEST_PATH_MAX];
    char rr[DSD_TEST_PATH_MAX];
    if (make_paths(scratch, sizeof scratch, csv, sizeof csv, rr, sizeof rr, "dsd_neo_rr_recipe") != 0) {
        return 1;
    }

    dsd_rr_provenance p;
    fill_sample(&p);
    fill_recipe(&p.recipe);

    int rc = dsd_rr_provenance_write(csv, &p) == 0 ? 0 : 1;

    dsd_rr_provenance got;
    DSD_MEMSET(&got, 0, sizeof got);
    rc |= dsd_rr_provenance_read(csv, &got) == 0 ? 0 : 1;
    rc |= got.recipe.present == 1 ? 0 : 1;
    rc |= got.recipe.protocol == DSD_RR_PROTO_P25 ? 0 : 1;
    rc |= got.recipe.tune_hz == 769768750LL ? 0 : 1;
    rc |= got.recipe.trunking == 1 ? 0 : 1;
    rc |= got.recipe.scan_list == 0 ? 0 : 1;
    rc |= got.recipe.simulcast == 1 ? 0 : 1;
    rc |= got.recipe.esk == 0 ? 0 : 1;
    /* The provenance half still round-trips beside it. */
    rc |= got.sid == 12059 ? 0 : 1;
    rc |= strcmp(got.system_name, "SARA System") == 0 ? 0 : 1;

    /* A conventional scan list, the other shape the wizard produces. */
    p.recipe.protocol = DSD_RR_PROTO_DMR_CONV;
    p.recipe.trunking = 0;
    p.recipe.scan_list = 1;
    p.recipe.simulcast = 0;
    p.recipe.esk = 0;
    p.recipe.tune_hz = 462562500LL;
    rc |= dsd_rr_provenance_write(csv, &p) == 0 ? 0 : 1;
    DSD_MEMSET(&got, 0, sizeof got);
    rc |= dsd_rr_provenance_read(csv, &got) == 0 ? 0 : 1;
    rc |= got.recipe.present == 1 ? 0 : 1;
    rc |= got.recipe.protocol == DSD_RR_PROTO_DMR_CONV ? 0 : 1;
    rc |= got.recipe.trunking == 0 && got.recipe.scan_list == 1 ? 0 : 1;
    rc |= got.recipe.tune_hz == 462562500LL ? 0 : 1;

    (void)remove(rr);
    (void)DSD_TEST_RMDIR(scratch);
    return rc;
}

/* A sidecar written before the recipe existed still reads, with no recipe. */
static int
expect_sidecar_without_recipe_reads_as_files_only(void) {
    char scratch[DSD_TEST_PATH_MAX];
    char csv[DSD_TEST_PATH_MAX];
    char rr[DSD_TEST_PATH_MAX];
    if (make_paths(scratch, sizeof scratch, csv, sizeof csv, rr, sizeof rr, "dsd_neo_rr_norecipe") != 0) {
        return 1;
    }

    int rc = write_sidecar_text(rr, "kind = chan\n"
                                    "sid = 6673\n"
                                    "site_ids = 4181\n"
                                    "partial_enc_as_de = 0\n"
                                    "system_name = Example\n"
                                    "imported_at = 1755500000\n");

    dsd_rr_provenance got;
    DSD_MEMSET(&got, 0, sizeof got);
    got.recipe.present = 1; /* a stale value must not leak through */
    rc |= dsd_rr_provenance_read(csv, &got) == 0 ? 0 : 1;
    rc |= got.recipe.present == 0 ? 0 : 1;
    rc |= got.recipe.protocol == DSD_RR_PROTO_UNSUPPORTED ? 0 : 1;
    rc |= got.sid == 6673 ? 0 : 1;

    /* A writer that has no recipe emits none: the sidecar must not gain a
       protocol line claiming something it does not know. */
    rc |= dsd_rr_provenance_write(csv, &got) == 0 ? 0 : 1;
    FILE* fp = dsd_fopen_existing_regular_file(rr, "r");
    rc |= fp != NULL ? 0 : 1;
    if (fp) {
        char line[256];
        while (fgets(line, sizeof line, fp)) {
            rc |= strncmp(line, "protocol", 8) != 0 ? 0 : 1;
            rc |= strncmp(line, "tune_hz", 7) != 0 ? 0 : 1;
        }
        (void)fclose(fp);
    }

    (void)remove(rr);
    (void)DSD_TEST_RMDIR(scratch);
    return rc;
}

/* A protocol token from a newer build is a future system, not a corrupt file. */
static int
expect_unknown_protocol_token_degrades_to_files_only(void) {
    char scratch[DSD_TEST_PATH_MAX];
    char csv[DSD_TEST_PATH_MAX];
    char rr[DSD_TEST_PATH_MAX];
    if (make_paths(scratch, sizeof scratch, csv, sizeof csv, rr, sizeof rr, "dsd_neo_rr_future") != 0) {
        return 1;
    }

    int rc = write_sidecar_text(rr, "kind = chan\n"
                                    "sid = 6673\n"
                                    "site_ids = 4181\n"
                                    "system_name = Example\n"
                                    "protocol = tetra_future\n"
                                    "tune_hz = 420000000\n"
                                    "trunking = 1\n");

    dsd_rr_provenance got;
    DSD_MEMSET(&got, 0, sizeof got);
    rc |= dsd_rr_provenance_read(csv, &got) == 0 ? 0 : 1;
    rc |= got.recipe.present == 0 ? 0 : 1;
    rc |= got.recipe.protocol == DSD_RR_PROTO_UNSUPPORTED ? 0 : 1;
    rc |= got.sid == 6673 ? 0 : 1;

    /* A known protocol with no usable frequency is equally not applicable. */
    rc |= write_sidecar_text(rr, "kind = chan\nsid = 6673\nsite_ids = 4181\nprotocol = p25\ntune_hz = 0\n");
    DSD_MEMSET(&got, 0, sizeof got);
    rc |= dsd_rr_provenance_read(csv, &got) == 0 ? 0 : 1;
    rc |= got.recipe.present == 0 ? 0 : 1;

    (void)remove(rr);
    (void)DSD_TEST_RMDIR(scratch);
    return rc;
}

static int
expect_recipe_malformed_values_fail(void) {
    char scratch[DSD_TEST_PATH_MAX];
    char csv[DSD_TEST_PATH_MAX];
    char rr[DSD_TEST_PATH_MAX];
    if (make_paths(scratch, sizeof scratch, csv, sizeof csv, rr, sizeof rr, "dsd_neo_rr_recipe_bad") != 0) {
        return 1;
    }

    dsd_rr_provenance got;
    int rc = write_sidecar_text(rr, "kind = group\nsid = 1\nprotocol = p25\ntune_hz = junk\n");
    DSD_MEMSET(&got, 0, sizeof got);
    rc |= dsd_rr_provenance_read(csv, &got) == -1 ? 0 : 1;

    rc |= write_sidecar_text(rr, "kind = group\nsid = 1\nprotocol = p25\ntune_hz = 1\ntrunking = 2\n");
    DSD_MEMSET(&got, 0, sizeof got);
    rc |= dsd_rr_provenance_read(csv, &got) == -1 ? 0 : 1;

    rc |= write_sidecar_text(rr, "kind = group\nsid = 1\nprotocol = p25\ntune_hz = -5\n");
    DSD_MEMSET(&got, 0, sizeof got);
    rc |= dsd_rr_provenance_read(csv, &got) == -1 ? 0 : 1;

    /* But a frequency that simply does not fit an int is NOT malformed: the
       writer emits %lld from a long long and rr_generate accepts sites up to
       6 GHz, so an int-ranged reader would reject a sidecar this build wrote -
       and a rejected sidecar drops the system out of the browser, the CSV picker
       and refresh, taking kind/sid/system_name down with it. */
    rc |= write_sidecar_text(rr, "kind = chan\nsid = 4242\nsystem_name = High Band\nprotocol = p25\n"
                                 "tune_hz = 4900000000\ntrunking = 1\n");
    DSD_MEMSET(&got, 0, sizeof got);
    rc |= dsd_rr_provenance_read(csv, &got) == 0 ? 0 : 1;
    rc |= got.recipe.present == 1 ? 0 : 1;
    rc |= got.recipe.tune_hz == 4900000000LL ? 0 : 1;
    rc |= got.sid == 4242 ? 0 : 1;
    rc |= strcmp(got.system_name, "High Band") == 0 ? 0 : 1;

    /* And it round-trips through the writer at that width. */
    rc |= dsd_rr_provenance_write(csv, &got) == 0 ? 0 : 1;
    DSD_MEMSET(&got, 0, sizeof got);
    rc |= dsd_rr_provenance_read(csv, &got) == 0 ? 0 : 1;
    rc |= got.recipe.tune_hz == 4900000000LL ? 0 : 1;

    (void)remove(rr);
    (void)DSD_TEST_RMDIR(scratch);
    return rc;
}

/* The recipe is derived from the plan the wizard applied and rebuilds a plan
   dsd_app_rr_fill_apply_payload() accepts, so a stored system applies exactly
   like a fresh import did. */
static int
expect_recipe_from_plan_and_back(void) {
    dsd_rr_import_plan plan;
    DSD_MEMSET(&plan, 0, sizeof plan);
    plan.ok = 1;
    plan.protocol = DSD_RR_PROTO_EDACS_EA;
    plan.conventional = 0;
    plan.trunking = 1;
    plan.chan_need = 2;
    plan.scan_list = 0;
    plan.simulcast = 0;
    plan.esk = 1;
    plan.partial_enc_as_de = 1;
    plan.tune_hz = 851012500LL;

    dsd_rr_recipe recipe;
    DSD_MEMSET(&recipe, 0, sizeof recipe);
    dsd_rr_recipe_from_plan(&plan, &recipe);
    int rc = recipe.present == 1 ? 0 : 1;
    rc |= recipe.protocol == DSD_RR_PROTO_EDACS_EA ? 0 : 1;
    rc |= recipe.tune_hz == 851012500LL ? 0 : 1;
    rc |= recipe.trunking == 1 && recipe.scan_list == 0 && recipe.simulcast == 0 && recipe.esk == 1 ? 0 : 1;

    dsd_rr_import_plan back;
    DSD_MEMSET(&back, 0xA5, sizeof back); /* must be fully overwritten */
    rc |= dsd_rr_recipe_to_plan(&recipe, 1, &back) == 0 ? 0 : 1;
    rc |= back.ok == 1 ? 0 : 1;
    rc |= back.protocol == DSD_RR_PROTO_EDACS_EA ? 0 : 1;
    rc |= back.conventional == 0 && back.trunking == 1 ? 0 : 1;
    rc |= back.chan_need == 2 ? 0 : 1;
    rc |= back.scan_list == 0 && back.simulcast == 0 && back.esk == 1 ? 0 : 1;
    rc |= back.partial_enc_as_de == 1 ? 0 : 1;
    rc |= back.tune_hz == 851012500LL ? 0 : 1;
    rc |= strcmp(back.freq_mhz, "851.0125") == 0 ? 0 : 1;
    rc |= strcmp(back.decode_flag, "-fE") == 0 ? 0 : 1;
    rc |= back.blocked_reason[0] == '\0' ? 0 : 1;
    /* Nothing on the heap: the caller owns no text and no warnings. */
    rc |= back.group_csv_text == NULL && back.chan_csv_text == NULL ? 0 : 1;
    rc |= back.warnings.count == 0 ? 0 : 1;

    /* Conventional scan list rebuilds with its flag and without trunking. */
    recipe.protocol = DSD_RR_PROTO_P25_CONV;
    recipe.trunking = 0;
    recipe.scan_list = 1;
    recipe.simulcast = 1;
    recipe.esk = 0;
    rc |= dsd_rr_recipe_to_plan(&recipe, 0, &back) == 0 ? 0 : 1;
    rc |= back.conventional == 1 && back.trunking == 0 && back.scan_list == 1 ? 0 : 1;
    rc |= strcmp(back.decode_flag, "-mq -Y") == 0 ? 0 : 1;
    rc |= back.partial_enc_as_de == 0 ? 0 : 1;

    /* A blocked plan records nothing; a missing recipe rebuilds nothing. */
    plan.ok = 0;
    dsd_rr_recipe_from_plan(&plan, &recipe);
    rc |= recipe.present == 0 ? 0 : 1;
    rc |= dsd_rr_recipe_to_plan(&recipe, 0, &back) == -1 ? 0 : 1;
    rc |= back.ok == 0 ? 0 : 1;
    rc |= dsd_rr_recipe_to_plan(NULL, 0, &back) == -1 ? 0 : 1;
    rc |= dsd_rr_recipe_to_plan(&recipe, 0, NULL) == -1 ? 0 : 1;
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= expect_round_trip();
    rc |= expect_full_width_site_ids_round_trip();
    rc |= expect_zero_timestamp_is_stamped();
    rc |= expect_missing_sidecar_fails();
    rc |= expect_unknown_keys_are_ignored();
    rc |= expect_malformed_values_fail();
    rc |= expect_recipe_round_trip();
    rc |= expect_sidecar_without_recipe_reads_as_files_only();
    rc |= expect_unknown_protocol_token_degrades_to_files_only();
    rc |= expect_recipe_malformed_values_fail();
    rc |= expect_recipe_from_plan_and_back();
    return rc;
}
