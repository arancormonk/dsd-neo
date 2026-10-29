// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The recording journal (`--wav-journal`, issue #708): one JSON line for each per-call WAV the decoder
 * publishes, discards or fails to keep, and for each recording a later history row claims.
 *
 * A reader (the app's recording store) links recordings to History rows by the record's ring instance, slot and row
 * stamp, and tells the files it manages from the rest by the journal. The record format is the contract in
 * docs/code_map.md ("Recording journal"). One journal per process, opened by the engine at the start of a decoder run
 * and closed at its end; a write that fails turns it off for the rest of the run.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_WAV_JOURNAL_H_
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_WAV_JOURNAL_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Bytes dsd_wav_temp_token_text() needs: 16 hex digits and the NUL. */
#define DSD_WAV_TEMP_TOKEN_TEXT_SIZE 17U

/**
 * @brief This process's per-call WAV token: 64 random bits drawn once per process, at the first per-call open or the
 * first call here, whichever comes first. Any thread.
 *
 * Every temp name (`TEMP_<date>_<time>_<token>_<seq>`) and every journal id (`<token>-<seq>`) this process makes
 * carries it, so a temp file with this token may still be open for writing, and one with another token was left by an
 * earlier process.
 */
uint64_t dsd_wav_temp_token(void);

/**
 * @brief The token as temp names and journal ids spell it: 16 uppercase hex digits.
 *
 * Returns 0, or -1 when @p out is NULL or @p out_size is below DSD_WAV_TEMP_TOKEN_TEXT_SIZE (@p out, when there is
 * one, is then "").
 */
int dsd_wav_temp_token_text(char* out, size_t out_size);

/** The journal's state; values are stable (the recording view carries them). */
typedef enum {
    DSD_WAV_JOURNAL_OFF = 0,    /**< no journal this run */
    DSD_WAV_JOURNAL_ON = 1,     /**< records are being written */
    DSD_WAV_JOURNAL_FAILED = 2, /**< the open or a write failed: off for the rest of the run */
} dsd_wav_journal_state;

/**
 * @brief Start the run's journal at @p path, appending to a journal that exists.
 *
 * A journal that ends inside a line (a write torn by a crash) gets a newline first, so the torn line never swallows the
 * next record. NULL or "" turns the journal off. A journal already open is closed first. Returns 0 when @p path opened
 * or the journal is off, -1 (logged; the state is FAILED) when it did not open.
 */
int dsd_wav_journal_open(const char* path);

/** End the run's journal: every recording still waiting for its row gets an `unlink`, and the state turns OFF. */
void dsd_wav_journal_close(void);

/** The journal's state. Any thread. */
dsd_wav_journal_state dsd_wav_journal_state_get(void);

/**
 * @brief Every recording still waiting for its history row loses it (`unlink`): the rows it waited for are gone.
 *
 * For a trunk-scan target restore, which invalidates the event layer's references into the ring; the event layer
 * resolves the other cases itself (a commit links, a drop, an epoch moving on and a full history reset unlink).
 */
void dsd_wav_journal_unlink_pending(void);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_WAV_JOURNAL_H_ */
