// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The app side of the recording journal (issue #708): one record parsed from a line, a `publish` line written
 * back by a compaction, and the names the decoder gives its per-call temp files.
 *
 * The record format is the contract in docs/code_map.md ("Recording journal"), written by src/core/file/wav_journal.c.
 * Pure functions, no I/O: the call recording store (recording_store.h) reads and writes the files.
 */

#ifndef DSD_NEO_SRC_UI_QT_RECORDING_JOURNAL_H_
#define DSD_NEO_SRC_UI_QT_RECORDING_JOURNAL_H_

#include <QByteArray>
#include <QString>
#include <QtGlobal>

namespace dsd_qt {

/** @brief One journal record, as far as the store reads it. */
struct RecordingJournalRecord {
    enum Type { Publish, Link, Unlink, Discard, Error };

    Type type = Publish;
    qint64 t = 0;
    /* `<16 uppercase hex token>-<sequence>`; empty for a record whose id is null. */
    QString id;
    /* publish: the final name relative to the journal's folder, '/'-separated, checked to stay inside it. */
    QString file;
    qint64 start = 0;
    qint64 durationMs = 0;
    int rate = 0;
    /* publish and link: the history ring, slot and row stamp, when the record names them. */
    bool hasInstance = false;
    quint64 instance = 0U;
    int slot = -1;
    bool hasRow = false;
    qulonglong row = 0U;
    bool pending = false;
    qulonglong epoch = 0U;
    bool rdio = false;
    bool failed = false;
    QString system;
    QString target;
    QString source;
};

/**
 * @brief Parse one journal line (without its newline).
 *
 * False for a line that does not parse (a torn tail), a version other than 1, an unknown type, a `link` or `unlink`
 * without an id, a `link` without its ring, slot and row, and a `publish` whose file is not a plain relative name
 * inside the journal's folder (absolute, a drive, a backslash, an empty, `.` or `..` component): the store manages
 * only what the decoder published in the folder it was told to use.
 */
bool recording_journal_parse(const QByteArray& line, RecordingJournalRecord* out);

/** @brief A `publish` line (with its newline) for @p record, as a compaction writes it back: never pending. */
QByteArray recording_journal_publish_line(const RecordingJournalRecord& record);

/**
 * @brief Whether @p fileName is a per-call temp name (`TEMP_<date>_<time>_<token>_<seq>`, `.wav` or not); its token,
 * 16 uppercase hex digits, goes to @p token.
 */
bool recording_temp_name_token(const QString& fileName, QString* token);

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_RECORDING_JOURNAL_H_ */
