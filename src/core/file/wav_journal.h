// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_SRC_CORE_FILE_WAV_JOURNAL_H_
#define DSD_NEO_SRC_CORE_FILE_WAV_JOURNAL_H_

/* The recording journal's writers (issue #708): the per-call WAV close (dsd_file.c) and the event layer
   (dsd_events.c). The record format is docs/code_map.md's "Recording journal"; <dsd-neo/core/wav_journal.h> opens and
   closes the journal. Every writer returns at once while no journal is on. */

#include <dsd-neo/core/file_io.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One published recording. Strings may be NULL (written as ""). */
typedef struct {
    const char* id;   /* "<token>-<sequence>", from the temp name; NULL when the name is not one open_wav_file() made */
    const char* path; /* the final name, as published */
    const char* dir;  /* the folder it was published in (the record's folder when @c path has none) */
    time_t start;     /* dsd_rdio_recording_start() */
    int64_t duration_ms; /* frames / rate, read before the close; 0 when unknown */
    int rate;            /* sample rate, Hz; 0 when unknown */
    int has_instance;    /* 0 when no history ring is known */
    uint64_t instance;   /* Event_History_I::instance */
    dsd_wav_owner owner; /* whose recording it is; slot DSD_WAV_OWNER_SLOT_UNKNOWN for none */
    int rdio_queued;     /* an rdio-scanner API upload was queued for it (its worker may delete the file) */
    int failed;          /* a short write latched the segment: the file ends where the write failed */
    const char* system;  /* the row's system string */
    const char* target;  /* the row's target, as shown */
    const char* source;  /* the row's source, as shown */
} wav_journal_publish_record;

/* Path forms the records use, pure string functions; @p win selects Windows rules ('\\' also separates, a drive root
   is "C:\\") so a test can exercise them on any host. The base name of @p path: */
const char* wav_journal_path_base(const char* path, int win);
/* The folder part of @p path: "" for a bare name, the root ("/", "C:\\") for a file in one. */
void wav_journal_path_folder(const char* path, int win, char* out, size_t out_size);
/* The `file` form of @p base in @p folder: relative to @p journal_dir and '/'-separated when @p folder is it or lies
   below it, else @p folder plus @p base. Both folders canonical. "" for an empty @p base. */
void wav_journal_path_name(const char* journal_dir, const char* folder, const char* base, int win, char* out,
                           size_t out_size);

/* 1 while a journal is being written. */
int wav_journal_active(void);

/* `publish`; a PENDING owner also joins its slot's pending list, until a commit links it or it is unlinked. */
void wav_journal_publish(const wav_journal_publish_record* rec);

/* `discard`: the segment held no audio (a 44-byte WAV) and was deleted. */
void wav_journal_discard(const char* id);

/* `error`: the segment is not published. @p kind is "open", "close" or "publish"; @p temp_path the temp name it keeps
   ("" when none; the record's folder is then @p dir). */
void wav_journal_error(const char* id, const char* kind, const char* temp_path, const char* dir);

/* The slot's row for @p epoch committed with push stamp @p stamp in ring @p instance: `link` for each recording of
   that epoch waiting for it. */
void wav_journal_link_pending(uint8_t slot, uint64_t epoch, uint64_t instance, uint64_t stamp);

/* The slot's row for @p epoch will never commit (a drop): `unlink` for each recording waiting for it. */
void wav_journal_unlink_pending_epoch(uint8_t slot, uint64_t epoch);

/* The slot moved on to @p epoch (or a full history reset passes 0): `unlink` for each recording waiting for another
   epoch's row. */
void wav_journal_unlink_pending_except(uint8_t slot, uint64_t epoch);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_CORE_FILE_WAV_JOURNAL_H_ */
