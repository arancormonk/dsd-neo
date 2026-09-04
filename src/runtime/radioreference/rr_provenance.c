// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Provenance sidecars for generated RadioReference files.
 *
 * One "<csv>.rr" per generated CSV, so a later refresh can re-fetch the same
 * system and rebuild the same file. Written through the atomic replace helper
 * because a refresh always overwrites an existing sidecar, and a plain rename()
 * fails on Windows when the destination exists. Parsed leniently so the format
 * can grow: unknown keys are skipped, only a known key with an unparseable value
 * is an error.
 */

#include <dsd-neo/core/parse.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/string_utils.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/path_policy.h>
#include <dsd-neo/runtime/radioreference_generate.h>
#include <dsd-neo/runtime/radioreference_import.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int
rr_sidecar_path(const char* csv_path, char* out, size_t out_sz) {
    if (!csv_path || csv_path[0] == '\0' || !out || out_sz == 0) {
        return -1;
    }
    int n = DSD_SNPRINTF(out, out_sz, "%s.rr", csv_path);
    return n > 0 && (size_t)n < out_sz ? 0 : -1;
}

/* Emits "<key> = <value>\n" with control characters folded to spaces, so a
   system name carrying a newline cannot forge a second key line. */
static void
rr_emit_text(FILE* fp, const char* key, const char* value) {
    DSD_FPRINTF(fp, "%s = ", key);
    for (const unsigned char* p = (const unsigned char*)value; *p != '\0'; p++) {
        fputc((*p < 0x20 || *p == 0x7F) ? ' ' : (int)*p, fp);
    }
    fputc('\n', fp);
}

/* Takes ownership of fp: closes it on every path. */
static int
rr_provenance_emit(FILE* fp, const dsd_rr_provenance* p) {
    long long stamp = p->imported_at != 0 ? p->imported_at : (long long)time(NULL);

    DSD_FPRINTF(fp, "# dsd-neo RadioReference provenance. Regenerated on refresh; do not edit.\n");
    rr_emit_text(fp, "kind", p->kind);
    DSD_FPRINTF(fp, "sid = %d\n", p->sid);
    rr_emit_text(fp, "site_ids", p->site_ids);
    DSD_FPRINTF(fp, "partial_enc_as_de = %d\n", p->partial_enc_as_de ? 1 : 0);
    rr_emit_text(fp, "system_name", p->system_name);
    rr_emit_text(fp, "site_label", p->site_label);
    DSD_FPRINTF(fp, "imported_at = %lld\n", stamp);

    /* The re-apply recipe, only when there is one. A sidecar with no recipe (an
       older file, or one whose protocol a newer build wrote) omits these keys
       entirely rather than emitting a half-recipe the reader would reject. */
    if (p->recipe.present) {
        const char* token = dsd_rr_protocol_token(p->recipe.protocol);
        if (token != NULL) {
            rr_emit_text(fp, "protocol", token);
            DSD_FPRINTF(fp, "tune_hz = %lld\n", p->recipe.tune_hz);
            DSD_FPRINTF(fp, "trunking = %d\n", p->recipe.trunking ? 1 : 0);
            DSD_FPRINTF(fp, "scan_list = %d\n", p->recipe.scan_list ? 1 : 0);
            DSD_FPRINTF(fp, "simulcast = %d\n", p->recipe.simulcast ? 1 : 0);
            DSD_FPRINTF(fp, "esk = %d\n", p->recipe.esk ? 1 : 0);
        }
    }

    if (fflush(fp) != 0) {
        int saved_errno = errno;
        fclose(fp);
        errno = saved_errno;
        return -1;
    }

    int fd = dsd_fileno(fp);
    if (fd >= 0 && dsd_fsync(fd) != 0) {
        int saved_errno = errno;
        fclose(fp);
        errno = saved_errno;
        return -1;
    }

    return fclose(fp) == 0 ? 0 : -1;
}

int
dsd_rr_provenance_write(const char* csv_path, const dsd_rr_provenance* p) {
    if (!p) {
        return -1;
    }

    char rr_path[1024];
    if (rr_sidecar_path(csv_path, rr_path, sizeof rr_path) != 0) {
        return -1;
    }

    /* The helper appends ".tmp.XXXXXX" (11 chars) to rr_path and picks the name
       itself - never construct a temp path here. */
    char tmp[1088];
    FILE* fp = dsd_fopen_private_temp_for_replace(rr_path, tmp, sizeof tmp, "w");
    if (!fp) {
        return -1;
    }

    if (rr_provenance_emit(fp, p) != 0) {
        (void)remove(tmp);
        return -1;
    }

    /* dsd_replace_file_with_temp is rename()+dir fsync on POSIX and
       MoveFileExA(MOVEFILE_REPLACE_EXISTING) on Win32; a bare rename() would fail
       on Windows every time the sidecar already exists. */
    if (dsd_replace_file_with_temp(tmp, rr_path) != 0) {
        (void)remove(tmp);
        return -1;
    }
    return 0;
}

static char*
rr_trim(char* s) {
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) {
        s[--n] = '\0';
    }
    return s;
}

/* The keys whose value is copied verbatim. Split from rr_provenance_assign()
   so that function stays inside tools/lizard.sh --strict's CCN 15; every key
   here is one more branch in whichever function holds it.

   @return 1 when @p key was one of them, 0 to keep looking. */
static int
rr_provenance_assign_text(dsd_rr_provenance* out, const char* key, const char* value) {
    if (strcmp(key, "kind") == 0) {
        DSD_STRNCPY(out->kind, value, sizeof out->kind - 1);
        return 1;
    }
    if (strcmp(key, "site_ids") == 0) {
        DSD_STRNCPY(out->site_ids, value, sizeof out->site_ids - 1);
        return 1;
    }
    if (strcmp(key, "system_name") == 0) {
        DSD_STRNCPY(out->system_name, value, sizeof out->system_name - 1);
        return 1;
    }
    if (strcmp(key, "site_label") == 0) {
        DSD_STRNCPY(out->site_label, value, sizeof out->site_label - 1);
        return 1;
    }
    return 0;
}

static int
rr_provenance_assign(dsd_rr_provenance* out, const char* key, const char* value) {
    if (rr_provenance_assign_text(out, key, value)) {
        return 0;
    }
    if (strcmp(key, "sid") == 0) {
        return dsd_parse_int_strict(value, 10, 0, INT_MAX, &out->sid);
    }
    if (strcmp(key, "partial_enc_as_de") == 0) {
        return dsd_parse_int_strict(value, 10, 0, 1, &out->partial_enc_as_de);
    }
    if (strcmp(key, "imported_at") == 0) {
        /* dsd_parse_int_strict takes an int*, and `long` is 32-bit under MSVC, so
           neither can fill a long long. There is no long-long strict parser. */
        uint64_t secs = 0;
        if (dsd_parse_uint64_strict(value, 10, (uint64_t)INT64_MAX, &secs) != 0) {
            return -1;
        }
        out->imported_at = (long long)secs;
        return 0;
    }
    if (strcmp(key, "protocol") == 0) {
        /* An unknown token is a system a newer build wrote, not a corrupt file:
           it resolves to UNSUPPORTED, which rr_provenance_finalize_recipe() reads
           as "no usable recipe" rather than an error. */
        out->recipe.protocol = dsd_rr_protocol_from_token(value);
        return 0;
    }
    if (strcmp(key, "tune_hz") == 0) {
        /* Parsed at the writer's width, not an int's. rr_provenance_emit() prints
           %lld from a long long and rr_generate.c accepts sites up to
           RR_FREQ_MAX_HZ (6 GHz), so an int-ranged parse would make this reader
           reject a sidecar it wrote itself - and a rejected sidecar is not
           "no recipe", it drops the file out of the Imported Systems browser,
           out of the CSV picker and out of refresh entirely. Same uint64 parser
           imported_at uses, for the same "no long-long strict parser" reason.
           0 is tolerated and folds to present == 0; a negative value is
           rejected by the parser itself. */
        uint64_t hz = 0;
        if (dsd_parse_uint64_strict(value, 10, (uint64_t)INT64_MAX, &hz) != 0) {
            return -1;
        }
        out->recipe.tune_hz = (long long)hz;
        return 0;
    }
    if (strcmp(key, "trunking") == 0) {
        return dsd_parse_int_strict(value, 10, 0, 1, &out->recipe.trunking);
    }
    if (strcmp(key, "scan_list") == 0) {
        return dsd_parse_int_strict(value, 10, 0, 1, &out->recipe.scan_list);
    }
    if (strcmp(key, "simulcast") == 0) {
        return dsd_parse_int_strict(value, 10, 0, 1, &out->recipe.simulcast);
    }
    if (strcmp(key, "esk") == 0) {
        return dsd_parse_int_strict(value, 10, 0, 1, &out->recipe.esk);
    }
    return 0; /* unknown key: ignored so the format can grow */
}

static int
rr_provenance_parse_line(dsd_rr_provenance* out, char* line) {
    char* text = rr_trim(line);
    if (text[0] == '\0' || text[0] == '#') {
        return 0;
    }
    char* eq = strchr(text, '=');
    if (!eq) {
        return 0;
    }
    *eq = '\0';
    const char* key = rr_trim(text);
    const char* value = rr_trim(eq + 1);
    if (key[0] == '\0') {
        return 0;
    }
    return rr_provenance_assign(out, key, value);
}

static int
rr_provenance_parse(FILE* fp, dsd_rr_provenance* out) {
    /* COUPLED to dsd_rr_provenance::site_ids. That field is the longest value
       this writer can emit, so the longest line is "site_ids = " (11) + the
       value (at most sizeof - 1) + "\n" + NUL. Sized off the field so the two
       cannot drift: a buffer that is one byte short does not fail loudly, it
       lets fgets split the line and silently drop the tail, which carries no
       '=' and is skipped as an unknown key. A hand-edited longer line still
       splits that way, which is the intended limit. */
    char line[sizeof(((dsd_rr_provenance*)0)->site_ids) + 32];
    /* Absent protocol key must read as "no recipe", not as enum value 0 (P25).
       Set before the loop so a file with no protocol line finalises to
       UNSUPPORTED / present == 0. */
    out->recipe.protocol = DSD_RR_PROTO_UNSUPPORTED;
    while (fgets(line, (int)sizeof line, fp) != NULL) {
        if (rr_provenance_parse_line(out, line) != 0) {
            return -1;
        }
    }
    if (ferror(fp)) {
        return -1;
    }
    /* present is derived, never stored: a recipe is usable only when its
       protocol resolved and it carries a real tuning frequency. */
    out->recipe.present = (out->recipe.protocol != DSD_RR_PROTO_UNSUPPORTED && out->recipe.tune_hz > 0) ? 1 : 0;
    return 0;
}

int
dsd_rr_provenance_read(const char* csv_path, dsd_rr_provenance* out) {
    if (!out) {
        return -1;
    }

    char rr_path[1024];
    if (rr_sidecar_path(csv_path, rr_path, sizeof rr_path) != 0) {
        return -1;
    }

    /* Runtime path policy, not platform/: this is the reviewed read sink for a
       file the user selected. Three arguments - the out buffer is mandatory. */
    char opened[2048];
    FILE* fp = dsd_path_fopen_user_read_file(rr_path, opened, sizeof opened);
    if (!fp) {
        return -1;
    }

    dsd_rr_provenance parsed;
    DSD_MEMSET(&parsed, 0, sizeof parsed);
    int rc = rr_provenance_parse(fp, &parsed);
    if (fclose(fp) != 0) {
        rc = -1;
    }
    if (rc != 0) {
        return -1; /* leave *out untouched on failure */
    }

    DSD_MEMCPY(out, &parsed, sizeof parsed);
    return 0;
}
