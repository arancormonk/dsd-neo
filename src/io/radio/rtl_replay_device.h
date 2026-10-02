// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_IO_RADIO_RTL_REPLAY_DEVICE_H
#define DSD_NEO_IO_RADIO_RTL_REPLAY_DEVICE_H

#include <atomic>
#include <stdint.h>

#include <dsd-neo/core/input_level.h>
#include <dsd-neo/io/iq_replay.h>
#include <dsd-neo/platform/threading.h>

#include "dsd-neo/io/iq_types.h"

/* What the replay reader knows of one capture chunk, the unit the demod takes as one block (issue #572). The reader
 * writes it into rtl_replay_eof_state::chunk before it commits the chunk, while the input ring is empty, and the demod
 * copies it as it reserves the chunk, before it releases any of it; the reader writes the next chunk's only once the ring
 * is empty again, so it always describes the chunk in the ring. */
struct rtl_replay_chunk_meta {
    uint64_t sequence;       /* the chunk's place in the replay, from 1; a loop does not restart it */
    uint64_t submit_gen;     /* the replay_last_submit_gen value the chunk was committed under */
    uint64_t media_start_ns; /* capture time of its first sample: the samples before it, time a MUTE omitted included */
    uint64_t media_end_ns;   /* capture time just past its last sample */
    int have_input_level;
    dsd_input_level_snapshot input_level; /* its raw input level, published when the demod starts its block */
};

typedef void (*rtl_replay_input_drained_cb)(void* user);
typedef void (*rtl_replay_wake_cb)(void* user);
typedef int (*rtl_replay_wait_event_boundary_cb)(void* user);
typedef void (*rtl_replay_event_cb)(const dsd_iq_event* event, void* user);
typedef void (*rtl_replay_loop_restart_cb)(const dsd_iq_replay_config* cfg, void* user);

/* The stream's replay end-of-stream state the replay reader drives. The EOF drain decision (input drained, demod
 * drained) is made under eof_m on every side, so neither the reader nor the demod can miss the other's last store. */
struct rtl_replay_eof_state {
    std::atomic<int>* stream_exit_flag;
    std::atomic<int>* replay_input_eof;
    std::atomic<int>* replay_input_drained;
    std::atomic<int>* replay_demod_drained;
    std::atomic<int>* replay_output_drained;
    std::atomic<int>* replay_forced_stop;
    /* Set once the reader thread has left, for any reason: a decoder waiting for a reader that left without marking
       EOF treats it as EOF. */
    std::atomic<int>* replay_reader_exited;
    std::atomic<uint64_t>* replay_last_submit_gen;
    std::atomic<uint64_t>* replay_last_submit_gen_at_eof;
    std::atomic<uint64_t>* replay_last_consume_gen;
    struct rtl_replay_chunk_meta* chunk; /* the chunk in the input ring (see struct rtl_replay_chunk_meta) */
    dsd_mutex_t* eof_m;
    dsd_cond_t* eof_cond;
    rtl_replay_input_drained_cb on_input_drained;
    /* Wakes every thread waiting on a replay condition (input and output rings, eof_cond): EOF, failure, stops. */
    rtl_replay_wake_cb wake_all;
    /* Waits, with no deadline, for the pipeline to go idle before an event or a loop rewind is applied (issue #572):
       the input ring empty, every chunk submitted acknowledged, the output ring empty, and every batch published
       acknowledged by the decoder. Returns 1 then, 0 on a stop, a forced stop or the global exit. */
    rtl_replay_wait_event_boundary_cb wait_event_boundary;
    rtl_replay_event_cb on_retune_event;
    rtl_replay_event_cb on_mute_event;
    rtl_replay_event_cb on_reset_event;
    rtl_replay_loop_restart_cb on_loop_restart;
    void* eof_user;
    void* event_user;
};

struct rtl_device* rtl_device_create_iq_replay(const dsd_iq_replay_config* cfg, struct input_ring_state* input_ring,
                                               const struct rtl_replay_eof_state* eof_state);

#endif /* DSD_NEO_IO_RADIO_RTL_REPLAY_DEVICE_H */
