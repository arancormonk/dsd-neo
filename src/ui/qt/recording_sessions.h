// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief What the session starters tell the call recording store (issue #708).
 *
 * A session records into its owner's folder (a saved system's, a scan list's, or the one Explore folder) and writes
 * that folder's journal. The store rewrites a journal only when no decoder can be writing it, so whoever starts a
 * session asks the store for the folder first, which marks it in use, and says when a start it asked for did not
 * happen. The end of a session that did start comes from the host (qt_ui.cpp wires it).
 */

#ifndef DSD_NEO_SRC_UI_QT_RECORDING_SESSIONS_H_
#define DSD_NEO_SRC_UI_QT_RECORDING_SESSIONS_H_

#include <QString>

namespace dsd_qt {

/** @brief The journal a session writes in its recording folder (--wav-journal <folder>/recordings.jsonl). */
constexpr char kRecordingJournalFileName[] = "recordings.jsonl";

class RecordingSessions {
  public:
    virtual ~RecordingSessions() = default;

    /**
     * @brief The folder a session of @p kind ("saved", "scan" or "explore") owned by @p uid records into, created
     * (with its folder.json) on first use and marked in use until endSession().
     *
     * Looked up by @p uid, so a renamed owner keeps its folder (@p name is only its label). Every "explore" session
     * shares one folder. Empty when no recording root is available.
     */
    virtual QString beginSession(const QString& kind, const QString& uid, const QString& name) = 0;

    /** @brief No session is running any more: every folder beginSession() marked is free again. */
    virtual void endSession() = 0;

  protected:
    RecordingSessions() = default;
    RecordingSessions(const RecordingSessions&) = default;
    RecordingSessions& operator=(const RecordingSessions&) = default;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_RECORDING_SESSIONS_H_ */
