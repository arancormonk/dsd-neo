// SPDX-License-Identifier: GPL-3.0-or-later
/* A stand-in for a radio receiver's squelch gate (issue #899); see channel_gate_fake.h. */
#include "channel_gate_fake.h"

#include <dsd-neo/core/power.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <dsd-neo/runtime/squelch.h>

static dsd_squelch_setting g_session;
static dsd_squelch_setting g_gate;
static int g_closed;
static int g_analog;
static int g_pending;
static int g_writes;
static int g_reads;
static channel_gate_fake_apply_fn g_apply;
static void (*g_before_write)(void);

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

void
channel_gate_fake_open(int analog, const dsd_squelch_setting* gate, channel_gate_fake_apply_fn apply) {
    g_apply = apply;
    g_pending = 0;
    g_writes = 0;
    g_analog = analog ? 1 : 0;
    g_session = *gate;
    /* The stream opens on it, as the open's own setting, not a write the apply callback hears. */
    g_gate = analog ? k_closed : *gate;
    g_closed = g_analog;
}

void
channel_gate_fake_start(int analog, int resolved, const dsd_squelch_setting* gate, const dsd_squelch_setting* digital,
                        const dsd_squelch_setting* session, channel_gate_fake_apply_fn apply) {
    channel_gate_fake_open(analog, session, apply);
    if (resolved) {
        g_gate = analog ? *gate : *digital;
        g_closed = 0;
    }
}

void
channel_gate_fake_land(int new_channel, int analog) {
    const int was_analog = g_analog;
    g_analog = analog ? 1 : 0;
    if (analog) {
        if (new_channel) {
            fake_apply(&k_closed, 1);
        }
    } else if (new_channel || was_analog) {
        fake_apply(&g_session, 0);
    }
}

dsd_squelch_setting
channel_gate_fake_gate(void) {
    return g_gate;
}

int
channel_gate_fake_closed(void) {
    return g_closed;
}

int
channel_gate_fake_analog(void) {
    return g_analog;
}

void
channel_gate_fake_requests_pending(int pending) {
    g_pending = pending ? 1 : 0;
}

void
channel_gate_fake_before_write(void (*fn)(void)) {
    g_before_write = fn;
}

int
channel_gate_fake_writes(void) {
    return g_writes;
}

int
channel_gate_fake_reads(void) {
    return g_reads;
}

static void
fake_write(const dsd_squelch_setting* gate, const dsd_squelch_setting* digital, const dsd_squelch_setting* session) {
    g_writes++;
    if (g_before_write) {
        g_before_write();
    }
    const dsd_squelch_setting* off_monitor = digital ? digital : gate;
    g_session = session ? *session : *off_monitor;
    fake_apply(g_analog ? gate : off_monitor, 0);
}

static int
fake_closed_start(void) {
    g_reads++;
    return g_closed;
}

static int
fake_pending(void) {
    g_reads++;
    return g_pending;
}

void
channel_gate_fake_install(dsd_rtl_stream_metrics_hooks* hooks) {
    hooks->write_squelch_gate = fake_write;
    hooks->gate_closed_start = fake_closed_start;
    hooks->receive_requests_pending = fake_pending;
}
