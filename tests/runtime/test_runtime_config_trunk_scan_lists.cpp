// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Issue #697: the trunk-scan lists a config names. [trunk_scan] targets_csv, targets_csv_2 .. targets_csv_8 and
 * lists_off load by list number with presence per key, compose across includes and profiles key by key, render and
 * load back, apply over what a session holds, and meet the start rules on the composed result.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/trunk_scan_lists.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/config_schema.h>
#include <dsd-neo/runtime/log.h>
#include <errno.h>
#include <memory>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <sys/types.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/platform/file_compat.h"
#include "test_support.h"

namespace {

int g_failures = 0;
char g_log_text[8192];

void
capture_log(dsd_neo_log_level_t level, const char* text, void* ctx) {
    (void)level;
    (void)ctx;
    size_t used = strlen(g_log_text);
    if (text && used < sizeof g_log_text - 1U) {
        DSD_SNPRINTF(g_log_text + used, sizeof g_log_text - used, "%s", text);
    }
}

void
check(bool condition, const char* what, int line) {
    if (!condition) {
        DSD_FPRINTF(stderr, "line %d: %s\n", line, what);
        g_failures++;
    }
}

#define CHECK(condition) check((condition), #condition, __LINE__)

int
write_temp_config(const std::string& contents, char* out_path, size_t out_sz) {
    char tmpl[DSD_TEST_PATH_MAX];
    int fd = dsd_test_mkstemp(tmpl, sizeof(tmpl), "dsdneo_cfg_lists");
    if (fd < 0) {
        DSD_FPRINTF(stderr, "dsd_test_mkstemp failed: %s\n", strerror(errno));
        return 1;
    }
    ssize_t wr = dsd_write(fd, contents.data(), contents.size());
    (void)dsd_close(fd);
    if (wr < 0 || (size_t)wr != contents.size()) {
        (void)remove(tmpl);
        return 1;
    }
    DSD_SNPRINTF(out_path, out_sz, "%s", tmpl);
    return 0;
}

/* Load @p ini (with @p profile, or none) into @p cfg; the temp file is removed. */
int
load_ini(const std::string& ini, const char* profile, dsdneoUserConfig* cfg) {
    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return -1;
    }
    const int rc = dsd_user_config_load_profile(path, profile, cfg);
    (void)remove(path);
    return rc;
}

bool
lists_are(const dsd_trunk_scan_list_spec* lists, int count, const char* const* paths, const int* off, int want) {
    if (count != want) {
        return false;
    }
    for (int i = 0; i < want; i++) {
        if (strcmp(lists[i].path, paths[i]) != 0 || (lists[i].start_off != 0U) != (off[i] != 0)) {
            return false;
        }
    }
    return true;
}

bool
cfg_lists_are(const dsdneoUserConfig& cfg, const char* const* paths, const int* off, int want) {
    return lists_are(cfg.trunk_scan_lists, cfg.trunk_scan_list_count, paths, off, want);
}

std::string
render(const dsdneoUserConfig& cfg) {
    FILE* tmp = tmpfile();
    if (!tmp) {
        return "";
    }
    dsd_user_config_render_ini(&cfg, tmp);
    std::string out;
    char buf[4096];
    if (fseek(tmp, 0, SEEK_SET) != 0) {
        fclose(tmp);
        return "";
    }
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof buf, tmp)) > 0) {
        out.append(buf, n);
    }
    fclose(tmp);
    return out;
}

/* Every list number has its key; lists_off says which start off; render writes them back and they load the same. */
void
test_numbered_keys_load_and_roundtrip() {
    auto cfg = std::make_unique<dsdneoUserConfig>();
    CHECK(load_ini("[trunk_scan]\n"
                   "enabled = true\n"
                   "targets_csv = \"/lists/security.csv\"\n"
                   "targets_csv_2 = \"/lists/hospital.csv\"\n"
                   "targets_csv_3 = \"/lists/city.csv\"\n"
                   "lists_off = 2, 3\n",
                   nullptr, cfg.get())
          == 0);
    static const char* const paths[] = {"/lists/security.csv", "/lists/hospital.csv", "/lists/city.csv"};
    static const int off[] = {0, 1, 1};
    CHECK(cfg->has_trunk_scan && cfg->trunk_scan_enabled);
    CHECK(cfg_lists_are(*cfg, paths, off, 3));

    const std::string text = render(*cfg);
    CHECK(text.find("targets_csv = \"/lists/security.csv\"\n") != std::string::npos);
    CHECK(text.find("targets_csv_2 = \"/lists/hospital.csv\"\n") != std::string::npos);
    CHECK(text.find("targets_csv_3 = \"/lists/city.csv\"\n") != std::string::npos);
    CHECK(text.find("lists_off = 2, 3\n") != std::string::npos);

    auto back = std::make_unique<dsdneoUserConfig>();
    CHECK(load_ini(text, nullptr, back.get()) == 0);
    CHECK(cfg_lists_are(*back, paths, off, 3));

    /* A save ends its lists with an empty key and always writes lists_off, so loading a one-list save over a
       session with three lists, one of them off, gives exactly the saved list, on (review M4). */
    auto one = std::make_unique<dsdneoUserConfig>();
    CHECK(load_ini("[trunk_scan]\nenabled = true\ntargets_csv = \"/lists/a.csv\"\n", nullptr, one.get()) == 0);
    const std::string one_text = render(*one);
    CHECK(one_text.find("targets_csv = \"/lists/a.csv\"\ntargets_csv_2 = \"\"\nlists_off = \"\"\n")
          != std::string::npos);
    auto saved = std::make_unique<dsdneoUserConfig>();
    CHECK(load_ini(one_text, nullptr, saved.get()) == 0);
    auto opts = std::make_unique<dsd_opts>();
    auto state = std::make_unique<dsd_state>();
    DSD_SNPRINTF(opts->trunk_scan_lists[0].path, sizeof opts->trunk_scan_lists[0].path, "%s", "/lists/x.csv");
    DSD_SNPRINTF(opts->trunk_scan_lists[1].path, sizeof opts->trunk_scan_lists[1].path, "%s", "/lists/y.csv");
    DSD_SNPRINTF(opts->trunk_scan_lists[2].path, sizeof opts->trunk_scan_lists[2].path, "%s", "/lists/z.csv");
    opts->trunk_scan_lists[0].start_off = 1U;
    opts->trunk_scan_list_count = 3;
    dsd_apply_user_config_to_opts(saved.get(), opts.get(), state.get());
    static const char* const just_a[] = {"/lists/a.csv"};
    static const int on[] = {0};
    CHECK(lists_are(opts->trunk_scan_lists, opts->trunk_scan_list_count, just_a, on, 1));

    /* Eight lists need no terminator, and a save with no lists writes no list key at all. */
    auto eight = std::make_unique<dsdneoUserConfig>();
    for (int i = 0; i < DSD_TRUNK_SCAN_MAX_LISTS; i++) {
        DSD_SNPRINTF(eight->trunk_scan_lists[i].path, sizeof eight->trunk_scan_lists[i].path, "/l%d.csv", i + 1);
    }
    eight->trunk_scan_list_count = DSD_TRUNK_SCAN_MAX_LISTS;
    eight->has_trunk_scan = 1;
    const std::string eight_text = render(*eight);
    CHECK(eight_text.find("targets_csv_8 = \"/l8.csv\"\n") != std::string::npos);
    CHECK(eight_text.find("targets_csv_9") == std::string::npos);
    auto none = std::make_unique<dsdneoUserConfig>();
    none->has_trunk_scan = 1;
    const std::string none_text = render(*none);
    CHECK(none_text.find("targets_csv") == std::string::npos && none_text.find("lists_off") == std::string::npos);
}

bool
refused_with(const dsdneoUserConfig& cfg, const char* why_part) {
    char why[256] = "";
    return dsd_user_config_check_trunk_scan_startable(&cfg, nullptr, why, sizeof why) != 0
           && strstr(why, why_part) != nullptr;
}

/* An empty targets_csv_N beside a targets_csv_3 the same file sets is a contradiction the gap rule refuses, in either
   key order; nothing is dropped without a word (review I1). */
void
test_empty_key_beside_a_later_list_is_a_gap_in_any_order() {
    auto before = std::make_unique<dsdneoUserConfig>();
    CHECK(load_ini("[trunk_scan]\nenabled = true\ntargets_csv = \"/a.csv\"\ntargets_csv_2 = \"\"\n"
                   "targets_csv_3 = \"/c.csv\"\n",
                   nullptr, before.get())
          == 0);
    CHECK(before->trunk_scan_list_count == 3 && strcmp(before->trunk_scan_lists[2].path, "/c.csv") == 0);
    CHECK(refused_with(*before, "without gaps"));
    auto after = std::make_unique<dsdneoUserConfig>();
    CHECK(load_ini("[trunk_scan]\nenabled = true\ntargets_csv_3 = \"/c.csv\"\ntargets_csv = \"/a.csv\"\n"
                   "targets_csv_2 = \"\"\n",
                   nullptr, after.get())
          == 0);
    CHECK(after->trunk_scan_list_count == 3 && strcmp(after->trunk_scan_lists[2].path, "/c.csv") == 0);
    CHECK(refused_with(*after, "without gaps"));

    /* lists_off before the paths it numbers is the same as after them. */
    static const char* const two[] = {"/a.csv", "/b.csv"};
    static const int second_off[] = {0, 1};
    auto off_first = std::make_unique<dsdneoUserConfig>();
    CHECK(load_ini("[trunk_scan]\nlists_off = 2\ntargets_csv_2 = \"/b.csv\"\ntargets_csv = \"/a.csv\"\n", nullptr,
                   off_first.get())
          == 0);
    CHECK(cfg_lists_are(*off_first, two, second_off, 2));
}

/* A gap and a lists_off entry past the last list stay in the composed config, for the start rules to refuse. */
void
test_start_rules_on_the_composed_lists() {
    struct rule_case {
        const char* ini;
        const char* why;
    };

    static const rule_case cases[] = {
        {"[trunk_scan]\nenabled = true\ntargets_csv_2 = \"/b.csv\"\n", "targets_csv is required"},
        {"[trunk_scan]\nenabled = true\ntargets_csv = \"/a.csv\"\ntargets_csv_3 = \"/c.csv\"\n", "without gaps"},
        {"[trunk_scan]\nenabled = true\ntargets_csv = \"/a.csv\"\nlists_off = 2\n", "has no targets_csv"},
        {"[trunk_scan]\nenabled = true\ntargets_csv = \"/a.csv\"\ntargets_csv_2 = \"/b.csv\"\nlists_off = 1, 2\n",
         "at least one list must start on"},
    };
    for (const rule_case& c : cases) {
        auto cfg = std::make_unique<dsdneoUserConfig>();
        CHECK(load_ini(c.ini, nullptr, cfg.get()) == 0);
        char why[256];
        CHECK(dsd_user_config_check_trunk_scan_startable(cfg.get(), nullptr, why, sizeof why) != 0);
        if (strstr(why, c.why) == nullptr) {
            DSD_FPRINTF(stderr, "start rule for %s: got \"%s\", wanted \"%s\"\n", c.ini, why, c.why);
            g_failures++;
        }
        /* The session refuses it at start the same way, from the lists it applied. */
        auto opts = std::make_unique<dsd_opts>();
        auto state = std::make_unique<dsd_state>();
        dsd_apply_user_config_to_opts(cfg.get(), opts.get(), state.get());
        CHECK(dsd_trunk_scan_lists_describe_fault(opts->trunk_scan_lists, opts->trunk_scan_list_count, why, sizeof why)
              != 0);
    }
    /* A scan that is off is no start rule's business. */
    auto off = std::make_unique<dsdneoUserConfig>();
    CHECK(load_ini("[trunk_scan]\nenabled = false\ntargets_csv = \"/a.csv\"\ntargets_csv_3 = \"/c.csv\"\n", nullptr,
                   off.get())
          == 0);
    CHECK(dsd_user_config_check_trunk_scan_startable(off.get(), nullptr, nullptr, 0U) == 0);
}

/* An include gives the base, the file overrides key by key, a profile over both: an omitted key inherits, an empty
   targets_csv_N shrinks the inherited set and an empty lists_off switches every inherited list on. */
void
test_includes_and_profiles_compose_per_key() {
    char included[DSD_TEST_PATH_MAX];
    CHECK(write_temp_config("[trunk_scan]\n"
                            "targets_csv = \"/a.csv\"\n"
                            "targets_csv_2 = \"/b.csv\"\n"
                            "targets_csv_3 = \"/c.csv\"\n"
                            "lists_off = 3\n",
                            included, sizeof included)
          == 0);
    const std::string root = std::string("include = \"") + included + "\"\n"
                             + "[trunk_scan]\n"
                               "enabled = true\n"
                               "targets_csv_2 = \"/x.csv\"\n"
                               "[profile.short]\n"
                               "trunk_scan.targets_csv_3 = \"\"\n"
                               "trunk_scan.lists_off = \"\"\n"
                               "[profile.offonly]\n"
                               "trunk_scan.lists_off = 1, 2\n";
    auto base = std::make_unique<dsdneoUserConfig>();
    CHECK(load_ini(root, nullptr, base.get()) == 0);
    static const char* const three[] = {"/a.csv", "/x.csv", "/c.csv"};
    static const int third_off[] = {0, 0, 1};
    CHECK(cfg_lists_are(*base, three, third_off, 3));

    auto shrunk = std::make_unique<dsdneoUserConfig>();
    CHECK(load_ini(root, "short", shrunk.get()) == 0);
    static const char* const two[] = {"/a.csv", "/x.csv"};
    static const int none_off[] = {0, 0};
    CHECK(cfg_lists_are(*shrunk, two, none_off, 2));

    auto offonly = std::make_unique<dsdneoUserConfig>();
    CHECK(load_ini(root, "offonly", offonly.get()) == 0);
    static const int first_two_off[] = {1, 1, 0};
    CHECK(cfg_lists_are(*offonly, three, first_two_off, 3));
    (void)remove(included);
}

/* An empty targets_csv_N clears what an earlier file left, never what a later one names: an include's empty
   targets_csv_2 under a file that sets targets_csv_3 is a gap the start rules refuse. A profile that shrinks an
   inherited set clears the removed lists' off states with them, so an inherited lists_off naming one does not refuse
   the start (review I1). */
void
test_empty_key_clears_only_what_earlier_layers_left() {
    char included[DSD_TEST_PATH_MAX];
    CHECK(write_temp_config("[trunk_scan]\ntargets_csv = \"/a.csv\"\ntargets_csv_2 = \"\"\n", included, sizeof included)
          == 0);
    auto gap = std::make_unique<dsdneoUserConfig>();
    CHECK(load_ini(std::string("include = \"") + included + "\"\n[trunk_scan]\nenabled = true\n"
                       + "targets_csv_3 = \"/c.csv\"\n",
                   nullptr, gap.get())
          == 0);
    CHECK(gap->trunk_scan_list_count == 3 && strcmp(gap->trunk_scan_lists[2].path, "/c.csv") == 0);
    CHECK(refused_with(*gap, "without gaps"));
    (void)remove(included);

    const std::string root = "[trunk_scan]\n"
                             "enabled = true\n"
                             "targets_csv = \"/a.csv\"\n"
                             "targets_csv_2 = \"/b.csv\"\n"
                             "targets_csv_3 = \"/c.csv\"\n"
                             "lists_off = 3\n"
                             "[profile.short]\n"
                             "trunk_scan.targets_csv_3 = \"\"\n"
                             "[profile.keepoff]\n"
                             "trunk_scan.targets_csv_3 = \"\"\n"
                             "trunk_scan.lists_off = 3\n";
    auto shrunk = std::make_unique<dsdneoUserConfig>();
    CHECK(load_ini(root, "short", shrunk.get()) == 0);
    static const char* const two[] = {"/a.csv", "/b.csv"};
    static const int none_off[] = {0, 0};
    CHECK(cfg_lists_are(*shrunk, two, none_off, 2));
    CHECK(dsd_user_config_check_trunk_scan_startable(shrunk.get(), nullptr, nullptr, 0U) == 0);
    /* A profile that also names lists_off keeps what it says, which the start rules then judge. */
    auto keepoff = std::make_unique<dsdneoUserConfig>();
    CHECK(load_ini(root, "keepoff", keepoff.get()) == 0);
    CHECK(refused_with(*keepoff, "has no targets_csv"));
}

/* lists_off that is no list of list numbers keeps what was there, with a warning; the validator calls it an error. */
void
test_bad_lists_off_keeps_previous() {
    static const char* const bad[] = {"0", "9", "2, 2", "2,", ",2", "two", "+2", "2 3"};
    for (const char* value : bad) {
        g_log_text[0] = '\0';
        auto cfg = std::make_unique<dsdneoUserConfig>();
        const std::string ini = std::string("[trunk_scan]\ntargets_csv = \"/a.csv\"\ntargets_csv_2 = \"/b.csv\"\n"
                                            "lists_off = 2\nlists_off = ")
                                + value + "\n";
        CHECK(load_ini(ini, nullptr, cfg.get()) == 0);
        static const char* const paths[] = {"/a.csv", "/b.csv"};
        static const int off[] = {0, 1};
        if (!cfg_lists_are(*cfg, paths, off, 2) || strstr(g_log_text, "invalid lists_off") == nullptr) {
            DSD_FPRINTF(stderr, "lists_off = %s was not refused whole: %s\n", value, g_log_text);
            g_failures++;
        }
        uint32_t mask = 0U;
        CHECK(dsd_trunk_scan_lists_off_parse(value, &mask) != 0);
    }
    uint32_t mask = 0U;
    CHECK(dsd_trunk_scan_lists_off_parse(" 8 ,1,\t3 ", &mask) == 0 && mask == 0x85U);
    CHECK(dsd_trunk_scan_lists_off_parse("", &mask) == 0 && mask == 0U);
    /* Blanks before a comma are no part of the number, however many (review M3). */
    CHECK(dsd_trunk_scan_lists_off_parse("2        , 3\t\t\t\t\t\t\t\t", &mask) == 0 && mask == 0x6U);
    CHECK(dsd_trunk_scan_lists_off_parse("        2", &mask) == 0 && mask == 0x2U);
    CHECK(dsd_trunk_scan_lists_off_parse("00000002", &mask) != 0); /* longer than any list number */
}

/* A path whose expansion does not fit is refused and the key keeps what it had, never cut to another file's name
   (any path key; before, the loader copied the first 1023 bytes). */
void
test_path_expansion_overflow_is_refused() {
    const std::string long_path = "/" + std::string(1100, 'p') + ".csv";
    auto cfg = std::make_unique<dsdneoUserConfig>();
    g_log_text[0] = '\0';
    CHECK(load_ini("[trunk_scan]\ntargets_csv = \"/a.csv\"\ntargets_csv_2 = \"/b.csv\"\ntargets_csv_2 = \"" + long_path
                       + "\"\n[trunking]\nchan_csv = \"/chan.csv\"\nchan_csv = \"" + long_path + "\"\n",
                   nullptr, cfg.get())
          == 0);
    static const char* const paths[] = {"/a.csv", "/b.csv"};
    static const int off[] = {0, 0};
    CHECK(cfg_lists_are(*cfg, paths, off, 2));
    CHECK(strcmp(cfg->trunk_chan_csv, "/chan.csv") == 0);
    CHECK(strstr(g_log_text, "invalid targets_csv_2; the path cannot be expanded or is too long") != nullptr);
    CHECK(strstr(g_log_text, "invalid chan_csv; the path cannot be expanded or is too long") != nullptr);
}

/* Applied over a session's lists: naming lists_off alone keeps its paths, naming paths alone keeps its off states,
   naming neither keeps both. */
void
test_apply_composes_over_the_session() {
    auto opts = std::make_unique<dsd_opts>();
    auto state = std::make_unique<dsd_state>();
    DSD_SNPRINTF(opts->trunk_scan_lists[0].path, sizeof opts->trunk_scan_lists[0].path, "%s", "/a.csv");
    DSD_SNPRINTF(opts->trunk_scan_lists[1].path, sizeof opts->trunk_scan_lists[1].path, "%s", "/b.csv");
    opts->trunk_scan_lists[1].start_off = 1U;
    opts->trunk_scan_list_count = 2;

    auto cfg = std::make_unique<dsdneoUserConfig>();
    CHECK(load_ini("[trunk_scan]\nlists_off = 1\n", nullptr, cfg.get()) == 0);
    dsd_apply_user_config_to_opts(cfg.get(), opts.get(), state.get());
    static const char* const paths[] = {"/a.csv", "/b.csv"};
    static const int first_off[] = {1, 0};
    CHECK(lists_are(opts->trunk_scan_lists, opts->trunk_scan_list_count, paths, first_off, 2));

    CHECK(load_ini("[trunk_scan]\ntargets_csv = \"/c.csv\"\ntargets_csv_2 = \"/d.csv\"\n", nullptr, cfg.get()) == 0);
    dsd_apply_user_config_to_opts(cfg.get(), opts.get(), state.get());
    static const char* const replaced[] = {"/c.csv", "/d.csv"};
    CHECK(lists_are(opts->trunk_scan_lists, opts->trunk_scan_list_count, replaced, first_off, 2));

    CHECK(load_ini("[trunk_scan]\nidle_dwell_ms = 4000\n", nullptr, cfg.get()) == 0);
    dsd_apply_user_config_to_opts(cfg.get(), opts.get(), state.get());
    CHECK(lists_are(opts->trunk_scan_lists, opts->trunk_scan_list_count, replaced, first_off, 2));

    /* One path key alone replaces that list only (an API config_apply names just what it changes)... */
    CHECK(load_ini("[trunk_scan]\ntargets_csv_2 = \"/x.csv\"\n", nullptr, cfg.get()) == 0);
    dsd_apply_user_config_to_opts(cfg.get(), opts.get(), state.get());
    static const char* const second[] = {"/c.csv", "/x.csv"};
    CHECK(lists_are(opts->trunk_scan_lists, opts->trunk_scan_list_count, second, first_off, 2));
    /* ...and an empty one clears that list and every later one the session holds. */
    DSD_SNPRINTF(opts->trunk_scan_lists[2].path, sizeof opts->trunk_scan_lists[2].path, "%s", "/y.csv");
    opts->trunk_scan_list_count = 3;
    CHECK(load_ini("[trunk_scan]\ntargets_csv_2 = \"\"\n", nullptr, cfg.get()) == 0);
    dsd_apply_user_config_to_opts(cfg.get(), opts.get(), state.get());
    static const char* const first_only[] = {"/c.csv"};
    CHECK(lists_are(opts->trunk_scan_lists, opts->trunk_scan_list_count, first_only, first_off, 1));

    /* An empty targets_csv now clears the lists (it used to be taken as "not set"). */
    CHECK(load_ini("[trunk_scan]\ntargets_csv = \"\"\nlists_off = \"\"\n", nullptr, cfg.get()) == 0);
    dsd_apply_user_config_to_opts(cfg.get(), opts.get(), state.get());
    CHECK(opts->trunk_scan_list_count == 0 && opts->trunk_scan_lists[0].path[0] == '\0');
}

/* A save writes the configured lists, or those waiting for the next start, never anything else. */
void
test_snapshot_and_next_start() {
    auto opts = std::make_unique<dsd_opts>();
    auto state = std::make_unique<dsd_state>();
    opts->trunk_scan_enabled = 1;
    DSD_SNPRINTF(opts->trunk_scan_lists[0].path, sizeof opts->trunk_scan_lists[0].path, "%s", "/a.csv");
    DSD_SNPRINTF(opts->trunk_scan_lists[1].path, sizeof opts->trunk_scan_lists[1].path, "%s", "/b.csv");
    opts->trunk_scan_lists[1].start_off = 1U;
    opts->trunk_scan_list_count = 2;

    auto snap = std::make_unique<dsdneoUserConfig>();
    dsd_snapshot_opts_to_user_config(opts.get(), state.get(), snap.get());
    static const char* const paths[] = {"/a.csv", "/b.csv"};
    static const int off[] = {0, 1};
    CHECK(cfg_lists_are(*snap, paths, off, 2));
    CHECK(dsd_user_config_names_trunk_scan_lists(snap.get()));

    dsd_opts_next_start* next = &opts->next_start;
    DSD_SNPRINTF(next->trunk_scan_lists[0].path, sizeof next->trunk_scan_lists[0].path, "%s", "/n1.csv");
    DSD_SNPRINTF(next->trunk_scan_lists[1].path, sizeof next->trunk_scan_lists[1].path, "%s", "/n2.csv");
    DSD_SNPRINTF(next->trunk_scan_lists[2].path, sizeof next->trunk_scan_lists[2].path, "%s", "/n3.csv");
    next->trunk_scan_lists[0].start_off = 1U;
    next->trunk_scan_list_count = 3;
    next->pending |= DSD_NEXT_START_TRUNK_SCAN_TARGETS;
    dsd_snapshot_opts_to_user_config(opts.get(), state.get(), snap.get());
    static const char* const waiting[] = {"/n1.csv", "/n2.csv", "/n3.csv"};
    static const int waiting_off[] = {1, 0, 0};
    CHECK(cfg_lists_are(*snap, waiting, waiting_off, 3));
    const std::string text = render(*snap);
    CHECK(text.find("targets_csv_3 = \"/n3.csv\"\n") != std::string::npos);
    CHECK(text.find("lists_off = 1\n") != std::string::npos);
}

/* --validate-config reports the list keys: a bad lists_off and a path that cannot fit at their line, the start rules
   on the composed file. */
void
test_validation_reports_list_keys() {
    const std::string long_path = "/" + std::string(1100, 'p') + ".csv";

    struct validate_case {
        std::string ini;
        const char* key;
        const char* message;
    };

    const validate_case cases[] = {
        {"[trunk_scan]\nenabled = true\ntargets_csv = \"/a.csv\"\nlists_off = 1,1\n", "lists_off", "Expected list"},
        {"[trunk_scan]\ntargets_csv_2 = \"" + long_path + "\"\n", "targets_csv_2",
         "Path cannot be expanded or is too long"},
        {"[trunk_scan]\nenabled = true\ntargets_csv = \"/a.csv\"\ntargets_csv_4 = \"/d.csv\"\n", "targets_csv",
         "without gaps"},
        {"[trunk_scan]\nenabled = true\ntargets_csv = \"/a.csv\"\nlists_off = 1\n", "lists_off",
         "at least one list must start on"},
    };
    for (const validate_case& c : cases) {
        char path[DSD_TEST_PATH_MAX];
        CHECK(write_temp_config(c.ini, path, sizeof path) == 0);
        dsdcfg_diagnostics_t diags;
        CHECK(dsd_user_config_validate(path, &diags) != 0);
        bool found = false;
        for (int i = 0; i < diags.count; i++) {
            found |= diags.items[i].level == DSDCFG_DIAG_ERROR && strcmp(diags.items[i].key, c.key) == 0
                     && strstr(diags.items[i].message, c.message) != nullptr;
        }
        if (!found) {
            DSD_FPRINTF(stderr, "validation of %.80s: no error on %s with \"%s\"\n", c.ini.c_str(), c.key, c.message);
            g_failures++;
        }
        dsdcfg_diags_free(&diags);
        (void)remove(path);
    }
    /* Every list key and lists_off are known keys. */
    char path[DSD_TEST_PATH_MAX];
    CHECK(write_temp_config("[trunk_scan]\nenabled = true\ntargets_csv = \"/a.csv\"\ntargets_csv_2 = \"/b.csv\"\n"
                            "targets_csv_8 = \"\"\nlists_off = 2\n",
                            path, sizeof path)
          == 0);
    dsdcfg_diagnostics_t diags;
    CHECK(dsd_user_config_validate(path, &diags) == 0 && diags.warning_count == 0);
    dsdcfg_diags_free(&diags);
    (void)remove(path);
}

/* --validate-config refuses exactly the paths the loader refuses, key by key: every PATH key in the schema, at the
   lengths either side of a 512- and a 1024-byte field (review M2: per_call_wav_dir keeps 511 bytes). */
void
test_validator_and_loader_agree_on_path_length() {
    static const size_t lengths[] = {511U, 512U, 1023U, 1024U};
    for (int e = 0; e < dsdcfg_schema_count(); e++) {
        const dsdcfg_schema_entry_t* entry = dsdcfg_schema_get(e);
        if (entry == nullptr || entry->type != DSDCFG_TYPE_PATH) {
            continue;
        }
        for (size_t length : lengths) {
            const std::string path = "/" + std::string(length - 1U, 'p');
            /* An input file path is saved only beside a file source. */
            const std::string source = strcmp(entry->section, "input") == 0 ? "source = \"file\"\n" : "";
            std::string ini = "[";
            ini += entry->section;
            ini += "]\n";
            ini += source;
            ini += entry->key;
            ini += " = \"";
            ini += path;
            ini += "\"\n";
            char file[DSD_TEST_PATH_MAX];
            CHECK(write_temp_config(ini, file, sizeof file) == 0);
            dsdcfg_diagnostics_t diags;
            const bool validator_refuses = dsd_user_config_validate(file, &diags) != 0;
            dsdcfg_diags_free(&diags);
            auto cfg = std::make_unique<dsdneoUserConfig>();
            CHECK(dsd_user_config_load(file, cfg.get()) == 0);
            /* What the loader kept is what a save writes back. */
            const bool loader_refuses = render(*cfg).find(path + "\"") == std::string::npos;
            (void)remove(file);
            if (validator_refuses != loader_refuses) {
                DSD_FPRINTF(stderr, "[%s] %s at %zu bytes: validator %s, loader %s\n", entry->section, entry->key,
                            length, validator_refuses ? "refuses" : "accepts", loader_refuses ? "refuses" : "keeps");
                g_failures++;
            }
        }
    }
}

/* The list numbers have one spelling each: targets_csv is list 1, and nothing names a ninth. */
void
test_unknown_list_keys_are_ignored() {
    auto cfg = std::make_unique<dsdneoUserConfig>();
    CHECK(load_ini("[trunk_scan]\ntargets_csv = \"/a.csv\"\ntargets_csv_1 = \"/x.csv\"\ntargets_csv_9 = \"/y.csv\"\n"
                   "targets_csv_02 = \"/z.csv\"\n",
                   nullptr, cfg.get())
          == 0);
    static const char* const paths[] = {"/a.csv"};
    static const int off[] = {0};
    CHECK(cfg_lists_are(*cfg, paths, off, 1));
}

} // namespace

int
main(void) {
    dsd_neo_log_set_tap(capture_log, NULL);
    test_numbered_keys_load_and_roundtrip();
    test_empty_key_beside_a_later_list_is_a_gap_in_any_order();
    test_empty_key_clears_only_what_earlier_layers_left();
    test_start_rules_on_the_composed_lists();
    test_includes_and_profiles_compose_per_key();
    test_bad_lists_off_keeps_previous();
    test_path_expansion_overflow_is_refused();
    test_apply_composes_over_the_session();
    test_snapshot_and_next_start();
    test_validation_reports_list_keys();
    test_unknown_list_keys_are_ignored();
    test_validator_and_loader_agree_on_path_length();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    return 0;
}
