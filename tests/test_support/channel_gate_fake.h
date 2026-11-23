// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * A stand-in for a radio receiver's squelch gate (issue #899), for tests that drive the decoder's side without a running
 * stream. It keeps the receiver's rules (rtl_stream_write_squelch_gate() and rtl_stream_gate_boundary() in
 * rtl_sdr_fm.cpp, which IO_RTL_RETUNE_PREPARE checks on the real receiver): a new channel on the analog family starts
 * closed (a level no power reaches) until the decoder writes; a landing or switch off the analog monitor runs the
 * digital gate the decoder last gave; a write applies to the channel the receiver is on, the analog gate there on the
 * monitor and the digital gate off it. It also says whether a receive request is still unsettled, as the
 * case sets it.
 *
 * channel_gate_fake_install() fills a hook table's gate fields, so the engine reaches it as it reaches the stream. Every
 * gate the receiver comes to run goes to the apply callback, as the stream's setter would take it.
 */
#ifndef DSD_NEO_TEST_CHANNEL_GATE_FAKE_H
#define DSD_NEO_TEST_CHANNEL_GATE_FAKE_H

#include <dsd-neo/core/power.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*channel_gate_fake_apply_fn)(const dsd_squelch_setting* gate);

/* A stream opens on a new channel, closed when @p analog, else under @p gate (the open's own setting, which @p apply
   does not hear); @p apply (may be NULL) takes each gate the receiver comes to run from here on. No request is
   unsettled. */
void channel_gate_fake_open(int analog, const dsd_squelch_setting* gate, channel_gate_fake_apply_fn apply);
/* A retune or family switch landed, the receiver on the analog monitor when @p analog (a typed digital row's symbol
   profile on the analog family is off it): a new channel (@p new_channel: a marked retune, a switch into analog, a
   return to the monitor) starts closed on the monitor; off it the receiver runs the digital gate. */
void channel_gate_fake_land(int new_channel, int analog);
/* The gate the receiver runs, whether it runs a closed start, and its family. */
dsd_squelch_setting channel_gate_fake_gate(void);
int channel_gate_fake_closed(void);
int channel_gate_fake_analog(void);
/* Whether a receive request is unsettled (the demod thread has not applied it), as the receiver reports it. */
void channel_gate_fake_requests_pending(int pending);
/* Called at the start of every write, before the receiver applies it: a case lands a retune there to put a landing
   between the decoder's resolve and its write. NULL: none. */
void channel_gate_fake_before_write(void (*fn)(void));
/* The writes made since the last open, and the receiver reads (closed-start and pending-request polls). */
int channel_gate_fake_writes(void);
int channel_gate_fake_reads(void);
/* Fill @p hooks' gate fields with this stand-in. */
void channel_gate_fake_install(dsd_rtl_stream_metrics_hooks* hooks);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_TEST_CHANNEL_GATE_FAKE_H */
