// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The raw I/Q network input's ingest (`iqtcp:` / `iqudp:`, issue #743): the stream side of the socket reader.
 *
 * The socket reader (<dsd-neo/io/iq_net.h>) hands over converted samples and stream events on its own thread. This
 * part runs on that thread too: it publishes the input level of each received block, resamples the stream to the DSP
 * input rate the rate plan settled on, collects the result into whole blocks of dsd_iq_live_block_samples() and writes
 * each whole block to the input ring, dropping a block the ring cannot take whole (a gap mark only). A reconnect or a
 * resume after a stall starts the input over and runs the stream's boundary (the caller's callback); a datagram the
 * locked sender lost starts it over with a gap mark. Private to src/io/radio/: the device backend RTL_BACKEND_NET_IQ
 * owns one.
 */

#ifndef DSD_NEO_IO_RADIO_RTL_NET_IQ_H
#define DSD_NEO_IO_RADIO_RTL_NET_IQ_H

#include <dsd-neo/io/iq_net.h>
#include <dsd-neo/io/iq_net_spec.h>
#include <stddef.h>
#include <stdint.h>

/* The stream's discontinuity boundary for a reconnect or a resume (@p reason: RTL_STREAM_GAP_REASON_NET_RECONNECT or
 * _NET_RESUME), run on the reader thread with no lock held, before the first sample after it is written. */
typedef void (*rtl_net_iq_boundary_fn)(const char* reason, void* ctx);

struct rtl_net_iq_config {
    dsd_iq_net_spec spec{};
    /* The rate the demodulator takes (the plan's DSP input rate) and the final DSP rate whose band the resampler
       protects (0.45 of it), as for a resampled replay. */
    uint32_t dsp_input_rate_hz = 0U;
    uint32_t final_dsp_rate_hz = 0U;
    rtl_net_iq_boundary_fn on_boundary = nullptr;
    void* boundary_ctx = nullptr;
};

/* Create the ingest and open the input on the calling thread (TCP connects, UDP binds). Logs why it failed and reports
 * it (dsd_input_failure_report()); NULL then. Allocates every buffer it uses. */
struct rtl_net_iq* rtl_net_iq_create(const struct rtl_net_iq_config* cfg, struct input_ring_state* ring);

/* Start the reader thread; 0, or -1 when it cannot start (or already ran). */
int rtl_net_iq_start(struct rtl_net_iq* n);

/* Stop the reader and join its thread: no callback runs after it returns. Idempotent. */
void rtl_net_iq_stop(struct rtl_net_iq* n);

/* Stop, close and free. NULL is a no-op. */
void rtl_net_iq_destroy(struct rtl_net_iq* n);

/* 1 while the stream delivers: connected (or a sender locked) and a whole block written since it came up or since the
 * last boundary; 0 while connecting, reconnecting, stalled or before the first block (dsd_iq_net_reader_available()). */
int rtl_net_iq_available(const struct rtl_net_iq* n);

/* The spec the input was opened with. */
const dsd_iq_net_spec* rtl_net_iq_spec(const struct rtl_net_iq* n);

/* Complex samples in one block written to the input ring (dsd_iq_live_block_samples() at the DSP input rate). */
uint32_t rtl_net_iq_block_samples(const struct rtl_net_iq* n);

/* Whether the stream is resampled to the DSP input rate, with the resampler's L/M and ratio error. 1 when it is. */
int rtl_net_iq_resample_info(const struct rtl_net_iq* n, int* out_l, int* out_m, double* out_ppm);

/* What the ingest did, for tests (counts since create). */
struct rtl_net_iq_state {
    uint64_t blocks_written = 0U;   /* whole blocks written to the input ring */
    uint64_t blocks_dropped = 0U;   /* whole blocks the full ring could not take (a gap mark each) */
    uint64_t boundaries = 0U;       /* reconnect or resume boundaries run */
    uint64_t known_losses = 0U;     /* locked-sender losses (gap marks) */
    uint64_t input_restarts = 0U;   /* partial blocks discarded with the resampler history cleared */
    uint64_t resampler_clears = 0U; /* ... of those, with a resampler to clear */
    uint64_t drop_clears = 0U;      /* dropped blocks behind a resampler, which started over at their gap mark */
    uint64_t samples_ingested = 0U; /* received samples whose callback returned; the counts above are read after it */
    uint64_t resampler_errors = 0U; /* resampler calls that failed (their slice lost); none with a sound design */
    int local_port = -1;          /* the socket's local port (a UDP spec on port 0 learns its port here), -1 for none */
    dsd_iq_net_counters reader{}; /* the socket reader's own counters */
};

void rtl_net_iq_get_state(const struct rtl_net_iq* n, struct rtl_net_iq_state* out);

/* A device whose samples come from @p cfg's raw I/Q stream (RTL_BACKEND_NET_IQ): a fixed source with no tuner, whose
 * tuner setters return DSD_ERR_NOT_SUPPORTED. The input opens here (rtl_net_iq_create()); NULL when it does not. */
struct rtl_device* rtl_device_create_net_iq(const struct rtl_net_iq_config* cfg, struct input_ring_state* input_ring);

/* The ingest of an RTL_BACKEND_NET_IQ device, NULL for any other (rtl_device.cpp). */
struct rtl_net_iq* rtl_device_net_iq(const struct rtl_device* dev);

/* Design the reader-side resampler an I/Q source needs from @p source_hz to the DSP input rate @p dsp_in_hz (issues
 * #701, #743): an I/Q replay's capture, a stream's samples. Its protected band is 0.45 of @p final_dsp_hz, the demod
 * rate before post_downsample; @p max_in complex samples go through per pass. 0 with *@p out made (NULL at equal
 * rates), or -1 with the reason logged under @p who ("IQ replay") naming the source as @p source_noun ("capture"). */
int rtl_iq_resampler_open(const char* who, const char* source_noun, uint32_t source_hz, uint32_t dsp_in_hz,
                          uint64_t final_dsp_hz, size_t max_in, struct dsd_iq_resampler** out);

#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
/* The clock the next ingest's reader runs stall detection and rate telemetry on (NULL: real time). */
void rtl_net_iq_test_set_clock(dsd_iq_net_clock_fn clock_ms, void* clock_ctx);
#endif

#endif /* DSD_NEO_IO_RADIO_RTL_NET_IQ_H */
