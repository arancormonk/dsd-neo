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
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stddef.h>
#include <string>
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
//
// Host, session and process records are also kept apart from the decoder's output, in a ring of their own and their own
// segment pair (host.prev.log, then host.log, each at most kHostSegmentLimit bytes) written the same way: a fast scan
// fills the main ring and tail with decoder lines within minutes, and the lifecycle lines that explain a failure must
// outlive them (issue #782). Copy and Share put them first.
class DiagnosticsLog {
  public:
    static constexpr qint64 kSegmentLimit = 128LL * 1024;
    static constexpr qint64 kHostSegmentLimit = 32LL * 1024;
    static constexpr int kHostRingLimit = 500;
    static constexpr int kWriteIntervalMs = 250;
    static constexpr int kSyncIntervalMs = 1000;
    static constexpr int kStartupSyncWindowMs = 10000;
    static constexpr int kAttemptFlushMs = 200;
    static constexpr int kShutdownFlushMs = 500;
    // How long the log's build thread waits for the platform's following lines to be durable (PlatformLinesLogged).
    static constexpr int kPlatformLinesFlushMs = 10000;

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

    // What became of a flush: everything asked for was appended and synced; its bound ran out first (or the log was
    // not published within it); or a write or sync failed.
    enum class FlushOutcome { Completed, TimedOut, Failed };

    // Where in a start an attempt note comes from: the USB access request that precedes a start of a local dongle,
    // or the start itself. Only the start records a session marker.
    enum class AttemptStage { UsbAccess, Start };

    // Test seam: runs at the start of every load of the saved records, on the constructing thread.
    using LoadHook = void (*)();

    // Extra platform facts for the process line (Android: boot count, device) and any lines that
    // follow it (Android: recent app exits). Registered before installTap().
    using PlatformFactsProvider = std::pair<QString, QStringList> (*)();
    // Told once the lines the provider gave are synced to the log, on the log's build thread, so the platform marks
    // them logged only then: marked when they were read, a process that died before the sync lost them for good
    // (Android: the exit records, #782). Not called when they could not be made durable.
    using PlatformLinesLogged = void (*)();

    static DiagnosticsLog& instance();
    // Installs the runtime log tap and returns at once: the log itself (its directory and saved records) is built on
    // a thread of its own and then published. Until then records wait, bounded and redacted.
    static void installTap();
    static void setPlatformFactsProvider(PlatformFactsProvider provider, PlatformLinesLogged logged = nullptr);
    // These three never wait for the log's construction, so they are safe from Android's main thread, and before
    // Qt/nativeInit: bounded redacted records wait for installTap()'s publish.
    static void submitHostDiagnostic(const QString& text);
    // Opens the startup window, at once or as soon as the log is published.
    static void armStartupDiagnostics();
    // flush() of the published log; a negative timeout waits without bound, otherwise the timeout also bounds the wait
    // for the publish. False if the log was not published in time.
    static bool flushDiagnostics(int timeoutMs);
    // flushDiagnostics(), with what became of the flush.
    static FlushOutcome flushDiagnosticsOutcome(int timeoutMs);
    // The flush at the application's quit (aboutToQuit): bounded by kShutdownFlushMs, or only asked for, without a
    // wait, once the caller has said it flushes again after its teardown (setFinalFlushAfterQuit(), Android's main(),
    // #782), so the teardown holds one bounded flush.
    static void flushAtQuit();
    static void setFinalFlushAfterQuit(bool owned);
    // How long flushAtQuit() waits at most: kShutdownFlushMs, or 0 (it only asks) once setFinalFlushAfterQuit(true).
    static int quitFlushBoundMs();
    // Test seams: whether installTap()'s publish has happened, and the thread that resolved the log's directory (the
    // one QStandardPaths call).
    static bool installedForTest();
    static std::thread::id directoryResolvedOnForTest();
    // Explicit directory supports isolated stores in restart/retention tests.
    explicit DiagnosticsLog(const QString& directory);
    DiagnosticsLog(const QString& directory, TestOptions options);
    ~DiagnosticsLog();
    void submit(const QString& source, const QString& level, const QString& text);
    QStringList snapshot(quint64* generation = nullptr) const;
    // The host, session and process records, oldest first, after the previous run's under a divider.
    QStringList hostSnapshot() const;
    // Return the generation of the cleared ring while still holding its lock.
    quint64 clear();
    // True once everything submitted before the call is appended and synced. A negative timeout
    // waits for the writer; on timeout or a write/sync failure it returns false.
    bool flush(int timeoutMs = -1);
    // flush(), with what became of it.
    FlushOutcome flushOutcome(int timeoutMs = -1);
    // flush(), then @p logged once the platform lines noteProcess() queued are durable in the host log: the writer step
    // that took them wrote them and a sync covered them with no host failure between, whichever flush or drain ran it.
    // Not when they were lost, or are still on their way when the flush returns or times out. Whether it was called.
    bool reportWhenDurable(int timeoutMs, PlatformLinesLogged logged);
    // Records the process line (also the head of every later segment). Once per instance.
    void noteProcess(const QString& facts, const QStringList& followingLines = {});
    // A session attempt is about to touch the device: records the line for the stage (a USB access request, or the
    // start's session marker; an empty context is the plain marker of a Starting transition, which a start note in the
    // last kAttemptDedupeMs absorbs), arms the startup window and waits up to kAttemptFlushMs for it to be durable.
    void noteSessionAttempt(const QString& context, AttemptStage stage = AttemptStage::Start);
    // Re-arms the startup window at a device boundary without a new marker.
    void armStartupWindow();
    // The main tail's writes.
    IoStats ioStats() const;
    // Manual-drain mode only: run one writer step now if the writer thread would at this clock reading, and say
    // whether it did.
    bool pump();
    void setIoFaultForTest(IoFault fault);
    static void setLoadHookForTest(LoadHook hook);
    static QString redact(QString text);
    // Test seam: how many producer error lines are listed, and how many have only %d and %u conversions and so count.
    static std::pair<int, int> producerLineCountsForTest();

    // Test seam: whether the startup window is open now.
    bool startupWindowOpenForTest();

    // Splits a captured byte stream (Android's redirected stdout and stderr) into records of one source. A key value
    // can run on past the line that names its option, so after a dropped record the lines that could still be that
    // value are dropped too: a single token, key characters only, or the rest of a quoted string.
    class StreamCapture {
      public:
        StreamCapture(DiagnosticsLog& log, QString source);
        void feed(const char* data, size_t size);
        // The end of the stream: an unterminated last line is still a record.
        void finish();

      private:
        void emitLine();
        bool continuesValue(const QString& text);
        // Follows the open quote through a line that is still its value.
        void followQuote(const QString& plain);

        DiagnosticsLog& m_log;
        QString m_source;
        std::string m_line;
        bool m_oversized = false;
        bool m_afterDropped = false;
        // The quote a dropped record left open (0: none), and how much of the quoted value has been dropped since.
        char16_t m_quote = 0;
        qsizetype m_quotedSize = 0;
        // An oversized line is skipped unread but followed for quotes: the quote open so far, and the last character.
        char16_t m_oversizedQuote = 0;
        char16_t m_oversizedLast = 0;
    };

  private:
    static constexpr int kAttemptDedupeMs = 60000;

    static DiagnosticsLog* create();
    // Under the host lock, once installTap() has run: the process line, the records held for the log, a held
    // startup-window request; then the log is ready.
    static void publish(DiagnosticsLog& log);
    // On the build thread, after the publish: flush, and tell the platform its lines are durable (PlatformLinesLogged).
    static void notePlatformLinesDurable();

    // One append-only segment pair (previous, then current) and its writer state: the main tail, or the host records'
    // own pair. Only the writer step touches it, except stats, which are under m_statsMutex.
    struct Segments {
        Segments(const char* currentName, const char* previousName, qint64 segmentLimit, const char* what);

        const char* current;
        const char* previous;
        qint64 limit;
        // What the rotation-failure note calls the pair.
        const char* label;
        std::unique_ptr<QFile> file;
        qint64 fileSize = 0;
        quint64 dirtyBytes = 0;
        quint64 lastSync = 0;
        // After a failed sync, the next attempt waits until here rather than retrying at once.
        quint64 syncRetryAt = 0;
        bool ioFailed = false;
        // Every failure counted, which a flush's verdict does not reset: what the platform lines' write is judged by.
        quint64 failures = 0;
        bool rotationFailureNoted = false;
        IoStats stats;

        void
        fail() {
            ioFailed = true;
            ++failures;
        }
    };

    // Where the platform's lines (noteProcess()'s following lines; Android's exit records) are on their way to the
    // host log: queued, taken by a writer step, then durable once that step's write and a later sync of the host log
    // succeed with no host failure between, or lost when either fails or Clear drops them first (#782 gate r2 H4).
    enum class PlatformLines { None, Queued, Taken, Durable, Lost };

    void load();
    void cleanDirectory() const;
    QByteArray loadSegments(const Segments& segments, qsizetype* currentAt = nullptr) const;
    void seedRing(const QByteArray& text);
    void seedHostRing(const QByteArray& text, qsizetype currentAt);
    void openCurrent(Segments& segments);
    static void closeCurrent(Segments& segments);
    bool appendBytes(Segments& segments, const QByteArray& bytes) const;
    bool syncCurrent(Segments& segments) const;
    void rotate(Segments& segments, const QByteArray& header);
    bool clearFiles(Segments& segments) const;
    static void trimFragment(const QString& path);
    bool writeBatch(Segments& segments, const std::deque<QByteArray>& batch);
    static bool syncDue(const Segments& segments, quint64 now);
    QByteArray headerExcluding(const std::deque<QByteArray>::const_iterator& from,
                               const std::deque<QByteArray>::const_iterator& end) const;
    // Which of this run's identity lines a record is, if any: the ring pins that very line, not its text.
    enum class Pin { None, Process, Session };

    void enqueue(const QByteArray& record, bool host, Pin pin = Pin::None);
    void enqueueLocked(const QByteArray& record, bool host, Pin pin = Pin::None);
    void ringNote(const QString& line, bool host = false);
    quint64 pushRingLocked(const QString& line);
    void appendRingLocked(const QString& line, Pin pin = Pin::None);
    void appendHostRingLocked(const QString& line);
    bool isProtectedLocked(int index) const;

    // What one writer step takes from the queue state, so its I/O runs without the queue lock.
    struct StepWork {
        std::deque<QByteArray> batch;
        std::deque<QByteArray> hostBatch;
        QByteArray header;
        quint64 now = 0;
        bool clear = false;
        bool window = false;
        bool flushing = false;
        // The batch holds the platform lines.
        bool platformLines = false;
    };

    void step(std::unique_lock<std::mutex>& lock, bool forceSync);
    // The step's I/O; @p platform is set to Durable or Lost once the platform lines' write is settled.
    bool writeStep(const StepWork& work, bool forceSync, PlatformLines* platform);
    PlatformLines settlePlatformLines(const StepWork& work, quint64 failuresBefore, bool clearedBefore);
    // writeStep()'s sync of @p segments when one is due, and a flush's verdict on the step (which clears the failures
    // it reports).
    bool syncIfDue(Segments& segments, const StepWork& work, bool forceSync) const;
    bool takeFlushResult(bool ok);
    bool dueLocked(quint64 now) const;
    void completeFlushes(quint64 through, bool ok);
    bool flushResult(quint64 ticket) const;
    QString filePath(const char* name) const;
    void writeLoop();
    // Under the queue lock: nothing is left to write, and how long the writer may wait before something is due.
    bool drainedLocked() const;
    quint64 writerWaitLocked(quint64 now) const;
    quint64 nowMs() const;
    bool inStartupWindow(quint64 now) const;

    mutable std::mutex m_ringMutex;
    QStringList m_ring;
    // One id per ring line, in step with m_ring: lines with the same text (the same source started by two runs) are
    // still different lines.
    QList<quint64> m_ringIds;
    quint64 m_lastRingId = 0;
    quint64 m_generation = 0;
    // The host ring, also under m_ringMutex: the previous run's divider and records (m_hostPrevious of them, 0 when
    // there is no divider), then this run's.
    QStringList m_hostRing;
    int m_hostPrevious = 0;

    // Queue state, under m_queueMutex.
    std::mutex m_queueMutex;
    std::condition_variable m_ready, m_flushed;
    std::deque<QByteArray> m_queue;
    std::deque<QByteArray> m_hostQueue;
    bool m_stop = false;
    bool m_clearPending = false;
    quint64 m_flushRequested = 0;
    quint64 m_flushDone = 0;
    std::vector<std::pair<quint64, bool>> m_flushResults;
    quint64 m_startupUntil = 0;
    quint64 m_lastDrain = 0;
    QByteArray m_processLine;
    QByteArray m_sessionLine;
    // An explicit start note absorbs the plain marker of the Starting transition that follows it.
    quint64 m_startNoteAt = 0;
    bool m_attemptAbsorb = false;
    // Ring lines eviction keeps at the front: the previous run's divider and identity, and this run's process line and
    // current session line. Copy and Share therefore always say which process, boot and session the rest belong to.
    // A session line a newer one replaces is released from the front. Ring ids; 0 is none.
    QList<quint64> m_protectedPrevious;
    quint64 m_protectedProcess = 0;
    quint64 m_protectedSession = 0;
    int m_pinned = 0;
    bool m_processNoted = false;
    QByteArray m_previousProcessLine;
    PlatformLines m_platform = PlatformLines::None;

    // Writer state: touched only by the writer step, which runs on one thread at a time.
    std::mutex m_stepMutex;
    Segments m_tail;
    Segments m_host;
    QByteArray m_header;
    IoFault m_fault = IoFault::None;
    // The platform lines are written to the host log and wait for its sync; the host failure count when they were.
    bool m_platformUnsynced = false;
    quint64 m_platformFailures = 0;

    mutable std::mutex m_statsMutex;

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
