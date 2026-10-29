// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "call_history_tap.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QList>
#include <chrono>
#include <dsd-neo/app_control/history_tap.h>
#include <dsd-neo/core/state.h>
#include <iterator>
#include <new>
#include <utility>

namespace dsd_qt {

namespace {

/* The most rows the tap holds while the spill cannot be written: twice what one slot's ring keeps. */
constexpr qsizetype kMaxPendingRows = qsizetype{2} * (DSD_EVENT_HISTORY_LEN - 1);
/* What the spill reader reads at a time; a spill line is about half a kilobyte, a named one about a kilobyte. */
constexpr qsizetype kChunkBytes = qsizetype{16} * 1024;

} // namespace

CallHistorySpill::CallHistorySpill(QString path) : m_path(std::move(path)) {}

bool
CallHistorySpill::append(const QByteArray& lines) {
    if (lines.isEmpty()) {
        return true;
    }
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_attempts++;
    QDir().mkpath(QFileInfo(m_path).absolutePath());
    QFile file(m_path);
    if (!file.open(QIODevice::ReadWrite | QIODevice::Append)) {
        return false;
    }
    (void)file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    QByteArray out;
    if (file.size() > 0 && file.seek(file.size() - 1) && file.read(1) != QByteArray("\n")) {
        out.append('\n');
    }
    out.append(lines);
    const bool written = file.write(out) == out.size() && file.flush();
    file.close();
    m_appends++;
    return written;
}

bool
CallHistorySpill::open(QFile* file, qint64* size) const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    *size = 0;
    file->setFileName(m_path);
    if (!file->open(QIODevice::ReadOnly)) {
        return false;
    }
    *size = file->size();
    return true;
}

bool
CallHistorySpill::truncateIfSize(qint64 size) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (size <= 0 || QFileInfo(m_path).size() != size) {
        return false;
    }
    return QFile::resize(m_path, 0);
}

void
CallHistorySpill::clear() {
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (QFileInfo::exists(m_path)) {
        (void)QFile::resize(m_path, 0);
    }
}

int
CallHistorySpill::appendsForTest() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_appends;
}

int
CallHistorySpill::appendAttemptsForTest() const {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_attempts;
}

CallHistorySpillReader::CallHistorySpillReader(QIODevice* device, qint64 limit)
    : m_device(device), m_remaining(qMax<qint64>(limit, 0)), m_chunk(kChunkBytes + 1, '\0') {}

bool
CallHistorySpillReader::next() {
    m_line.truncate(0); // keeps the buffer
#ifdef DSD_NEO_TEST_HOOKS
    if (m_failAfterLinesForTest == 0) {
        m_remaining = 0;
        m_failed = true;
        return false;
    }
    if (m_failAfterLinesForTest > 0) {
        m_failAfterLinesForTest--;
    }
#endif
    bool read = false;
    bool tooLong = false;
    while (m_remaining > 0) {
        // readLine() stops after a newline, and keeps a byte for its terminating NUL.
        const qint64 got = m_device->readLine(m_chunk.data(), qMin<qint64>(m_remaining, kChunkBytes) + 1);
        if (got <= 0) {
            // Short of the length the spill had when opened: a read error, not its end. What follows was not read,
            // and the line read so far is not whole.
            m_remaining = 0;
            m_failed = true;
            return false;
        }
        m_remaining -= got;
        read = true;
        const bool ended = m_chunk.at(static_cast<qsizetype>(got) - 1) == '\n';
        const qsizetype bytes = static_cast<qsizetype>(ended ? got - 1 : got);
        if (!tooLong && m_line.size() + bytes > kMaxLineBytes) {
            tooLong = true;
            m_line.truncate(0);
        }
        if (!tooLong) {
            // Grown by doubling, never past kMaxLineBytes.
            if (m_line.size() + bytes > m_line.capacity()) {
                m_line.reserve(qMin(kMaxLineBytes, qMax(m_line.capacity() * 2, m_line.size() + bytes)));
            }
            m_line.append(m_chunk.constData(), bytes);
            m_peakBytes = qMax(m_peakBytes, m_chunk.size() + m_line.capacity());
        }
        if (ended) {
            if (!tooLong) {
                return true;
            }
            tooLong = false;
            read = false;
        }
    }
    return read && !tooLong;
}

CallHistoryTap::CallHistoryTap(CallHistoryTapSource source, CallHistorySpill* spill)
    : m_source(source), m_spill(spill) {}

CallHistoryTap::~CallHistoryTap() { (void)stop(); }

bool
CallHistoryTap::start(Context context) {
    if (m_running || m_source == nullptr || m_spill == nullptr) {
        return false;
    }
    // Allocated here, on the GUI thread and outside any lock, once per start; never per poll.
    m_rings.reset(new (std::nothrow) Event_History_I[2]);
    if (!m_rings) {
        return false;
    }
    m_context = std::move(context);
    m_result = Result();
    m_result.clear = m_context.clear;
    m_clearBound[0] = false;
    m_clearBound[1] = false;
    // A cursor that has read nothing copies on its first read: the tap starts from what the ring holds now, and its
    // seen map tells it which of those rows the model already has.
    m_cursor[0] = dsd_app_history_tap_cursor{};
    m_cursor[1] = dsd_app_history_tap_cursor{};
    m_pending.clear();
    m_pendingPolls = 0;
    m_appendFailing = false;
    m_dropReported = false;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_batchPollsRun = m_batchPolls;
        m_stop = false;
        m_pollNow = false;
        m_polling = false;
    }
    m_thread = std::thread(&CallHistoryTap::run, this);
    m_running = true;
    return true;
}

CallHistoryTap::Result
CallHistoryTap::stop() {
    if (!m_running) {
        return Result();
    }
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_wake.notify_all();
    m_thread.join();
    m_running = false;
    m_rings.reset();
    m_context = Context();
    // The final append failed too (the spill is still unwritable): the rows go to the model with the rest of the
    // result, since the next start clears what the tap holds.
    for (const QByteArray& line : std::as_const(m_pending)) {
        m_result.unwritten.append(line);
    }
    m_pending.clear();
    return std::exchange(m_result, Result());
}

void
CallHistoryTap::setTimingForTest(int pollMs, int batchPolls) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_pollMs = qMax(1, pollMs);
    m_batchPolls = qMax(1, batchPolls);
}

void
CallHistoryTap::waitForPollForTest() {
    if (!m_running) {
        return;
    }
    std::unique_lock<std::mutex> lock(m_mutex);
    // A poll already under way may have copied before the caller's last publish: wait for the one after it.
    const quint64 target = m_polls + (m_polling ? 2U : 1U);
    m_pollNow = true;
    m_wake.notify_all();
    m_polled.wait(lock, [this, target]() { return m_polls >= target; });
}

void
CallHistoryTap::run() {
    std::unique_lock<std::mutex> lock(m_mutex);
    while (!m_stop) {
        m_polling = true;
        m_pollNow = false;
        lock.unlock();
        poll();
        lock.lock();
        m_polling = false;
        m_polls++;
        m_polled.notify_all();
        // Real time: a wait, never compared with anything decoded.
        m_wake.wait_for(lock, std::chrono::milliseconds(m_pollMs), [this]() { return m_stop || m_pollNow; });
    }
    lock.unlock();
    // The commits since the last poll (a stop while in the background leaves the final rings published), then
    // everything held.
    poll();
    flush();
}

void
CallHistoryTap::poll() {
    for (int slot = 0; slot < 2; slot++) {
        Event_History_I* ring = &m_rings[slot];
        dsd_app_history_tap_meta meta = {};
        if (m_source(slot, ring, &m_cursor[slot], &meta) != 1) {
            continue;
        }
        const qulonglong pushSeq = static_cast<qulonglong>(ring->push_seq);
        const quint64 instance = static_cast<quint64>(ring->instance);
        // The clear mark, settled against this ring as the model's settleClear() would: a pending one binds to
        // what the first read finds, and one made on another ring covers nothing here.
        CallHistoryModel::ClearMark& clear = m_result.clear;
        if (clear.active && clear.pending) {
            if (!m_clearBound[slot]) {
                clear.pushSeq[slot] = pushSeq;
                clear.ring = instance;
                m_clearBound[slot] = true;
            }
            clear.pending = !(m_clearBound[0] && m_clearBound[1]);
        } else if (clear.active && clear.ring != instance) {
            clear = CallHistoryModel::ClearMark();
        }
        CallHistoryModel::RingWalk walk;
        walk.seen = &m_context.seen;
        walk.session = m_context.session;
        walk.sessionLabel = m_context.sessionLabel;
        // As refresh() attributes: unknown options cannot establish identity.
        const bool singleSystem = meta.opts_published != 0 && meta.scanner_mode == 0 && meta.trunk_scan_enabled == 0;
        walk.systemUid = singleSystem ? m_context.sessionUid : QString();
        walk.cleared = clear.active && (!clear.pending || m_clearBound[slot]);
        walk.clearedThrough = clear.pushSeq[slot];
        QList<CallHistoryModel::FreshRow> fresh;
        CallHistoryModel::walkRing(*ring, slot, walk, fresh);
        // The walk goes newest first; each slot's rows are held oldest first, so a backlog the spill cannot take
        // loses its oldest rows (a poll's slot 0 rows before its slot 1 rows).
        for (auto it = fresh.crbegin(); it != fresh.crend(); ++it) {
            m_pending.append(CallHistoryModel::spillLine(*it));
        }
        if (m_result.read && m_result.ring != instance) {
            m_result.pushSeq[0] = 0U;
            m_result.pushSeq[1] = 0U;
        }
        m_result.read = true;
        m_result.ring = instance;
        m_result.pushSeq[slot] = pushSeq;
    }
    CallHistoryModel::pruneSeenMap(m_context.seen);
    if (m_pending.isEmpty()) {
        return;
    }
    // At most one append every m_batchPolls polls: the oldest held row waits that long, no longer. A burst that
    // fills the bound is written at once instead, unless the spill is failing: then the backlog is only cut to the
    // bound, and the append is retried on the same five-poll cadence.
    ++m_pendingPolls;
    if (m_appendFailing) {
        trimPending();
    }
    if (m_pendingPolls >= m_batchPollsRun || (!m_appendFailing && m_pending.size() >= kMaxPendingRows)) {
        flush();
    }
}

void
CallHistoryTap::flush() {
    if (m_pending.isEmpty()) {
        return;
    }
    QByteArray lines;
    for (const QByteArray& line : m_pending) {
        lines.append(line);
    }
    m_pendingPolls = 0;
    if (m_spill->append(lines)) {
        m_pending.clear();
        m_appendFailing = false;
        m_dropReported = false;
        return;
    }
    m_appendFailing = true;
    trimPending();
}

void
CallHistoryTap::trimPending() {
    // A failed append (a full disk) keeps the rows for the next one, but only the newest kMaxPendingRows: the
    // ring holds the newest rows for the model besides, and an unbounded backlog would only grow while the disk
    // stays full. Reported once per run of failures.
    const qsizetype excess = m_pending.size() - kMaxPendingRows;
    if (excess > 0) {
        m_pending.erase(m_pending.begin(), m_pending.begin() + excess);
        if (!m_dropReported) {
            qWarning("call history: the background capture cannot write its spill; dropping the oldest of the rows "
                     "it holds");
            m_dropReported = true;
        }
    }
}

} // namespace dsd_qt
