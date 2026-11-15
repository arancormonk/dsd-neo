// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Learned over-the-air aliases are kept with the system they were heard on (B28): two systems give the same radio ID
 * to different radios, so system A's learned name never names system B's radio, in memory or after a restart that
 * reloads the group file. The aliases arrive through the real decoders (P25 Phase 1 link control for Motorola and
 * Tait, Phase 2 MAC for Harris); the reception's system is what dsd_state::rx_id holds, as a checked network status
 * leaves it.
 */

#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/alias_learn.h>
#include <dsd-neo/core/bit_packing.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/embedded_alias.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/key_set.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/scan_profile.h>
#include <dsd-neo/core/source_alias.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/protocol/p25/p25_vpdu.h>
#include <dsd-neo/runtime/scan_options.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

void p25_lcw(dsd_opts* opts, dsd_state* state, uint8_t LCW_bits[], uint8_t irrecoverable_errors);

static int g_fail;

static void
expect_true(const char* tag, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", tag);
        g_fail = 1;
    }
}

/* Two made-up systems that share radio ID 42. The alias records' own FQ-SUID names one home system for both, so the
 * scope can only come from the reception. */
enum {
    WACN_A = 0xBEE00,
    SYS_A = 0x123,
    WACN_B = 0xBEE00,
    SYS_B = 0x456,
    RID = 42,
};

static void
set_bits(uint8_t* bits, size_t start, size_t len, uint32_t value) {
    for (size_t i = 0; i < len; i++) {
        bits[start + i] = (uint8_t)((value >> (len - 1U - i)) & 1U);
    }
}

/* ---- Motorola alias record through Phase 1 link control (p25_lcw) ------------------------------------------ */

enum { RECORD_MAX_BITS = 56 + (200 * 8) + 16 };

typedef struct {
    uint8_t bits[RECORD_MAX_BITS];
    size_t nbits;
} alias_record;

/* The encoded octets that descramble to @p text (UTF-16BE octets). */
static void
find_encoding(const uint8_t* text, size_t count, uint8_t* encoded) {
    uint8_t decoded[200];
    DSD_MEMSET(encoded, 0, count);
    for (size_t i = 0; i < count; i++) {
        int found = 0;
        for (unsigned candidate = 0; candidate < 256U && !found; candidate++) {
            encoded[i] = (uint8_t)candidate;
            apx_embedded_alias_unscramble(encoded, count, (uint16_t)count, decoded, sizeof decoded);
            found = decoded[i] == text[i];
        }
        if (!found) {
            DSD_FPRINTF(stderr, "find_encoding: no encoding for octet %zu\n", i);
            exit(2);
        }
    }
}

static void
build_record(alias_record* r, uint32_t rid, const char* ascii) {
    uint8_t text[200];
    uint8_t encoded[200];
    const size_t n = strlen(ascii);
    for (size_t i = 0; i < n; i++) {
        text[(i * 2U) + 0U] = 0U;
        text[(i * 2U) + 1U] = (uint8_t)ascii[i];
    }
    find_encoding(text, n * 2U, encoded);
    DSD_MEMSET(r, 0, sizeof(*r));
    set_bits(r->bits, 0, 20, WACN_A);
    set_bits(r->bits, 20, 12, SYS_A);
    set_bits(r->bits, 32, 24, rid);
    for (size_t i = 0; i < n * 2U; i++) {
        set_bits(r->bits, 56U + (i * 8U), 8, encoded[i]);
    }
    const size_t body = 56U + (n * 16U);
    set_bits(r->bits, body, 16, dsd_crc_ccitt16_bits(r->bits, body));
    r->nbits = body + 16U;
}

static void
p1_send_record(dsd_opts* opts, dsd_state* st, const alias_record* r, uint8_t sn) {
    const size_t blocks = (r->nbits + 43U) / 44U;
    uint8_t lcw[72];
    DSD_MEMSET(lcw, 0, sizeof lcw);
    set_bits(lcw, 0, 8, 0x15U);
    set_bits(lcw, 8, 8, 0x90U);
    set_bits(lcw, 32, 8, (uint32_t)blocks);
    set_bits(lcw, 56, 4, sn);
    p25_lcw(opts, st, lcw, 0);
    for (size_t bn = 1U; bn <= blocks; bn++) {
        DSD_MEMSET(lcw, 0, sizeof lcw);
        set_bits(lcw, 0, 8, 0x17U);
        set_bits(lcw, 8, 8, 0x90U);
        set_bits(lcw, 16, 8, (uint32_t)bn);
        set_bits(lcw, 24, 4, sn);
        for (size_t i = 0; i < 44U; i++) {
            const size_t at = ((bn - 1U) * 44U) + i;
            lcw[28U + i] = at < r->nbits ? r->bits[at] : 0U;
        }
        p25_lcw(opts, st, lcw, 0);
    }
}

/* Tait (MFID 0xD8) talker alias link control: eight 7-bit characters from bit 16. */
static void
p1_send_tait(dsd_opts* opts, dsd_state* st, const char text[8]) {
    uint8_t lcw[72];
    DSD_MEMSET(lcw, 0, sizeof lcw);
    set_bits(lcw, 8, 8, 0xD8U);
    for (size_t i = 0; i < 8U; i++) {
        set_bits(lcw, 16U + (i * 7U), 7, (uint8_t)text[i]);
    }
    p25_lcw(opts, st, lcw, 0);
}

/* Harris (MFID 0xA4) 0xA8 Phase 2 MAC alias: seven characters, received on slot 1's SACCH for slot 0's call. */
static void
p2_send_harris(dsd_opts* opts, dsd_state* st, const char text[7]) {
    uint8_t bits[24 * 8];
    DSD_MEMSET(bits, 0, sizeof bits);
    set_bits(bits, 0, 8, 0xA8U);
    set_bits(bits, 8, 8, 0xA4U);
    set_bits(bits, 16, 8, 10U);
    for (size_t i = 0; i < 7U; i++) {
        set_bits(bits, 24U + (i * 8U), 8, (uint8_t)text[i]);
    }
    unsigned long long mac[24] = {0};
    for (size_t j = 0; j < 10U; j++) {
        mac[1U + j] = convert_bits_into_output(&bits[j * 8U], 8);
    }
    st->currentslot = 1;
    process_MAC_VPDU_crc(opts, st, 1, P25_MAC_PDU_SIGNAL, mac, 1);
}

/* ---- Fixture ----------------------------------------------------------------------------------------------- */

typedef struct {
    dsd_opts* opts;
    dsd_state* st;
    char group_path[DSD_TEST_PATH_MAX];
} fixture;

static void
fixture_open(fixture* f, int with_group_file) {
    DSD_MEMSET(f, 0, sizeof(*f));
    f->opts = (dsd_opts*)calloc(1, sizeof(*f->opts));
    f->st = (dsd_state*)calloc(1, sizeof(*f->st));
    Event_History_I* history = (Event_History_I*)calloc(2U, sizeof(Event_History_I));
    if (!f->opts || !f->st || !history) {
        DSD_FPRINTF(stderr, "allocation failed\n");
        exit(2);
    }
    f->st->event_history_s = history;
    init_event_history(&history[0], 0, DSD_EVENT_HISTORY_LEN);
    init_event_history(&history[1], 0, DSD_EVENT_HISTORY_LEN);
    if (with_group_file) {
        const int fd = dsd_test_mkstemp(f->group_path, sizeof f->group_path, "scoped_alias_group");
        if (fd < 0) {
            DSD_FPRINTF(stderr, "mkstemp failed\n");
            exit(2);
        }
        (void)dsd_close(fd);
        (void)remove(f->group_path);
        DSD_SNPRINTF(f->opts->group_in_file, sizeof f->opts->group_in_file, "%s", f->group_path);
    }
}

/* A restart: a fresh decoder state on the same options (the same group file). */
static void
fixture_restart(fixture* f) {
    dsd_state_ext_free_all(f->st);
    Event_History_I* history = f->st->event_history_s;
    DSD_MEMSET(f->st, 0, sizeof(*f->st));
    f->st->event_history_s = history;
    init_event_history(&history[0], 0, DSD_EVENT_HISTORY_LEN);
    init_event_history(&history[1], 0, DSD_EVENT_HISTORY_LEN);
}

static void
fixture_close(fixture* f) {
    dsd_state_ext_free_all(f->st);
    free(f->st->event_history_s);
    free(f->st);
    free(f->opts);
    if (f->group_path[0] != '\0') {
        (void)remove(f->group_path);
    }
}

static void
write_file(const char* path, const char* text) {
    FILE* fp = dsd_fopen_private(path, "w");
    if (fp == NULL || fputs(text, fp) < 0 || fclose(fp) != 0) {
        DSD_FPRINTF(stderr, "file not written\n");
        exit(2);
    }
}

/* Occurrences of @p needle in @p path (0 for a missing file). */
static int
file_count(const char* path, const char* needle) {
    FILE* fp = dsd_fopen_private(path, "r");
    if (fp == NULL) {
        return 0;
    }
    static char buf[16384];
    const size_t n = fread(buf, 1, sizeof buf - 1U, fp);
    fclose(fp);
    buf[n] = '\0';
    int count = 0;
    for (const char* p = strstr(buf, needle); p != NULL; p = strstr(p + 1, needle)) {
        count++;
    }
    return count;
}

/* The tuner moves to another system's control channel (a trunk-scan switch resets the record), whose checked
 * network status then names it. WACN 0 leaves the reception without a system. */
static void
tune_system(dsd_state* st, uint32_t wacn, uint16_t sysid) {
    dsd_rxid_reset(st);
    if (wacn != 0U) {
        dsd_rxid_note_p25_network(st, wacn, sysid);
    }
}

/* A conventional P25 channel: no network status, only the NAC of its checked NIDs. */
static void
tune_conventional(dsd_state* st, uint16_t nac) {
    dsd_rxid_reset(st);
    dsd_rxid_note_access(st, DSD_RXID_FAMILY_P25, DSD_ACCESS_CODE_NAC, nac);
}

static void
begin_call(fixture* f, int protocol, uint32_t tg, uint32_t src) {
    const dsd_call_observation observation = {
        .protocol = protocol,
        .slot = 0U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = tg,
        .policy_target_id = tg,
        .ota_source_id = src,
    };
    if (dsd_call_state_observe(f->st, &observation, DSD_CALL_BOUNDARY_BEGIN) <= 0) {
        DSD_FPRINTF(stderr, "call did not begin\n");
        exit(2);
    }
    dsd_event_sync_slot(f->opts, f->st, 0U);
}

static void
end_call(fixture* f) {
    (void)dsd_call_state_end_ex(f->st, 0U, 1.0, DSD_CALL_END_TERMINATOR);
    dsd_event_sync_slot(f->opts, f->st, 0U);
}

/* A Motorola alias heard on a Phase 1 call of @p rid on the current system. */
static void
hear_motorola(fixture* f, uint32_t tg, uint32_t rid, const char* name, uint8_t sn) {
    alias_record r;
    build_record(&r, rid, name);
    begin_call(f, DSD_SYNC_P25P1_POS, tg, rid);
    p1_send_record(f->opts, f->st, &r, sn);
    end_call(f);
}

static int
radio_name_is(const dsd_state* st, uint32_t id, const char* want) {
    char name[64] = {0};
    const int found = dsd_source_label_lookup(st, id, NULL, 0, name, sizeof name);
    return want ? (found && strcmp(name, want) == 0) : !found;
}

static int
scoped_name_is(const dsd_state* st, const dsd_alias_scope* scope, uint32_t id, const char* want) {
    char name[64] = {0};
    const int found = dsd_source_label_lookup_scoped(st, scope, id, NULL, 0, name, sizeof name);
    return want ? (found && strcmp(name, want) == 0) : !found;
}

static unsigned int
policy_generation(const dsd_state* st) {
    uint64_t context = 0;
    unsigned int generation = 0;
    dsd_tg_policy_table_version(st, &context, &generation);
    return generation;
}

static dsd_alias_scope
p25_scope(uint32_t wacn, uint32_t sysid) {
    dsd_alias_scope scope = {DSD_ALIAS_SCOPE_P25, wacn, sysid, 1U};
    return scope;
}

/* ---- Tests ------------------------------------------------------------------------------------------------- */

/* A -> B -> A without per-target lists: A's radio 42 is Alice, B's radio 42 is Bob, and back on A it is Alice
 * again. A reception that has not named its system yet names neither. */
static void
test_scoped_alias_a_b_a(void) {
    fixture f;
    fixture_open(&f, 0);
    tune_system(f.st, WACN_A, SYS_A);
    hear_motorola(&f, 300U, RID, "Alice", 0x1U);
    expect_true("A learns 42 Alice", radio_name_is(f.st, RID, "Alice"));

    tune_system(f.st, WACN_B, SYS_B);
    expect_true("B does not show A's name", radio_name_is(f.st, RID, NULL));
    hear_motorola(&f, 301U, RID, "Bob", 0x2U);
    expect_true("B learns 42 Bob", radio_name_is(f.st, RID, "Bob"));

    tune_system(f.st, WACN_A, SYS_A);
    expect_true("back on A, 42 is Alice", radio_name_is(f.st, RID, "Alice"));
    const dsd_alias_scope b = p25_scope(WACN_B, SYS_B);
    expect_true("B's name is still B's", scoped_name_is(f.st, &b, RID, "Bob"));
    expect_true("the current scope by default", scoped_name_is(f.st, NULL, RID, "Alice"));

    tune_system(f.st, 0U, 0U);
    expect_true("no system yet names neither", radio_name_is(f.st, RID, NULL));
    fixture_close(&f);
}

/* Persisted rows carry their scope: a restart reloads them scoped and learnable (a rename replaces the row of its
 * scope), never as imported rows that name the radio on every system. The first learn appends once per scope; a
 * repeat keeps the generation and appends nothing. A rewrite of the file keeps the scope cells. */
static void
test_scoped_alias_reload(void) {
    fixture f;
    fixture_open(&f, 1);
    write_file(f.group_path, "id,mode,name\n100,A,Dispatch\n");
    expect_true("group file loads", dsd_tg_policy_reload_group_file(f.opts, f.st) == 0);

    tune_system(f.st, WACN_A, SYS_A);
    hear_motorola(&f, 300U, RID, "Alice", 0x1U);
    const unsigned int learned = policy_generation(f.st);
    hear_motorola(&f, 300U, RID, "Alice", 0x2U);
    expect_true("a repeat keeps the generation", policy_generation(f.st) == learned);
    tune_system(f.st, WACN_B, SYS_B);
    hear_motorola(&f, 301U, RID, "Bob", 0x3U);
    expect_true("A's row appended once, with its scope", file_count(f.group_path, ",scope=p25:BEE00:123\n") == 1
                                                             && file_count(f.group_path, "42,D,Alice,") == 1);
    expect_true("B's row appended once, with its scope",
                file_count(f.group_path, ",scope=p25:BEE00:456\n") == 1 && file_count(f.group_path, "42,D,Bob,") == 1);

    fixture_restart(&f);
    expect_true("restart reloads the file", dsd_tg_policy_reload_group_file(f.opts, f.st) == 0);
    dsd_tg_policy_entry row;
    int scoped_rows = 0;
    for (size_t i = 0; dsd_tg_policy_entry_at(f.st, i, &row); i++) {
        scoped_rows += row.id_start == RID && row.source == DSD_TG_POLICY_SOURCE_RUNTIME_ALIAS && row.scope.valid;
    }
    expect_true("reloaded alias rows are learned and scoped", scoped_rows == 2);
    expect_true("after a restart, no system names neither", radio_name_is(f.st, RID, NULL));
    tune_system(f.st, WACN_B, SYS_B);
    expect_true("after a restart, B's 42 is Bob", radio_name_is(f.st, RID, "Bob"));
    tune_system(f.st, WACN_A, SYS_A);
    expect_true("after a restart, A's 42 is Alice", radio_name_is(f.st, RID, "Alice"));

    hear_motorola(&f, 300U, RID, "Alice Two", 0x4U);
    expect_true("a reloaded row is learnable: the rename replaces it", radio_name_is(f.st, RID, "Alice Two"));
    expect_true("a rename stays session-only", file_count(f.group_path, "Alice Two") == 0);
    tune_system(f.st, WACN_B, SYS_B);
    expect_true("the rename leaves B's row alone", radio_name_is(f.st, RID, "Bob"));

    expect_true("a talkgroup edit", dsd_tg_policy_set_mode(f.st, 100U, 100U, "B") == 0);
    expect_true("rewrite", dsd_tg_policy_write_group_file(f.opts, f.st) == 0);
    expect_true("the rewrite names the scope column", file_count(f.group_path, "id,mode,name,scope\n") == 1);
    expect_true("the rewrite keeps both scope cells",
                file_count(f.group_path, "42,D,Alice Two,scope=p25:BEE00:123\n") == 1
                    && file_count(f.group_path, "42,D,Bob,scope=p25:BEE00:456\n") == 1);
    fixture_restart(&f);
    expect_true("reload after the rewrite", dsd_tg_policy_reload_group_file(f.opts, f.st) == 0);
    tune_system(f.st, WACN_A, SYS_A);
    expect_true("rewritten A row still scoped", radio_name_is(f.st, RID, "Alice Two"));
    tune_system(f.st, WACN_B, SYS_B);
    expect_true("rewritten B row still scoped", radio_name_is(f.st, RID, "Bob"));
    fixture_close(&f);
}

/* Rows the user typed (a talkgroup row, or a mode D row without a scope, an older file's persisted alias included)
 * keep their precedence on every system: the alias is not learned over them and nothing is appended. */
static void
test_user_rows_keep_precedence(void) {
    fixture f;
    fixture_open(&f, 1);
    write_file(f.group_path, "id,mode,name\n42,D,Typed Radio\n");
    expect_true("typed-row file loads", dsd_tg_policy_reload_group_file(f.opts, f.st) == 0);
    tune_system(f.st, WACN_A, SYS_A);
    hear_motorola(&f, 300U, RID, "Alice", 0x1U);
    expect_true("typed alias row wins over the learned one", radio_name_is(f.st, RID, "Typed Radio"));
    expect_true("nothing appended over a typed row", file_count(f.group_path, "Alice") == 0);
    tune_system(f.st, WACN_B, SYS_B);
    expect_true("an unscoped typed row names the radio everywhere", radio_name_is(f.st, RID, "Typed Radio"));
    fixture_close(&f);

    fixture_open(&f, 1);
    write_file(f.group_path, "id,mode,name\n42,D,Alice,,scope=p25:BEE00:123\n42,D,Typed Radio\n");
    expect_true("mixed file loads", dsd_tg_policy_reload_group_file(f.opts, f.st) == 0);
    tune_system(f.st, WACN_A, SYS_A);
    expect_true("a typed row after a scoped one still wins", radio_name_is(f.st, RID, "Typed Radio"));
    fixture_close(&f);

    fixture_open(&f, 1);
    write_file(f.group_path, "id,mode,name\n42,A,Dispatch 42\n");
    expect_true("talkgroup-row file loads", dsd_tg_policy_reload_group_file(f.opts, f.st) == 0);
    tune_system(f.st, WACN_A, SYS_A);
    hear_motorola(&f, 300U, RID, "Alice", 0x1U);
    expect_true("a talkgroup row for the ID wins", radio_name_is(f.st, RID, "Dispatch 42"));
    expect_true("nothing appended over a talkgroup row", file_count(f.group_path, "Alice") == 0);
    fixture_close(&f);
}

/* The scope comes from the reception, never from the alias's own metadata: a Tait row (whose metadata carries only a
 * NAC) and a Harris row are scoped like a Motorola one. */
static void
test_scope_comes_from_reception(void) {
    fixture f;
    fixture_open(&f, 1);
    tune_system(f.st, WACN_A, SYS_A);
    f.st->nac = 0x293;
    begin_call(&f, DSD_SYNC_P25P1_POS, 300U, RID);
    p1_send_tait(f.opts, f.st, "TAITUNIT");
    end_call(&f);
    expect_true("Tait alias learned on A", radio_name_is(f.st, RID, "TAITUNIT"));
    expect_true("Tait row appended with the reception's scope",
                file_count(f.group_path, "42,D,TAITUNIT,293 Tait,scope=p25:BEE00:123\n") == 1);
    tune_system(f.st, WACN_B, SYS_B);
    expect_true("Tait alias does not name B's radio", radio_name_is(f.st, RID, NULL));

    begin_call(&f, DSD_SYNC_P25P2_POS, 301U, RID);
    p2_send_harris(f.opts, f.st, "HARRIS7");
    expect_true("Harris alias learned on B", radio_name_is(f.st, RID, "HARRIS7"));
    expect_true("Harris row appended with the reception's scope",
                file_count(f.group_path, "scope=p25:BEE00:456\n") == 1);
    end_call(&f);
    tune_system(f.st, WACN_A, SYS_A);
    expect_true("Harris alias does not rename A's radio", radio_name_is(f.st, RID, "TAITUNIT"));
    fixture_close(&f);
}

/* A reception that names neither its system nor an access code keeps the behaviour from before scopes: the alias is
 * learned without a scope, names the radio on every system (behind a system's own learned row), and is appended as a
 * plain row, which the next start loads as an imported row naming the radio everywhere. */
static void
test_unknown_reception_like_main(void) {
    fixture f;
    fixture_open(&f, 1);
    write_file(f.group_path, "id,mode,name\n100,A,Dispatch\n");
    expect_true("group file loads", dsd_tg_policy_reload_group_file(f.opts, f.st) == 0);
    tune_system(f.st, 0U, 0U);
    hear_motorola(&f, 300U, RID, "Nowhere", 0x1U);
    expect_true("unscoped alias names the radio", radio_name_is(f.st, RID, "Nowhere"));
    expect_true("unscoped alias appended as a plain row",
                file_count(f.group_path, "42,D,Nowhere,") == 1 && file_count(f.group_path, "scope=") == 0);
    tune_system(f.st, WACN_A, SYS_A);
    expect_true("unscoped alias names A's radio too, as before", radio_name_is(f.st, RID, "Nowhere"));
    hear_motorola(&f, 300U, RID, "Alice", 0x2U);
    expect_true("A's own learned row comes first on A", radio_name_is(f.st, RID, "Alice"));
    expect_true("A's row appended with its scope",
                file_count(f.group_path, "42,D,Alice,") == 1 && file_count(f.group_path, "scope=p25:BEE00:123\n") == 1);
    tune_system(f.st, WACN_B, SYS_B);
    expect_true("B, with no row of its own, falls back to it", radio_name_is(f.st, RID, "Nowhere"));
    tune_system(f.st, 0U, 0U);
    hear_motorola(&f, 300U, RID, "Nowhere Two", 0x3U);
    expect_true("an unscoped rename replaces the unscoped row", radio_name_is(f.st, RID, "Nowhere Two"));
    expect_true("an unscoped rename stays session-only", file_count(f.group_path, "Nowhere Two") == 0);

    fixture_restart(&f);
    expect_true("restart reloads the file", dsd_tg_policy_reload_group_file(f.opts, f.st) == 0);
    tune_system(f.st, WACN_A, SYS_A);
    expect_true("the plain row loads as an imported row and wins, as on main", radio_name_is(f.st, RID, "Nowhere"));
    fixture_close(&f);
}

/* A conventional P25 channel sends no network status: the NAC of its checked NIDs scopes the alias. */
static void
test_conventional_nac_scope(void) {
    fixture f;
    fixture_open(&f, 1);
    tune_conventional(f.st, 0x293U);
    hear_motorola(&f, 300U, RID, "Conv One", 0x1U);
    expect_true("NAC-scoped alias learned", radio_name_is(f.st, RID, "Conv One"));
    expect_true("NAC-scoped row appended with its scope", file_count(f.group_path, "42,D,Conv One,") == 1
                                                              && file_count(f.group_path, "scope=p25nac:293:0\n") == 1);
    tune_conventional(f.st, 0x5A1U);
    expect_true("another NAC does not show it", radio_name_is(f.st, RID, NULL));
    tune_system(f.st, WACN_A, SYS_A);
    expect_true("a trunked system does not show it", radio_name_is(f.st, RID, NULL));
    fixture_restart(&f);
    expect_true("restart reloads the file", dsd_tg_policy_reload_group_file(f.opts, f.st) == 0);
    tune_conventional(f.st, 0x293U);
    expect_true("after a restart the NAC row is still scoped", radio_name_is(f.st, RID, "Conv One"));
    tune_conventional(f.st, 0x5A1U);
    expect_true("after a restart another NAC still does not show it", radio_name_is(f.st, RID, NULL));
    fixture_close(&f);
}

/* The learning API: results, NULL scope = the reception's, another scope's row untouched, an invalid scope's row not
 * appended, a long UTF-8 name cut at a character boundary. */
static void
test_learn_api(void) {
    fixture f;
    fixture_open(&f, 1);
    const dsd_alias_scope a = p25_scope(WACN_A, SYS_A);
    const dsd_alias_scope b = p25_scope(WACN_B, SYS_B);
    expect_true("learn A added",
                dsd_alias_learn_radio(f.opts, f.st, &a, RID, "Alice", "M") == DSD_TG_POLICY_LEARN_ADDED);
    expect_true("learn A repeat",
                dsd_alias_learn_radio(f.opts, f.st, &a, RID, "Alice", "M") == DSD_TG_POLICY_LEARN_UNCHANGED);
    expect_true("learn B added", dsd_alias_learn_radio(f.opts, f.st, &b, RID, "Bob", "M") == DSD_TG_POLICY_LEARN_ADDED);
    expect_true("rename A",
                dsd_alias_learn_radio(f.opts, f.st, &a, RID, "Alicia", "M") == DSD_TG_POLICY_LEARN_REPLACED);
    expect_true("rename A leaves B", scoped_name_is(f.st, &b, RID, "Bob") && scoped_name_is(f.st, &a, RID, "Alicia"));
    expect_true("radio 0 refused",
                dsd_alias_learn_radio(f.opts, f.st, &a, 0U, "X", NULL) == DSD_TG_POLICY_LEARN_FAILED);
    expect_true("NULL name refused",
                dsd_alias_learn_radio(f.opts, f.st, &a, RID, NULL, NULL) == DSD_TG_POLICY_LEARN_FAILED);

    tune_system(f.st, WACN_B, SYS_B);
    expect_true("NULL scope is the reception's",
                dsd_alias_learn_radio(f.opts, f.st, NULL, 43U, "Carol", NULL) == DSD_TG_POLICY_LEARN_ADDED
                    && scoped_name_is(f.st, &b, 43U, "Carol") && scoped_name_is(f.st, &a, 43U, NULL));
    expect_true("Carol appended with B's scope", file_count(f.group_path, "43,D,Carol,,scope=p25:BEE00:456\n") == 1);
    const dsd_alias_scope none = {DSD_ALIAS_SCOPE_NONE, 0U, 0U, 0U};
    expect_true("an invalid scope is the reception's",
                dsd_alias_learn_radio(f.opts, f.st, &none, 46U, "Erin", NULL) == DSD_TG_POLICY_LEARN_ADDED
                    && scoped_name_is(f.st, &b, 46U, "Erin"));
    tune_system(f.st, 0U, 0U);
    expect_true("unscoped learn added",
                dsd_alias_learn_radio(f.opts, f.st, &none, 44U, "Dave", NULL) == DSD_TG_POLICY_LEARN_ADDED);
    expect_true("unscoped learn appended as a plain row", file_count(f.group_path, "44,D,Dave\n") == 1);

    /* 47 'x' then a 3-byte character: the row keeps 49 bytes, so the character is dropped whole. */
    char long_name[64];
    DSD_MEMSET(long_name, 'x', 47U);
    DSD_SNPRINTF(long_name + 47, sizeof long_name - 47U, "%s", "\xE2\x82\xAC");
    expect_true("long name learned",
                dsd_alias_learn_radio(f.opts, f.st, &a, 45U, long_name, NULL) == DSD_TG_POLICY_LEARN_ADDED);
    char name[64] = {0};
    expect_true("long name cut at a character boundary",
                dsd_source_label_lookup_scoped(f.st, &a, 45U, NULL, 0, name, sizeof name) && strlen(name) == 47U
                    && name[46] == 'x');
    fixture_close(&f);
}

/* The name key moves with the list's identity, its generation and the reception's scope; equal generations of
 * different lists differ; a repeat keeps it; a frontend copy agrees with the live state. */
static void
test_name_key(void) {
    fixture one;
    fixture two;
    fixture_open(&one, 0);
    fixture_open(&two, 0);
    const dsd_alias_scope a = p25_scope(WACN_A, SYS_A);
    (void)dsd_alias_learn_radio(NULL, one.st, &a, RID, "Alice", NULL);
    (void)dsd_alias_learn_radio(NULL, two.st, &a, RID, "Alice", NULL);
    expect_true("equal generations", policy_generation(one.st) == policy_generation(two.st));
    expect_true("different lists give different keys", dsd_alias_name_key(one.st) != dsd_alias_name_key(two.st));

    const uint64_t before = dsd_alias_name_key(one.st);
    (void)dsd_alias_learn_radio(NULL, one.st, &a, RID, "Alice", NULL);
    expect_true("a repeat keeps the key", dsd_alias_name_key(one.st) == before);
    (void)dsd_alias_learn_radio(NULL, one.st, &a, RID, "Alicia", NULL);
    const uint64_t renamed = dsd_alias_name_key(one.st);
    expect_true("a rename moves the key", renamed != before);
    tune_system(one.st, WACN_A, SYS_A);
    const uint64_t on_a = dsd_alias_name_key(one.st);
    expect_true("a scope moves the key", on_a != renamed);
    tune_system(one.st, WACN_B, SYS_B);
    expect_true("another scope moves the key", dsd_alias_name_key(one.st) != on_a);

    dsd_source_alias_store* radios = dsd_source_alias_store_create();
    dsd_source_alias_entry radio = {.id_start = 7U, .id_end = 7U};
    DSD_SNPRINTF(radio.name, sizeof radio.name, "%s", "Seven");
    expect_true("radio list built", radios && dsd_source_alias_store_append(radios, &radio) == 0);
    const uint64_t without_radios = dsd_alias_name_key(one.st);
    dsd_source_alias_install(one.st, radios);
    expect_true("a radio list moves the key", dsd_alias_name_key(one.st) != without_radios);

    dsd_state* copy = (dsd_state*)calloc(1, sizeof(*copy));
    expect_true("copy allocated", copy != NULL);
    if (copy) {
        copy->rx_id = one.st->rx_id;
        expect_true("policy copied", dsd_tg_policy_copy_snapshot(copy, one.st) == 0);
        expect_true("radio list copied", dsd_source_alias_copy_snapshot(copy, one.st) == 0);
        expect_true("a frontend copy has the live key", dsd_alias_name_key(copy) == dsd_alias_name_key(one.st));
        dsd_state_ext_free_all(copy);
        free(copy);
    }
    expect_true("NULL state", dsd_alias_name_key(NULL) == 0U);
    fixture_close(&one);
    fixture_close(&two);
}

/* What each protocol's received identity scopes, and what it does not. */
static void
test_scope_from_rxid(void) {
    dsd_state* st = (dsd_state*)calloc(1, sizeof(*st));
    if (!st) {
        g_fail = 1;
        return;
    }
    dsd_alias_scope s;
    expect_true("empty record: no scope", dsd_alias_scope_current(st, &s) == 0 && !s.valid && s.kind == 0U);
    dsd_rxid_note_p25_site(st, 0x123U, 1U, 2U);
    expect_true("P25 SYSID without WACN or NAC: no scope", dsd_alias_scope_current(st, &s) == 0);
    dsd_rxid_note_access(st, DSD_RXID_FAMILY_P25, DSD_ACCESS_CODE_NAC, 0x293U);
    expect_true("P25 NAC without WACN", dsd_alias_scope_current(st, &s) == 1 && s.kind == DSD_ALIAS_SCOPE_P25_NAC
                                            && s.a == 0x293U && s.b == 0U);
    dsd_rxid_note_p25_network(st, 0xBEE00U, 0x123U);
    expect_true("P25 WACN/SYSID", dsd_alias_scope_current(st, &s) == 1 && s.kind == DSD_ALIAS_SCOPE_P25
                                      && s.a == 0xBEE00U && s.b == 0x123U && s.valid == 1U);

    dsd_rxid_reset(st);
    dsd_rxid_note_dmr_site(st, DSD_RXID_DMR_TRUNK_TIER3, 1, 12U, 3U, 1U, "TIII Small");
    expect_true("DMR Tier III network", dsd_alias_scope_current(st, &s) == 1 && s.kind == DSD_ALIAS_SCOPE_DMR
                                            && s.a == ((DSD_RXID_DMR_TRUNK_TIER3 << 8) | 1U) && s.b == 12U);
    dsd_rxid_reset(st);
    dsd_rxid_note_dmr_site(st, DSD_RXID_DMR_TRUNK_CAPACITY_PLUS, 0, 0U, 2U, 0U, "Cap+");
    expect_true("DMR Capacity Plus site without a colour code: no scope", dsd_alias_scope_current(st, &s) == 0);
    dsd_rxid_note_access(st, DSD_RXID_FAMILY_DMR, DSD_ACCESS_CODE_COLOR_CODE, 1U);
    dsd_rxid_note_dmr_site(st, DSD_RXID_DMR_TRUNK_CAPACITY_PLUS, 0, 0U, 2U, 0U, "Cap+");
    expect_true("DMR Capacity Plus site and colour code", dsd_alias_scope_current(st, &s) == 1
                                                              && s.a == ((DSD_RXID_DMR_TRUNK_CAPACITY_PLUS << 8) | 1U)
                                                              && s.b == 2U);
    dsd_rxid_reset(st);
    dsd_rxid_note_dmr_xpt_free_lcn(st, 3U);
    expect_true("DMR XPT: no scope", dsd_alias_scope_current(st, &s) == 0);

    dsd_rxid_reset(st);
    dsd_rxid_note_nxdn_location(st, 1U, 12U, 3U);
    expect_true("NXDN Type-C location",
                dsd_alias_scope_current(st, &s) == 1 && s.kind == DSD_ALIAS_SCOPE_NXDN && s.a == 0x101U && s.b == 12U);
    dsd_rxid_reset(st);
    dsd_rxid_note_nxdn_typed_system(st, 0x5A5A5U, 200U);
    expect_true("NXDN Type-D system", dsd_alias_scope_current(st, &s) == 1 && s.a == 0x200U && s.b == 0x5A5A5U);
    dsd_rxid_reset(st);
    dsd_rxid_note_nxdn_idas_site(st, 1U, 200U);
    expect_true("NXDN IDAS site alone: no scope", dsd_alias_scope_current(st, &s) == 0);

    dsd_rxid_reset(st);
    dsd_rxid_note_tetra_cell(st, 0x3FFU, 0x123U, 5U);
    expect_true("TETRA network", dsd_alias_scope_current(st, &s) == 1 && s.kind == DSD_ALIAS_SCOPE_TETRA
                                     && s.a == 0x3FFU && s.b == 0x123U);

    dsd_rxid_reset(st);
    dsd_rxid_note_edacs_site(st, 3U);
    expect_true("EDACS: no scope", dsd_alias_scope_current(st, &s) == 0);
    expect_true("NULL state", dsd_alias_scope_current(NULL, &s) == 0 && !s.valid);

    const dsd_alias_scope x = p25_scope(1U, 2U);
    const dsd_alias_scope y = p25_scope(1U, 3U);
    const dsd_alias_scope stray = {DSD_ALIAS_SCOPE_P25, 1U, 2U, 0U};
    expect_true("equal scopes", dsd_alias_scope_equal(&x, &x));
    expect_true("different scopes", !dsd_alias_scope_equal(&x, &y));
    expect_true("invalid scopes are one scope", dsd_alias_scope_equal(&stray, NULL));
    expect_true("valid vs invalid", !dsd_alias_scope_equal(&x, &stray));
    free(st);
}

/* A malformed scope cell is reported and ignored: the row loads as an imported row, as before. */
static void
test_malformed_scope_cell(void) {
    fixture f;
    fixture_open(&f, 1);
    write_file(f.group_path, "id,mode,name\n42,D,Odd,,scope=p26:BEE00:123\n43,D,Wide,,scope=p25:123456789:1\n");
    expect_true("file with malformed scopes loads", dsd_tg_policy_reload_group_file(f.opts, f.st) == 0);
    dsd_tg_policy_entry row;
    int imported = 0;
    for (size_t i = 0; dsd_tg_policy_entry_at(f.st, i, &row); i++) {
        imported += row.source == DSD_TG_POLICY_SOURCE_IMPORTED && !row.scope.valid;
    }
    expect_true("malformed scope rows load as imported rows", imported == 2);
    tune_system(f.st, WACN_A, SYS_A);
    expect_true("an imported row names the radio everywhere", radio_name_is(f.st, RID, "Odd"));
    fixture_close(&f);

    /* A hand-typed scope cell on a talkgroup row never makes it a radio's alias. */
    fixture_open(&f, 1);
    write_file(f.group_path, "id,mode,name\n100,A,Dispatch,,scope=p25:BEE00:123\n101,B,Quiet,scope=p25:BEE00:123\n"
                             "102,DE,Locked,,scope=p25:BEE00:123\n");
    expect_true("file with scoped talkgroup rows loads", dsd_tg_policy_reload_group_file(f.opts, f.st) == 0);
    int talkgroup_rows = 0;
    for (size_t i = 0; dsd_tg_policy_entry_at(f.st, i, &row); i++) {
        talkgroup_rows += row.source == DSD_TG_POLICY_SOURCE_IMPORTED && !row.scope.valid;
    }
    expect_true("scope cells on talkgroup and lockout rows are ignored", talkgroup_rows == 3);
    tune_system(f.st, WACN_A, SYS_A);
    char tg_name[64] = {0};
    expect_true("the talkgroup row still decides and labels its talkgroup",
                dsd_tg_policy_lookup_group_label(f.st, 100U, NULL, 0, tg_name, sizeof tg_name)
                    && strcmp(tg_name, "Dispatch") == 0);
    dsd_tg_policy_decision locked;
    f.opts->trunk_tune_group_calls = 1;
    expect_true("a DE row with a scope cell stays a lockout",
                dsd_tg_policy_evaluate_group_call(f.opts, f.st, 102U, 7U, 0, 0, &locked) == 0
                    && locked.match == DSD_TG_POLICY_MATCH_EXACT && locked.tune_allowed == 0
                    && (locked.block_reasons & DSD_TG_POLICY_BLOCK_MODE) != 0);
    fixture_close(&f);
}

/* The scope cell round-trips under the other header shapes: a tags header (metadata one column past the empty tags
 * cell) and an extended policy header, for the append, the reload and the rewrite. */
static void
test_scope_cell_header_shapes(void) {
    static const struct {
        const char* file;
        const char* appended;
        const char* header_after_rewrite;
        const char* rewritten;
    } shapes[] = {
        {"id,mode,name,tags\n100,A,Dispatch,ops\n", "42,D,Alice,,M,scope=p25:BEE00:123\n", "id,mode,name,tags,scope\n",
         "42,D,Alice,,scope=p25:BEE00:123\n"},
        {"id,mode,name,priority,preempt,audio,record,stream,tags\n100,A,Dispatch,5,false,on,on,on,ops\n",
         "42,D,Alice,0,false,on,on,on,,M,scope=p25:BEE00:123\n",
         "id,mode,name,priority,preempt,audio,record,stream,tags,scope\n",
         "42,D,Alice,0,false,on,on,on,,scope=p25:BEE00:123\n"},
    };

    const dsd_alias_scope a = p25_scope(WACN_A, SYS_A);
    for (size_t i = 0; i < sizeof shapes / sizeof shapes[0]; i++) {
        fixture f;
        fixture_open(&f, 1);
        write_file(f.group_path, shapes[i].file);
        expect_true("shape loads", dsd_tg_policy_reload_group_file(f.opts, f.st) == 0);
        expect_true("shape learn",
                    dsd_alias_learn_radio(f.opts, f.st, &a, RID, "Alice", "M") == DSD_TG_POLICY_LEARN_ADDED);
        expect_true("shape append line", file_count(f.group_path, shapes[i].appended) == 1);
        fixture_restart(&f);
        expect_true("shape reload", dsd_tg_policy_reload_group_file(f.opts, f.st) == 0);
        expect_true("shape reload keeps the scope",
                    scoped_name_is(f.st, &a, RID, "Alice") && radio_name_is(f.st, RID, NULL));
        char tags[64] = {0};
        dsd_tg_policy_entry row;
        expect_true("shape keeps the talkgroup's tags", dsd_tg_policy_entry_at(f.st, 0U, &row)
                                                            && DSD_SNPRINTF(tags, sizeof tags, "%s", row.tags) > 0
                                                            && strcmp(tags, "ops") == 0);
        expect_true("shape edit", dsd_tg_policy_set_mode(f.st, 100U, 100U, "B") == 0);
        expect_true("shape rewrite", dsd_tg_policy_write_group_file(f.opts, f.st) == 0);
        expect_true("shape rewrite header", file_count(f.group_path, shapes[i].header_after_rewrite) == 1);
        expect_true("shape rewrite line", file_count(f.group_path, shapes[i].rewritten) == 1);
        fixture_restart(&f);
        expect_true("shape reload after the rewrite", dsd_tg_policy_reload_group_file(f.opts, f.st) == 0);
        expect_true("shape rewrite keeps the scope",
                    scoped_name_is(f.st, &a, RID, "Alice") && radio_name_is(f.st, RID, NULL));
        expect_true("shape rewrite stays complete", dsd_tg_policy_write_group_file(f.opts, f.st) == 0);
        fixture_close(&f);
    }
}

/* A learn while a -Y row's or trunk-scan target's own list for the same file is in effect is mirrored into the global
 * list held aside, ranked as that list's next reload of the file would rank it: a scoped row as a learned row of its
 * scope, an unscoped (plain) row as an imported row. */
static void
test_held_list_mirror(void) {
    fixture f;
    fixture_open(&f, 1);
    write_file(f.group_path, "id,mode,name\n100,A,Dispatch\n");
    expect_true("mirror load", dsd_tg_policy_reload_group_file(f.opts, f.st) == 0);
    dsd_scan_options parsed = {0};
    parsed.values.present = DSD_SCAN_OPT_GROUP;
    DSD_SNPRINTF(parsed.values.group_file, sizeof(parsed.values.group_file), "%s", f.group_path);
    dsd_key_set keys = {0};
    dsd_scan_row_profile* profile = NULL;
    expect_true("row profile", dsd_scan_profile_load(&parsed, 0, &profile, &keys) == 0);
    expect_true("row groups begin", dsd_scan_groups_begin(f.st) == 0);
    dsd_scan_groups_enter(f.st, profile);
    expect_true("row list in effect", dsd_scan_groups_row_active(f.st));

    tune_system(f.st, WACN_A, SYS_A);
    hear_motorola(&f, 300U, RID, "Alice", 0x1U);
    tune_system(f.st, 0U, 0U);
    hear_motorola(&f, 300U, RID, "Plain", 0x2U);
    expect_true("both rows appended",
                file_count(f.group_path, "42,D,Alice,") == 1 && file_count(f.group_path, "42,D,Plain,") == 1);
    dsd_scan_groups_leave(f.st);

    tune_system(f.st, WACN_A, SYS_A);
    char held_name[64] = {0};
    (void)dsd_source_label_lookup(f.st, RID, NULL, 0, held_name, sizeof held_name);
    int imported_plain = 0;
    int scoped_alice = 0;
    dsd_tg_policy_entry row;
    for (size_t i = 0; dsd_tg_policy_entry_at(f.st, i, &row); i++) {
        imported_plain += strcmp(row.name, "Plain") == 0 && row.source == DSD_TG_POLICY_SOURCE_IMPORTED;
        scoped_alice += strcmp(row.name, "Alice") == 0 && row.scope.valid;
    }
    expect_true("held list mirrored the scoped row scoped", scoped_alice == 1);
    expect_true("held list mirrored the plain row as an imported row", imported_plain == 1);
    expect_true("held list keeps saving", dsd_tg_policy_write_group_file(f.opts, f.st) == 0);

    fixture_restart(&f);
    expect_true("reload of the file", dsd_tg_policy_reload_group_file(f.opts, f.st) == 0);
    tune_system(f.st, WACN_A, SYS_A);
    char reload_name[64] = {0};
    (void)dsd_source_label_lookup(f.st, RID, NULL, 0, reload_name, sizeof reload_name);
    expect_true("held list ranks as its reload does",
                strcmp(held_name, reload_name) == 0 && strcmp(reload_name, "Plain") == 0);
    dsd_scan_profile_free(profile);
    dsd_key_set_free(&keys);
    fixture_close(&f);
}

/* dsd_tg_policy_learn_exact() (the P25 Phase 1 LSD soft ID path) learns an entry without a scope in the reception's. */
static void
test_learn_exact_takes_reception_scope(void) {
    fixture f;
    fixture_open(&f, 0);
    tune_system(f.st, WACN_A, SYS_A);
    dsd_tg_policy_entry entry;
    expect_true("entry",
                dsd_tg_policy_make_exact_entry(RID, "D", "Soft ID", DSD_TG_POLICY_SOURCE_RUNTIME_ALIAS, &entry) == 0);
    expect_true("learned", dsd_tg_policy_learn_exact(f.st, &entry) == DSD_TG_POLICY_LEARN_ADDED);
    dsd_tg_policy_entry row;
    expect_true("stored with A's scope", dsd_tg_policy_entry_at(f.st, 0U, &row) && row.scope.valid
                                             && row.scope.kind == DSD_ALIAS_SCOPE_P25 && row.scope.b == SYS_A);
    tune_system(f.st, WACN_B, SYS_B);
    expect_true("not B's radio", radio_name_is(f.st, RID, NULL));
    tune_system(f.st, WACN_A, SYS_A);
    expect_true("A's radio", radio_name_is(f.st, RID, "Soft ID"));
    fixture_close(&f);
}

int
main(void) {
    test_scoped_alias_a_b_a();
    test_scoped_alias_reload();
    test_user_rows_keep_precedence();
    test_scope_comes_from_reception();
    test_unknown_reception_like_main();
    test_conventional_nac_scope();
    test_learn_api();
    test_name_key();
    test_scope_from_rxid();
    test_malformed_scope_cell();
    test_scope_cell_header_shapes();
    test_held_list_mirror();
    test_learn_exact_takes_reception_scope();
    if (g_fail) {
        return 1;
    }
    DSD_FPRINTF(stderr, "CORE_SCOPED_ALIAS: OK\n");
    return 0;
}
