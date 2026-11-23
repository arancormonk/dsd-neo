// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * A stand-in for a radio receiver's channels (issue #899), for tests that drive the decoder's side without a running
 * stream. It keeps the receiver's two rules (rtl_stream_write_channel_gate() and rtl_stream_gate_enter() in
 * rtl_sdr_fm.cpp, which IO_RTL_RETUNE_PREPARE checks on the real receiver): a new channel on the analog family starts
 * closed (a level no power reaches), one on the digital family runs the gate last written; and a write applies only
 * when it is tagged with the channel the receiver is on, else it is dropped.
 *
 * channel_gate_fake_install() fills a hook table's channel fields, so the engine reaches it as it reaches the stream.
 * Every gate the receiver comes to run goes to the apply callback, as the stream's setter would take it.
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

/* A stream opens on a channel of its own, closed when @p analog, else under @p gate (the open's own setting, which
   @p apply does not hear); @p apply (may be NULL) takes each gate the receiver comes to run from here on. Returns the
   channel. */
uint32_t channel_gate_fake_open(int analog, const dsd_squelch_setting* gate, channel_gate_fake_apply_fn apply);
/* A retune or family request naming @p channel landed, the receiver on the family @p analog says (0: none named, or the
   channel it is on: nothing moves). */
void channel_gate_fake_land(uint32_t channel, int analog);
/* The channel the receiver is on, and the gate it runs. */
uint32_t channel_gate_fake_channel(void);
dsd_squelch_setting channel_gate_fake_gate(void);
/* 1 while the receiver runs a new analog channel closed. */
int channel_gate_fake_closed(void);
/* What a live family request does with the channel the decoder staged for it (rtl_stream_stage_family_channel()):
   takes it, for the caller to land where the request applies. 0: none staged. */
uint32_t channel_gate_fake_take_staged(void);
/* The receiver refused a family request that named @p channel (rtl_stream_refused_channel()). */
void channel_gate_fake_refuse(uint32_t channel);
/* Called at the start of every write, before the receiver judges it: a case lands a retune there to put a landing
   between the decoder's resolve and its write. NULL: none. */
void channel_gate_fake_before_write(void (*fn)(uint32_t channel));
/* The writes the receiver applied since the last open, the writes made at all, and the reads of the channel it is on
   (rtl_stream_current_channel()). */
int channel_gate_fake_applied_writes(void);
int channel_gate_fake_writes(void);
int channel_gate_fake_reads(void);
/* Fill @p hooks' channel fields with this stand-in. */
void channel_gate_fake_install(dsd_rtl_stream_metrics_hooks* hooks);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_TEST_CHANNEL_GATE_FAKE_H */
