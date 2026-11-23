// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * A stand-in for a radio receiver's channel-tagged squelch gates (issue #899), for tests that drive the decoder's side
 * without a running stream. It keeps the receiver's rules (rtl_stream_write_channel_gate() and the calls around it in
 * rtl_sdr_fm.cpp, which IO_RTL_RETUNE_PREPARE checks on the real receiver): the receiver numbers the channel it
 * produces; a write for that channel applies at once, a write for a channel still to land (a binding) is kept for its
 * landing, any other is stale and dropped; each binding keeps its own gate; a landing runs its binding's gate unless a
 * newer binding is outstanding or has landed, else closed (a level no power reaches); an open starts a channel of its
 * own; the epoch moves with every change of the channel or of the bindings outstanding; a write of the gate the
 * receiver runs already changes nothing.
 *
 * channel_gate_fake_install() fills a hook table's channel-gate fields, so the engine reaches it as it reaches the
 * stream. Every gate the receiver comes to run goes to the apply callback, as the stream's setters would take it.
 */
#ifndef DSD_NEO_TEST_CHANNEL_GATE_FAKE_H
#define DSD_NEO_TEST_CHANNEL_GATE_FAKE_H

#include <dsd-neo/core/power.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*channel_gate_fake_apply_fn)(const dsd_squelch_setting* gate);

/* A stream opens on a channel of its own under @p gate, every binding dropped; @p apply (may be NULL) takes each gate
   the receiver runs from here on. */
void channel_gate_fake_open(const dsd_squelch_setting* gate, channel_gate_fake_apply_fn apply);
/* A retune or family request that carries @p gate: the channel it lands. */
uint32_t channel_gate_fake_bind(const dsd_squelch_setting* gate);
/* @p channel's retune or family request landed: the receiver is on it, under its gate or closed. */
void channel_gate_fake_land(uint32_t channel);
/* @p channel will not land (dropped, refused, failed). */
void channel_gate_fake_release(uint32_t channel);
/* The gate @p channel would land with, while it is outstanding: 1, else 0. */
int channel_gate_fake_bound(uint32_t channel, dsd_squelch_setting* out);
/* The channel the receiver is on, and the gate it runs. */
uint32_t channel_gate_fake_channel(void);
dsd_squelch_setting channel_gate_fake_gate(void);
/* 1 while the receiver runs the closed gate a replaced landing runs. */
int channel_gate_fake_closed(void);
/* What a live family request does with a gate the decoder staged for it (rtl_stream_stage_family_gate()): takes it, for
   the caller to land where the request applies. 0: none staged. */
uint32_t channel_gate_fake_take_staged(void);
/* Called at the start of every write, before the receiver judges it: a case lands a retune there to put a landing
   between the decoder's resolve and its write. NULL: none. */
void channel_gate_fake_before_write(void (*fn)(uint32_t channel));
/* The writes the receiver applied (RTL_STREAM_GATE_APPLIED) since the last open. */
int channel_gate_fake_applied_writes(void);
/* The writes made at all since the last open, whatever the receiver did with them. */
int channel_gate_fake_writes(void);
/* Fill @p hooks' channel-gate fields (write_channel_gate, channel_gate_epoch, channel_gate_opened, stage_family_gate,
   unstage_family_gate) with this stand-in. */
void channel_gate_fake_install(dsd_rtl_stream_metrics_hooks* hooks);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_TEST_CHANNEL_GATE_FAKE_H */
