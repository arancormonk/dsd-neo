// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "map_tile_worker.h"

#include <QByteArray>
#include <QChar>
#include <QCoreApplication>
#include <QImage>
#include <QStringList>
#include <Qt>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/tile_fetch.h>
#include <functional>
#include <list>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "map_tile_http.h"
#include "realtime_clock.h"

namespace dsd_qt {

namespace {

/* Worker threads still running, deleted workers' included. */
std::atomic<int> g_liveThreads{0};

constexpr int kUserAgentCap = 128;
/* Digits of the longest coordinates a template can be filled with: zoom 30, then a 10-digit column and row. */
constexpr int kZoomDigitsMax = 2;
constexpr int kCoordDigitsMax = 10;

/* One tile that needs the network, with the validators its cache lookup found in its epoch's template cache. */
struct NetJob {
    MapTileKey key;
    quint64 generation = 0;
    quint64 epoch = 0;
    /* Asked for by a view the device's position placed: the cache keeps its result in memory only. */
    bool memoryOnly = false;
    QByteArray ifNoneMatch;
    QByteArray ifModifiedSince;
};

/* What one fetch came to. Miss: the server answered, but has no PNG for this tile (a hole: 404, 410, 204 or an empty
 * 200; or a 200 whose body is not a PNG the map can use): that tile is skipped for its generation, with no backoff. */
enum class Outcome { Success, Miss, Cancelled, Invalid, Unsupported, Failure };

/* The answer with the times it went out and came back, before it is decoded or stored. */
struct Exchange {
    dsd_tile_fetch_response resp{};
    qint64 requestTime = 0;
    qint64 responseTime = 0;
};

/* Whether @p resp says the server has no tile there (a hole): gone, or empty (a 204, or a 200 without a body). */
bool
tile_missing(const dsd_tile_fetch_response& resp) {
    if (resp.status != DSD_TILE_FETCH_ERR_HTTP) {
        return false;
    }
    return resp.http_status == 404 || resp.http_status == 410 || resp.http_status == 204 || resp.http_status == 200;
}

/* Whether an answer over the size cap still means "no usable tile here" (an oversized tile, or a hole with a large
 * page) rather than an outage or a rate limit, which back off for the whole source like any other failure. */
bool
too_large_is_miss(const dsd_tile_fetch_response& resp) {
    const long s = resp.http_status;
    return s == 200 || s == 204 || s == 404 || s == 410;
}

/* Any other failure: a miss for this tile only (a hole, an oversized tile), or a failure the whole source backs off
 * from. Over the body or header cap, an oversized tile is a miss; an oversized 429 or 5xx page is still an outage
 * (backoff, Retry-After, the cached copy kept). */
Outcome
failure_outcome(const dsd_tile_fetch_response& resp) {
    if (resp.status == DSD_TILE_FETCH_ERR_TOO_LARGE) {
        return too_large_is_miss(resp) ? Outcome::Miss : Outcome::Failure;
    }
    return tile_missing(resp) ? Outcome::Miss : Outcome::Failure;
}

MapTileCache::Storage
storage_for(bool memoryOnly) {
    return memoryOnly ? MapTileCache::Storage::MemoryOnly : MapTileCache::Storage::DiskAndMemory;
}

struct FetchResult {
    Outcome outcome = Outcome::Failure;
    QImage image;
    bool hasRetryAfter = false;
    qint64 retryAfterMs = 0;
    /* A 304 selected nothing stored: the retry goes out without validators. */
    bool dropValidators = false;
    /* A 200 with a body that is not a PNG the map can use (the only evidence for NoPngTiles; a hole is not). */
    bool notPng = false;
    /* mapTileReusable() of what was delivered: false means the canvas shows it for this presentation only. */
    bool reusable = false;
};

bool
visible_ascii(const QString& text) {
    return std::all_of(text.cbegin(), text.cend(), [](QChar c) { return c.unicode() > 0x20U && c.unicode() < 0x7FU; });
}

/* Every worker thread not yet joined. Joined when it has finished (reaped at the next start) or at shutdown; a thread
 * still running when the shutdown's wait runs out is detached. Leaked on purpose: a detached thread may still look
 * itself up after static destruction began. */
struct ThreadEntry {
    quint64 id = 0;
    std::thread thread;
    bool done = false;
    std::function<void()> stop;
};

struct ThreadRegistry {
    std::mutex mutex;
    std::condition_variable changed;
    std::list<ThreadEntry> entries;
    quint64 nextId = 0;
    bool postRoutineInstalled = false;
};

ThreadRegistry&
registry() {
    static auto* const instance = new ThreadRegistry();
    return *instance;
}

void
mark_thread_done(quint64 id) {
    ThreadRegistry& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    for (ThreadEntry& entry : r.entries) {
        if (entry.id == id) {
            entry.done = true;
        }
    }
    r.changed.notify_all();
}

/* Join the threads that have already finished; each join returns at once. Call with the registry's mutex held. */
void
reap_finished_locked(ThreadRegistry& r) {
    for (auto it = r.entries.begin(); it != r.entries.end();) {
        if (it->done) {
            it->thread.join();
            it = r.entries.erase(it);
        } else {
            ++it;
        }
    }
}

void
shutdown_post_routine() {
    {
        /* Consumed by this run: a thread started from here on (a later QCoreApplication) installs it again. */
        ThreadRegistry& r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        r.postRoutineInstalled = false;
    }
    (void)MapTileWorker::shutdownThreads(MapTileWorker::kShutdownJoinMs);
}

/* Count {z}, {x} and {y} in @p rest; @return false at any other brace. */
bool
scan_placeholders(const QString& rest, int counts[3]) {
    static const QString kNames[3] = {QStringLiteral("{z}"), QStringLiteral("{x}"), QStringLiteral("{y}")};
    for (int i = 0; i < rest.size(); ++i) {
        const QChar c = rest.at(i);
        if (c == QLatin1Char('}')) {
            return false;
        }
        if (c != QLatin1Char('{')) {
            continue;
        }
        int which = -1;
        for (int n = 0; n < 3; ++n) {
            if (rest.mid(i, 3) == kNames[n]) {
                which = n;
            }
        }
        if (which < 0) {
            return false;
        }
        counts[which]++;
        i += 2;
    }
    return true;
}

/* A port: 1 to 5 digits, 1..65535. */
bool
valid_port(const QString& port) {
    if (port.isEmpty() || port.size() > 5) {
        return false;
    }
    int value = 0;
    for (const QChar c : port) {
        if (c < QLatin1Char('0') || c > QLatin1Char('9')) {
            return false;
        }
        value = value * 10 + (c.unicode() - u'0');
    }
    return value >= 1 && value <= 65535;
}

/* A bracketed IPv6 literal's inside: hex digits, ':' and '.' only, with at least one ':'. */
bool
valid_ipv6_inner(const QString& inner) {
    return inner.contains(QLatin1Char(':')) && std::all_of(inner.cbegin(), inner.cend(), [](QChar c) {
               const QChar l = c.toLower();
               return c.isDigit() || (l >= QLatin1Char('a') && l <= QLatin1Char('f')) || c == QLatin1Char(':')
                      || c == QLatin1Char('.');
           });
}

/* One label of a registered name: letters, digits, '-' and '_', not empty, not starting or ending with '-'. */
bool
valid_label(const QString& label) {
    if (label.isEmpty() || label.startsWith(QLatin1Char('-')) || label.endsWith(QLatin1Char('-'))) {
        return false;
    }
    return std::all_of(label.cbegin(), label.cend(), [](QChar c) {
        return (c.unicode() < 0x80U && c.isLetterOrNumber()) || c == QLatin1Char('-') || c == QLatin1Char('_');
    });
}

/* A registered name (dot-separated valid labels) or a bracketed IPv6 literal. */
bool
valid_host(const QString& host) {
    if (host.size() >= 2 && host.startsWith(QLatin1Char('[')) && host.endsWith(QLatin1Char(']'))) {
        return valid_ipv6_inner(host.mid(1, host.size() - 2));
    }
    const QStringList labels = host.split(QLatin1Char('.'));
    return !host.isEmpty() && std::all_of(labels.cbegin(), labels.cend(), valid_label);
}

/* @p authority is a valid host with an optional valid port. */
bool
valid_host_port(const QString& authority) {
    const qsizetype close = authority.lastIndexOf(QLatin1Char(']'));
    const qsizetype colon = authority.lastIndexOf(QLatin1Char(':'));
    if (colon > close) {
        return valid_host(authority.left(colon)) && valid_port(authority.mid(colon + 1));
    }
    return valid_host(authority);
}

/* The host and port at the start of @p rest (the template after its scheme); empty when there is none, or it holds a
 * brace or userinfo. */
QString
template_authority(const QString& rest) {
    int end = 0;
    while (end < rest.size() && rest.at(end) != QLatin1Char('/') && rest.at(end) != QLatin1Char('?')
           && rest.at(end) != QLatin1Char('#')) {
        ++end;
    }
    QString authority = rest.left(end);
    /* No userinfo: a credential there would be shown as the server's name and stored as the Host value. A host and
     * port libcurl would refuse (no host, a port that is not a number) is refused here too, before it is saved. */
    if (authority.contains(QLatin1Char('{')) || authority.contains(QLatin1Char('}'))
        || authority.contains(QLatin1Char('@')) || !valid_host_port(authority)) {
        return QString();
    }
    return authority;
}

/* The length of a template of @p size characters filled with the longest coordinates. */
qint64
longest_url(qsizetype size, const int counts[3]) {
    return static_cast<qint64>(size) - 3LL * (counts[0] + counts[1] + counts[2])
           + static_cast<qint64>(kZoomDigitsMax) * counts[0]
           + static_cast<qint64>(kCoordDigitsMax) * (counts[1] + counts[2]);
}

} // namespace

struct MapTileWorker::Core {
    std::mutex mutex;
    std::condition_variable wake;

    /* Shared with the GUI thread, under the mutex. */
    bool stop = false;
    QString templ;
    quint64 templateSerial = 0;
    quint64 epoch = 0;
    quint64 generation = 0;
    /* The latest generation was asked for by a view the device's position placed. */
    bool memoryOnly = false;
    /* This epoch has seen a PNG from its source (a cache hit or a fetch), and a 200 whose body is not a usable PNG.
     * NoPngTiles needs the second without the first; holes (404, 410, 204, empty 200) count as neither. */
    bool pngSeen = false;
    bool notPngSeen = false;
    std::deque<MapTileKey> pending;
    std::deque<NetJob> network;
    bool inflight = false;
    MapTileKey inflightKey;
    quint64 inflightGeneration = 0;
    /* Set when any generation that kept the fetch in flight was a device-placed view: its result stays in memory. */
    bool inflightMemoryOnly = false;
    /* The epoch the fetch in flight began in: a restart since then makes its outcome belong to nobody. */
    quint64 inflightEpoch = 0;
    MapTileWorker* receiver = nullptr;
    qint64 backoffStepMs = 0;
    quint64 backoffUntilMs = 0;
    State lastPosted = State::Off;

    /* Set by a stop; read without the mutex after a fetch returns, to skip decoding and storing at teardown. */
    std::atomic<bool> stopping{false};

    /* Set before the thread starts, read-only after. */
    Options options;
    QByteArray userAgent;
    dsd_tile_fetch_cancel* cancel = nullptr;
    /* Connection reuse across this worker's fetches; only the worker's thread fetches with it. */
    dsd_tile_fetch_session* session = nullptr;

    /* The worker thread's own. */
    std::unique_ptr<MapTileCache> cache;
    quint64 cacheSerial = 0;

    Core() = default;
    Core(const Core&) = delete;
    Core& operator=(const Core&) = delete;

    ~Core() {
        dsd_tile_fetch_session_destroy(session);
        dsd_tile_fetch_cancel_destroy(cancel);
    }

    quint64
    monoMs() const {
        return options.monoMs ? options.monoMs() : dsd_realtime_mono_ms();
    }

    qint64
    wallSecs() const {
        return options.wallSecs ? options.wallSecs() : realtimeSecsSinceEpoch();
    }

    static void run(std::shared_ptr<Core> core, quint64 registryId);
    void requestStop();
    void lookupNext(std::unique_lock<std::mutex>& lock);
    void fetchNext(std::unique_lock<std::mutex>& lock);
    void perform(const NetJob& job, const QString& url, Exchange* ex) const;
    static FetchResult interpret(const NetJob& job, Exchange* ex, MapTileCache& tiles,
                                 const MapTileRequestFields& fields);
    void apply(const NetJob& job, const FetchResult& result);
    void answered(const FetchResult& result);
    MapTileCache& cacheFor(const QString& templateText, quint64 serial);
    MapTileRequestFields fieldsFor(const QString& templateText) const;
    void postState(State next);
    void postTile(quint64 gen, const MapTileKey& key, const QImage& image, bool stale, bool reusable) const;
    void postGone(quint64 gen, const MapTileKey& key) const;
};

void
MapTileWorker::Core::run(std::shared_ptr<Core> core, quint64 registryId) {
    {
        std::unique_lock<std::mutex> lock(core->mutex);
        while (!core->stop) {
            if (!core->pending.empty()) {
                core->lookupNext(lock);
            } else if (!core->network.empty()) {
                core->fetchNext(lock);
            } else {
                core->wake.wait(lock);
            }
        }
    }
    core.reset();
    g_liveThreads.fetch_sub(1);
    mark_thread_done(registryId);
}

void
MapTileWorker::Core::requestStop() {
    {
        std::lock_guard<std::mutex> lock(mutex);
        stop = true;
        receiver = nullptr;
        pending.clear();
        network.clear();
    }
    stopping.store(true);
    dsd_tile_fetch_cancel_request(cancel);
    wake.notify_all();
}

MapTileCache&
MapTileWorker::Core::cacheFor(const QString& templateText, quint64 serial) {
    if (!cache || cacheSerial != serial) {
        const QString root = options.cacheRoot.isEmpty() ? MapTileCache::defaultRoot() : options.cacheRoot;
        cache.reset(new MapTileCache(root, templateText, options.wallSecs));
        cacheSerial = serial;
    }
    return *cache;
}

MapTileRequestFields
MapTileWorker::Core::fieldsFor(const QString& templateText) const {
    MapTileRequestFields fields;
    fields.userAgent = userAgent;
    QString host;
    (void)MapTileWorker::validTemplate(templateText, &host);
    /* As libcurl sends Host: without the scheme's default port. */
    if (host.endsWith(QStringLiteral(":443"))) {
        host.chop(4);
    }
    fields.host = host.toLatin1();
    return fields;
}

void
MapTileWorker::Core::postState(State next) {
    if (receiver == nullptr || next == lastPosted) {
        return;
    }
    lastPosted = next;
    MapTileWorker* target = receiver;
    const quint64 at = epoch;
    QMetaObject::invokeMethod(
        target,
        [target, at, next]() {
            if (target->m_epoch == at) {
                target->deliverState(next);
            }
        },
        Qt::QueuedConnection);
}

void
MapTileWorker::Core::postTile(quint64 gen, const MapTileKey& key, const QImage& image, bool stale,
                              bool reusable) const {
    if (receiver == nullptr || image.isNull()) {
        return;
    }
    MapTileWorker* target = receiver;
    QMetaObject::invokeMethod(
        target, [target, gen, key, image, stale, reusable]() { target->deliverTile(gen, key, image, stale, reusable); },
        Qt::QueuedConnection);
}

void
MapTileWorker::Core::postGone(quint64 gen, const MapTileKey& key) const {
    if (receiver == nullptr) {
        return;
    }
    MapTileWorker* target = receiver;
    QMetaObject::invokeMethod(
        target, [target, gen, key]() { target->deliverTile(gen, key, QImage(), false, false); }, Qt::QueuedConnection);
}

void
MapTileWorker::Core::lookupNext(std::unique_lock<std::mutex>& lock) {
    const MapTileKey key = pending.front();
    pending.pop_front();
    const quint64 gen = generation;
    const bool genMemoryOnly = memoryOnly;
    const QString templateText = templ;
    const quint64 serial = templateSerial;
    lock.unlock();
    const MapTileRequestFields fields = fieldsFor(templateText);
    const MapTileCache::Lookup found = cacheFor(templateText, serial).lookup(key, fields, storage_for(genMemoryOnly));
    lock.lock();
    if (gen != generation) {
        return;
    }
    if (!found.image.isNull() && !pngSeen) {
        pngSeen = true;
        if (lastPosted == State::NoPngTiles) {
            postState(State::Ready);
        }
    }
    postTile(gen, key, found.image, found.reuse == MapTileCache::Reuse::StaleUsable, found.reusable);
    if (found.reuse != MapTileCache::Reuse::Fresh) {
        NetJob job;
        job.key = key;
        job.generation = gen;
        job.epoch = epoch;
        job.memoryOnly = genMemoryOnly;
        job.ifNoneMatch = found.ifNoneMatch;
        job.ifModifiedSince = found.ifModifiedSince;
        network.push_back(job);
    }
    if (pending.empty() && network.empty() && lastPosted == State::Retrying) {
        /* The whole view came from the cache: nothing waits on the server, so the notice goes; the backoff deadline
         * stays for the next fetch. No fetch can be in flight here: lookups and fetches run on this one thread, and a
         * failed fetch's retry is back in `network` before the next lookup. */
        postState(State::Ready);
    }
}

void
MapTileWorker::Core::fetchNext(std::unique_lock<std::mutex>& lock) {
    const quint64 now = monoMs();
    if (backoffUntilMs > now) {
        wake.wait_for(lock, std::chrono::milliseconds(backoffUntilMs - now));
        return;
    }
    const NetJob job = network.front();
    network.pop_front();
    const QString templateText = templ;
    const QString url = MapTileWorker::tileUrl(templateText, job.key);
    if (url.isEmpty()) {
        network.clear();
        postState(State::InvalidSource);
        return;
    }
    const quint64 serial = templateSerial;
    inflight = true;
    inflightKey = job.key;
    inflightGeneration = job.generation;
    inflightEpoch = job.epoch;
    inflightMemoryOnly = job.memoryOnly;
    dsd_tile_fetch_cancel_clear(cancel);
    lock.unlock();
    Exchange ex;
    perform(job, url, &ex);
    lock.lock();
    /* Decided once the answer is in, and closed to adoption in the same step: a device-placed view that kept the
     * fetch while it ran makes it memory only, and the generation and storage are fixed together. A view asking for
     * the key from here on looks it up afresh with its own origin, so a retry can never carry a hand view's storage
     * into a device-placed one. */
    inflight = false;
    NetJob done = job;
    done.memoryOnly = inflightMemoryOnly;
    done.generation = inflightGeneration;
    lock.unlock();
    if (options.afterAnswerForTest) {
        options.afterAnswerForTest();
    }
    FetchResult result;
    if (stopping.load()) {
        /* Torn down while it ran: no decode and no disk write, so the thread leaves as soon as libcurl has. */
        result.outcome = Outcome::Cancelled;
    } else {
        result = interpret(done, &ex, cacheFor(templateText, serial), fieldsFor(templateText));
    }
    dsd_tile_fetch_response_free(&ex.resp);
    lock.lock();
    apply(done, result);
}

void
MapTileWorker::Core::perform(const NetJob& job, const QString& url, Exchange* ex) const {
    const QByteArray urlBytes = url.toLatin1();
    dsd_tile_fetch_request req{};
    req.url = urlBytes.constData();
    req.if_none_match = job.ifNoneMatch.isEmpty() ? nullptr : job.ifNoneMatch.constData();
    req.if_modified_since = job.ifModifiedSince.isEmpty() ? nullptr : job.ifModifiedSince.constData();
    req.cancel = cancel;
    req.transport = options.transport;
    req.session = session;
    ex->requestTime = wallSecs();
    (void)dsd_tile_fetch(&req, &ex->resp);
    ex->responseTime = wallSecs();
}

/* A 200: decode, then store with the storage and request fields the fetch had. */
static FetchResult
interpret_body(const MapTileKey& key, Exchange* ex, MapTileCache& tiles, const MapTileRequestFields& sent,
               MapTileCache::Storage storage) {
    FetchResult result;
    const dsd_tile_fetch_response* resp = &ex->resp;
    const QByteArray png(reinterpret_cast<const char*>(resp->body), static_cast<qsizetype>(resp->body_len));
    result.image = mapTileDecodePng(png);
    if (result.image.isNull()) {
        /* Not a PNG the map can use (a JPEG source, an HTML page, over 512×512): skipped, not retried. */
        result.outcome = Outcome::Miss;
        result.notPng = true;
        return result;
    }
    (void)tiles.store(key, *resp, png, result.image, ex->requestTime, ex->responseTime, sent, storage,
                      &result.reusable);
    result.outcome = Outcome::Success;
    return result;
}

FetchResult
MapTileWorker::Core::interpret(const NetJob& job, Exchange* ex, MapTileCache& tiles,
                               const MapTileRequestFields& fields) {
    FetchResult result;
    const dsd_tile_fetch_response* resp = &ex->resp;
    const MapTileCache::Storage storage = storage_for(job.memoryOnly);
    /* What went out, validators included, so a Vary naming them stores the values sent (RFC 9111 §4.1). */
    MapTileRequestFields sent = fields;
    sent.ifNoneMatch = job.ifNoneMatch;
    sent.ifModifiedSince = job.ifModifiedSince;
    switch (resp->status) {
        case DSD_TILE_FETCH_OK: result = interpret_body(job.key, ex, tiles, sent, storage); break;
        case DSD_TILE_FETCH_NOT_MODIFIED:
            /* A 304 to a request that carried no validator selects nothing. */
            if (!job.ifNoneMatch.isEmpty() || !job.ifModifiedSince.isEmpty()) {
                result.image =
                    tiles.refresh(job.key, *resp, ex->requestTime, ex->responseTime, sent, storage, &result.reusable);
            }
            result.outcome = result.image.isNull() ? Outcome::Failure : Outcome::Success;
            result.dropValidators = result.image.isNull();
            return result;
        case DSD_TILE_FETCH_ERR_CANCELLED: result.outcome = Outcome::Cancelled; return result;
        case DSD_TILE_FETCH_ERR_INVALID: result.outcome = Outcome::Invalid; return result;
        case DSD_TILE_FETCH_ERR_UNSUPPORTED: result.outcome = Outcome::Unsupported; return result;
        default: result.outcome = failure_outcome(*resp); break;
    }
    if (result.outcome == Outcome::Miss) {
        /* The server has no usable tile there now: a stale copy must not come back from the cache. */
        tiles.invalidate(job.key, storage);
        return result;
    }
    if (result.outcome == Outcome::Failure && (resp->header_present & DSD_TILE_FETCH_HDR_RETRY_AFTER) != 0U
        && (resp->header_invalid & DSD_TILE_FETCH_HDR_RETRY_AFTER) == 0U) {
        result.hasRetryAfter =
            MapTileWorker::retryAfterMs(QByteArray(resp->retry_after), ex->responseTime, &result.retryAfterMs);
    }
    return result;
}

void
MapTileWorker::Core::apply(const NetJob& job, const FetchResult& result) {
    /* Tiles were turned off or the template changed while it ran: no tile, no state, no backoff and no retry, so
     * nothing of the old source reaches the new one (its validators included). A 200 it stored stays in its own
     * template's cache. */
    if (job.epoch != epoch) {
        return;
    }
    switch (result.outcome) {
        case Outcome::Success:
            answered(result);
            postTile(job.generation, job.key, result.image, false, result.reusable);
            return;
        case Outcome::Miss:
            /* Skipped for this generation; the rest of the view goes on at once. A stale copy already shown goes. */
            answered(result);
            postGone(job.generation, job.key);
            return;
        case Outcome::Cancelled:
            /* Cancelled by a clear that a later request for the same tile overtook: still wanted, so again. */
            if (!stop && job.generation == generation) {
                network.push_front(job);
            }
            return;
        case Outcome::Invalid:
            network.clear();
            postState(State::InvalidSource);
            return;
        case Outcome::Unsupported:
            network.clear();
            postState(State::Unavailable);
            return;
        case Outcome::Failure: break;
    }
    backoffStepMs = MapTileWorker::nextBackoffMs(backoffStepMs);
    const qint64 waitMs = result.hasRetryAfter ? std::max(kBackoffFirstMs, result.retryAfterMs) : backoffStepMs;
    backoffUntilMs = monoMs() + static_cast<quint64>(waitMs);
    postState(State::Retrying);
    if (job.generation == generation) {
        NetJob again = job;
        if (result.dropValidators) {
            again.ifNoneMatch.clear();
            again.ifModifiedSince.clear();
        }
        network.push_front(again);
    }
}

void
MapTileWorker::Core::answered(const FetchResult& result) {
    /* The server answered: whatever wait a failure set is over. */
    backoffStepMs = 0;
    backoffUntilMs = 0;
    pngSeen = pngSeen || result.outcome == Outcome::Success;
    notPngSeen = notPngSeen || result.notPng;
    if (!pngSeen && notPngSeen) {
        if (network.empty()) {
            /* This view is done: the source sent bodies that are not PNG and not one PNG since it was chosen. */
            postState(State::NoPngTiles);
        }
        return;
    }
    /* A PNG, or a hole in a source not yet known to send anything but PNG: the server is fine. */
    postState(State::Ready);
}

MapTileWorker::MapTileWorker(QObject* parent) : MapTileWorker(Options(), parent) {}

MapTileWorker::MapTileWorker(Options options, QObject* parent) : QObject(parent), m_core(std::make_shared<Core>()) {
    m_core->options = std::move(options);
    m_core->cancel = dsd_tile_fetch_cancel_create();
    m_core->session = dsd_tile_fetch_session_create();
    char agent[kUserAgentCap];
    if (dsd_tile_fetch_user_agent(agent, sizeof(agent)) == 0) {
        m_core->userAgent = QByteArray(agent);
    }
    m_core->receiver = this;
    m_template = QString::fromLatin1(kDefaultTemplate);
    m_templateValid = validTemplate(m_template);
    m_available = m_core->options.transport != nullptr || dsd_tile_fetch_available() != 0;
    m_core->templ = m_template;
    m_core->templateSerial = 1;
}

MapTileWorker::~MapTileWorker() {
    /* Never waits: the thread holds its own reference to the core and ends by itself; the registry joins it later
     * (at the next worker's start, or at application exit within kShutdownJoinMs). */
    m_core->requestStop();
}

bool
MapTileWorker::ensureThread() {
    if (m_threadStarted) {
        return true;
    }
    ThreadRegistry& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    reap_finished_locked(r);
    if (!r.postRoutineInstalled && QCoreApplication::instance() != nullptr) {
        qAddPostRoutine(shutdown_post_routine);
        r.postRoutineInstalled = true;
    }
    ThreadEntry entry;
    entry.id = ++r.nextId;
    const std::weak_ptr<Core> weak = m_core;
    entry.stop = [weak]() {
        const std::shared_ptr<Core> core = weak.lock();
        if (core) {
            core->requestStop();
        }
    };
    g_liveThreads.fetch_add(1);
    try {
        entry.thread = std::thread(&Core::run, m_core, entry.id);
    } catch (const std::system_error&) {
        g_liveThreads.fetch_sub(1);
        return false;
    }
    r.entries.push_back(std::move(entry));
    m_threadStarted = true;
    return true;
}

bool
MapTileWorker::enabled() const {
    return m_enabled;
}

void
MapTileWorker::setEnabled(bool on) {
    if (on == m_enabled) {
        return;
    }
    m_enabled = on;
    if (on && !ensureThread()) {
        m_available = false;
    }
    restart(false, false);
    Q_EMIT enabledChanged();
}

QString
MapTileWorker::urlTemplate() const {
    return m_template;
}

void
MapTileWorker::setUrlTemplate(const QString& text) {
    if (text == m_template) {
        return;
    }
    m_template = text;
    m_templateValid = validTemplate(m_template);
    restart(true, true);
    Q_EMIT urlTemplateChanged();
}

MapTileWorker::State
MapTileWorker::state() const {
    return m_state;
}

bool
MapTileWorker::available() const {
    return m_available;
}

quint64
MapTileWorker::generation() const {
    return m_generation;
}

MapTileWorker::State
MapTileWorker::idleState() const {
    if (!m_enabled) {
        return State::Off;
    }
    if (!m_available) {
        return State::Unavailable;
    }
    return m_templateValid ? State::Ready : State::InvalidSource;
}

void
MapTileWorker::restart(bool templateChanged, bool resetBackoff) {
    const State idle = idleState();
    {
        std::lock_guard<std::mutex> lock(m_core->mutex);
        m_core->generation++;
        m_core->epoch++;
        m_core->pngSeen = false;
        m_core->notPngSeen = false;
        m_core->pending.clear();
        m_core->network.clear();
        m_core->lastPosted = idle;
        if (templateChanged) {
            m_core->templ = m_template;
            m_core->templateSerial++;
        }
        if (resetBackoff) {
            m_core->backoffStepMs = 0;
            m_core->backoffUntilMs = 0;
        }
        if (m_core->inflight) {
            dsd_tile_fetch_cancel_request(m_core->cancel);
        }
        m_generation = m_core->generation;
        m_epoch = m_core->epoch;
    }
    m_epochGeneration = m_generation;
    m_core->wake.notify_all();
    deliverState(idle);
}

quint64
MapTileWorker::requestTiles(const QList<MapTileKey>& keys, ViewOrigin origin) {
    if (idleState() != State::Ready || !m_threadStarted) {
        return m_generation;
    }
    const bool memoryOnly = origin != ViewOrigin::ByHand;
    {
        std::lock_guard<std::mutex> lock(m_core->mutex);
        m_core->generation++;
        m_core->memoryOnly = memoryOnly;
        m_core->pending.clear();
        m_core->network.clear();
        bool keepInflight = false;
        for (const MapTileKey& key : keys) {
            if (!key.valid()) {
                continue;
            }
            if (m_core->inflight && m_core->inflightEpoch == m_core->epoch && key == m_core->inflightKey) {
                keepInflight = true;
                continue;
            }
            if (std::find(m_core->pending.cbegin(), m_core->pending.cend(), key) == m_core->pending.cend()) {
                m_core->pending.push_back(key);
            }
        }
        if (keepInflight) {
            m_core->inflightGeneration = m_core->generation;
            m_core->inflightMemoryOnly = m_core->inflightMemoryOnly || memoryOnly;
        } else if (m_core->inflight) {
            dsd_tile_fetch_cancel_request(m_core->cancel);
        }
        m_generation = m_core->generation;
    }
    m_core->wake.notify_all();
    return m_generation;
}

void
MapTileWorker::clearRequests() {
    {
        std::lock_guard<std::mutex> lock(m_core->mutex);
        m_core->generation++;
        m_core->pending.clear();
        m_core->network.clear();
        if (m_core->inflight) {
            dsd_tile_fetch_cancel_request(m_core->cancel);
        }
        m_generation = m_core->generation;
    }
    m_core->wake.notify_all();
}

void
MapTileWorker::deliverState(State next) {
    if (next == m_state) {
        return;
    }
    m_state = next;
    Q_EMIT stateChanged();
}

void
MapTileWorker::deliverTile(quint64 gen, const MapTileKey& key, const QImage& image, bool stale, bool reusable) {
    /* A tile from before tiles were turned off or the template changed belongs to another source. */
    if (!m_enabled || gen < m_epochGeneration) {
        return;
    }
    Q_EMIT tileReady(gen, key.z, key.x, key.y, image, stale, reusable);
}

bool
MapTileWorker::validTemplate(const QString& urlTemplate, QString* host) {
    static const QString kScheme = QStringLiteral("https://");
    if (urlTemplate.size() > static_cast<int>(DSD_TILE_FETCH_URL_MAX) || !visible_ascii(urlTemplate)
        || !urlTemplate.startsWith(kScheme, Qt::CaseInsensitive)) {
        return false;
    }
    const QString rest = urlTemplate.mid(kScheme.size());
    const QString authority = template_authority(rest);
    if (authority.isEmpty()) {
        return false;
    }
    /* A fragment is never sent: a placeholder there would fetch one resource for many tiles. No brace may sit in it. */
    const qsizetype hash = rest.indexOf(QLatin1Char('#'));
    const QString sent = hash < 0 ? rest : rest.left(hash);
    if (hash >= 0 && (rest.indexOf(QLatin1Char('{'), hash) >= 0 || rest.indexOf(QLatin1Char('}'), hash) >= 0)) {
        return false;
    }
    int counts[3] = {0, 0, 0};
    if (!scan_placeholders(sent, counts) || counts[0] != 1 || counts[1] != 1 || counts[2] != 1
        || longest_url(urlTemplate.size(), counts) > static_cast<qint64>(DSD_TILE_FETCH_URL_MAX)) {
        return false;
    }
    if (host != nullptr) {
        *host = authority;
    }
    return true;
}

QString
MapTileWorker::tileUrl(const QString& urlTemplate, const MapTileKey& key) {
    if (!key.valid() || !validTemplate(urlTemplate)) {
        return QString();
    }
    QString url = urlTemplate;
    url.replace(QStringLiteral("{z}"), QString::number(key.z));
    url.replace(QStringLiteral("{x}"), QString::number(key.x));
    url.replace(QStringLiteral("{y}"), QString::number(key.y));
    return url;
}

qint64
MapTileWorker::nextBackoffMs(qint64 previousMs) {
    if (previousMs <= 0) {
        return kBackoffFirstMs;
    }
    return std::min(previousMs * 2, kBackoffMaxMs);
}

bool
MapTileWorker::retryAfterMs(const QByteArray& value, qint64 nowSecs, qint64* waitMs) {
    const QByteArray text = value.trimmed();
    qint64 secs = 0;
    if (map_tile_http::parseDeltaSeconds(text, &secs)) {
        *waitMs = secs * 1000;
        return true;
    }
    qint64 when = 0;
    if (map_tile_http::parseHttpDate(text, nowSecs, &when)) {
        *waitMs = std::max<qint64>(0, when - nowSecs) * 1000;
        return true;
    }
    return false;
}

int
MapTileWorker::liveThreads() {
    return g_liveThreads.load();
}

MapTileWorker::ShutdownResult
MapTileWorker::shutdownThreads(int timeoutMs) {
    ThreadRegistry& r = registry();
    std::vector<std::function<void()>> stops;
    {
        std::lock_guard<std::mutex> lock(r.mutex);
        for (const ThreadEntry& entry : r.entries) {
            if (!entry.done) {
                stops.push_back(entry.stop);
            }
        }
    }
    /* Outside the registry's mutex: a stop takes the core's, and a finishing thread takes the registry's. */
    for (const std::function<void()>& stop : stops) {
        stop();
    }
    ShutdownResult result;
    std::unique_lock<std::mutex> lock(r.mutex);
    (void)r.changed.wait_for(lock, std::chrono::milliseconds(std::max(0, timeoutMs)), [&r]() {
        return std::all_of(r.entries.cbegin(), r.entries.cend(), [](const ThreadEntry& e) { return e.done; });
    });
    for (ThreadEntry& entry : r.entries) {
        if (entry.done) {
            entry.thread.join();
            result.joined++;
        } else {
            entry.thread.detach();
            result.released++;
        }
    }
    r.entries.clear();
    return result;
}

#ifdef DSD_NEO_TEST_HOOKS
void
MapTileWorker::wakeForTest() {
    m_core->wake.notify_all();
}

bool
MapTileWorker::memoryOnlyForTest() const {
    std::lock_guard<std::mutex> lock(m_core->mutex);
    return m_core->memoryOnly;
}

qint64
MapTileWorker::backoffRemainingMsForTest() const {
    std::lock_guard<std::mutex> lock(m_core->mutex);
    const quint64 now = m_core->monoMs();
    return m_core->backoffUntilMs > now ? static_cast<qint64>(m_core->backoffUntilMs - now) : 0;
}
#endif

} // namespace dsd_qt
