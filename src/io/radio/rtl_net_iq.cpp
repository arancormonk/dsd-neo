// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The raw I/Q network input's ingest (issue #743): level, resampling, whole blocks into the input ring.
 *
 * Everything here but create, start, stop and destroy runs on the socket reader's thread (its callbacks), so the
 * block being filled, the resampler and the level scratch need no lock. The reader never waits on the ring: a block
 * the ring cannot take whole is dropped whole and leaves a gap mark, as a SoapySDR or Airspy device drops what a full
 * ring cannot take, so the queued samples and the decoder's generation stay as they are. The demod starts its filters
 * over at the gap mark, so a resampler starts over there too, with the input after the dropped block (its calls end
 * where a block does, so the drop costs that block alone): its first output after the gap is the cascade's first input
 * after its reset, the alignment the squelch's reader model describes (issue #899).
 */

#include "rtl_net_iq.h"

#include <atomic>
#include <dsd-neo/core/input_level.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/dsp/iq_resampler.h>
#include <dsd-neo/io/iq_net.h>
#include <dsd-neo/io/iq_net_spec.h>
#include <dsd-neo/io/iq_replay.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/runtime/input_failure.h>
#include <dsd-neo/runtime/input_ring.h>
#include <dsd-neo/runtime/log.h>
#include <math.h>
#include <new>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "rtl_stream_shared.hpp"

/* Complex samples the resampler takes per call: its output for one slice is the most a block of output is made of. */
static const size_t kNetIqResampleSliceSamples = 4096U;

struct rtl_net_iq {
    dsd_iq_net_reader* reader = nullptr;
    struct input_ring_state* ring = nullptr;
    dsd_iq_net_format format = DSD_IQ_NET_FORMAT_NONE;
    uint32_t dsp_input_rate_hz = 0U;
    /* The block: chunk_pairs complex samples at the DSP input rate, filled to fill. */
    uint32_t chunk_pairs = 0U;
    float* chunk = nullptr;
    size_t fill = 0U;
    /* Made at create when the stream's rate is not the DSP input rate; NULL otherwise. */
    dsd_iq_resampler* resampler = nullptr;
    float* resampled = nullptr;
    size_t resampled_cap = 0U;
    /* The input level's integer view of what it measures (cu8 bytes, or cs8/cs16 as 16-bit samples), and the samples
       of short reads it collects until they span a block (level_fill complex samples). */
    uint8_t* level_u8 = nullptr;
    int16_t* level_s16 = nullptr;
    float* level_pending = nullptr;
    size_t level_fill = 0U;
    rtl_net_iq_boundary_fn on_boundary = nullptr;
    void* boundary_ctx = nullptr;
    std::atomic<uint64_t> blocks_written{0U};
    std::atomic<uint64_t> blocks_dropped{0U};
    std::atomic<uint64_t> boundaries{0U};
    std::atomic<uint64_t> known_losses{0U};
    std::atomic<uint64_t> input_restarts{0U};
    std::atomic<uint64_t> resampler_clears{0U};
    std::atomic<uint64_t> drop_clears{0U};
    /* Samples whose callback has returned, every count above it written first (a test waits for it, issue #899). */
    std::atomic<uint64_t> samples_ingested{0U};
    std::atomic<uint64_t> resampler_errors{0U};
};

#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
static std::atomic<dsd_iq_net_clock_fn> g_test_clock{nullptr};
static std::atomic<void*> g_test_clock_ctx{nullptr};

void
rtl_net_iq_test_set_clock(dsd_iq_net_clock_fn clock_ms, void* clock_ctx) {
    g_test_clock_ctx.store(clock_ctx, std::memory_order_relaxed);
    g_test_clock.store(clock_ms, std::memory_order_release);
}
#endif

/* The level of @p pairs received samples, from the sender's own sample values: cu8 as the RTL-SDR's bytes, cs8 and cs16
 * as 16-bit samples (cs8 times 256, so its rails clip as a 16-bit stream's do), cs32 and cf32 as the floats. The
 * converters are exact for the integer formats, so rounding back gives the sample sent. */
static void
net_iq_measure_level(struct rtl_net_iq* n, const float* iq, size_t pairs) {
    const size_t count = pairs * 2U;
    dsd_input_level_snapshot snapshot;
    int rc = -1;
    switch (n->format) {
        case DSD_IQ_NET_FORMAT_CU8:
            for (size_t i = 0U; i < count; i++) {
                n->level_u8[i] = (uint8_t)lrintf(iq[i] * 127.5f + 127.5f);
            }
            rc = dsd_input_level_metrics_from_cu8(n->level_u8, count, DSD_INPUT_LEVEL_SOURCE_NET_IQ, &snapshot);
            break;
        case DSD_IQ_NET_FORMAT_CS8:
        case DSD_IQ_NET_FORMAT_CS16:
            for (size_t i = 0U; i < count; i++) {
                const long v = lrintf(iq[i] * 32768.0f);
                n->level_s16[i] = (int16_t)(v > 32767L ? 32767L : v);
            }
            rc = dsd_input_level_metrics_from_cs16(n->level_s16, count, (double)DSD_INPUT_LEVEL_CS16_MAX_FULL_SCALE,
                                                   DSD_INPUT_LEVEL_SOURCE_NET_IQ, &snapshot);
            break;
        default: rc = dsd_input_level_metrics_from_cf32(iq, count, DSD_INPUT_LEVEL_SOURCE_NET_IQ, &snapshot); break;
    }
    if (rc == 0) {
        rtl_stream_input_level_publish(&snapshot);
    }
}

/* Publish the level of what the reader received, each snapshot over at least one block's worth of samples: a read of
 * a few samples (a TCP fragment, a short datagram) waits for the reads after it, so a handful near the rail cannot
 * read as clipping. A read of a block or more is measured as it came. */
static void
net_iq_publish_level(struct rtl_net_iq* n, const float* iq, size_t pairs) {
    const size_t span = n->chunk_pairs;
    while (pairs > 0U) {
        if (n->level_fill == 0U && pairs >= span) {
            net_iq_measure_level(n, iq, pairs);
            return;
        }
        const size_t take = pairs < span - n->level_fill ? pairs : span - n->level_fill;
        DSD_MEMCPY(n->level_pending + n->level_fill * 2U, iq, take * 2U * sizeof(float));
        n->level_fill += take;
        iq += take * 2U;
        pairs -= take;
        if (n->level_fill == span) {
            net_iq_measure_level(n, n->level_pending, span);
            n->level_fill = 0U;
        }
    }
}

/* Write the full block to the input ring, or drop it whole when the ring cannot take it: 1 when it was dropped. */
static int
net_iq_write_block(struct rtl_net_iq* n) {
    struct input_ring_state* ring = n->ring;
    const size_t need = (size_t)n->chunk_pairs * 2U;
    const uint64_t discard_generation = input_ring_discard_generation(ring);
    float* p1 = nullptr;
    float* p2 = nullptr;
    size_t n1 = 0U;
    size_t n2 = 0U;
    if ((size_t)input_ring_reserve(ring, need, &p1, &n1, &p2, &n2) < need) {
        /* Drop-on-full: the samples queued stay, and the block after this one does not follow them. */
        ring->producer_drops.fetch_add((uint64_t)need);
        input_ring_note_gap(ring);
        n->blocks_dropped.fetch_add(1U, std::memory_order_relaxed);
        dsd_iq_net_reader_note_drop(n->reader);
        return 1;
    }
    DSD_MEMCPY(p1, n->chunk, n1 * sizeof(float));
    if (n2 > 0U) {
        DSD_MEMCPY(p2, n->chunk + n1, n2 * sizeof(float));
    }
    if (!input_ring_discard_generation_matches(ring, discard_generation)) {
        /* A purge came between the reservation and now: it discarded what this block follows. */
        ring->producer_drops.fetch_add((uint64_t)need);
        return 0;
    }
    input_ring_commit(ring, need);
    n->blocks_written.fetch_add(1U, std::memory_order_relaxed);
    dsd_iq_net_reader_note_written(n->reader);
    return 0;
}

/* Add @p pairs samples at the DSP input rate to the block, writing each block as it fills. Returns 1 when a block was
 * dropped behind a resampler, which then starts over, so that its next output is the first after the gap mark; the
 * rest of @p iq, made before the gap, is not written, and @p rest gets its length. The resampler's calls end where a
 * block does (net_iq_resample_slice()), so that rest is at most a few outputs of one input (net_iq_on_samples()). */
static int
net_iq_append(struct rtl_net_iq* n, const float* iq, size_t pairs, size_t* rest) {
    *rest = 0U;
    while (pairs > 0U) {
        const size_t room = (size_t)n->chunk_pairs - n->fill;
        const size_t take = pairs < room ? pairs : room;
        DSD_MEMCPY(n->chunk + n->fill * 2U, iq, take * 2U * sizeof(float));
        n->fill += take;
        iq += take * 2U;
        pairs -= take;
        if (n->fill == (size_t)n->chunk_pairs) {
            const int dropped = net_iq_write_block(n);
            n->fill = 0U;
            if (dropped && n->resampler) {
                *rest = pairs;
                dsd_iq_resampler_clear(n->resampler);
                n->drop_clears.fetch_add(1U, std::memory_order_relaxed);
                return 1;
            }
        }
    }
    return 0;
}

/* The inputs the resampler takes next: at most a slice, and no more than can fill the block being filled
   (floor(room M / L), at least one input), so a block the ring drops ends a resampler call: those inputs make at most
   ceil(k L / M) <= room outputs. Only a one-input call, on an upsampling reader with less room left than the input's
   L / M outputs, makes more (at most ceil(L / M) - 1 past the block). */
static size_t
net_iq_resample_slice(const struct rtl_net_iq* n, size_t pairs) {
    const uint64_t room = (uint64_t)n->chunk_pairs - (uint64_t)n->fill;
    uint64_t fit = room * (uint64_t)n->resampler->M / (uint64_t)n->resampler->L;
    if (fit < 1U) {
        fit = 1U;
    }
    size_t slice = pairs < kNetIqResampleSliceSamples ? pairs : kNetIqResampleSliceSamples;
    if ((uint64_t)slice > fit) {
        slice = (size_t)fit;
    }
    return slice;
}

static void
net_iq_on_samples(void* ctx, const float* iq, size_t pairs) {
    struct rtl_net_iq* n = static_cast<struct rtl_net_iq*>(ctx);
    if (!n || !iq || pairs == 0U) {
        return;
    }
    const size_t total = pairs;
    net_iq_publish_level(n, iq, pairs);
    if (!n->resampler) {
        size_t rest = 0U;
        (void)net_iq_append(n, iq, pairs, &rest);
        n->samples_ingested.fetch_add((uint64_t)total, std::memory_order_release);
        return;
    }
    const float* refed = nullptr;
    while (pairs > 0U) {
        const size_t slice = net_iq_resample_slice(n, pairs);
        const int made = dsd_iq_resampler_process(n->resampler, iq, slice, n->resampled, n->resampled_cap);
        size_t used = slice;
        size_t rest = 0U;
        if (made > 0 && net_iq_append(n, n->resampled, (size_t)made, &rest) && rest > 0U) {
            /* A dropped block ended inside the call's one input: the started-over reader takes that input again, so
               its outputs start the next block and the drop costs the block alone. Once: an input whose outputs alone
               fill a block and drop it again lets the rest go, counted as dropped. */
            const float* last = iq + (slice - 1U) * 2U;
            if (last != refed) {
                refed = last;
                used = slice - 1U;
            } else {
                n->ring->producer_drops.fetch_add((uint64_t)rest * 2U);
            }
        } else if (made < 0) {
            /* Cannot happen with slices within the design's max_in; counted, so a misconfiguration shows. */
            n->resampler_errors.fetch_add(1U, std::memory_order_relaxed);
        }
        iq += used * 2U;
        pairs -= used;
    }
    n->samples_ingested.fetch_add((uint64_t)total, std::memory_order_release);
}

/* The samples after this point do not follow the ones before it: the block being filled and the samples a level block
 * was collecting are let go, so the first level after the gap measures the new samples only, and the resampler starts
 * over at the gap, as the demod's filters do (from the first sample after it, so a carrier is no step from silence). */
static void
net_iq_restart_input(struct rtl_net_iq* n) {
    n->fill = 0U;
    n->level_fill = 0U;
    n->input_restarts.fetch_add(1U, std::memory_order_relaxed);
    if (n->resampler) {
        dsd_iq_resampler_clear(n->resampler);
        n->resampler_clears.fetch_add(1U, std::memory_order_relaxed);
    }
}

static void
net_iq_on_event(void* ctx, const dsd_iq_net_event* event) {
    struct rtl_net_iq* n = static_cast<struct rtl_net_iq*>(ctx);
    if (!n || !event) {
        return;
    }
    switch (event->kind) {
        case DSD_IQ_NET_EVENT_RECONNECT:
        case DSD_IQ_NET_EVENT_RESUME:
            /* Before the first sample after it: what was queued before the gap goes with the boundary's purge, and the
               decoder ends what it was assembling. */
            net_iq_restart_input(n);
            if (n->on_boundary) {
                n->on_boundary(event->kind == DSD_IQ_NET_EVENT_RECONNECT ? RTL_STREAM_GAP_REASON_NET_RECONNECT
                                                                         : RTL_STREAM_GAP_REASON_NET_RESUME,
                               n->boundary_ctx);
            }
            /* Counted once it ran, so a reader of the count sees everything the boundary did. */
            n->boundaries.fetch_add(1U, std::memory_order_release);
            break;
        case DSD_IQ_NET_EVENT_KNOWN_LOSS:
            /* The locked sender's samples were lost before the resampler: a gap in the ring, as a driver overflow
               leaves, with the queued samples kept. */
            net_iq_restart_input(n);
            input_ring_note_gap(n->ring);
            n->known_losses.fetch_add(1U, std::memory_order_relaxed);
            break;
        case DSD_IQ_NET_EVENT_DISCONNECTED:
            /* Nothing more follows the partial block; the reconnect's boundary comes before anything does. */
            n->fill = 0U;
            break;
        case DSD_IQ_NET_EVENT_CONNECTED:
        case DSD_IQ_NET_EVENT_STALL:
        default: break;
    }
}

static void
net_iq_free(struct rtl_net_iq* n) {
    if (!n) {
        return;
    }
    dsd_iq_net_reader_destroy(n->reader);
    if (n->resampler) {
        dsd_iq_resampler_free(n->resampler);
        free(n->resampler);
    }
    free(n->resampled);
    free(n->chunk);
    free(n->level_u8);
    free(n->level_s16);
    free(n->level_pending);
    delete n;
}

/* The buffers the reader thread fills: the block, the resampler's output for one slice, the level's integer view. */
static int
net_iq_alloc_buffers(struct rtl_net_iq* n, const struct rtl_net_iq_config* cfg) {
    n->chunk = static_cast<float*>(calloc((size_t)n->chunk_pairs * 2U, sizeof(float)));
    if (!n->chunk) {
        return -1;
    }
    if (cfg->spec.rate_hz != cfg->dsp_input_rate_hz) {
        if (rtl_iq_resampler_open("I/Q stream", "stream", cfg->spec.rate_hz, cfg->dsp_input_rate_hz,
                                  cfg->final_dsp_rate_hz, kNetIqResampleSliceSamples, &n->resampler)
            != 0) {
            return -1;
        }
        n->resampled_cap = dsd_iq_resampler_max_out(n->resampler, kNetIqResampleSliceSamples);
        n->resampled = static_cast<float*>(calloc(n->resampled_cap * 2U, sizeof(float)));
        if (!n->resampled) {
            return -1;
        }
    }
    n->level_pending = static_cast<float*>(calloc((size_t)n->chunk_pairs * 2U, sizeof(float)));
    if (!n->level_pending) {
        return -1;
    }
    const size_t level_count = (size_t)DSD_IQ_NET_MAX_CALLBACK_SAMPLES * 2U;
    if (cfg->spec.format == DSD_IQ_NET_FORMAT_CU8) {
        n->level_u8 = static_cast<uint8_t*>(calloc(level_count, sizeof(uint8_t)));
        return n->level_u8 ? 0 : -1;
    }
    if (cfg->spec.format == DSD_IQ_NET_FORMAT_CS8 || cfg->spec.format == DSD_IQ_NET_FORMAT_CS16) {
        n->level_s16 = static_cast<int16_t*>(calloc(level_count, sizeof(int16_t)));
        return n->level_s16 ? 0 : -1;
    }
    return 0;
}

struct rtl_net_iq*
rtl_net_iq_create(const struct rtl_net_iq_config* cfg, struct input_ring_state* ring) {
    if (!cfg || !ring || cfg->dsp_input_rate_hz == 0U) {
        return nullptr;
    }
    struct rtl_net_iq* n = new (std::nothrow) rtl_net_iq();
    if (!n) {
        return nullptr;
    }
    n->ring = ring;
    n->format = cfg->spec.format;
    n->dsp_input_rate_hz = cfg->dsp_input_rate_hz;
    n->chunk_pairs = dsd_iq_live_block_samples(cfg->dsp_input_rate_hz);
    n->on_boundary = cfg->on_boundary;
    n->boundary_ctx = cfg->boundary_ctx;
    if (net_iq_alloc_buffers(n, cfg) != 0) {
        LOG_ERROR("I/Q stream: cannot set up its input buffers.\n");
        net_iq_free(n);
        return nullptr;
    }
    dsd_iq_net_reader_config rcfg = {};
    rcfg.on_samples = net_iq_on_samples;
    rcfg.on_event = net_iq_on_event;
    rcfg.ctx = n;
    rcfg.publish_state =
        1; /* the session's stream: its state is the one frontends show (dsd_iq_net_published_state()) */
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    rcfg.clock_ctx = g_test_clock_ctx.load(std::memory_order_relaxed);
    rcfg.clock_ms = g_test_clock.load(std::memory_order_acquire);
#endif
    n->reader = dsd_iq_net_reader_create(&cfg->spec, &rcfg);
    if (!n->reader) {
        LOG_ERROR("I/Q stream: cannot set up the input.\n");
        net_iq_free(n);
        return nullptr;
    }
    dsd_iq_net_open_error err = {};
    if (dsd_iq_net_reader_open(n->reader, &err) != 0) {
        LOG_ERROR("%s.\n", err.message);
        dsd_input_failure_report((dsd_input_failure_kind)err.failure_kind, err.native_code);
        net_iq_free(n);
        return nullptr;
    }
    return n;
}

int
rtl_net_iq_start(struct rtl_net_iq* n) {
    return n ? dsd_iq_net_reader_start(n->reader) : -1;
}

void
rtl_net_iq_stop(struct rtl_net_iq* n) {
    if (n) {
        dsd_iq_net_reader_stop(n->reader);
    }
}

void
rtl_net_iq_destroy(struct rtl_net_iq* n) {
    net_iq_free(n);
}

int
rtl_net_iq_available(const struct rtl_net_iq* n) {
    return n ? dsd_iq_net_reader_available(n->reader) : 0;
}

const dsd_iq_net_spec*
rtl_net_iq_spec(const struct rtl_net_iq* n) {
    return n ? dsd_iq_net_reader_spec(n->reader) : nullptr;
}

uint32_t
rtl_net_iq_block_samples(const struct rtl_net_iq* n) {
    return n ? n->chunk_pairs : 0U;
}

int
rtl_net_iq_resample_info(const struct rtl_net_iq* n, int* out_l, int* out_m, double* out_ppm) {
    if (!n || !n->resampler) {
        return 0;
    }
    if (out_l) {
        *out_l = n->resampler->L;
    }
    if (out_m) {
        *out_m = n->resampler->M;
    }
    if (out_ppm) {
        *out_ppm = dsd_iq_resampler_ratio_error_ppm(n->resampler);
    }
    return 1;
}

void
rtl_net_iq_get_state(const struct rtl_net_iq* n, rtl_net_iq_state* out) {
    if (!out) {
        return;
    }
    *out = rtl_net_iq_state{};
    if (!n) {
        return;
    }
    out->samples_ingested = n->samples_ingested.load(std::memory_order_acquire);
    out->blocks_written = n->blocks_written.load(std::memory_order_relaxed);
    out->blocks_dropped = n->blocks_dropped.load(std::memory_order_relaxed);
    out->boundaries = n->boundaries.load(std::memory_order_acquire);
    out->known_losses = n->known_losses.load(std::memory_order_relaxed);
    out->input_restarts = n->input_restarts.load(std::memory_order_relaxed);
    out->resampler_clears = n->resampler_clears.load(std::memory_order_relaxed);
    out->drop_clears = n->drop_clears.load(std::memory_order_relaxed);
    out->resampler_errors = n->resampler_errors.load(std::memory_order_relaxed);
    out->local_port = dsd_iq_net_reader_local_port(n->reader);
    dsd_iq_net_reader_get_counters(n->reader, &out->reader);
}
