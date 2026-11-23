// SPDX-License-Identifier: GPL-3.0-or-later
/* A stand-in for a radio receiver's channels (issue #899); see channel_gate_fake.h. */
#include "channel_gate_fake.h"

#include <dsd-neo/core/power.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <dsd-neo/runtime/squelch.h>
#include <stdint.h>

static uint32_t g_next;
static uint32_t g_channel;
static uint32_t g_opened;
static uint32_t g_staged;
static uint32_t g_refused;
static dsd_squelch_setting g_written;
static dsd_squelch_setting g_gate;
static int g_closed;
static int g_applied_writes;
static int g_writes;
static int g_reads;
static channel_gate_fake_apply_fn g_apply;
static void (*g_before_write)(uint32_t channel);

static const dsd_squelch_setting k_closed = {DSD_SQUELCH_MODE_LEVEL, DSD_SQUELCH_LEVEL_CLOSED,
                                             DSD_SQUELCH_MARGIN_DEFAULT_DB};

/* The receiver runs @p gate from its next block. */
static void
fake_apply(const dsd_squelch_setting* gate, int closed) {
    if (closed == g_closed && dsd_squelch_setting_equal(&g_gate, gate)) {
        return;
    }
    g_gate = *gate;
    g_closed = closed;
    if (g_apply) {
        g_apply(gate);
    }
}

static uint32_t
fake_new_channel(void) {
    return ++g_next;
}

uint32_t
channel_gate_fake_open(int analog, const dsd_squelch_setting* gate, channel_gate_fake_apply_fn apply) {
    g_apply = apply;
    g_staged = 0U;
    g_refused = 0U;
    g_applied_writes = 0;
    g_writes = 0;
    g_opened = fake_new_channel();
    g_channel = g_opened;
    g_written = *gate;
    /* The stream opens on it, as the open's own setting, not a write the apply callback hears. */
    g_gate = analog ? k_closed : *gate;
    g_closed = analog ? 1 : 0;
    return g_opened;
}

void
channel_gate_fake_land(uint32_t channel, int analog) {
    if (channel == 0U || channel == g_channel) {
        return;
    }
    g_channel = channel;
    if (analog) {
        fake_apply(&k_closed, 1);
    } else if (g_closed) {
        fake_apply(&g_written, 0);
    }
}

uint32_t
channel_gate_fake_channel(void) {
    return g_channel;
}

dsd_squelch_setting
channel_gate_fake_gate(void) {
    return g_gate;
}

int
channel_gate_fake_closed(void) {
    return g_closed;
}

uint32_t
channel_gate_fake_take_staged(void) {
    const uint32_t channel = g_staged;
    g_staged = 0U;
    return channel;
}

void
channel_gate_fake_refuse(uint32_t channel) {
    g_refused = channel;
}

void
channel_gate_fake_before_write(void (*fn)(uint32_t channel)) {
    g_before_write = fn;
}

int
channel_gate_fake_applied_writes(void) {
    return g_applied_writes;
}

int
channel_gate_fake_writes(void) {
    return g_writes;
}

int
channel_gate_fake_reads(void) {
    return g_reads;
}

static int
fake_write(uint32_t channel, const dsd_squelch_setting* gate) {
    g_writes++;
    if (g_before_write) {
        g_before_write(channel);
    }
    if (channel == 0U || channel != g_channel) {
        return DSD_CHANNEL_GATE_STALE;
    }
    g_applied_writes++;
    g_written = *gate;
    fake_apply(gate, 0);
    return DSD_CHANNEL_GATE_APPLIED;
}

static uint32_t
fake_current(void) {
    g_reads++;
    return g_channel;
}

static uint32_t
fake_opened(void) {
    return g_opened;
}

static uint32_t
fake_refused(void) {
    return g_refused;
}

static void
fake_stage(uint32_t channel) {
    g_staged = channel;
}

static int
fake_unstage(uint32_t channel) {
    if (channel != 0U && channel == g_staged) {
        g_staged = 0U;
        return 0;
    }
    return channel != 0U ? 1 : 0;
}

void
channel_gate_fake_install(dsd_rtl_stream_metrics_hooks* hooks) {
    hooks->new_channel = fake_new_channel;
    hooks->current_channel = fake_current;
    hooks->channel_gate_opened = fake_opened;
    hooks->refused_channel = fake_refused;
    hooks->write_channel_gate = fake_write;
    hooks->stage_family_channel = fake_stage;
    hooks->unstage_family_channel = fake_unstage;
}
