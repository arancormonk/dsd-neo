// SPDX-License-Identifier: GPL-3.0-or-later
#include <assert.h>
#include <dsd-neo/core/receive_types.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/runtime/receive_policy.h>
#include <string.h>

static void
test_values(void) {
    unsigned int value = 999;
    assert(dsd_receive_value_parse(DSD_RECEIVE_DCC, "0", &value) == 0 && value == 0);
    assert(dsd_receive_value_parse(DSD_RECEIVE_RAN, "63", &value) == 0 && value == 63);
    assert(dsd_receive_value_parse(DSD_RECEIVE_NAC, "370", &value) == 0 && value == 0x370);
    assert(dsd_receive_value_parse(DSD_RECEIVE_NAC, "0xFFE", &value) == 0 && value == 0xFFE);
    assert(dsd_receive_value_parse(DSD_RECEIVE_NAC, "001", &value) == 0 && value == 1);
    assert(dsd_receive_value_parse(DSD_RECEIVE_NAC, "0", &value) < 0);
    assert(dsd_receive_value_parse(DSD_RECEIVE_NAC, "FFF", &value) < 0);
    assert(dsd_receive_value_parse(DSD_RECEIVE_DCC, "16", &value) < 0);
    assert(dsd_receive_value_parse(DSD_RECEIVE_RAN, "64", &value) < 0);
    assert(dsd_receive_value_parse(DSD_RECEIVE_SLOT, "0", &value) < 0);
    assert(dsd_receive_value_parse(DSD_RECEIVE_SLOT, "3", &value) < 0);
    assert(dsd_receive_value_parse(DSD_RECEIVE_NAC, "+370", &value) < 0);
    assert(dsd_receive_value_parse(DSD_RECEIVE_RAN, " 1", &value) < 0);
    assert(dsd_receive_value_parse(DSD_RECEIVE_DCC, "1.0", &value) < 0);
    assert(dsd_receive_value_parse(DSD_RECEIVE_NAC, "0x", &value) < 0);
}

static void
test_filters(void) {
    dsd_receive_policy policy = {0};
    unsigned int seen = 0;
    char error[160];
    assert(dsd_receive_option_apply(&policy, &seen, "--dcc-allow", "0/4/6/8", error, sizeof(error)) == 0);
    assert(dsd_receive_option_apply(&policy, &seen, "--dcc-block", "6/15", error, sizeof(error)) == 0);
    dsd_receive_identity identity = {4, DSD_RECEIVE_DCC, 0, 1, 0};
    assert(dsd_receive_policy_match(&policy, &identity) == DSD_RECEIVE_CHECKING);
    identity.known = 1;
    assert(dsd_receive_policy_match(&policy, &identity) == DSD_RECEIVE_ALLOWED);
    identity.value = 6;
    assert(dsd_receive_policy_match(&policy, &identity) == DSD_RECEIVE_REJECTED);
    identity.value = 0;
    assert(dsd_receive_policy_match(&policy, &identity) == DSD_RECEIVE_ALLOWED);
    assert(dsd_receive_option_apply(&policy, &seen, "--slot-block", "2", error, sizeof(error)) == 0);
    identity.slot = 2;
    assert(dsd_receive_policy_match(&policy, &identity) == DSD_RECEIVE_REJECTED);
    identity.slot = 0; /* FDMA is unaffected by the slot filter. */
    identity.kind = DSD_RECEIVE_NAC;
    assert(dsd_receive_policy_match(&policy, &identity) == DSD_RECEIVE_OFF);
    assert(dsd_receive_option_apply(&policy, &seen, "--nac-block", "370", error, sizeof(error)) == 0);
    identity.known = 0;
    assert(dsd_receive_policy_match(&policy, &identity) == DSD_RECEIVE_CHECKING);
    dsd_receive_policy previous = policy;
    unsigned int previous_seen = seen;
    assert(dsd_receive_option_apply(&policy, &seen, "--no-dcc-filter", NULL, error, sizeof(error)) < 0);
    assert(dsd_receive_policy_equal(&policy, &previous) && seen == previous_seen);
    seen = 0; /* A row's explicit disable replaces the inherited complete filter. */
    assert(dsd_receive_option_apply(&policy, &seen, "--no-dcc-filter", NULL, error, sizeof(error)) == 0);
    assert(!policy.filters[DSD_RECEIVE_DCC].enabled);
    assert(!dsd_receive_set_contains(&policy.filters[DSD_RECEIVE_DCC].allow, 4));
    const char* bad[] = {"/1", "1/", "1//2", "1,2", "1/1", "1/-2", "1/16", "1/2junk"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        dsd_receive_set set = {{123}};
        dsd_receive_set saved = set;
        assert(dsd_receive_set_parse(DSD_RECEIVE_DCC, bad[i], &set, error, sizeof(error)) < 0);
        assert(memcmp(&set, &saved, sizeof(set)) == 0);
    }
    dsd_receive_set set;
    assert(dsd_receive_set_parse(DSD_RECEIVE_NAC, "0xFFE/001/370", &set, error, sizeof(error)) == 0);
    char text[64];
    assert(dsd_receive_set_format(DSD_RECEIVE_NAC, &set, text, sizeof(text)) > 0);
    assert(strcmp(text, "001/370/FFE") == 0);
}

static void
test_names(void) {
    dsd_service_names names = {0};
    char error[160];
    assert(dsd_receive_names_parse("--slot-name '2=Second slot' --dcc-name \"7=Hospital\" --tone-name '141.3=20R "
                                   "Traveler' --tone-name D023I=Dispatch",
                                   &names, error, sizeof(error))
           == 0);
    dsd_receive_identity identity = {7, DSD_RECEIVE_DCC, 1, 2, 0};
    assert(strcmp(dsd_receive_service_name(&names, &identity), "Hospital") == 0);
    identity.value = 2;
    assert(strcmp(dsd_receive_service_name(&names, &identity), "Second slot") == 0);
    identity.kind = DSD_RECEIVE_DCS;
    identity.value = 047 * 2; /* D047N and D023I are the same signal. */
    assert(strcmp(dsd_receive_service_name(&names, &identity), "Dispatch") == 0);
    dsd_service_names previous = names;
    assert(dsd_receive_name_add(&names, DSD_RECEIVE_CTCSS, "D047N=Another name", error, sizeof(error)) < 0);
    assert(dsd_receive_names_equal(&names, &previous));
    assert(dsd_receive_name_add(&names, DSD_RECEIVE_CTCSS, "D047N=Dispatch", error, sizeof(error)) == 0);
    char text[DSD_RECEIVE_NAMES_TEXT_MAX + 1];
    assert(dsd_receive_names_format(&names, text, sizeof(text)) > 0);
    dsd_service_names roundtrip = {0};
    assert(dsd_receive_names_parse(text, &roundtrip, error, sizeof(error)) == 0);
    assert(dsd_receive_names_equal(&names, &roundtrip));
    const char* bad[] = {"7=", "16=Bad", "7= Hospital", "7=Hospital ", "7=A,B", "7=A\nB", "7=A\"B"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        assert(dsd_receive_name_add(&names, DSD_RECEIVE_DCC, bad[i], error, sizeof(error)) < 0);
        assert(dsd_receive_names_equal(&names, &previous));
    }
    DSD_MEMSET(&names, 0, sizeof(names));
    for (int i = 0; i < DSD_RECEIVE_NAME_MAX; i++) {
        char argument[96];
        DSD_SNPRINTF(argument, sizeof(argument), "%d=Service %d", i, i);
        assert(dsd_receive_name_add(&names, DSD_RECEIVE_RAN, argument, error, sizeof(error)) == 0);
    }
    assert(dsd_receive_name_add(&names, DSD_RECEIVE_RAN, "32=Overflow", error, sizeof(error)) < 0);
    char long_name[100];
    DSD_MEMSET(long_name, 'a', sizeof(long_name));
    long_name[0] = '1';
    long_name[1] = '=';
    long_name[sizeof(long_name) - 1] = '\0';
    assert(dsd_receive_name_add(&names, DSD_RECEIVE_RAN, long_name, error, sizeof(error)) < 0);
    /* Names are well-formed UTF-8 without C0 or C1 controls. */
    DSD_MEMSET(&names, 0, sizeof(names));
    const unsigned int before = names.count;
    assert(dsd_receive_name_add(&names, DSD_RECEIVE_RAN, "2=Caf\xC3\xA9 \xE2\x86\x92 \xF0\x9F\x9A\x92", error,
                                sizeof(error))
           == 0);
    assert(names.count == before + 1);
    const char* invalid[] = {"3=\xC3", "3=\xC0\xAF",     "3=\xED\xA0\x80", "3=\xF4\x90\x80\x80",
                             "3=\xFF", "3=x\xC2\x9B[2J", "3=\xE2\x82",     "3=\x80"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        assert(dsd_receive_name_add(&names, DSD_RECEIVE_RAN, invalid[i], error, sizeof(error)) < 0);
        assert(names.count == before + 1);
    }
}

/* A list is accepted only when its saved spelling fits as well: NACs 1..200 (512 of them, short hex spellings) save as
   exactly DSD_RECEIVE_LIST_MAX characters, and one more NAC would save longer than any list can load. */
static void
test_list_fits_as_saved(void) {
    char text[DSD_RECEIVE_LIST_MAX + 1];
    char error[160];
    for (unsigned int last = 0x200; last <= 0x201; last++) {
        text[0] = '\0';
        size_t used = 0;
        for (unsigned int nac = 1; nac <= last; nac++) {
            const int n = DSD_SNPRINTF(text + used, sizeof text - used, "%s%X", nac > 1 ? "/" : "", nac);
            assert(n > 0 && (size_t)n < sizeof text - used);
            used += (size_t)n;
        }
        dsd_receive_set set = {{0}};
        const int rc = dsd_receive_set_parse(DSD_RECEIVE_NAC, text, &set, error, sizeof error);
        if (last == 0x200) {
            char saved[DSD_RECEIVE_LIST_MAX + 1];
            assert(rc == 0);
            assert(dsd_receive_set_format(DSD_RECEIVE_NAC, &set, saved, sizeof saved) == DSD_RECEIVE_LIST_MAX);
        } else {
            assert(rc < 0 && strstr(error, "too long") != NULL);
            assert(!dsd_receive_set_contains(&set, 1));
        }
    }
}

int
main(void) {
    test_values();
    test_filters();
    test_names();
    test_list_fits_as_saved();
    return 0;
}
