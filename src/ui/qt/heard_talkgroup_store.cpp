// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "heard_talkgroup_store.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QJsonObject>
#include <QJsonValue>
#include <QLatin1String>
#include <QPair>
#include <QStringList>
#include <Qt>
#include <algorithm>
#include <mutex>
#include <tuple>
#include <utility>

#include <dsd-neo/core/heard_talkgroup.h>
#include <dsd-neo/runtime/log.h>

#include "json_store.h"

namespace dsd_qt {

namespace {

constexpr int kMaxVariant = 255;
constexpr qint64 kMaxId = 0xFFFFFFFFLL;

quint64
entry_key(int variant, quint32 id) {
    return (static_cast<quint64>(static_cast<quint32>(variant)) << 32) | id;
}

int
key_variant(quint64 key) {
    return static_cast<int>(key >> 32);
}

quint32
key_id(quint64 key) {
    return static_cast<quint32>(key & 0xFFFFFFFFULL);
}

QJsonArray
entries_to_json(QVector<HeardTalkgroupStore::Entry> entries, const QJsonArray& kept) {
    std::sort(entries.begin(), entries.end(),
              [](const HeardTalkgroupStore::Entry& a, const HeardTalkgroupStore::Entry& b) {
                  return std::tie(a.owner, a.variant, a.id) < std::tie(b.owner, b.variant, b.id);
              });
    QJsonArray array;
    for (const HeardTalkgroupStore::Entry& entry : entries) {
        QJsonObject obj;
        obj.insert(QLatin1String("v"), 1);
        obj.insert(QLatin1String("owner"), entry.owner);
        obj.insert(QLatin1String("variant"), entry.variant);
        obj.insert(QLatin1String("id"), static_cast<qint64>(entry.id));
        obj.insert(QLatin1String("first"), entry.first);
        obj.insert(QLatin1String("last"), entry.last);
        array.append(obj);
    }
    for (const auto& value : kept) {
        array.append(value);
    }
    return array;
}

#ifdef DSD_NEO_TEST_HOOKS
std::function<void()>&
before_write_hook() {
    static std::function<void()> hook;
    return hook;
}
#endif

} // namespace

HeardTalkgroupStore::HeardTalkgroupStore(const QString& path, QObject* parent)
    : QObject(parent), m_fileName(path), m_shared(std::make_shared<Shared>()) {
    // One worker: two writes of one file never overlap, and they land in the order they were queued.
    m_pool.setMaxThreadCount(1);
    load();
    // As call history's: a quit ends the event loop the UI poll needs, and Android may kill a process off screen.
    if (QCoreApplication* app = QCoreApplication::instance()) {
        connect(app, &QCoreApplication::aboutToQuit, this, &HeardTalkgroupStore::flushNow);
    }
    if (QGuiApplication* app = qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
        connect(app, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
            if (state == Qt::ApplicationSuspended || state == Qt::ApplicationHidden) {
                flushNow();
            }
        });
    }
}

HeardTalkgroupStore::~HeardTalkgroupStore() {
    // The worker must not outlive this object; what is still owed after it is written here.
    m_pool.waitForDone();
    if (m_loaded && dirty() && !json_store_save_array(m_fileName, entries_to_json(flatEntries(), m_kept))) {
        LOG_WARN("%s could not be saved at teardown; the latest remembered talkgroups are lost.\n",
                 qUtf8Printable(m_fileName));
    }
}

void
HeardTalkgroupStore::load() {
    JsonStoreLoad status = JsonStoreLoad::Unreadable;
    const QJsonArray array = json_store_load_array(m_fileName, &status);
    m_loaded = status != JsonStoreLoad::Unreadable;
    if (!m_loaded) {
        // json_store has logged why and posted the notice. The file stays as it is: nothing replaces it this run.
        LOG_WARN("Remembered talkgroups are not learnt this session: %s could not be read.\n",
                 qUtf8Printable(m_fileName));
        return;
    }
    int dropped = 0;
    for (const auto& value : array) {
        if (!value.isObject() || value.toObject().value(QLatin1String("v")).toInteger(-1) != 1) {
            m_kept.append(value);
        } else if (!takeStored(value)) {
            dropped++;
        }
    }
    for (OwnerEntries& entries : m_owners) {
        (void)evictOverCap(entries);
    }
    if (dropped > 0) {
        LOG_WARN("%s: %d remembered talkgroup entries were not valid and are dropped.\n", qUtf8Printable(m_fileName),
                 dropped);
    }
}

bool
HeardTalkgroupStore::takeStored(const QJsonValue& value) {
    const QJsonObject obj = value.toObject();
    const QString owner = obj.value(QLatin1String("owner")).toString();
    const qint64 variant = obj.value(QLatin1String("variant")).toInteger(-1);
    const qint64 id = obj.value(QLatin1String("id")).toInteger(-1);
    const qint64 first = obj.value(QLatin1String("first")).toInteger(-1);
    const qint64 last = obj.value(QLatin1String("last")).toInteger(-1);
    if (owner.isEmpty() || variant < 1 || variant > kMaxVariant || id < 1 || id > kMaxId || first < 0 || last < first) {
        return false;
    }
    OwnerEntries& entries = m_owners[owner];
    const quint64 key = entry_key(static_cast<int>(variant), static_cast<quint32>(id));
    const auto it = entries.find(key);
    if (it == entries.end()) {
        entries.insert(key, {first, last});
    } else {
        it->first = qMin(it->first, first);
        it->last = qMax(it->last, last);
    }
    return true;
}

QVector<quint64>
HeardTalkgroupStore::evictOverCap(OwnerEntries& entries) {
    QVector<quint64> evicted;
    const qsizetype excess = entries.size() - kMaxEntriesPerOwner;
    if (excess <= 0) {
        return evicted;
    }
    // Oldest last heard first, ties to the lower variant, then the lower id: the key orders by variant, then id.
    QVector<QPair<qint64, quint64>> order;
    order.reserve(entries.size());
    for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
        order.append(qMakePair(it->last, it.key()));
    }
    std::nth_element(order.begin(), order.begin() + (excess - 1), order.end());
    for (qsizetype i = 0; i < excess; ++i) {
        entries.remove(order.at(i).second);
        evicted.append(order.at(i).second);
    }
    return evicted;
}

bool
HeardTalkgroupStore::loaded() const {
    return m_loaded;
}

void
HeardTalkgroupStore::setBeforeEdit(std::function<void()> hook) {
    m_beforeEdit = std::move(hook);
}

void
HeardTalkgroupStore::setOwnerPolicy(std::function<bool(const QString&)> policy) {
    m_policy = std::move(policy);
}

bool
HeardTalkgroupStore::remembering(const QString& ownerUid) const {
    return m_loaded && !ownerUid.isEmpty() && m_policy && m_policy(ownerUid);
}

void
HeardTalkgroupStore::observe(const QString& ownerUid, int variant, quint32 id, qint64 whenSecs) {
    if (variant < 1 || variant > kMaxVariant || id == 0 || whenSecs < 0 || !remembering(ownerUid)) {
        return;
    }
    OwnerEntries& entries = m_owners[ownerUid];
    const quint64 key = entry_key(variant, id);
    const auto it = entries.find(key);
    if (it == entries.end()) {
        entries.insert(key, {whenSecs, whenSecs});
        QVector<quint64> touched = evictOverCap(entries);
        // A late observation older than every kept one is what the cap evicts: nothing changed.
        if (entries.contains(key)) {
            touched.append(key);
            changed(ownerUid, touched);
        }
        return;
    }
    if (whenSecs >= it->first && whenSecs <= it->last) {
        return;
    }
    it->first = qMin(it->first, whenSecs);
    it->last = qMax(it->last, whenSecs);
    changed(ownerUid, {key});
}

quint64
HeardTalkgroupStore::entryKey(int variant, quint32 id) {
    return entry_key(variant, id);
}

int
HeardTalkgroupStore::entryVariant(quint64 key) {
    return key_variant(key);
}

quint32
HeardTalkgroupStore::entryId(quint64 key) {
    return key_id(key);
}

bool
HeardTalkgroupStore::entryLast(const QString& ownerUid, quint64 key, qint64* last) const {
    const auto owner = m_owners.constFind(ownerUid);
    if (owner == m_owners.cend()) {
        return false;
    }
    const auto it = owner->constFind(key);
    if (it == owner->cend()) {
        return false;
    }
    *last = it->last;
    return true;
}

void
HeardTalkgroupStore::visitEntries(const QString& ownerUid, const std::function<void(quint64, qint64)>& visit) const {
    const auto owner = m_owners.constFind(ownerUid);
    if (owner == m_owners.cend()) {
        return;
    }
    for (auto it = owner->cbegin(); it != owner->cend(); ++it) {
        visit(it.key(), it->last);
    }
}

QVector<HeardTalkgroupStore::Entry>
HeardTalkgroupStore::entriesFor(const QString& ownerUid) const {
    QVector<Entry> out;
    const auto owner = m_owners.constFind(ownerUid);
    if (owner == m_owners.cend()) {
        return out;
    }
    out.reserve(owner->size());
    for (auto it = owner->cbegin(); it != owner->cend(); ++it) {
        out.append({ownerUid, key_variant(it.key()), key_id(it.key()), it->first, it->last});
    }
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
        return std::make_tuple(-a.last, a.variant, a.id) < std::make_tuple(-b.last, b.variant, b.id);
    });
    return out;
}

quint32
HeardTalkgroupStore::policyId(int variant, quint32 id) {
    return variant == DSD_HEARD_TG_NXDN_D ? (id & 0x7FFU) : id;
}

void
HeardTalkgroupStore::forget(const QString& ownerUid, quint32 id) {
    const auto numbered = [id](quint64 key) { return policyId(key_variant(key), key_id(key)) == id; };
    const auto held = m_owners.constFind(ownerUid);
    if (!m_loaded || held == m_owners.constEnd() || std::none_of(held->keyBegin(), held->keyEnd(), numbered)) {
        return;
    }
    // Before the removal: what the hook folds may observe the id again, and the forget then takes it too.
    if (m_beforeEdit) {
        m_beforeEdit();
    }
    const auto owner = m_owners.find(ownerUid);
    if (owner == m_owners.end()) {
        return;
    }
    const qsizetype removed =
        owner->removeIf([&numbered](const OwnerEntries::iterator& it) { return numbered(it.key()); });
    if (removed == 0) {
        return;
    }
    if (owner->isEmpty()) {
        m_owners.erase(owner);
    }
    changed(ownerUid);
    queueWrite();
}

void
HeardTalkgroupStore::clear(const QString& ownerUid) {
    if (!m_loaded || !m_owners.contains(ownerUid)) {
        return;
    }
    if (m_beforeEdit) {
        m_beforeEdit();
    }
    if (m_owners.remove(ownerUid) == 0) {
        return;
    }
    changed(ownerUid);
    queueWrite();
}

void
HeardTalkgroupStore::purgeMissingOwners(const QSet<QString>& liveUids) {
    if (!m_loaded) {
        return;
    }
    QStringList gone;
    for (auto it = m_owners.cbegin(); it != m_owners.cend(); ++it) {
        if (!liveUids.contains(it.key())) {
            gone.append(it.key());
        }
    }
    if (gone.isEmpty()) {
        return;
    }
    for (const QString& owner : gone) {
        m_owners.remove(owner);
        changed(owner);
    }
    queueWrite();
}

void
HeardTalkgroupStore::changed(const QString& ownerUid, const QVector<quint64>& entries) {
    m_generation++;
    Q_EMIT ownerChanged(ownerUid, entries);
}

bool
HeardTalkgroupStore::dirty() const {
    return m_shared->written.load() != m_generation;
}

void
HeardTalkgroupStore::flushIfDue(qint64 realtimeMonoMs) {
    if (!m_loaded || !dirty() || (m_lastFlushMs >= 0 && realtimeMonoMs - m_lastFlushMs < kFlushIntervalMs)) {
        return;
    }
    if (m_queuedGeneration == m_generation && m_shared->pending.load() > 0) {
        return; // the write in the queue already carries the latest state
    }
    m_lastFlushMs = realtimeMonoMs;
    queueWrite();
}

void
HeardTalkgroupStore::flushNow() {
    if (!m_loaded || !dirty()) {
        return;
    }
    if (m_queuedGeneration == m_generation && m_shared->pending.load() > 0) {
        return;
    }
    queueWrite();
}

void
HeardTalkgroupStore::waitForWrites() {
    m_pool.waitForDone();
}

QVector<HeardTalkgroupStore::Entry>
HeardTalkgroupStore::flatEntries() const {
    QVector<Entry> entries;
    for (auto owner = m_owners.cbegin(); owner != m_owners.cend(); ++owner) {
        for (auto it = owner->cbegin(); it != owner->cend(); ++it) {
            entries.append({owner.key(), key_variant(it.key()), key_id(it.key()), it->first, it->last});
        }
    }
    return entries;
}

void
HeardTalkgroupStore::queueWrite() {
    if (!m_loaded) {
        return; // never: an unreadable file is not replaced
    }
    m_queuedGeneration = m_generation;
    // A flat copy the worker owns, built here; its JSON is built on the worker. m_kept is never edited after load,
    // so the worker's copy shares its data with nothing that changes. It replaces a state queued earlier that no
    // write has taken yet: the write waiting behind the running one writes only the newest.
    QVector<Entry> entries = flatEntries();
    {
        const std::lock_guard<std::mutex> locker(m_shared->lock);
        m_shared->entries = std::move(entries);
        m_shared->kept = m_kept;
        m_shared->generation = m_generation;
        m_shared->hasState = true;
        if (m_shared->jobWaiting) {
            return;
        }
        m_shared->jobWaiting = true;
    }
    m_shared->pending.fetch_add(1);
    m_pool.start([shared = m_shared, fileName = m_fileName]() { runWrite(shared, fileName); });
}

bool
HeardTalkgroupStore::takeQueued(Shared& shared, QVector<Entry>* entries, QJsonArray* kept, quint64* generation) {
    const std::lock_guard<std::mutex> locker(shared.lock);
    shared.jobWaiting = false;
    if (!shared.hasState) {
        return false;
    }
    shared.hasState = false;
    *entries = std::exchange(shared.entries, QVector<Entry>());
    *kept = std::exchange(shared.kept, QJsonArray());
    *generation = shared.generation;
    return true;
}

void
HeardTalkgroupStore::runWrite(const std::shared_ptr<Shared>& shared, const QString& fileName) {
    QVector<Entry> entries;
    QJsonArray kept;
    quint64 generation = 0;
    if (takeQueued(*shared, &entries, &kept, &generation)) {
#ifdef DSD_NEO_TEST_HOOKS
        if (before_write_hook()) {
            before_write_hook()();
        }
#endif
        if (json_store_save_array(fileName, entries_to_json(std::move(entries), kept))) {
            // One write runs at a time and each takes the newest state queued, so generations only grow.
            shared->written.store(generation);
        } else {
            LOG_WARN("%s could not be saved; it is retried.\n", qUtf8Printable(fileName));
        }
    }
    shared->pending.fetch_sub(1);
}

#ifdef DSD_NEO_TEST_HOOKS
void
HeardTalkgroupStore::setBeforeWriteForTest(std::function<void()> hook) {
    before_write_hook() = std::move(hook);
}
#endif

} // namespace dsd_qt
