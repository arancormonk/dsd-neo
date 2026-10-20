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
    dsd_receive_identity identity = {4, DSD_RECEIVE_DCC, 0, 1};
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
    dsd_receive_identity identity = {7, DSD_RECEIVE_DCC, 1, 2};
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
}

int
main(void) {
    test_values();
    test_filters();
    test_names();
    return 0;
}
