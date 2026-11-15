// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#pragma once

/*
 * A system resolver a POSIX test executable puts in place of the C library's (issue #743 B18).
 *
 * fake_getaddrinfo.c defines getaddrinfo() and freeaddrinfo() themselves: an executable built with it resolves every
 * name through them, the platform layer's dsd_socket_resolve() included, so a test can tell whether a name was looked
 * up through getaddrinfo() at all, how often, and with which address family, and can change the answer between
 * lookups as a DNS record does. It answers DSD_FAKE_RESOLVER_HOST only; every other name fails as unknown, so it
 * belongs only in a test whose names are all numeric (dsd_socket_resolve() reads those without a lookup). Add the
 * source to the test executable itself, never to a library other tests link. POSIX only.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The one name the fake resolver knows (".invalid" never resolves through a real resolver: RFC 6761). */
#define DSD_FAKE_RESOLVER_HOST "dsd-neo-resolver-test.invalid"

/**
 * @brief Answer the next lookups of DSD_FAKE_RESOLVER_HOST with @p first, then @p later for every one after it, and
 *        clear the lookup count.
 *
 * Addresses are IPv4 in host byte order (0x7F000001 is 127.0.0.1).
 */
void dsd_fake_resolver_reset(uint32_t first, uint32_t later);

/** @brief Answer every lookup after the first with @p later from now on, the count kept (safe while another thread
 *         looks the name up). */
void dsd_fake_resolver_set_later(uint32_t later);

/** @brief Lookups of DSD_FAKE_RESOLVER_HOST since the last reset. */
int dsd_fake_resolver_lookups(void);

/** @brief Lookups since the last reset whose hints did not ask for AF_INET alone. */
int dsd_fake_resolver_non_ipv4_lookups(void);

#ifdef __cplusplus
}
#endif
