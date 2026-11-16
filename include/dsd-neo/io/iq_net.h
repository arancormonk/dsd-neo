// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Raw I/Q over the network (`iqtcp:` / `iqudp:`): the input spec, the wire formats and the socket reader.
 *
 * Another program sends headerless interleaved little-endian I/Q (I first, then Q). dsd-neo connects to it over TCP
 * (`iqtcp:HOST:PORT:RATE:FORMAT[:FREQ]`) or binds and receives its UDP datagrams (`iqudp:BIND:PORT:RATE:FORMAT[:FREQ]`).
 * This module parses the spec, converts each wire format to the decoder's interleaved floats, and runs the reader
 * that owns the socket: it hands converted samples and stream events to a callback and keeps the availability state
 * and counters the stream backend reads. It knows nothing of the input ring, the resampler or the decoder.
 *
 * Radio builds only. The spec and the wire-format names (<dsd-neo/io/iq_net_spec.h>) are in every build.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_IO_IQ_NET_H_
#define DSD_NEO_INCLUDE_DSD_NEO_IO_IQ_NET_H_

#ifdef USE_RADIO

#include <dsd-neo/io/iq_net_spec.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Wire formats ---------------------------------------------------------------------------------------------- */

/**
 * @brief The Signal search clip level (`dsd_signal_capture.clip_level`) of @p format's converted samples.
 *
 * Between the converter's rails and the codes next to them: cs8 127/128 (the positive rail; -1.0 is the other), cu8
 * 0.996 (rails at +-1.0, next codes at +-0.992), and 0.999 (DSD_SIGNAL_CLIP_LEVEL_DEFAULT) for cs16, cs32 and cf32.
 */
float dsd_iq_net_format_clip_level(dsd_iq_net_format format);

/**
 * @brief Convert @p samples complex samples of @p format at @p src to `2 * samples` interleaved floats at @p dst.
 *
 * Integer formats are assembled from their little-endian bytes, so the result does not depend on the host. cf32
 * enters bounded: a component that is Inf, NaN or at least 2^60 in magnitude becomes +0.0f, every other one passes
 * bit for bit, values above full scale included (bound_cf32_to_f32(), the rule rtl_cf32_copy_bounded() applies to
 * SoapySDR and cf32 replay samples). @p src may sit at any alignment; @p dst must not overlap it.
 *
 * @return @p samples, or 0 when @p format is not a wire format or a pointer is NULL.
 */
size_t dsd_iq_net_convert(dsd_iq_net_format format, const uint8_t* src, size_t samples, float* dst);

/* ---- Reader ---------------------------------------------------------------------------------------------------- */

/** Stall threshold: no valid sample for this long (real time) is a stall. */
#define DSD_IQ_NET_STALL_MS             2000U
/** Longest a TCP connect may take before it counts as failed. */
#define DSD_IQ_NET_CONNECT_TIMEOUT_MS   10000U
/** Longest the reader waits in one socket wait, so a stop, an exit and stall detection are seen this often. */
#define DSD_IQ_NET_WAIT_SLICE_MS        200U
/** Reconnect backoff: this many ms times the failed attempt's number, at most DSD_IQ_NET_BACKOFF_MAX_MS. */
#define DSD_IQ_NET_BACKOFF_STEP_MS      200U
#define DSD_IQ_NET_BACKOFF_MAX_MS       2000U
/** Receive buffer: a whole datagram of any size, and the most one TCP read takes. */
#define DSD_IQ_NET_RX_BYTES             65536U
/** SO_RCVBUF the reader asks for (best effort: the system may cap it). */
#define DSD_IQ_NET_SO_RCVBUF_BYTES      (4 * 1024 * 1024)

/** Most complex samples one on_samples() call carries. */
#define DSD_IQ_NET_MAX_CALLBACK_SAMPLES (DSD_IQ_NET_RX_BYTES / 2U)

typedef enum {
    /** The first connection is up (TCP), or the first UDP sender is locked. No samples came before it. */
    DSD_IQ_NET_EVENT_CONNECTED = 1,
    /** The TCP connection was lost; the reader reconnects. Availability is already false. */
    DSD_IQ_NET_EVENT_DISCONNECTED = 2,
    /** A new TCP connection after a loss, or a different UDP sender locked after a stall (`net_reconnect`).
     *  Delivered before the first sample from it: run the stream boundary here. Partial samples were discarded. */
    DSD_IQ_NET_EVENT_RECONNECT = 3,
    /** No valid sample for DSD_IQ_NET_STALL_MS on a connection that stays up. Availability is already false. A UDP
     *  sender's lock is released: the next valid sender is locked again. */
    DSD_IQ_NET_EVENT_STALL = 4,
    /** Samples again from the same connection or UDP sender after a stall (`net_resume`). Delivered before the first
     *  resumed sample: run the stream boundary here. A TCP partial sample carried across the stall is kept. It also
     *  comes when a TCP connection stalled before it delivered any sample (CONNECTED or RECONNECT, STALL, RESUME):
     *  then nothing came before it on that connection, so it marks the start of the data, not lost samples. */
    DSD_IQ_NET_EVENT_RESUME = 5,
    /** The locked UDP sender sent a datagram that does not hold whole samples, so it was dropped: samples were lost
     *  before any resampler. Discard any partial output chunk, clear resampler history and mark a gap before the next
     *  samples. */
    DSD_IQ_NET_EVENT_KNOWN_LOSS = 6,
} dsd_iq_net_event_kind;

/** One reader event. Every pointer is borrowed for the duration of the callback. */
typedef struct {
    dsd_iq_net_event_kind kind;
    const char* peer; /**< "HOST:PORT" of the TCP server as the spec names it, or "A.B.C.D:PORT" of the UDP sender */
    int error_code;   /**< DISCONNECTED: the native socket error, 0 when the sender closed the connection */
    size_t bytes;     /**< KNOWN_LOSS: the dropped datagram's length */
} dsd_iq_net_event;

/** Converted samples: `2 * pairs` floats, I then Q, at most DSD_IQ_NET_MAX_CALLBACK_SAMPLES pairs. */
typedef void (*dsd_iq_net_samples_fn)(void* ctx, const float* iq, size_t pairs);
typedef void (*dsd_iq_net_event_fn)(void* ctx, const dsd_iq_net_event* event);
/** A monotonic real-time clock in milliseconds. */
typedef uint64_t (*dsd_iq_net_clock_fn)(void* ctx);

typedef struct {
    dsd_iq_net_samples_fn on_samples; /**< may be NULL */
    dsd_iq_net_event_fn on_event;     /**< may be NULL */
    void* ctx;                        /**< passed to both */
    /** The clock stall detection and rate telemetry read. NULL in production: dsd_realtime_mono_ms(). Socket waits,
     *  connect timeouts and backoff always run on real time. */
    dsd_iq_net_clock_fn clock_ms;
    void* clock_ctx;
} dsd_iq_net_reader_config;

/**
 * Availability states. Only DSD_IQ_NET_STATE_STREAMING is available (dsd_iq_net_reader_available()): the stream has a
 * live source and a frame read may wait for its samples.
 */
typedef enum {
    DSD_IQ_NET_STATE_STOPPED = 0,    /**< not opened, open failed, or the reader ended */
    DSD_IQ_NET_STATE_CONNECTING = 1, /**< connecting or reconnecting (TCP) / binding (UDP) */
    DSD_IQ_NET_STATE_WAITING = 2,    /**< up, but nothing written since it came up or since the last boundary */
    DSD_IQ_NET_STATE_STREAMING = 3,  /**< the consumer wrote samples since then (dsd_iq_net_reader_note_written()) */
    DSD_IQ_NET_STATE_STALLED = 4,    /**< a stall is declared and no sample has come since */
} dsd_iq_net_state;

/** Running totals; each counter only grows for the reader's life. */
typedef struct {
    uint64_t bytes;                 /**< payload bytes received, dropped ones included */
    uint64_t samples;               /**< complex samples handed to on_samples() */
    uint64_t datagrams;             /**< UDP datagrams with a payload, from any sender */
    uint64_t empty_datagrams;       /**< UDP zero-length datagrams (ignored) */
    uint64_t malformed_datagrams;   /**< UDP datagrams not a whole number of samples (dropped whole) */
    uint64_t foreign_datagrams;     /**< UDP datagrams from a sender other than the locked one (dropped) */
    uint64_t known_losses;          /**< KNOWN_LOSS events */
    uint64_t connect_attempts;      /**< TCP connects tried, the first included */
    uint64_t reconnects;            /**< RECONNECT events */
    uint64_t stalls;                /**< STALL events */
    uint64_t resumes;               /**< RESUME events */
    uint64_t carry_discarded_bytes; /**< TCP partial-sample bytes discarded at a reconnect */
    uint64_t rate_warnings;         /**< rate telemetry warnings given */
} dsd_iq_net_counters;

/** Why dsd_iq_net_reader_open() failed. */
typedef struct {
    int failure_kind; /**< a dsd_input_failure_kind (<dsd-neo/runtime/input_failure.h>) */
    int native_code;  /**< the native socket error, 0 when there is none */
    char message[DSD_IQ_NET_HOST_MAX + 96];
} dsd_iq_net_open_error;

typedef struct dsd_iq_net_reader dsd_iq_net_reader;

/**
 * @brief Create a reader for @p spec (copied). Allocates every buffer the reader uses; nothing is allocated later.
 * @return The reader, or NULL when @p spec is not usable (bad kind or format, port outside 0..65535, TCP port 0) or
 *         memory runs out.
 */
dsd_iq_net_reader* dsd_iq_net_reader_create(const dsd_iq_net_spec* spec, const dsd_iq_net_reader_config* config);

/**
 * @brief Open the input on the calling thread: TCP resolves the host and connects (within
 *        DSD_IQ_NET_CONNECT_TIMEOUT_MS); UDP binds (SO_EXCLUSIVEADDRUSE on Windows).
 *
 * Both ask for a DSD_IQ_NET_SO_RCVBUF_BYTES receive buffer; TCP also turns keepalive on. The UDP port is the
 * input's alone: no SO_REUSEADDR, so a port another socket holds is refused, and it is free again once this reader
 * stopped. A stop or the process exit
 * flag cancels a connect within about 100 ms. Call once, before dsd_iq_net_reader_start() / _run().
 *
 * @param err Receives why it failed (may be NULL); nothing is logged.
 * @return 0 when open (state WAITING), -1 when not (state STOPPED).
 */
int dsd_iq_net_reader_open(dsd_iq_net_reader* reader, dsd_iq_net_open_error* err);

/**
 * @brief Start the reader on a thread of its own, which applies the session's floating-point mode
 *        (dsd_runtime_fp_mode_apply_thread("iq_net")) and then runs dsd_iq_net_reader_run().
 * @return 0 when the thread started, -1 when the reader is not open, its loop already started (here or through
 *         dsd_iq_net_reader_run()) or the thread failed.
 */
int dsd_iq_net_reader_start(dsd_iq_net_reader* reader);

/**
 * @brief The reader loop, for a caller that runs it on a thread it made (and applied the floating-point mode on).
 *        Runs once per open reader; a second call, or a call after dsd_iq_net_reader_start(), returns at once.
 *
 * Returns after dsd_iq_net_reader_stop() or once the process exit flag is set, with the socket closed and the state
 * STOPPED. Callbacks run on this thread with no reader lock held, in order: a RECONNECT or RESUME event comes before
 * the first sample after it. TCP: reads in DSD_IQ_NET_WAIT_SLICE_MS waits, carries a partial sample to the next read
 * (kept across a stall, discarded at a reconnect), and on a loss reconnects forever, resolving the host again at each
 * attempt: the first at once, then after DSD_IQ_NET_BACKOFF_STEP_MS times the attempt's number, capped at
 * DSD_IQ_NET_BACKOFF_MAX_MS (a connection that ends before it delivers a sample does not reset the count). UDP:
 * locks the first sender whose datagram holds whole samples; drops others, zero-length datagrams (no activity) and
 * datagrams that are not whole samples, with one log line per run for each kind.
 */
void dsd_iq_net_reader_run(dsd_iq_net_reader* reader);

/**
 * @brief Stop the reader: latch the stop, shut down the socket the reader has published (never one it closed), and
 *        join the reader's own thread when dsd_iq_net_reader_start() made one. A caller that runs the loop on its own
 *        thread joins that thread itself, before dsd_iq_net_reader_destroy(). Any thread; idempotent; returns within
 *        about one wait slice. A stop also cancels a dsd_iq_net_reader_open() in progress on another thread.
 */
void dsd_iq_net_reader_stop(dsd_iq_net_reader* reader);

/** Stop (dsd_iq_net_reader_stop()), close the socket and free @p reader. NULL is a no-op. */
void dsd_iq_net_reader_destroy(dsd_iq_net_reader* reader);

/** The reader's dsd_iq_net_state, from any thread. */
dsd_iq_net_state dsd_iq_net_reader_state(const dsd_iq_net_reader* reader);

/** 1 when the state is DSD_IQ_NET_STATE_STREAMING, else 0 (NULL included). Any thread; one load. */
int dsd_iq_net_reader_available(const dsd_iq_net_reader* reader);

/**
 * @brief The consumer wrote samples to its output (for the stream: a whole chunk reached the ring). Moves WAITING to
 *        STREAMING; any other state is left alone. Cheap enough to call after every write.
 */
void dsd_iq_net_reader_note_written(dsd_iq_net_reader* reader);

/** The consumer dropped samples after conversion (ring full): the rate telemetry window in progress is discarded. */
void dsd_iq_net_reader_note_drop(dsd_iq_net_reader* reader);

/** The local port of the open socket (a UDP reader opened on port 0 learns its port here), or -1. */
int dsd_iq_net_reader_local_port(const dsd_iq_net_reader* reader);

/** The spec the reader was created with. */
const dsd_iq_net_spec* dsd_iq_net_reader_spec(const dsd_iq_net_reader* reader);

/** Copy the counters (each read on its own, so totals taken while the reader runs need not agree). */
void dsd_iq_net_reader_get_counters(const dsd_iq_net_reader* reader, dsd_iq_net_counters* out);

#ifdef __cplusplus
}
#endif

#endif /* USE_RADIO */

#endif /* DSD_NEO_INCLUDE_DSD_NEO_IO_IQ_NET_H_ */
