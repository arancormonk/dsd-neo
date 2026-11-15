// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Private pieces of the network I/Q module (iq_net_*.c), shared between its files and with its test.
 */

#ifndef DSD_NEO_IO_RADIO_IQ_NET_INTERNAL_H
#define DSD_NEO_IO_RADIO_IQ_NET_INTERNAL_H

#include <dsd-neo/io/iq_net.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Spec ---- */

/*
 * Split text in place at every ':' into at most max_fields fields, empty ones kept ("a::b" is three fields). Returns
 * the field count, or max_fields + 1 when there are more (fields then holds the first max_fields).
 */
size_t iq_net_split_fields(char* text, char** fields, size_t max_fields);

enum { IQ_NET_HZ_OK = 0, IQ_NET_HZ_SYNTAX = -1, IQ_NET_HZ_RANGE = -2, IQ_NET_HZ_FRACTION = -3 };

/*
 * Parse plain decimal Hz, `[0-9]*('.'[0-9]*)?` with at least one digit, then one optional suffix character from
 * suffixes (k/K = 10^3, m/M = 10^6, g/G = 10^9), in exact integer arithmetic. With whole_only a value that is not a
 * whole number of Hz is IQ_NET_HZ_FRACTION; without it the value rounds half up to a whole Hz. Above UINT32_MAX is
 * IQ_NET_HZ_RANGE. *out_hz is written only on IQ_NET_HZ_OK.
 */
int iq_net_parse_hz(const char* text, const char* suffixes, int whole_only, uint32_t* out_hz);

/* Whether a reader can run spec: a kind, a wire format, a non-empty host, a port in 0..65535 (not 0 for TCP). */
int iq_net_spec_usable(const dsd_iq_net_spec* spec);

/* ---- TCP partial-sample carry ---- */

/* The bytes of a complex sample a TCP read cut short, kept for the next read. */
typedef struct {
    uint8_t bytes[8];
    size_t len;
} iq_net_carry;

/* Copy the carried bytes to the start of buf (room for 8) and return how many. The carry stays as it is until
   iq_net_carry_keep() replaces it, so a read that fails loses nothing. */
size_t iq_net_carry_restore(const iq_net_carry* carry, uint8_t* buf);

/* Of the total bytes at buf, keep the trailing partial sample in carry and return the number of whole samples. */
size_t iq_net_carry_keep(iq_net_carry* carry, const uint8_t* buf, size_t total, size_t sample_bytes);

/* ---- Rate telemetry ---- */

#define IQ_NET_RATE_WARMUP_MS 5000U
#define IQ_NET_RATE_WINDOW_MS 10000U
#define IQ_NET_RATE_WINDOWS   3

/*
 * Observed throughput against the spec's rate (not a loss detector). After a warm-up, valid samples are counted in
 * back-to-back windows over full elapsed time. A window that saw a boundary, a stall, a known loss or a drop is
 * discarded and breaks a streak. IQ_NET_RATE_WINDOWS consecutive windows strictly below 0.9x the rate, or strictly
 * above 1.1x, give one warning per arm (one per connection).
 */
typedef struct {
    uint32_t rate_hz;
    int measuring;            /* 0 while paused (stalled) or never armed */
    uint64_t window_start_ms; /* the window in progress starts here; the warm-up ends at the first one */
    uint64_t window_samples;
    int window_discarded;
    int streak;      /* windows off the same side in a row */
    int streak_side; /* -1 below, +1 above */
    uint64_t streak_samples[IQ_NET_RATE_WINDOWS];
    int warned;
} iq_net_rate_monitor;

/* A new connection: forget everything, re-arm the warning, warm up from now_ms. */
void iq_net_rate_arm(iq_net_rate_monitor* m, uint32_t rate_hz, uint64_t now_ms);
/* Measuring again after a pause (a resume): warm up from now_ms, streak cleared, the warning stays given. */
void iq_net_rate_restart(iq_net_rate_monitor* m, uint64_t now_ms);
/* Stop measuring (a stall) until the next arm or restart. */
void iq_net_rate_pause(iq_net_rate_monitor* m);
/* Discard the window in progress. */
void iq_net_rate_discard(iq_net_rate_monitor* m);
/*
 * Close every window that ended by now_ms, then count samples (received at now_ms) in the window in progress.
 * Returns 1 when a closed window completed a warning streak (once per arm), with the streak's mean rate in samples
 * per second in *out_rate; else 0.
 */
int iq_net_rate_update(iq_net_rate_monitor* m, uint64_t now_ms, uint64_t samples, uint64_t* out_rate);

/* The warning's text, with a trailing newline. */
void iq_net_rate_warning_text(char* out, size_t out_size, uint64_t measured_rate, uint32_t rate_hz);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_IO_RADIO_IQ_NET_INTERNAL_H */
