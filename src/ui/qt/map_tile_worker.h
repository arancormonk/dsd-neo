// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Loads the map's tiles on a thread of its own: cache first, then one HTTPS request at a time (issue #800).
 *
 * A plain std::thread with a condition variable, not a QThread: on Android Qt blocks every QThread event loop while
 * the app is suspended. Results come back to the GUI thread through QMetaObject::invokeMethod(Qt::QueuedConnection).
 *
 * Rules the owner relies on:
 * - Nothing happens while tiles are off: no request, no disk read, no directory made. Turning them on is the user's
 *   choice, made on the map.
 * - Only tiles in view are loaded, and only the ones the latest requestTiles() named: there is no prefetch. Every
 *   requestTiles() starts a new generation and drops whatever the previous one had not reached; clearRequests(),
 *   turning tiles off and a template change do the same and cancel the request in flight. The owner calls
 *   requestTiles() on every pan or zoom, and clearRequests() when the map closes, shows its List or goes to the
 *   background. A tile already being fetched that the new generation still wants is kept, not cancelled.
 * - One tile at a time, in the order given (the owner puts the centre first). Cache hits are answered before any
 *   request goes out. A PNG is decoded on the worker, at most 512×512.
 * - After a failure the worker waits before its next request: 1 s, doubling to 5 min, or the server's Retry-After
 *   (seconds or a date) when the failure carried one, never less than 1 s. A success resets the wait; so does a
 *   template change. Cache hits are still answered while it waits.
 * - The destructor never waits for the thread: it sets the stop flag and the cancel token. The thread ends on its own
 *   once the fetch in flight returns (about 100 ms after the cancel with libcurl 8.20 or later; an older libcurl
 *   stuck in a name lookup returns when the system resolver does). A fetch that returns after the stop is neither
 *   decoded nor stored; one that returned just before it still is. It shares nothing with the deleted object: its
 *   state is reference-counted, and no result is posted once the destructor has run.
 * - Every thread is joined: one that has finished at the next worker's start, and the rest by shutdownThreads().
 *   The first thread started while a QCoreApplication exists installs a post routine (qAddPostRoutine, run as
 *   QCoreApplication is destroyed) that stops them all and joins them within kShutdownJoinMs; a thread started
 *   after that routine ran installs it again. An owner with a teardown trace (android/main.cpp) should call
 *   shutdownThreads() in its own stage right after the QML engine goes, so the wait is measured; the routine then
 *   finds nothing. Only a thread still inside libcurl when the wait runs out (an old libcurl in a name lookup) is
 *   detached, and then leaves while the process exits.
 * - Work begun before tiles were turned off or the template changed delivers nothing, posts no state, adds no
 *   backoff and is not retried, so the new source never shows or revalidates the old one's tiles.
 * - The URL template may hold a key, so it is never logged, never put in an error and never written to disk (the
 *   cache directory is named by its hash).
 */

#ifndef DSD_NEO_SRC_UI_QT_MAP_TILE_WORKER_H_
#define DSD_NEO_SRC_UI_QT_MAP_TILE_WORKER_H_

#include <QImage>
#include <QList>
#include <QObject>
#include <QString>
#include <QtGlobal>
#include <dsd-neo/runtime/tile_fetch.h>
#include <functional>
#include <memory>

#include "map_tile_cache.h"

namespace dsd_qt {

class MapTileWorker : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    Q_PROPERTY(QString urlTemplate READ urlTemplate WRITE setUrlTemplate NOTIFY urlTemplateChanged)
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(bool available READ available NOTIFY stateChanged)

  public:
    /** @brief The default tile source (spec D2). */
    static constexpr const char* kDefaultTemplate = "https://tile.openstreetmap.org/{z}/{x}/{y}.png";
    static constexpr qint64 kBackoffFirstMs = 1000;
    static constexpr qint64 kBackoffMaxMs = 5LL * 60 * 1000;
    /** How long the exit-time shutdown waits for every worker thread, in total. */
    static constexpr int kShutdownJoinMs = 2000;

    enum class State {
        Off,           /**< Tiles are off: nothing is loaded. */
        Ready,         /**< Loading as asked; the last request, if any, succeeded. */
        Retrying,      /**< The last request failed; the next waits out the backoff. */
        Unavailable,   /**< This build cannot fetch tiles (no libcurl, or no asynchronous resolver). */
        InvalidSource, /**< The URL template is not a valid https template, or the server refused it as such. */
    };
    Q_ENUM(State)

    struct Options {
        /** Cache root; empty means MapTileCache::defaultRoot(). */
        QString cacheRoot;
        /** Transport for every request; null means the built-in libcurl one. It must outlive the worker's thread
         *  (liveThreads() back to 0), not only the worker. */
        const dsd_tile_fetch_transport* transport = nullptr;
        /** Wall-clock seconds for HTTP freshness and Retry-After dates; empty reads the real-time clock. Like monoMs it
         *  runs on the worker's thread and outlives the worker: it must not capture the owner. */
        std::function<qint64()> wallSecs;
        /** Monotonic milliseconds for the backoff; empty reads dsd_realtime_mono_ms(). */
        std::function<quint64()> monoMs;
    };

    explicit MapTileWorker(QObject* parent = nullptr);
    explicit MapTileWorker(Options options, QObject* parent = nullptr);
    ~MapTileWorker() override;

    MapTileWorker(const MapTileWorker&) = delete;
    MapTileWorker& operator=(const MapTileWorker&) = delete;

    bool enabled() const;
    /** @brief Turn tiles on or off. Off drops every request and cancels the one in flight. */
    void setEnabled(bool on);

    QString urlTemplate() const;
    /** @brief Use the template @p text; a change drops every request, resets the backoff and selects that template's
     *         cache. An invalid template puts the worker in InvalidSource and nothing is fetched. */
    void setUrlTemplate(const QString& text);

    State state() const;
    /** @brief Whether tiles can be fetched in this build (an injected transport always can). */
    bool available() const;

    /**
     * @brief Ask for the tiles in view, in priority order. Duplicates and invalid keys are skipped.
     *
     * @return The new generation; every tileReady() for this request carries it. Ignored (returns the current
     *         generation unchanged) while tiles are off or the template is invalid.
     */
    quint64 requestTiles(const QList<MapTileKey>& keys);

    /** @brief Drop every request and cancel the one in flight (map closed, List shown, app in the background). */
    void clearRequests();

    /** @brief The latest generation. */
    quint64 generation() const;

    /**
     * @brief Whether @p urlTemplate is a usable tile template.
     *
     * It must be `https://` with a non-empty host free of placeholders and of userinfo, visible ASCII only, hold each
     * of `{z}`, `{x}` and `{y}` and no other brace, and still fit DSD_TILE_FETCH_URL_MAX bytes with the longest
     * coordinates.
     *
     * @param host Receives the host (with any port) for the "sent to" line when valid.
     */
    static bool validTemplate(const QString& urlTemplate, QString* host = nullptr);

    /** @brief @p urlTemplate with @p key's coordinates in place; empty when the template is not valid. */
    static QString tileUrl(const QString& urlTemplate, const MapTileKey& key);

    /** @brief The wait after a failure: 1 s after a success, else double @p previousMs, at most 5 min. */
    static qint64 nextBackoffMs(qint64 previousMs);

    /**
     * @brief Retry-After as a wait in milliseconds: delta-seconds, or an HTTP-date less @p nowSecs (not below 0).
     *
     * @return false when @p value is neither.
     */
    static bool retryAfterMs(const QByteArray& value, qint64 nowSecs, qint64* waitMs);

    /** @brief How many worker threads are still running, deleted workers' included. */
    static int liveThreads();

    struct ShutdownResult {
        int joined = 0;   /**< Threads that ended within the wait and were joined. */
        int released = 0; /**< Threads still running when it ran out, detached. */
    };

    /**
     * @brief Stop every worker thread and join each one that ends within @p timeoutMs in total.
     *
     * Run at application exit by a post routine the first worker thread installs; callable directly (tests). Live
     * workers stop loading for good. GUI thread only.
     */
    static ShutdownResult shutdownThreads(int timeoutMs);

#ifdef DSD_NEO_TEST_HOOKS
    /** @brief Wake the thread so it reads an injected clock again. */
    void wakeForTest();
    /** @brief The wait the next failure-free request is held for, in milliseconds from the injected clock. */
    qint64 backoffRemainingMsForTest() const;
#endif

  Q_SIGNALS:
    /**
     * @brief A tile to draw.
     *
     * @param generation The request it answers; a result for an older generation may still arrive and should be
     *                   dropped when the key is no longer in view.
     * @param stale      The image is a stale copy shown while it is revalidated; a fresh one may follow.
     */
    void tileReady(quint64 generation, int z, int x, int y, const QImage& image, bool stale);
    void enabledChanged();
    void urlTemplateChanged();
    void stateChanged();

  private:
    struct Core;

    bool ensureThread();
    void restart(bool templateChanged, bool resetBackoff);
    void deliverState(State next);
    void deliverTile(quint64 gen, const MapTileKey& key, const QImage& image, bool stale);
    State idleState() const;

    std::shared_ptr<Core> m_core;
    bool m_threadStarted = false;
    bool m_enabled = false;
    QString m_template;
    bool m_templateValid = false;
    bool m_available = false;
    State m_state = State::Off;
    quint64 m_generation = 0;
    /* Bumped by every on/off and template change; a state posted before the latest one is dropped. */
    quint64 m_epoch = 0;
    /* The generation that change started; a tile from an earlier generation belongs to the old source. */
    quint64 m_epochGeneration = 0;
};

} // namespace dsd_qt

#endif // DSD_NEO_SRC_UI_QT_MAP_TILE_WORKER_H_
