// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_IO_AIRSPY_SOURCE_H
#define DSD_NEO_IO_AIRSPY_SOURCE_H
#include <dsd-neo/core/airspy_config.h>
#include <stddef.h>
#include <stdint.h>

struct airspy_source;
typedef void (*airspy_source_callback)(void* context, const float* samples, size_t pairs, uint64_t dropped);
/* Context is borrowed and must remain valid until airspy_source_close() returns.
 * Sample storage is borrowed only for the duration of each callback. */
airspy_source* airspy_source_open(const dsd_airspy_config* config, airspy_source_callback callback, void* context);
void airspy_source_close(airspy_source* s);
int airspy_source_start(airspy_source* s);
int airspy_source_stop(airspy_source* s);
int airspy_source_running(airspy_source* s);
/* Lock-free, for the stream monitor: the receive callbacks counted so far (each one counts, first thing, whatever it
 * carries); whether a frequency or rate change is stopping and restarting the stream; and how many times the stream
 * started (airspy_source_start() and every restart a change made). */
uint64_t airspy_source_progress(const airspy_source* s);
int airspy_source_reconfiguring(const airspy_source* s);
uint64_t airspy_source_generation(const airspy_source* s);
int airspy_source_frequency(airspy_source* s, uint32_t frequency);
int airspy_source_rate(airspy_source* s, uint32_t rate);
int airspy_source_controls(airspy_source* s, const dsd_airspy_config* config);
int airspy_source_info(airspy_source* s, dsd_airspy_info* info);
/* Descriptor remains owned by the platform host. Clear before waiting for in_use. */
int airspy_source_set_fd(int fd);
int airspy_source_fd_in_use(void);
/* The descriptor the open path holds, or -1 (#782). */
int airspy_source_fd_held(void);
/* The last open/claim error (0: none), latched by a failed airspy_source_open(); the host clears it per session. */
int airspy_source_last_open_error(void);
void airspy_source_clear_open_error(void);
int airspy_source_list(uint64_t* serials, int capacity);
#endif
