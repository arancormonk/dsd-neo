// SPDX-License-Identifier: GPL-3.0-or-later
/* A stand-in for a radio receiver's channel-tagged squelch gates (issue #899); see channel_gate_fake.h. */
#include "channel_gate_fake.h"

#include <dsd-neo/core/power.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <dsd-neo/runtime/squelch.h>
#include <stddef.h>
#include <stdint.h>

#define CHANNEL_GATE_FAKE_BINDINGS 8U

typedef struct {
    uint32_t channel;
    dsd_squelch_setting gate;
} channel_gate_fake_binding;

static channel_gate_fake_binding g_bindings[CHANNEL_GATE_FAKE_BINDINGS];
static uint32_t g_last_channel;
static uint32_t g_channel;
static uint32_t g_landed_binding;
static uint32_t g_opened;
static uint32_t g_epoch;
static uint32_t g_staged;
static dsd_squelch_setting g_gate;
static int g_closed;
static int g_applied_writes;
static int g_writes;
static channel_gate_fake_apply_fn g_apply;
static void (*g_before_write)(uint32_t channel);

static const dsd_squelch_setting k_closed = {DSD_SQUELCH_MODE_LEVEL, 3.0e38, DSD_SQUELCH_MARGIN_DEFAULT_DB};

static channel_gate_fake_binding*
fake_binding(uint32_t channel) {
    if (channel == 0U) {
        return NULL;
    }
    for (size_t i = 0; i < CHANNEL_GATE_FAKE_BINDINGS; i++) {
        if (g_bindings[i].channel == channel) {
            return &g_bindings[i];
        }
    }
    return NULL;
}

static uint32_t
fake_newest(void) {
    uint32_t newest = 0U;
    for (size_t i = 0; i < CHANNEL_GATE_FAKE_BINDINGS; i++) {
        if (g_bindings[i].channel > newest) {
            newest = g_bindings[i].channel;
        }
    }
    return newest;
}

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

/* The receiver is on @p channel now, under @p gate. */
static void
fake_run(uint32_t channel, const dsd_squelch_setting* gate, int closed) {
    g_channel = channel;
    g_epoch++;
    fake_apply(gate, closed);
}

void
channel_gate_fake_open(const dsd_squelch_setting* gate, channel_gate_fake_apply_fn apply) {
    DSD_MEMSET(g_bindings, 0, sizeof g_bindings);
    g_apply = apply;
    g_staged = 0U;
    g_applied_writes = 0;
    g_writes = 0;
    g_opened = ++g_last_channel;
    fake_run(g_opened, gate, 0);
}

uint32_t
channel_gate_fake_bind(const dsd_squelch_setting* gate) {
    size_t slot = 0;
    for (size_t i = 0; i < CHANNEL_GATE_FAKE_BINDINGS; i++) {
        if (g_bindings[i].channel == 0U) {
            slot = i;
            break;
        }
        if (g_bindings[i].channel < g_bindings[slot].channel) {
            slot = i;
        }
    }
    g_bindings[slot].channel = ++g_last_channel;
    g_bindings[slot].gate = *gate;
    return g_bindings[slot].channel;
}

void
channel_gate_fake_release(uint32_t channel) {
    channel_gate_fake_binding* binding = fake_binding(channel);
    if (binding) {
        DSD_MEMSET(binding, 0, sizeof *binding);
        g_epoch++;
    }
}

void
channel_gate_fake_land(uint32_t channel) {
    channel_gate_fake_binding* binding = fake_binding(channel);
    const int own = binding && channel == fake_newest() && channel > g_landed_binding;
    const dsd_squelch_setting gate = own ? binding->gate : k_closed;
    channel_gate_fake_release(channel);
    if (channel > g_landed_binding) {
        g_landed_binding = channel;
    }
    fake_run(channel, &gate, !own);
}

int
channel_gate_fake_bound(uint32_t channel, dsd_squelch_setting* out) {
    const channel_gate_fake_binding* binding = fake_binding(channel);
    if (!binding) {
        return 0;
    }
    if (out) {
        *out = binding->gate;
    }
    return 1;
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

static int
fake_write(uint32_t channel, const dsd_squelch_setting* gate) {
    g_writes++;
    if (g_before_write) {
        g_before_write(channel);
    }
    if (channel != 0U && channel == g_channel) {
        g_applied_writes++;
        fake_apply(gate, 0);
        return DSD_CHANNEL_GATE_APPLIED;
    }
    channel_gate_fake_binding* binding = fake_binding(channel);
    if (!binding) {
        return DSD_CHANNEL_GATE_STALE;
    }
    binding->gate = *gate;
    return DSD_CHANNEL_GATE_BOUND;
}

static uint32_t
fake_epoch(void) {
    return g_epoch;
}

static uint32_t
fake_opened(void) {
    return g_opened;
}

static uint32_t
fake_stage(const dsd_squelch_setting* gate) {
    channel_gate_fake_release(g_staged);
    g_staged = channel_gate_fake_bind(gate);
    return g_staged;
}

static int
fake_unstage(uint32_t channel) {
    if (channel != 0U && channel == g_staged) {
        g_staged = 0U;
        channel_gate_fake_release(channel);
        return 0;
    }
    return channel != 0U ? 1 : 0;
}

void
channel_gate_fake_install(dsd_rtl_stream_metrics_hooks* hooks) {
    hooks->write_channel_gate = fake_write;
    hooks->channel_gate_epoch = fake_epoch;
    hooks->channel_gate_opened = fake_opened;
    hooks->stage_family_gate = fake_stage;
    hooks->unstage_family_gate = fake_unstage;
}
