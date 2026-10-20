// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_RUNTIME_RECEIVE_POLICY_H
#define DSD_NEO_RUNTIME_RECEIVE_POLICY_H

#include <dsd-neo/core/receive_types.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Edit one filter (0..3) or the complete names table (4); leaves the policy unchanged on failure. */
int dsd_receive_edit_parse(dsd_receive_policy* policy, int kind, int enabled, const char* allow, const char* block,
                           const char* names, char* error, size_t capacity);
int dsd_receive_policy_validate(const dsd_receive_policy* policy, int kind, char* error, size_t capacity);
/** Semantic equality, independent of structure padding and unused naming entries. */
int dsd_receive_filter_equal(const dsd_receive_filter* a, const dsd_receive_filter* b);
int dsd_receive_names_equal(const dsd_service_names* a, const dsd_service_names* b);
int dsd_receive_policy_equal(const dsd_receive_policy* a, const dsd_receive_policy* b);

const char* dsd_receive_kind_name(int kind);
int dsd_receive_value_parse(int kind, const char* text, unsigned int* out);
int dsd_receive_value_format(int kind, unsigned int value, char* out, size_t capacity);
int dsd_receive_set_parse(int kind, const char* text, dsd_receive_set* out, char* error, size_t capacity);
int dsd_receive_set_format(int kind, const dsd_receive_set* set, char* out, size_t capacity);
int dsd_receive_set_contains(const dsd_receive_set* set, unsigned int value);
dsd_receive_verdict dsd_receive_policy_match(const dsd_receive_policy* policy, const dsd_receive_identity* identity);
/** Empty string when no rule matches. Access/tone names outrank slot names. */
const char* dsd_receive_service_name(const dsd_service_names* names, const dsd_receive_identity* identity);
/** Atomic addition. A conflicting selector or oversized/CSV-unsafe name is refused. */
int dsd_receive_name_add(dsd_service_names* names, int kind, const char* argument, char* error, size_t capacity);
int dsd_receive_names_parse(const char* text, dsd_service_names* out, char* error, size_t capacity);
int dsd_receive_names_format(const dsd_service_names* names, char* out, size_t capacity);
/** Shared [receive] key validation, also used by strict configuration validation. Atomic on failure. */
int dsd_receive_config_set(dsd_receive_policy* policy, const char* key, const char* value, char* error,
                           size_t capacity);
/** Recognize a canonical switch (without =operand). 1 takes an operand, 0 takes none, -1 is unknown. */
int dsd_receive_option_argument(const char* option);
/** Apply one recognized option atomically. seen starts at zero for a scope/CLI invocation. First filter option
 * replaces that kind's inherited filter; first name option replaces the inherited naming table. */
int dsd_receive_option_apply(dsd_receive_policy* policy, unsigned int* seen, const char* option, const char* argument,
                             char* error, size_t capacity);

#ifdef __cplusplus
}
#endif
#endif
