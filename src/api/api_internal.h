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

#include <stddef.h>
#include <stdint.h>

#include "json.h"

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

/**
 * Decryption requests the API submits carry this bit in their request id, so
 * their results are told apart from a frontend's, whose ids count up from 1.
 */
#define DSD_API_REQUEST_ID_TAG (UINT64_C(1) << 63)

/**
 * @brief Parse a JSON array of topic names into @p out_mask.
 *
 * "all" selects every topic. Returns 0, or -1 when @p topics is not a non-empty
 * array of known topic names (nothing is written then).
 */
int dsd_api_topics_parse(const dsd_json_node* topics, uint32_t* out_mask);

/** Name of a single topic bit, or NULL. */
const char* dsd_api_topic_name(uint32_t bit);

/**
 * @brief Open a response object: {"id":<id>,"ok":<ok>.
 *
 * A numeric id is echoed as its literal, so a 64-bit id survives exactly; a
 * string id as the string; anything else as null. The caller adds its fields,
 * then closes with dsd_api_response_end().
 */
void dsd_api_response_begin(dsd_json_buf* out, dsd_json_writer* w, const dsd_json_node* id, int ok);
/** Add {"error":{"code":..,"message":..}} to an open response. */
void dsd_api_response_error(dsd_json_writer* w, const char* code, const char* message);
/** Close the response object and append the line's newline. Returns 0, or -1 when the writer failed. */
int dsd_api_response_end(dsd_json_buf* out, dsd_json_writer* w);

/* ---- server → feed/commands ---- */

/** Append @p line (including its trailing newline) to every session subscribed to @p topic. */
void dsd_api_broadcast(const char* line, size_t len, uint32_t topic);

/** Nonzero when at least one authenticated session is subscribed to @p topic. */
int dsd_api_topic_interest(uint32_t topic);

/** Number of authenticated sessions; the feed builds nothing while it is zero. */
int dsd_api_client_count(void);

/* ---- feed ---- */

/** Register the telemetry observer. Called by dsd_api_start() before any session exists. */
void dsd_api_feed_start(void);
/** Unregister the observer and drop the cache; returns once no feed callback is running. */
void dsd_api_feed_stop(void);

/**
 * @brief Copy the most recent cached telemetry object for a one-shot @c get.
 *
 * @param what One of "status", "snapshot" (an alias of status), "call",
 *             "system", "metrics", "quality".
 * @param out  Receives the object without its trailing newline.
 * @return 1 when a cached object was written, 0 when the topic is known but
 *         nothing was published yet, -1 for an unknown @p what.
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

/** Build the command catalog as a JSON array (no newline). Returns 0 or -1. */
int dsd_api_command_catalog(dsd_json_buf* out);

/** Number of commands in the catalog. */
int dsd_api_command_count_total(void);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_API_API_INTERNAL_H_ */
