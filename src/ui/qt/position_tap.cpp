// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "position_tap.h"

#include <chrono>
#include <dsd-neo/app_control/position_view.h>
#include <dsd-neo/core/position.h>
#include <new>
#include <utility>

namespace dsd_qt {

namespace {

/* Records one read copies: a page of about 47 KB, read until the view has no more. */
constexpr size_t kPage = 256;

/* Whether the read through @p cursor has every record of its stream, and that stream's run had stopped when it was
 * published: the sample names another run (none: a publish with no run) or says this one is no longer running. */
bool
stream_read_to_end(const dsd_app_position_cursor& cursor) {
    if (cursor.stream_id == 0U || cursor.more != 0U || cursor.last_seq < cursor.newest_seq) {
        return false;
    }
    return cursor.sample.stream_id != cursor.stream_id || cursor.sample.running == 0U;
}

} // namespace

PositionPage
position_read_page(PositionSource source, PositionReadState& state, dsd_position_fix* out, size_t cap) {
    PositionPage page;
    if (source == nullptr) {
        page.count = -1;
        return page;
    }
    dsd_app_position_cursor cursor{};
    const int n = source(state.stream, state.seq, out, cap, &cursor);
    if (n < 0) {
        page.count = -1;
        return page;
    }
    if (cursor.reset != 0U) {
        // A new run: its first point never joins the last run's. Reports were lost only if the old run's tail was not
        // read (a stop and a start inside one poll period).
        page.gap |= kPositionGapBreak;
        if (!state.ended) {
            page.gap |= kPositionGapLoss;
        }
    }
    if (cursor.lost != 0U) {
        page.gap |= kPositionGapBreak | kPositionGapLoss;
    }
    state.stream = cursor.stream_id;
    state.seq = cursor.last_seq;
    state.ended = stream_read_to_end(cursor);
    state.sample = cursor.sample;
    page.count = n;
    page.more = cursor.more != 0U;
    return page;
}

PositionTap::PositionTap(PositionSource source) : m_source(source) {}

PositionTap::~PositionTap() { (void)stop(); }

bool
PositionTap::start(const PositionReadState& from) {
    if (m_running || m_source == nullptr) {
        return false;
    }
    // Allocated here, on the GUI thread and outside any lock, once per start; never per poll.
    Result fresh;
    fresh.fixes.reset(new (std::nothrow) dsd_position_fix[kCapacity]);
    fresh.gaps.reset(new (std::nothrow) quint8[kCapacity]);
    m_page.reset(new (std::nothrow) dsd_position_fix[kPage]);
    if (!fresh.fixes || !fresh.gaps || !m_page) {
        m_page.reset();
        return false;
    }
    fresh.ran = true;
    fresh.read = from;
    m_result = std::move(fresh);
    m_pendingGap = 0;
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_pollMsRun = m_pollMs;
        m_stop = false;
        m_pollNow = false;
        m_polling = false;
    }
    m_thread = std::thread(&PositionTap::run, this);
    m_running = true;
    return true;
}

PositionTap::Result
PositionTap::stop() {
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
    m_page.reset();
    m_result.trailingGap = std::exchange(m_pendingGap, quint8{0});
    return std::exchange(m_result, Result());
}

void
PositionTap::setPollIntervalForTest(int pollMs) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_pollMs = qMax(1, pollMs);
}

void
PositionTap::waitForPollForTest() {
    if (!m_running) {
        return;
    }
    std::unique_lock<std::mutex> lock(m_mutex);
    // A poll already under way may have read before the caller's last publish: wait for the one after it.
    const quint64 target = m_polls + (m_polling ? 2U : 1U);
    m_pollNow = true;
    m_wake.notify_all();
    m_polled.wait(lock, [this, target]() { return m_polls >= target; });
}

void
PositionTap::run() {
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
        m_wake.wait_for(lock, std::chrono::milliseconds(m_pollMsRun), [this]() { return m_stop || m_pollNow; });
    }
    lock.unlock();
    // What was published since the last poll (a stop while away leaves the run's final records published).
    poll();
}

/* Append @p fix with the marks @p gap (and any carried ones). A full queue drops its oldest record, and the record
 * then oldest carries the loss: no trail joins across it, and the List says reports may be missing there. */
void
PositionTap::push(const dsd_position_fix& fix, quint8 gap) {
    Result& q = m_result;
    if (q.count == kCapacity) {
        q.head = (q.head + 1) % kCapacity;
        q.count--;
        q.dropped++;
        q.gaps[static_cast<size_t>(q.head)] |= kPositionGapBreak | kPositionGapLoss;
    }
    const auto at = static_cast<size_t>((q.head + q.count) % kCapacity);
    q.fixes[at] = fix;
    q.gaps[at] = static_cast<quint8>(gap | std::exchange(m_pendingGap, quint8{0}));
    q.count++;
}

void
PositionTap::poll() {
    for (;;) {
        const PositionPage page = position_read_page(m_source, m_result.read, m_page.get(), kPage);
        if (page.count < 0) {
            return;
        }
        if (page.count == 0) {
            m_pendingGap |= page.gap;
            return;
        }
        push(m_page[0], page.gap);
        for (int i = 1; i < page.count; ++i) {
            push(m_page[static_cast<size_t>(i)], 0U);
        }
        if (!page.more) {
            return;
        }
    }
}

} // namespace dsd_qt
