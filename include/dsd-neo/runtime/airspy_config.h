// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_RUNTIME_AIRSPY_CONFIG_H
#define DSD_NEO_RUNTIME_AIRSPY_CONFIG_H
#include <dsd-neo/core/airspy_config.h>
#include <dsd-neo/core/opts_fwd.h>
#include <stdio.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Keys use airspy_ prefixes. Return 0 on success, -1 on invalid/unknown input. */
int dsd_airspy_config_set(dsd_airspy_config* cfg, const char* key, const char* value);
int dsd_airspy_config_valid(const dsd_airspy_config* c);
void dsd_airspy_config_render(FILE* out, const dsd_airspy_config* c);
int dsd_normalize_airspy_input_spec(dsd_opts* opts);
#ifdef __cplusplus
}
#endif
#endif
