// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "map_tile_worker.h"

#include <QByteArray>
#include <QMetaObject>
#include <Qt>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <dsd-neo/runtime/decode_clock.h>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>

#include "realtime_clock.h"

namespace dsd_qt {

namespace {

/* Worker threads still running, deleted workers' included. */
std::atomic<int> g_liveThreads{0};

constexpr int kUserAgentCap = 128;
/* Digits of the longest coordinates a template can be filled with: zoom 30, then a 10-digit column and row. */
constexpr int kZoomDigitsMax = 2;
constexpr int kCoordDigitsMax = 10;

/* One tile that needs the network, with the validators its cache lookup found. */
struct NetJob {
    MapTileKey key;
    quint64 generation = 0;
    QByteArray ifNoneMatch;
    QByteArray ifModifiedSince;
};

/* What one fetch came to. */
enum class Outcome { Success, Cancelled, Invalid, Unsupported, Failure };

struct FetchResult {
    Outcome outcome = Outcome::Failure;
    QImage image;
    bool hasRetryAfter = false;
    qint64 retryAfterMs = 0;
    /* A 304 selected nothing stored: the retry goes out without validators. */
    bool dropValidators = false;
};

bool
visible_ascii(const QString& text) {
    return std::all_of(text.cbegin(), text.cend(), [](QChar c) { return c.unicode() > 0x20U && c.unicode() < 0x7FU; });
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

/* The host and port at the start of @p rest (the template after its scheme); empty when there is none or it holds a
 * brace. */
QString
template_authority(const QString& rest) {
    int end = 0;
    while (end < rest.size() && rest.at(end) != QLatin1Char('/') && rest.at(end) != QLatin1Char('?')
           && rest.at(end) != QLatin1Char('#')) {
        ++end;
    }
    const QString authority = rest.left(end);
    if (authority.contains(QLatin1Char('{')) || authority.contains(QLatin1Char('}'))) {
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
    std::deque<MapTileKey> pending;
    std::deque<NetJob> network;
    bool inflight = false;
    MapTileKey inflightKey;
    quint64 inflightGeneration = 0;
    MapTileWorker* receiver = nullptr;
    qint64 backoffStepMs = 0;
    quint64 backoffUntilMs = 0;
    State lastPosted = State::Off;

    /* Set before the thread starts, read-only after. */
    Options options;
    QByteArray userAgent;
    dsd_tile_fetch_cancel* cancel = nullptr;

    /* The worker thread's own. */
    std::unique_ptr<MapTileCache> cache;
    quint64 cacheSerial = 0;

    Core() = default;
    Core(const Core&) = delete;
    Core& operator=(const Core&) = delete;

    ~Core() { dsd_tile_fetch_cancel_destroy(cancel); }

    quint64
    monoMs() const {
        return options.monoMs ? options.monoMs() : dsd_realtime_mono_ms();
    }

    qint64
    wallSecs() const {
        return options.wallSecs ? options.wallSecs() : realtimeSecsSinceEpoch();
    }

    static void run(std::shared_ptr<Core> core);
    void lookupNext(std::unique_lock<std::mutex>& lock);
    void fetchNext(std::unique_lock<std::mutex>& lock);
    FetchResult perform(const NetJob& job, const QString& url, const QString& templateText, quint64 serial);
    FetchResult interpret(const NetJob& job, dsd_tile_fetch_response* resp, qint64 requestTime, qint64 responseTime,
                          const MapTileRequestFields& fields);
    void apply(const NetJob& job, const FetchResult& result);
    MapTileCache& cacheFor(const QString& templateText, quint64 serial);
    MapTileRequestFields fieldsFor(const QString& templateText) const;
    void postState(State next);
    void postTile(quint64 gen, const MapTileKey& key, const QImage& image, bool stale) const;
};

void
MapTileWorker::Core::run(std::shared_ptr<Core> core) {
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
MapTileWorker::Core::postTile(quint64 gen, const MapTileKey& key, const QImage& image, bool stale) const {
    if (receiver == nullptr || image.isNull()) {
        return;
    }
    MapTileWorker* target = receiver;
    QMetaObject::invokeMethod(
        target, [target, gen, key, image, stale]() { target->deliverTile(gen, key, image, stale); },
        Qt::QueuedConnection);
}

void
MapTileWorker::Core::lookupNext(std::unique_lock<std::mutex>& lock) {
    const MapTileKey key = pending.front();
    pending.pop_front();
    const quint64 gen = generation;
    const QString templateText = templ;
    const quint64 serial = templateSerial;
    lock.unlock();
    const MapTileRequestFields fields = fieldsFor(templateText);
    const MapTileCache::Lookup found = cacheFor(templateText, serial).lookup(key, fields);
    lock.lock();
    if (gen != generation) {
        return;
    }
    postTile(gen, key, found.image, found.reuse == MapTileCache::Reuse::StaleUsable);
    if (found.reuse != MapTileCache::Reuse::Fresh) {
        NetJob job;
        job.key = key;
        job.generation = gen;
        job.ifNoneMatch = found.ifNoneMatch;
        job.ifModifiedSince = found.ifModifiedSince;
        network.push_back(job);
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
    dsd_tile_fetch_cancel_clear(cancel);
    lock.unlock();
    const FetchResult result = perform(job, url, templateText, serial);
    lock.lock();
    inflight = false;
    NetJob done = job;
    done.generation = inflightGeneration;
    apply(done, result);
}

FetchResult
MapTileWorker::Core::perform(const NetJob& job, const QString& url, const QString& templateText, quint64 serial) {
    const QByteArray urlBytes = url.toLatin1();
    dsd_tile_fetch_request req{};
    req.url = urlBytes.constData();
    req.if_none_match = job.ifNoneMatch.isEmpty() ? nullptr : job.ifNoneMatch.constData();
    req.if_modified_since = job.ifModifiedSince.isEmpty() ? nullptr : job.ifModifiedSince.constData();
    req.cancel = cancel;
    req.transport = options.transport;
    const MapTileRequestFields fields = fieldsFor(templateText);
    (void)cacheFor(templateText, serial);
    const qint64 requestTime = wallSecs();
    dsd_tile_fetch_response resp;
    (void)dsd_tile_fetch(&req, &resp);
    const qint64 responseTime = wallSecs();
    const FetchResult result = interpret(job, &resp, requestTime, responseTime, fields);
    dsd_tile_fetch_response_free(&resp);
    return result;
}

FetchResult
MapTileWorker::Core::interpret(const NetJob& job, dsd_tile_fetch_response* resp, qint64 requestTime,
                               qint64 responseTime, const MapTileRequestFields& fields) {
    FetchResult result;
    switch (resp->status) {
        case DSD_TILE_FETCH_OK: {
            const QByteArray png(reinterpret_cast<const char*>(resp->body), static_cast<qsizetype>(resp->body_len));
            result.image = mapTileDecodePng(png);
            if (!result.image.isNull()) {
                (void)cache->store(job.key, *resp, png, result.image, requestTime, responseTime, fields);
                result.outcome = Outcome::Success;
            }
            return result;
        }
        case DSD_TILE_FETCH_NOT_MODIFIED:
            /* A 304 to a request that carried no validator selects nothing. */
            if (!job.ifNoneMatch.isEmpty() || !job.ifModifiedSince.isEmpty()) {
                result.image = cache->refresh(job.key, *resp, requestTime, responseTime, fields);
            }
            result.outcome = result.image.isNull() ? Outcome::Failure : Outcome::Success;
            result.dropValidators = result.image.isNull();
            return result;
        case DSD_TILE_FETCH_ERR_CANCELLED: result.outcome = Outcome::Cancelled; return result;
        case DSD_TILE_FETCH_ERR_INVALID: result.outcome = Outcome::Invalid; return result;
        case DSD_TILE_FETCH_ERR_UNSUPPORTED: result.outcome = Outcome::Unsupported; return result;
        default: break;
    }
    if ((resp->header_present & DSD_TILE_FETCH_HDR_RETRY_AFTER) != 0U
        && (resp->header_invalid & DSD_TILE_FETCH_HDR_RETRY_AFTER) == 0U) {
        result.hasRetryAfter =
            MapTileWorker::retryAfterMs(QByteArray(resp->retry_after), responseTime, &result.retryAfterMs);
    }
    return result;
}

void
MapTileWorker::Core::apply(const NetJob& job, const FetchResult& result) {
    switch (result.outcome) {
        case Outcome::Success:
            backoffStepMs = 0;
            backoffUntilMs = 0;
            postState(State::Ready);
            postTile(job.generation, job.key, result.image, false);
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

MapTileWorker::MapTileWorker(QObject* parent) : MapTileWorker(Options(), parent) {}

MapTileWorker::MapTileWorker(Options options, QObject* parent) : QObject(parent), m_core(std::make_shared<Core>()) {
    m_core->options = std::move(options);
    m_core->cancel = dsd_tile_fetch_cancel_create();
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
    {
        std::lock_guard<std::mutex> lock(m_core->mutex);
        m_core->stop = true;
        m_core->receiver = nullptr;
        m_core->pending.clear();
        m_core->network.clear();
    }
    dsd_tile_fetch_cancel_request(m_core->cancel);
    m_core->wake.notify_all();
    /* The thread was detached when it started; it holds its own reference to the core. */
}

bool
MapTileWorker::ensureThread() {
    if (m_threadStarted) {
        return true;
    }
    g_liveThreads.fetch_add(1);
    try {
        std::thread(&Core::run, m_core).detach();
    } catch (const std::system_error&) {
        g_liveThreads.fetch_sub(1);
        return false;
    }
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
MapTileWorker::requestTiles(const QList<MapTileKey>& keys) {
    if (idleState() != State::Ready || !m_threadStarted) {
        return m_generation;
    }
    {
        std::lock_guard<std::mutex> lock(m_core->mutex);
        m_core->generation++;
        m_core->pending.clear();
        m_core->network.clear();
        bool keepInflight = false;
        for (const MapTileKey& key : keys) {
            if (!key.valid()) {
                continue;
            }
            if (m_core->inflight && key == m_core->inflightKey) {
                keepInflight = true;
                continue;
            }
            if (std::find(m_core->pending.cbegin(), m_core->pending.cend(), key) == m_core->pending.cend()) {
                m_core->pending.push_back(key);
            }
        }
        if (keepInflight) {
            m_core->inflightGeneration = m_core->generation;
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
MapTileWorker::deliverTile(quint64 gen, const MapTileKey& key, const QImage& image, bool stale) {
    /* A tile from before tiles were turned off or the template changed belongs to another source. */
    if (!m_enabled || gen < m_epochGeneration) {
        return;
    }
    Q_EMIT tileReady(gen, key.z, key.x, key.y, image, stale);
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
    int counts[3] = {0, 0, 0};
    if (!scan_placeholders(rest, counts) || counts[0] == 0 || counts[1] == 0 || counts[2] == 0
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

#ifdef DSD_NEO_TEST_HOOKS
void
MapTileWorker::wakeForTest() {
    m_core->wake.notify_all();
}

qint64
MapTileWorker::backoffRemainingMsForTest() const {
    std::lock_guard<std::mutex> lock(m_core->mutex);
    const quint64 now = m_core->monoMs();
    return m_core->backoffUntilMs > now ? static_cast<qint64>(m_core->backoffUntilMs - now) : 0;
}
#endif

} // namespace dsd_qt
