// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Small JSON writer and parser for the control/telemetry API (module-private).
 *
 * The API speaks newline-delimited JSON (NDJSON). Rather than pull in a
 * third-party JSON library, this module provides the minimal, auditable pieces
 * the server needs: a growable buffer, a streaming writer with correct
 * comma/escape handling, and a bounded DOM parser for incoming requests.
 *
 * The parser reads untrusted network input, so it is strict: RFC 8259 grammar
 * only, well-formed UTF-8 only, no NUL in strings, and bounded nesting and node
 * counts. Numbers keep their literal text, so integer accessors convert exactly
 * (a 64-bit value never passes through a double).
 *
 * Thread-safety: none of this is internally synchronized. A writer/parser is
 * owned by one thread for its lifetime.
 */

#ifndef DSD_NEO_SRC_API_JSON_H_
#define DSD_NEO_SRC_API_JSON_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum nesting depth the writer and parser accept. */
enum { DSD_JSON_MAX_DEPTH = 32 };

/** Upper bound on nodes the parser will allocate for one document. */
enum { DSD_JSON_MAX_NODES = 4096 };

/** Longest number literal the parser accepts. */
enum { DSD_JSON_MAX_NUMBER_LEN = 64 };

/*============================================================================
 * Growable byte buffer
 *============================================================================*/

typedef struct dsd_json_buf {
    char* data; /**< NUL-terminated just past @c len when non-NULL. */
    size_t len; /**< Bytes used, excluding the terminator. */
    size_t cap; /**< Allocated capacity including room for the terminator. */
    int oom;    /**< Nonzero once an allocation failed; buffer stays valid. */
} dsd_json_buf;

/** Initialize an empty buffer. Never fails; lazily allocates on first append. */
void dsd_json_buf_init(dsd_json_buf* b);
/** Free the buffer's storage and zero it. Safe on NULL and on a zeroed buffer. */
void dsd_json_buf_free(dsd_json_buf* b);
/** Reset to empty without freeing storage, for reuse. Clears a previous allocation failure. */
void dsd_json_buf_reset(dsd_json_buf* b);
/** Drop the first @p n bytes, keeping the rest (and the terminator). */
void dsd_json_buf_consume(dsd_json_buf* b, size_t n);
/** Append @p n bytes. Returns 0 on success, -1 on allocation failure. */
int dsd_json_buf_append(dsd_json_buf* b, const char* data, size_t n);
/** Append a NUL-terminated string. Returns 0 on success, -1 on failure. */
int dsd_json_buf_puts(dsd_json_buf* b, const char* s);
/** Append one byte. Returns 0 on success, -1 on failure. */
int dsd_json_buf_putc(dsd_json_buf* b, char c);
/** Ensure room for @p extra bytes plus the terminator. Returns 0 or -1. */
int dsd_json_buf_reserve(dsd_json_buf* b, size_t extra);

/*============================================================================
 * Streaming writer
 *============================================================================*/

/**
 * @brief Writer state layered over a dsd_json_buf.
 *
 * Tracks nesting and whether a comma is due, so callers emit values without
 * hand-managing separators. An error (overflow, bad call order) is sticky: once
 * @c failed is set every subsequent call is a no-op returning -1, leaving the
 * buffer holding whatever partial document was produced.
 */
typedef struct dsd_json_writer {
    dsd_json_buf* buf;
    int failed;
    int depth;                               /**< Current container nesting, 0 at top level. */
    unsigned char first[DSD_JSON_MAX_DEPTH]; /**< 1 when the container has no elements yet. */
    unsigned char is_obj[DSD_JSON_MAX_DEPTH];
    unsigned char after_key[DSD_JSON_MAX_DEPTH]; /**< 1 when the next value must not emit a comma. */
} dsd_json_writer;

/** Bind a writer to @p buf. @p buf must outlive the writer. */
void dsd_json_writer_init(dsd_json_writer* w, dsd_json_buf* buf);
/** Nonzero when the writer has hit a sticky error. */
int dsd_json_writer_failed(const dsd_json_writer* w);

/** Open an object/array at the current position. Returns 0 or -1. */
int dsd_json_obj_begin(dsd_json_writer* w);
int dsd_json_arr_begin(dsd_json_writer* w);
/** Close the current object/array. Returns 0 or -1. */
int dsd_json_obj_end(dsd_json_writer* w);
int dsd_json_arr_end(dsd_json_writer* w);

/**
 * @brief Emit an object key. Only valid directly inside an object.
 *
 * Emits the separator and the escaped key with its trailing colon. The value
 * that follows must be one of the dsd_json_value() family, which suppresses its
 * own comma because of the pending key.
 */
int dsd_json_key(dsd_json_writer* w, const char* key);

/** Emit primitive/string values at the current position. */
int dsd_json_value_str(dsd_json_writer* w, const char* v);
/** A fixed char field of @p cap bytes: the text up to its terminator, or all @p cap bytes when it has none. */
int dsd_json_value_strn(dsd_json_writer* w, const char* v, size_t cap);
int dsd_json_value_i64(dsd_json_writer* w, int64_t v);
int dsd_json_value_u64(dsd_json_writer* w, uint64_t v);
/** A 64-bit value as a decimal string, for identifiers a double-based client would round. */
int dsd_json_value_u64_str(dsd_json_writer* w, uint64_t v);
/** A finite double; NaN and infinities become null. */
int dsd_json_value_double(dsd_json_writer* w, double v);
int dsd_json_value_bool(dsd_json_writer* w, int v);
int dsd_json_value_null(dsd_json_writer* w);
/**
 * @brief Emit an already-serialized JSON value verbatim.
 *
 * The writer still applies its separator/last-key bookkeeping, so this composes
 * correctly with dsd_json_key() and the container helpers. @p raw must be a
 * single valid JSON value; nothing about it is validated.
 */
int dsd_json_value_raw(dsd_json_writer* w, const char* raw);

/** Convenience: key plus value. Returns 0 or -1. */
int dsd_json_kv_str(dsd_json_writer* w, const char* key, const char* v);
int dsd_json_kv_strn(dsd_json_writer* w, const char* key, const char* v, size_t cap);
int dsd_json_kv_i64(dsd_json_writer* w, const char* key, int64_t v);
int dsd_json_kv_u64(dsd_json_writer* w, const char* key, uint64_t v);
int dsd_json_kv_u64_str(dsd_json_writer* w, const char* key, uint64_t v);
int dsd_json_kv_double(dsd_json_writer* w, const char* key, double v);
int dsd_json_kv_bool(dsd_json_writer* w, const char* key, int v);
int dsd_json_kv_null(dsd_json_writer* w, const char* key);

/**
 * @brief Escape and append @p v as a JSON string body (without surrounding quotes).
 *
 * Bytes that are not valid UTF-8 are replaced with U+FFFD so the output is
 * always valid UTF-8; control characters, DEL and the U+2028/U+2029 line
 * separators become \uXXXX escapes. Returns 0/-1.
 */
int dsd_json_buf_append_escaped(dsd_json_buf* b, const char* v);
/** dsd_json_buf_append_escaped() for the first @p n bytes of @p v. */
int dsd_json_buf_append_escaped_n(dsd_json_buf* b, const char* v, size_t n);

/*============================================================================
 * DOM parser
 *============================================================================*/

typedef enum {
    DSD_JSON_NULL = 0,
    DSD_JSON_BOOL,
    DSD_JSON_NUMBER,
    DSD_JSON_STRING,
    DSD_JSON_ARRAY,
    DSD_JSON_OBJECT
} dsd_json_type;

typedef struct dsd_json_node dsd_json_node;

struct dsd_json_node {
    dsd_json_type type;
    int boolean;           /**< Meaningful for DSD_JSON_BOOL. */
    double number;         /**< DSD_JSON_NUMBER: the value as a finite double. */
    char* string;          /**< Owned, NUL-terminated: a STRING's value, or a NUMBER's literal text. */
    dsd_json_node** items; /**< Array elements or object values. */
    char** keys;           /**< Object keys (owned); NULL for arrays. */
    size_t count;          /**< Element count in items/keys. */
};

/**
 * @brief Parse one JSON value from @p text.
 *
 * @param text  NUL-terminated document. Trailing whitespace is allowed; any
 *              other trailing content is an error.
 * @param out   Receives the parsed root on success.
 * @param err   Optional buffer for a short human-readable error message.
 * @param errsz Size of @p err.
 * @return 0 on success, -1 on malformed input or an internal limit.
 */
int dsd_json_parse(const char* text, dsd_json_node** out, char* err, size_t errsz);

/** Free a parsed tree. Safe on NULL. */
void dsd_json_free(dsd_json_node* node);

/** Look up a key in an object node; NULL when absent or not an object. */
const dsd_json_node* dsd_json_obj_get(const dsd_json_node* obj, const char* key);

/**
 * @brief Integer accessors. Return 0 and write @p out, or -1 (out untouched).
 *
 * A number converts only when it is an integer in range: an integer literal
 * exactly, or a fraction/exponent form whose value is a whole number no larger
 * than 2^53 in magnitude. dsd_json_as_u64() also takes a string holding a
 * decimal or 0x-prefixed hexadecimal value, which is how a client sends 64-bit
 * identifiers and key words without rounding them through a double.
 */
int dsd_json_as_i64(const dsd_json_node* n, int64_t* out);
int dsd_json_as_u64(const dsd_json_node* n, uint64_t* out);
/** A number as a finite double. Returns 0/-1. */
int dsd_json_as_double(const dsd_json_node* n, double* out);
/** true/false, or the numbers 0 and 1. Returns 0/-1. */
int dsd_json_as_bool(const dsd_json_node* n, int* out);
/** Borrow the string value; NULL when @p n is not a string. */
const char* dsd_json_as_str(const dsd_json_node* n);

/**
 * @brief A string that fits a fixed field of @p cap bytes, terminator included.
 *
 * Returns the string only when its length is < @p cap (the parser has already
 * rejected embedded NULs), so a caller can copy it whole; otherwise NULL.
 */
const char* dsd_json_as_str_bounded(const dsd_json_node* n, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_API_JSON_H_ */
