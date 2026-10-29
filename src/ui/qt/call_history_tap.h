// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The call history's background capture (issue #708, B1).
 *
 * On Android Qt pauses the GUI thread while the app is in the background, and the UI poll tick is the only reader of
 * the event-history ring. The ring keeps 254 committed rows a slot, so a busy system overwrites calls the History
 * never saw. CallHistoryTap reads the ring on its own thread while the app is away, through the second reader
 * app-control keeps for it (dsd_app_history_tap_copy()), and walks each copy with the model's own rules
 * (CallHistoryModel::walkRing()) on its own copy of the model's seen map and clear mark. What it finds goes to
 * CallHistorySpill, an append-only JSON-lines file, which the model folds in when it reads the ring again, and
 * after a relaunch if the process died in the background.
 */

#ifndef DSD_NEO_SRC_UI_QT_CALL_HISTORY_TAP_H_
#define DSD_NEO_SRC_UI_QT_CALL_HISTORY_TAP_H_

#include <QByteArray>
#include <QFile>
#include <QHash> // IWYU pragma: keep -- Context holds the seen map (a QHash) by value.
#include <QIODevice>
#include <QList>
#include <QString>
#include <QtGlobal>
#include <condition_variable>
#include <dsd-neo/app_control/history_tap.h>
#include <dsd-neo/core/state.h>
#include <memory>
#include <mutex>
#include <thread>

#include "call_history_model.h"

namespace dsd_qt {

/**
 * @brief The spill: one JSON line per row the tap found (CallHistoryModel::spillLine()), appended.
 *
 * The tap appends on its thread; the model reads and truncates on the GUI thread. Each operation holds the spill's
 * mutex, and the model truncates only what it read and a save covered (truncateIfSize()), so a line the tap
 * appended after that read is never lost.
 */
class CallHistorySpill {
  public:
    explicit CallHistorySpill(QString path);

    /**
     * @brief Append @p lines, each ending in a newline.
     *
     * A file whose last byte is not a newline (a line a kill tore) gets one first, so the torn line never swallows
     * the first line written after it. @return whether every byte was written.
     */
    bool append(const QByteArray& lines);

    /**
     * @brief Open the spill in @p file to read; @p size receives its length now (0 when there is none).
     *
     * Read it no further than @p size: the tap appends only past it, and only the GUI thread, which reads the spill,
     * truncates it, so the bytes before it stay as they are while it reads without the mutex.
     */
    bool open(QFile* file, qint64* size) const;

    /** @brief Empty the spill if it still holds exactly @p size bytes; @return whether it was emptied. */
    bool truncateIfSize(qint64 size);

    /** @brief Empty the spill whatever it holds. */
    void clear();

    /** @brief How many appends wrote to the file. */
    int appendsForTest() const;

    /** @brief How many appends were tried, written or not. */
    int appendAttemptsForTest() const;

  private:
    QString m_path;
    mutable std::mutex m_mutex;
    int m_appends = 0;
    int m_attempts = 0;
};

/**
 * @brief Reads JSON lines from a device one at a time, holding one line at most.
 *
 * A long background stretch spills far more rows than History keeps: read whole, and split into lines, the spill
 * would cost twice its size in memory when the app resumes or relaunches. The reader holds the line it returns, its
 * buffer growing to the longest line read, and a line past kMaxLineBytes, which no build writes, is passed over a
 * chunk at a time. A last line without its newline (one a kill tore) comes back as it is.
 */
class CallHistorySpillReader {
  public:
    static constexpr qsizetype kMaxLineBytes = qsizetype{1} << 20;

    /** @brief Read @p device from where it is, at most @p limit bytes. */
    CallHistorySpillReader(QIODevice* device, qint64 limit);

    /** @brief Read the next line into line(); @return false at the end. */
    bool next();

    /** @brief The line next() read, without its newline; valid until the next call. */
    const QByteArray&
    line() const {
        return m_line;
    }

    /** @brief The most bytes the reader's buffers have held at once. */
    qsizetype
    peakBytes() const {
        return m_peakBytes;
    }

  private:
    QIODevice* m_device;
    qint64 m_remaining;
    QByteArray m_chunk;
    QByteArray m_line;
    qsizetype m_peakBytes = 0;
};

/**
 * @brief Reads the published history rings once a second while the app is in the background.
 *
 * Started and stopped from the GUI thread only. Each poll copies a slot's ring when its committed rows changed (a
 * poll that finds nothing new costs well under a microsecond), walks the copy outside every lock, and holds the rows
 * it found. They are appended to the spill at most every five polls, so a busy system writes about a kilobyte a row
 * in one append every five seconds, and at stop. The two ring buffers (3.75 MB each) are allocated at start, outside
 * any lock, and freed at stop.
 */
class CallHistoryTap {
  public:
    /** @brief What the tap starts from: the model's seen map, session and clear mark as they are now. */
    struct Context {
        CallHistoryModel::SeenMap seen;
        qint64 session = 0;
        QString sessionLabel;
        QString sessionUid;
        CallHistoryModel::ClearMark clear;
    };

    /** @brief What the tap leaves: the clear mark as its reads settled it, the ring position it read last, and the
     * rows its last append could not write. */
    struct Result {
        CallHistoryModel::ClearMark clear;
        bool read = false;
        qulonglong pushSeq[2] = {0U, 0U};
        quint64 ring = 0U;
        /* Spill lines, oldest first, that never reached the spill (it was still unwritable at stop). Only the model
         * has them now: the next start clears what the tap holds. */
        QByteArray unwritten;
    };

    CallHistoryTap(CallHistoryTapSource source, CallHistorySpill* spill);
    ~CallHistoryTap();

    CallHistoryTap(const CallHistoryTap&) = delete;
    CallHistoryTap& operator=(const CallHistoryTap&) = delete;

    /** @brief Start polling from @p context. @return false when already running or the ring buffers cannot be had. */
    bool start(Context context);

    /** @brief Poll once more, write what is held to the spill, and stop; what could not be written comes back in
     * Result::unwritten. A tap that is not running returns {}. */
    Result stop();

    bool
    running() const {
        return m_running;
    }

    /** @brief Poll every @p pollMs and append after @p batchPolls polls; takes effect at the next start. */
    void setTimingForTest(int pollMs, int batchPolls);

    /** @brief Wait until a poll that started after this call has finished (at once when not running). */
    void waitForPollForTest();

  private:
    void run();
    void poll();
    void flush();
    /** @brief Cut the held rows to the bound, oldest first, reporting the first cut of a run of failed appends. */
    void trimPending();

    CallHistoryTapSource m_source;
    CallHistorySpill* m_spill;
    /* GUI thread only, but for m_pollMs and m_batchPolls, which a test sets under m_mutex. */
    bool m_running = false;
    std::thread m_thread;
    /* Under m_mutex: the stop request, a test's poll request, and the poll count a test waits on. */
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::condition_variable m_polled;
    bool m_stop = false;
    bool m_pollNow = false;
    bool m_polling = false;
    quint64 m_polls = 0;
    int m_pollMs = 1000;
    int m_batchPolls = 5;
    /* The tap thread's own while it runs; the GUI thread hands them over at start and takes them back after the
     * join at stop. */
    Context m_context;
    Result m_result;
    bool m_clearBound[2] = {false, false};
    std::unique_ptr<Event_History_I[]> m_rings;
    dsd_app_history_tap_cursor m_cursor[2] = {};
    /* The spill lines found and not yet appended, oldest first. */
    QList<QByteArray> m_pending;
    int m_pendingPolls = 0;
    bool m_appendFailing = false;
    bool m_dropReported = false;
    int m_batchPollsRun = 5;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_CALL_HISTORY_TAP_H_ */
