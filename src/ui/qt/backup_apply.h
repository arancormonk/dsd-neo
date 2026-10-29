// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Staging a validated backup and applying it at the next start (issue #580).
 *
 * A restore never changes the running configuration. An inspected backup is staged into its own directory
 * (stage_restore()), promoted to `<data>/restore-pending/` (promote_staging()) and, once the user confirms, marked
 * with a COMMITTED file holding the sha256 of the staging manifest (commit_pending()). The next start applies it in
 * apply_pending_restore(), the first thing ui_load() does, before any model reads a store.
 *
 * Staging layout, every file written through QSaveFile with mode 0600:
 *   files/<archive path>   each carried file ("files/imports/…")
 *   stores/<store name>    each store the backup holds, rebased onto this data root
 *   prefs.json             the portable settings (a JSON object)
 *   avoids.json            the signal-search avoid list (a JSON array, possibly empty)
 *   manifest.json          {"stagingVersion": 1, "files": [{"path", "size", "sha256"}] for every file above,
 *                           "summary": summary_to_map()}
 *
 * The apply is all-or-nothing. Before its first change it writes an undo journal (`restore-pending/journal/`) of the
 * old configuration: the five stores and the avoid list, every file under imports/ a store names (a recording never
 * counts), every owned bundle directory a library row claims, and the files the app generates under decryption/ and
 * scan_lists/ (compiled profile key files, scan-list CSVs; anything else there is kept). Only those files are ever
 * deleted; any other file whose name an incoming file would take is kept and the incoming file
 * gets a free name instead, its references following. A crash re-runs to the same end state; an I/O failure rolls
 * back. Every write, delete and rename goes through one check that refuses a path outside the data root or one with
 * a symlink (or junction) among its existing components below the root.
 *
 * Staging functions are worker-safe (they touch only their own directory); the others run on the UI thread.
 */

#ifndef DSD_NEO_SRC_UI_QT_BACKUP_APPLY_H_
#define DSD_NEO_SRC_UI_QT_BACKUP_APPLY_H_

#include <QByteArray> // IWYU pragma: keep -- stage_restore() takes the archive bytes.
#include <QMap>
#include <QString>
#include <QVariant>
#include <QVariantMap>
#include <QtGlobal>
#include <functional>
#include "backup_archive.h" // IWYU pragma: export -- callers stage a ParseResult and report BackupError names.

namespace dsd_qt {
namespace backup {

constexpr int kStagingVersion = 1;

/** `<root>/restore-staging-<jobId>`, or empty when @p jobId is not 1-63 characters of [A-Za-z0-9_-]. */
QString staging_dir_for_job(const QString& jobId);

/**
 * Stage @p parsed (a successful backup_parse_archive() against @p dataRoot) with the file bytes of @p archive into
 * @p stagingDir, a staging_dir_for_job() directory of that root; an existing one is replaced. Each library row's staged
 * file is dry-run validated the way the library validates an import (ImportedFilesModel::validate()). A row that fails
 * is left out rather than refusing the backup: its files are not staged, every typed reference to them is cleared,
 * and the summary lists it ("<name>: couldn't be read on this phone").
 *
 * @param dataRoot json_store_root(), read by the caller: a worker passes the value the UI thread read.
 * @param summary  Optional; on success, the staged backup's summary (the parse's, less the rows left out).
 * @param proceed  Optional; asked once before any file is written, then before each library file's dry run, the costly
 *                 step (at most three a row: a library still losing rows on the third pass is refused as "Damaged").
 *                 False stops the staging: it returns false with @p error empty and nothing left at @p stagingDir.
 * @param error On failure, the backup_error_name(): "Damaged" when the parse result does not belong to this data root
 *              or does not match the archive, "StagingFailed" when the staging directory could not be written.
 * @return true when everything is staged; on failure nothing is left at @p stagingDir.
 */
bool stage_restore(const QString& dataRoot, const ParseResult& parsed, const QByteArray& archive,
                   const QString& stagingDir, QString* error, Summary* summary = nullptr,
                   const std::function<bool()>& proceed = {});

/** Rename @p stagingDir to `<root>/restore-pending`, replacing an uncommitted one; never a committed one. */
bool promote_staging(const QString& stagingDir);

/**
 * Write `restore-pending/COMMITTED` holding the lower-case hex sha256 of `restore-pending/manifest.json`. Already
 * committed counts as success. Refused when there is no pending restore or an apply has started on it.
 */
bool commit_pending();

/**
 * Remove `restore-pending/`, unless its apply has started (committed, with `journal/COMPLETE`). The marker goes first:
 * false with the stage no longer committed means some files stay but the restore will never apply (the next start
 * removes them); false with the stage still committed means nothing was removed. True when none is left.
 */
bool discard_pending();

/** Whether `restore-pending/` exists. */
bool pending_exists();

/**
 * Whether `restore-pending/` exists and is committed: its COMMITTED marker is a regular file, as the apply reads it.
 */
bool pending_committed();

/**
 * Whether the apply has started on the committed `restore-pending/` (its journal is complete): only the apply may
 * finish or undo it, so discard_pending() refuses it.
 */
bool pending_apply_started();

/** Remove every `<root>/restore-staging-*`. Only while no staging job runs. */
void sweep_stale_staging();

/**
 * Remove one staging_dir_for_job() directory (a cancelled job's), never following a link inside it. False when
 * @p stagingDir is not such a directory or something of it stays; true when none is left.
 */
bool discard_staging(const QString& stagingDir);

/** What a test makes a named apply step do. */
enum class Fault {
    None,
    IoError, /**< The step fails, as a write or delete failing would. */
    Crash,   /**< apply_pending_restore() returns at once, as if the process died there; the disk is left as is. */
};

/**
 * What apply_pending_restore() needs from its caller. An empty function takes the default: the engine is idle,
 * free space is dsd_path_free_bytes() (a negative value means unknown and is not a shortage), no fault.
 *
 * The fault seam is consulted before each step, by name. Per-item steps are consulted once per item.
 *   journal.begin, journal.files (per file), journal.prefs, journal.manifest, journal.complete;
 *   forward.delete (per old file), forward.generated, forward.files (per staged file), forward.stores (per store),
 *   forward.avoids, forward.prefs;
 *   rollback.delete (per written destination), rollback.files (per journal file), rollback.prefs;
 *   result.write, marker.remove, cleanup.
 */
struct ApplyEnv {
    std::function<bool()> engineBusy;
    std::function<qint64(const QString&)> freeBytes;
    std::function<Fault(const char* step)> fault;
};

enum class ApplyOutcome {
    None,     /**< Nothing to apply (or an unconfirmed stage was removed). */
    Applied,  /**< The backup is the configuration now. */
    Failed,   /**< Nothing was changed; the pending restore is gone. */
    Deferred, /**< Not tried now (the engine is busy, or storage is short); the pending restore is kept. */
    Stuck,    /**< Neither finishing nor undoing worked; the next start tries again. */
};

/** "", "applied", "failed", "deferred" or "stuck". */
QString apply_outcome_name(ApplyOutcome outcome);

struct ApplyResult {
    ApplyOutcome outcome = ApplyOutcome::None;
    /** A sentence for the user; empty for None and Applied. */
    QString reason;
    /** The staged backup's summary_to_map(), when the staging could be read. */
    QVariantMap summary;
};

/**
 * Apply a committed pending restore. Call before any model reads a store. Steps:
 *  0. No `restore-pending/` → None. Uncommitted → removed, None.
 *  1. The engine busy → Deferred, nothing touched. Without a complete journal: the marker, the staging version and
 *     every staged file's size and sha256 are verified (else the stage is removed → Failed), and free space for the
 *     staged and journal bytes plus 1 MiB is required (else Deferred).
 *  2. Without a complete journal, the journal is built from the live configuration (old files copied, settings,
 *     destinations with the free names chosen for blocked ones), `journal/COMPLETE` last. A blocked bundle whose row
 *     keeps its file outside it cannot take a free name (that file names it by its id): Failed, nothing changed.
 *  3. Roll forward from the journal and the staging only.
 *  4. Success: result file (best effort), the marker removed (the commit point), the stage removed → Applied.
 *  5. A failure in 3, or a staging that no longer verifies after the journal is complete → roll back from the
 *     journal, then the same ending → Failed.
 *  6. The rollback or the marker removal failing → Stuck, everything kept.
 */
ApplyResult apply_pending_restore(const ApplyEnv& env);

/** `<root>/backup-restore-result.json` ({outcome, reason, summary}), or an empty map. */
QVariantMap read_result_file();

/** Remove the result file. True when none is left. */
bool clear_result_file();

} // namespace backup
} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_BACKUP_APPLY_H_ */
