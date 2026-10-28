// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Stage markers for the Android app's teardown (issue #782).
 *
 * Android's QtActivity waits in its onDestroy() for main() to return, and a relaunch of the app waits for that old
 * process. main() returns only after the QML engine, its models and the decoder host are destroyed, and some model
 * destructors wait with no timeout: for a worker pool, a synchronous save, a store flush or a network client. To say
 * which stage held a relaunch, android/main.cpp installs hooks once its event loop has returned, and each stage then
 * logs how long it took as a host diagnostic.
 *
 * Header-only, with no link dependency: the hooks carry the log and the clock. Until they are installed (always, on
 * the desktop and in tests that do not install them) a stage does nothing.
 *
 * The final diagnostics flush is the one stage no record can follow durably, so main() writes how long it took to a
 * small marker file afterwards, and the next process's start reports it (teardownFlushMarkerWrite/Take).
 */

#ifndef DSD_NEO_SRC_UI_QT_TEARDOWN_TRACE_H_
#define DSD_NEO_SRC_UI_QT_TEARDOWN_TRACE_H_

#include <QByteArray>
#include <QFile>
#include <QIODevice>
#include <QList>
#include <QString>
#include <atomic>
#include <cstdint>

namespace dsd_qt {

/** @brief Where stage markers go, and the monotonic millisecond clock they are timed with. */
struct TeardownTraceHooks {
    void (*log)(const QString& line);
    uint64_t (*nowMs)();
};

namespace teardown_trace_detail {

inline std::atomic<const TeardownTraceHooks*>&
hooks() {
    static std::atomic<const TeardownTraceHooks*> installed{nullptr};
    return installed;
}

} // namespace teardown_trace_detail

/** @brief Turns stage markers on (@p hooks must outlive every stage) or, with nullptr, off again. */
inline void
teardownTraceInstall(const TeardownTraceHooks* hooks) {
    teardown_trace_detail::hooks().store(hooks, std::memory_order_release);
}

/**
 * @brief Logs `teardown: <name> took <n> ms` when it goes out of scope, if markers were on when it was made.
 *
 * Declare it first in the destructor or scope to time: what the body waits for is what it measures.
 */
class TeardownStage {
  public:
    explicit TeardownStage(const char* name)
        : m_hooks(teardown_trace_detail::hooks().load(std::memory_order_acquire)), m_name(name),
          m_start(m_hooks != nullptr ? m_hooks->nowMs() : 0) {}

    ~TeardownStage() {
        if (m_hooks != nullptr) {
            const uint64_t now = m_hooks->nowMs();
            const uint64_t took = now >= m_start ? now - m_start : 0;
            m_hooks->log(QStringLiteral("teardown: %1 took %2 ms").arg(QString::fromUtf8(m_name)).arg(took));
        }
    }

    TeardownStage(const TeardownStage&) = delete;
    TeardownStage& operator=(const TeardownStage&) = delete;
    TeardownStage(TeardownStage&&) = delete;
    TeardownStage& operator=(TeardownStage&&) = delete;

  private:
    const TeardownTraceHooks* m_hooks;
    const char* m_name;
    uint64_t m_start;
};

/**
 * @brief Records at @p path how long the final diagnostics flush took and whether it completed in its bound.
 *
 * Not synced: it only has to outlive the process, which the page cache does. Best effort; false on any failure.
 */
inline bool
teardownFlushMarkerWrite(const QString& path, uint64_t tookMs, bool completed) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    const QByteArray text = QByteArray("flush ") + QByteArray::number(static_cast<qulonglong>(tookMs))
                            + (completed ? QByteArray(" completed\n") : QByteArray(" timed-out\n"));
    return file.write(text) == text.size();
}

/**
 * @brief The record for the marker at @p path, which a previous process's teardown wrote, or empty with none.
 *
 * The marker is removed either way, so it is reported once; a malformed one is dropped silently.
 */
inline QString
teardownFlushMarkerTake(const QString& path) {
    QFile file(path);
    if (!file.exists()) {
        return {};
    }
    QByteArray text;
    if (file.open(QIODevice::ReadOnly)) {
        text = file.read(64).trimmed();
        file.close();
    }
    file.remove();
    const QList<QByteArray> fields = text.split(' ');
    bool ok = false;
    const qulonglong took = fields.size() == 3 && fields.at(0) == "flush" ? fields.at(1).toULongLong(&ok) : 0;
    if (!ok || (fields.at(2) != "completed" && fields.at(2) != "timed-out")) {
        return {};
    }
    const QString outcome = fields.at(2) == "completed" ? QStringLiteral("") : QStringLiteral(", and timed out");
    return QStringLiteral("teardown: the previous process's final diagnostics flush took %1 ms%2")
        .arg(took)
        .arg(outcome);
}

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_TEARDOWN_TRACE_H_ */
