// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The map's reads of the position view (<dsd-neo/app_control/position_view.h>, issue #800), and its background
 *        capture.
 *
 * position_read_page() is the one way the position model and its tap read the view: it advances a reader's cursor
 * and says where a trail must break and where reports may be missing.
 *
 * On Android Qt pauses the GUI thread while the app is in the background, and the UI poll tick is the model's only
 * reader of the view. A busy system (100 radios polled every 30 s makes about 200 reports a minute) overruns the
 * published ring in minutes. PositionTap reads the view on its own thread while the app is away, the
 * call_history_tap.h pattern: a std::thread with a condition variable (a QThread's event loop is paused with the app),
 * started on hide, polling once a second into a bounded queue of kCapacity records (the model's own cap: 200 tracks x
 * 100 points), and handed over to the model at its first poll after show. A full queue drops its oldest record first
 * and marks the loss on the record that is then oldest.
 */

#ifndef DSD_NEO_SRC_UI_QT_POSITION_TAP_H_
#define DSD_NEO_SRC_UI_QT_POSITION_TAP_H_

#include <QtGlobal>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <dsd-neo/app_control/position_view.h>
#include <dsd-neo/core/position.h>
#include <memory>
#include <mutex>
#include <thread>

namespace dsd_qt {

/** @brief The view the model and the tap read: dsd_app_position_copy_since() in the app, a stand-in in tests. */
using PositionSource = int (*)(uint64_t stream_id, uint64_t after_seq, dsd_position_fix* out, size_t cap,
                               dsd_app_position_cursor* next);

/** @brief Marks a reader puts before a record (or carries to the next one). */
enum PositionGapBits : quint8 {
    /** No line may join a point before this one to a point from here on (a stream change or a loss). */
    kPositionGapBreak = 1U,
    /** Reports may be missing here (the view lost some, the queue dropped some, or a stream ended unread). */
    kPositionGapLoss = 2U,
};

/** @brief Where one reader of the view stands. */
struct PositionReadState {
    quint64 stream = 0; /**< the stream read last; 0 before any */
    quint64 seq = 0;    /**< the last seq read in it */
    /** Every record of @c stream was read after its run stopped (the sample's running flag cleared for it, or a later
     * publish with no run): a reset to the next stream then loses nothing. */
    bool ended = false;
    /** The clock sample stored with the ring at the last read (<dsd-neo/app_control/position_view.h>). */
    dsd_app_position_sample sample{};
};

/** @brief What one read of the view gave. */
struct PositionPage {
    int count = 0;  /**< records copied; -1 when the read failed */
    quint8 gap = 0; /**< PositionGapBits to put before the first record (carried when none came) */
    bool more = false;
};

/**
 * @brief Read up to @p cap records after @p state from @p source into @p out, and advance @p state.
 *
 * A reset (the published stream is not the one @p state names) breaks every trail, and is a loss too unless @p state
 * had read its stream to the end; a cursor that reports lost is a break and a loss.
 */
PositionPage position_read_page(PositionSource source, PositionReadState& state, dsd_position_fix* out, size_t cap);

/**
 * @brief Reads the position view once a second while the app is in the background.
 *
 * Started and stopped from the GUI thread only. The queue (kCapacity records, about 3.7 MB with its marks) and one read
 * page are allocated at start, outside any lock, and handed to the model with the result at stop; no poll allocates.
 */
class PositionTap {
  public:
    /** Records the queue holds: the model's own cap (PositionModel::kTrackCapacity x kPointsPerTrack). */
    static constexpr int kCapacity = 20000;

    /** @brief What the tap leaves: its queue, oldest first, and where its reads left off. */
    struct Result {
        std::unique_ptr<dsd_position_fix[]> fixes; /**< the queue's storage (kCapacity), a ring from head */
        std::unique_ptr<quint8[]> gaps;            /**< PositionGapBits before each record, parallel to fixes */
        int head = 0;
        int count = 0;
        quint8 trailingGap = 0; /**< marks after the last record, for whatever the model reads next */
        quint64 dropped = 0;    /**< records the full queue dropped, oldest first */
        bool ran = false;       /**< the tap was running: read is where it left off */
        PositionReadState read;

        const dsd_position_fix&
        fixAt(int i) const {
            return fixes[static_cast<size_t>((head + i) % kCapacity)];
        }

        quint8
        gapAt(int i) const {
            return gaps[static_cast<size_t>((head + i) % kCapacity)];
        }
    };

    explicit PositionTap(PositionSource source);
    ~PositionTap();

    PositionTap(const PositionTap&) = delete;
    PositionTap& operator=(const PositionTap&) = delete;

    /** @brief Start polling after @p from. @return false when already running or the queue cannot be had. */
    bool start(const PositionReadState& from);

    /** @brief Poll once more and stop; the queue and read position come back. A tap not running returns {}. */
    Result stop();

    bool
    running() const {
        return m_running;
    }

    /** @brief Poll every @p pollMs; takes effect at the next start. */
    void setPollIntervalForTest(int pollMs);

    /** @brief Wait until a poll that started after this call has finished (at once when not running). */
    void waitForPollForTest();

  private:
    void run();
    void poll();
    void push(const dsd_position_fix& fix, quint8 gap);

    PositionSource m_source;
    /* GUI thread only. */
    bool m_running = false;
    std::thread m_thread;
    /* Under m_mutex: the stop request, a test's poll request and interval, and the poll count a test waits on. */
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::condition_variable m_polled;
    bool m_stop = false;
    bool m_pollNow = false;
    bool m_polling = false;
    quint64 m_polls = 0;
    int m_pollMs = 1000;
    int m_pollMsRun = 1000;
    /* The tap thread's own while it runs; handed over at start, taken back after the join at stop. */
    Result m_result;
    quint8 m_pendingGap = 0;
    std::unique_ptr<dsd_position_fix[]> m_page;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_POSITION_TAP_H_ */
