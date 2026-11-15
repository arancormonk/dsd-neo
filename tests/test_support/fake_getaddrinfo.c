// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* getaddrinfo() and freeaddrinfo() for a POSIX test executable; see fake_getaddrinfo.h. */

#include "fake_getaddrinfo.h"

#include <dsd-neo/platform/atomic_compat.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

/* The first answer is set by the test before anything looks the name up; the later one can change while a reader
   thread looks it up, so it is atomic (an IPv4 address held in an int). */
static uint32_t g_first_answer = 0x7F000001U;
static atomic_int g_later_answer;
static atomic_int g_lookups;
static atomic_int g_non_ipv4_lookups;

/* One answer: the addrinfo and the address it points at, freed together. */
typedef struct {
    struct addrinfo info;
    struct sockaddr_in addr;
} fake_answer;

/* freeaddrinfo() frees the pointer getaddrinfo() returned, which is the answer itself. */
_Static_assert(offsetof(fake_answer, info) == 0, "the addrinfo must start the answer");

void
dsd_fake_resolver_reset(uint32_t first, uint32_t later) {
    g_first_answer = first;
    dsd_fake_resolver_set_later(later);
    atomic_store(&g_lookups, 0);
    atomic_store(&g_non_ipv4_lookups, 0);
}

void
dsd_fake_resolver_set_later(uint32_t later) {
    atomic_store(&g_later_answer, (int)later);
}

int
dsd_fake_resolver_lookups(void) {
    return atomic_load(&g_lookups);
}

int
dsd_fake_resolver_non_ipv4_lookups(void) {
    return atomic_load(&g_non_ipv4_lookups);
}

int
getaddrinfo(const char* node, const char* service, const struct addrinfo* hints, struct addrinfo** res) {
    (void)service;
    if (!res) {
        return EAI_FAIL;
    }
    *res = NULL;
    if (!node || strcmp(node, DSD_FAKE_RESOLVER_HOST) != 0) {
        return EAI_NONAME;
    }
    if (!hints || hints->ai_family != AF_INET) {
        atomic_fetch_add(&g_non_ipv4_lookups, 1);
    }
    const int lookup = atomic_fetch_add(&g_lookups, 1);
    fake_answer* answer = (fake_answer*)calloc(1, sizeof *answer);
    if (!answer) {
        return EAI_MEMORY;
    }
    answer->addr.sin_family = AF_INET;
    answer->addr.sin_addr.s_addr = htonl(lookup == 0 ? g_first_answer : (uint32_t)atomic_load(&g_later_answer));
    answer->info.ai_family = AF_INET;
    answer->info.ai_socktype = hints ? hints->ai_socktype : 0;
    answer->info.ai_addrlen = (socklen_t)sizeof answer->addr;
    answer->info.ai_addr = (struct sockaddr*)&answer->addr;
    *res = (struct addrinfo*)(void*)answer;
    return 0;
}

void
freeaddrinfo(struct addrinfo* res) {
    /* Each answer is one allocation that starts with its addrinfo, and no answer has a next one. */
    free(res);
}
