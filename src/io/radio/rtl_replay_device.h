// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_IO_RADIO_RTL_REPLAY_DEVICE_H
#define DSD_NEO_IO_RADIO_RTL_REPLAY_DEVICE_H

#include <atomic>
#include <stdint.h>

#include <dsd-neo/io/iq_replay.h>
#include <dsd-neo/platform/threading.h>

#include "dsd-neo/io/iq_types.h"

typedef void (*rtl_replay_input_drained_cb)(void* user);
typedef void (*rtl_replay_wake_cb)(void* user);
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
    dsd_mutex_t* eof_m;
    dsd_cond_t* eof_cond;
    rtl_replay_input_drained_cb on_input_drained;
    /* Wakes every thread waiting on a replay condition (input and output rings, eof_cond): EOF, failure, stops. */
    rtl_replay_wake_cb wake_all;
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
