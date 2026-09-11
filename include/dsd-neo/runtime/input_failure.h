// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_RUNTIME_INPUT_FAILURE_H
#define DSD_NEO_RUNTIME_INPUT_FAILURE_H
#ifdef __cplusplus
extern "C" {
#endif
/** Session input failure facts, retained independently of decoder snapshots.
 * No credentials, key material, paths, or log parsing enter this interface. */
typedef enum {
    DSD_INPUT_FAILURE_NONE = 0,
    DSD_INPUT_FAILURE_REFUSED = 1,
    DSD_INPUT_FAILURE_TIMEOUT = 2,
    DSD_INPUT_FAILURE_RESOLVE = 3,
    DSD_INPUT_FAILURE_NETWORK = 4,
    DSD_INPUT_FAILURE_FILE = 5,
    DSD_INPUT_FAILURE_CONFIGURATION = 6,
    DSD_INPUT_FAILURE_DEVICE = 7
} dsd_input_failure_kind;

typedef struct {
    int kind;
    int native_code;
} dsd_input_failure;

void dsd_input_failure_clear(void);
void dsd_input_failure_report(dsd_input_failure_kind kind, int code);
void dsd_input_failure_get(dsd_input_failure* out);
dsd_input_failure_kind dsd_input_failure_classify_socket(int code);
#ifdef __cplusplus
}
#endif
#endif
