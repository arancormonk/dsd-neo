// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_SRC_IO_AUDIO_BACKENDS_UDP_INPUT_TEST_SUPPORT_H_
#define DSD_NEO_SRC_IO_AUDIO_BACKENDS_UDP_INPUT_TEST_SUPPORT_H_

#ifdef __cplusplus
extern "C" {
#endif

/* Test seam for the UDP input (issue #834), built only with DSD_NEO_TEST_HOOKS; not part of any public header. */

/* The next @p count receive threads udp_input_start() starts fail to start, as a host out of threads fails them. */
void udp_input_test_fail_thread_starts(int count);

/* While @p hold is set, the receive thread takes nothing from the socket, as a thread descheduled with datagrams
   queued: they stay in the socket until it is cleared (or a backlog mark takes them). Set before udp_input_start(): a
   thread already waiting on the socket when it is set wakes for the next datagram and takes it. */
void udp_input_test_hold_receive(int hold);

/* While @p endless is set, the socket reads as a sender that never stops: every read takes a datagram of silence, as a
   producer that keeps the socket readable faster than it is drained. */
void udp_input_test_endless_socket(int endless);

/* The next @p count socket reads take one-sample datagrams (1, 2, ...) the test queued, ahead of the socket itself:
   exactly what was queued, whatever the system's receive buffer would hold. */
void udp_input_test_queue_datagrams(int count);

/* The next socket read waits @p ms first, as a thread descheduled in the middle of a drain. */
void udp_input_test_stall_next_read(unsigned int ms);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_IO_AUDIO_BACKENDS_UDP_INPUT_TEST_SUPPORT_H_ */
