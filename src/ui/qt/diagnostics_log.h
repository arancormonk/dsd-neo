// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_DIAGNOSTICS_LOG_H
#define DSD_NEO_DIAGNOSTICS_LOG_H
#include <QAbstractListModel>
#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QtGlobal>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

class QFile;

namespace dsd_qt {
// Process diagnostics survive decoder session transitions. QString copies cannot
// promise erasure: only redacted text may enter the ring or the writer queue.
//
// The persistent tail is two append-only segments (tail.prev.log, then tail.log), each at
// most kSegmentLimit bytes. Records are appended, never rewritten, and synced at most once a
// second, except during the startup window after a session attempt, when every append is
// synced so a phone that falls over seconds after Play keeps its start sequence (issue #745).
class DiagnosticsLog {
  public:
    static constexpr qint64 kSegmentLimit = 128 * 1024;
    static constexpr int kWriteIntervalMs = 250;
    static constexpr int kSyncIntervalMs = 1000;
    static constexpr int kStartupSyncWindowMs = 10000;
    static constexpr int kAttemptFlushMs = 200;
    static constexpr int kShutdownFlushMs = 500;

    struct IoStats {
        quint64 appends = 0;
        quint64 syncs = 0;
        quint64 bytes = 0;
        quint64 rotations = 0;
        quint64 syncFailures = 0;
    };

    // Test seam: a monotonic millisecond clock, and a manual mode in which nothing is written
    // until pump() or flush() runs the writer step on the calling thread.
    struct TestOptions {
        std::function<quint64()> clockMs;
        bool manualDrain = false;
    };

    enum class IoFault { None, Write, Sync, SyncAndOpen };

    // Where in a start an attempt note comes from: the USB access request that precedes a start of a local dongle,
    // or the start itself. Only that pair, for one attempt, shares a marker.
    enum class AttemptStage { UsbAccess, Start };

    // Extra platform facts for the process line (Android: boot count, device) and any lines that
    // follow it (Android: recent app exits). Registered before installTap().
    using PlatformFactsProvider = std::pair<QString, QStringList> (*)();

    static DiagnosticsLog& instance();
    static void installTap();
    static void setPlatformFactsProvider(PlatformFactsProvider provider);
    // Safe before Qt/nativeInit: bounded redacted records wait for installTap().
    static void submitHostDiagnostic(const QString& text);
    // Explicit directory supports isolated stores in restart/retention tests.
    explicit DiagnosticsLog(const QString& directory);
    DiagnosticsLog(const QString& directory, TestOptions options);
    ~DiagnosticsLog();
    void submit(const QString& source, const QString& level, const QString& text);
    QStringList snapshot(quint64* generation = nullptr) const;
    // Return the generation of the cleared ring while still holding its lock.
    quint64 clear();
    // True once everything submitted before the call is appended and synced. A negative timeout
    // waits for the writer; on timeout or a write/sync failure it returns false.
    bool flush(int timeoutMs = -1);
    // Records the process line (also the head of every later segment). Once per instance.
    void noteProcess(const QString& facts, const QStringList& followingLines = {});
    // A session attempt is about to touch the device: records the marker (once per attempt context
    // within kAttemptDedupeMs), arms the startup window and waits up to kAttemptFlushMs for it to be durable.
    // attemptKey pairs a USB access note with its start note (the source type: the context text can gain the dongle's
    // label between the two); empty means the context itself.
    void noteSessionAttempt(const QString& context, AttemptStage stage = AttemptStage::Start,
                            const QString& attemptKey = QString());
    // Re-arms the startup window at a device boundary without a new marker.
    void armStartupWindow();
    IoStats ioStats() const;
    // Manual-drain mode only: run one writer step now if the writer thread would at this clock reading, and say
    // whether it did.
    bool pump();
    void setIoFaultForTest(IoFault fault);
    static QString redact(QString text);

  private:
    static constexpr int kAttemptDedupeMs = 60000;

    void load();
    void cleanDirectory() const;
    QByteArray loadSegments() const;
    void seedRing(const QByteArray& text);
    void openCurrent();
    void closeCurrent();
    bool appendBytes(const QByteArray& bytes);
    bool syncCurrent();
    void rotate(const QByteArray& header);
    bool clearFiles();
    void trimFragment(const QString& path) const;
    bool writeBatch(const std::deque<QByteArray>& batch);
    QByteArray headerExcluding(std::deque<QByteArray>::const_iterator from,
                               std::deque<QByteArray>::const_iterator end) const;
    void enqueue(const QByteArray& record);
    void ringNote(const QString& line);
    void appendRingLocked(const QString& line);
    bool isProtectedLocked(const QString& line) const;

    // What one writer step takes from the queue state, so its I/O runs without the queue lock.
    struct StepWork {
        std::deque<QByteArray> batch;
        QByteArray header;
        quint64 now = 0;
        bool clear = false;
        bool window = false;
        bool flushing = false;
    };

    void step(std::unique_lock<std::mutex>& lock, bool forceSync);
    bool writeStep(const StepWork& work, bool forceSync);
    bool dueLocked(quint64 now) const;
    void completeFlushes(quint64 through, bool ok);
    bool flushResult(quint64 ticket) const;
    QString filePath(const char* name) const;
    void writeLoop();
    quint64 nowMs() const;
    bool inStartupWindow(quint64 now) const;

    mutable std::mutex m_ringMutex;
    QStringList m_ring;
    quint64 m_generation = 0;

    // Queue state, under m_queueMutex.
    std::mutex m_queueMutex;
    std::condition_variable m_ready, m_flushed;
    std::deque<QByteArray> m_queue;
    bool m_stop = false;
    bool m_clearPending = false;
    quint64 m_flushRequested = 0;
    quint64 m_flushDone = 0;
    std::vector<std::pair<quint64, bool>> m_flushResults;
    quint64 m_startupUntil = 0;
    quint64 m_lastDrain = 0;
    QByteArray m_processLine;
    QByteArray m_sessionLine;
    // A USB access note waiting for the start note of the same attempt, which then adds no second marker.
    QString m_usbAttemptKey;
    quint64 m_usbAttemptAt = 0;
    // An explicit start note absorbs the plain marker of the Starting transition that follows it.
    quint64 m_startNoteAt = 0;
    bool m_attemptAbsorb = false;
    // Ring lines eviction keeps at the front: the previous run's divider and identity, and this run's process line and
    // current session line. Copy and Share therefore always say which process, boot and session the rest belong to.
    // A session line a newer one replaces is released from the front.
    QStringList m_protectedPrevious;
    QString m_protectedProcess;
    QString m_protectedSession;
    int m_pinned = 0;
    bool m_processNoted = false;
    QByteArray m_previousProcessLine;

    // Writer state: touched only by the writer step, which runs on one thread at a time.
    std::mutex m_stepMutex;
    std::unique_ptr<QFile> m_file;
    qint64 m_fileSize = 0;
    quint64 m_dirtyBytes = 0;
    quint64 m_lastSync = 0;
    // After a failed sync, the next attempt waits until here rather than retrying at once.
    quint64 m_syncRetryAt = 0;
    bool m_ioFailed = false;
    bool m_rotationFailureNoted = false;
    QByteArray m_header;
    IoFault m_fault = IoFault::None;

    mutable std::mutex m_statsMutex;
    IoStats m_stats;

    QString m_directory;
    TestOptions m_options;
    std::thread m_writer;
};

class DiagnosticsLogModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(bool paused READ paused WRITE setPaused NOTIFY pausedChanged)
    Q_PROPERTY(int pendingCount READ pendingCount NOTIFY pendingCountChanged)
    // Whether a view shows the rows (Main.qml binds the diagnostics screen's visibility). While
    // inactive, ticks reset nothing; becoming active resets once (issue #718).
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
  public:
    explicit DiagnosticsLogModel(DiagnosticsLog* log = nullptr, QObject* parent = nullptr);
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    bool
    paused() const {
        return m_paused;
    }

    void setPaused(bool value);

    int
    pendingCount() const {
        return m_pending;
    }

    bool
    active() const {
        return m_active;
    }

    void setActive(bool value);

    void markSessionStarting();
    Q_INVOKABLE void refresh();
    Q_INVOKABLE QString allText() const;
    Q_INVOKABLE void copyAll() const;
    Q_INVOKABLE void clear();
  Q_SIGNALS:
    void pausedChanged();
    void pendingCountChanged();
    void activeChanged();

  private:
    void showRows(const QStringList& rows);

    DiagnosticsLog* m_log;
    QStringList m_rows;
    quint64 m_seen = 0;
    bool m_paused = false;
    int m_pending = 0;
    bool m_active = true;
    // While inactive: the rows consumed (m_seen) but not shown yet, and whether there are any.
    QStringList m_held;
    bool m_stale = false;
};
} // namespace dsd_qt
#endif
