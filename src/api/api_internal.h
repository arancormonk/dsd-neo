// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Shared internals for the TCP JSON Lines API module.
 */

#ifndef DSD_NEO_SRC_API_API_INTERNAL_H_
#define DSD_NEO_SRC_API_API_INTERNAL_H_

#include <dsd-neo/api/json.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Telemetry subscription topics. */
enum {
    DSD_API_TOPIC_CALL = 1u << 0,
    DSD_API_TOPIC_EVENT = 1u << 1,
    DSD_API_TOPIC_SYSTEM = 1u << 2,
    DSD_API_TOPIC_METRICS = 1u << 3,
    DSD_API_TOPIC_QUALITY = 1u << 4,
    DSD_API_TOPIC_STATUS = 1u << 5,
    DSD_API_TOPIC_RESULT = 1u << 6,
};

enum { DSD_API_TOPIC_COUNT = 7 };

/** Parse a JSON array of topic names. Returns a mask (0 when none valid). */
uint32_t dsd_api_topics_parse(const dsd_json_node* topics);

/** Name of a single topic bit, or NULL. */
const char* dsd_api_topic_name(uint32_t bit);

/* ---- server → feed/commands ---- */

/** Append @p line (including its trailing newline) to every session subscribed to @p topic. */
void dsd_api_broadcast(const char* line, size_t len, uint32_t topic);

/** Nonzero when at least one session is subscribed to @p topic. */
int dsd_api_topic_interest(uint32_t topic);

/** Number of connected sessions (any authentication state). */
int dsd_api_client_count(void);

/* ---- feed ---- */

void dsd_api_feed_start(void);
void dsd_api_feed_stop(void);

/**
 * @brief Copy the most recent cached telemetry line for a one-shot @c get.
 *
 * @param what One of "status", "call", "system", "metrics", "quality".
 * @return 0 when a cached line was written to @p out, -1 otherwise.
 */
int dsd_api_feed_latest(const char* what, dsd_json_buf* out);

/* ---- commands ---- */

/**
 * @brief Execute one already-parsed request and build its response line.
 *
 * @param request Parsed root object with an optional "id", a "cmd" string and
 *                an optional "params" object.
 * @param out     Receives the complete response line including trailing newline.
 * @return 0 on success, -1 on an internal failure.
 */
int dsd_api_command_execute(const dsd_json_node* request, dsd_json_buf* out);

/** Build the command catalog response line (including trailing newline). */
int dsd_api_command_catalog(dsd_json_buf* out);

/** Number of commands in the catalog. */
int dsd_api_command_count_total(void);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_API_API_INTERNAL_H_ */
