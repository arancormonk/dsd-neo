// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "map_tile_cache.h"

#include <QBuffer>
#include <QChar>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QLatin1String>
#include <QPair>
#include <QSaveFile>
#include <QSize>
#include <QStandardPaths>
#include <QStringList>
#include <algorithm>
#include <dsd-neo/runtime/tile_fetch.h>
#include <qflags.h>
#include <utility>
#include <vector>

#include "map_tile_http.h"
#include "realtime_clock.h"

namespace dsd_qt {

namespace {

/* Header fields the cache stores; Retry-After belongs to a failure, not to a stored response. */
constexpr unsigned int kStoredFields = DSD_TILE_FETCH_HDR_ETAG | DSD_TILE_FETCH_HDR_LAST_MODIFIED
                                       | DSD_TILE_FETCH_HDR_CACHE_CONTROL | DSD_TILE_FETCH_HDR_EXPIRES
                                       | DSD_TILE_FETCH_HDR_DATE | DSD_TILE_FETCH_HDR_AGE | DSD_TILE_FETCH_HDR_VARY;
constexpr unsigned int kFreshnessFields = DSD_TILE_FETCH_HDR_EXPIRES | DSD_TILE_FETCH_HDR_DATE | DSD_TILE_FETCH_HDR_AGE;

/* File layout: magic, metadata length (4 bytes, big-endian), metadata JSON, PNG. */
constexpr char kMagic[] = "DSDTILE1";
constexpr int kMagicLen = 8;
constexpr int kMetaLenBytes = 4;
constexpr qint64 kMetaMaxBytes = 64LL * 1024;
constexpr char kTileSuffix[] = ".tile";

QString
tile_file_name(const MapTileKey& key) {
    return QStringLiteral("%1-%2-%3%4").arg(key.z).arg(key.x).arg(key.y).arg(QLatin1String(kTileSuffix));
}

/* "<digits>-<digits>-<digits>.tile". */
bool
is_tile_file_name(const QString& name) {
    const QString suffix = QLatin1String(kTileSuffix);
    if (!name.endsWith(suffix)) {
        return false;
    }
    const QStringList parts = name.left(name.size() - suffix.size()).split(QLatin1Char('-'));
    if (parts.size() != 3) {
        return false;
    }
    return std::all_of(parts.cbegin(), parts.cend(), [](const QString& part) {
        return !part.isEmpty() && part.size() <= 10
               && std::all_of(part.cbegin(), part.cend(), [](QChar c) { return c.isDigit() && c.unicode() < 128; });
    });
}

bool
is_namespace_name(const QString& name) {
    return name.size() == 32 && std::all_of(name.cbegin(), name.cend(), [](QChar c) {
               return (c >= QLatin1Char('0') && c <= QLatin1Char('9'))
                      || (c >= QLatin1Char('a') && c <= QLatin1Char('f'));
           });
}

QJsonObject
meta_to_json(const MapTileMeta& meta) {
    QJsonObject o;
    o.insert(QStringLiteral("etag"), QString::fromLatin1(meta.etag));
    o.insert(QStringLiteral("lastModified"), QString::fromLatin1(meta.lastModified));
    o.insert(QStringLiteral("cacheControl"), QString::fromLatin1(meta.cacheControl));
    o.insert(QStringLiteral("expires"), QString::fromLatin1(meta.expires));
    o.insert(QStringLiteral("date"), QString::fromLatin1(meta.date));
    o.insert(QStringLiteral("age"), QString::fromLatin1(meta.age));
    o.insert(QStringLiteral("vary"), QString::fromLatin1(meta.vary));
    o.insert(QStringLiteral("present"), static_cast<qint64>(meta.present));
    o.insert(QStringLiteral("invalid"), static_cast<qint64>(meta.invalid));
    o.insert(QStringLiteral("requestTime"), meta.requestTime);
    o.insert(QStringLiteral("responseTime"), meta.responseTime);
    QJsonArray nominated;
    for (const auto& pair : meta.varyValues) {
        nominated.append(QJsonArray{QString::fromLatin1(pair.first), QString::fromLatin1(pair.second)});
    }
    o.insert(QStringLiteral("varyValues"), nominated);
    return o;
}

bool
json_text(const QJsonObject& o, const char* name, QByteArray* out) {
    const QJsonValue v = o.value(QLatin1String(name));
    if (!v.isString()) {
        return false;
    }
    *out = v.toString().toLatin1();
    return true;
}

bool
json_int(const QJsonObject& o, const char* name, qint64* out) {
    const QJsonValue v = o.value(QLatin1String(name));
    if (!v.isDouble()) {
        return false;
    }
    *out = v.toInteger();
    return true;
}

bool
json_vary_values(const QJsonObject& o, QList<QPair<QByteArray, QByteArray>>* out) {
    const QJsonValue v = o.value(QStringLiteral("varyValues"));
    if (!v.isArray()) {
        return false;
    }
    const QJsonArray array = v.toArray();
    for (const auto& item : array) {
        const QJsonArray pair = item.toArray();
        if (pair.size() != 2 || !pair.at(0).isString() || !pair.at(1).isString()) {
            return false;
        }
        out->append(qMakePair(pair.at(0).toString().toLatin1(), pair.at(1).toString().toLatin1()));
    }
    return true;
}

bool
meta_from_json(const QJsonObject& o, MapTileMeta* meta) {
    qint64 present = 0;
    qint64 invalid = 0;
    const bool texts = json_text(o, "etag", &meta->etag) && json_text(o, "lastModified", &meta->lastModified)
                       && json_text(o, "cacheControl", &meta->cacheControl) && json_text(o, "expires", &meta->expires)
                       && json_text(o, "date", &meta->date) && json_text(o, "age", &meta->age)
                       && json_text(o, "vary", &meta->vary);
    const bool numbers = json_int(o, "present", &present) && json_int(o, "invalid", &invalid)
                         && json_int(o, "requestTime", &meta->requestTime)
                         && json_int(o, "responseTime", &meta->responseTime);
    if (!texts || !numbers || !json_vary_values(o, &meta->varyValues)) {
        return false;
    }
    meta->present = static_cast<unsigned int>(present) & kStoredFields;
    meta->invalid = static_cast<unsigned int>(invalid) & kStoredFields;
    return true;
}

/* Split a stored file into its metadata and PNG. */
bool
split_file(const QByteArray& bytes, MapTileMeta* meta, QByteArray* png) {
    if (bytes.size() < kMagicLen + kMetaLenBytes || !bytes.startsWith(QByteArray(kMagic, kMagicLen))) {
        return false;
    }
    qint64 metaLen = 0;
    for (int i = 0; i < kMetaLenBytes; ++i) {
        metaLen = (metaLen << 8) | static_cast<unsigned char>(bytes.at(kMagicLen + i));
    }
    const qint64 metaStart = kMagicLen + kMetaLenBytes;
    if (metaLen <= 0 || metaLen > kMetaMaxBytes || metaStart + metaLen >= bytes.size()) {
        return false;
    }
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes.mid(metaStart, metaLen), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        return false;
    }
    if (meta != nullptr && !meta_from_json(doc.object(), meta)) {
        return false;
    }
    *png = bytes.mid(metaStart + metaLen);
    return !png->isEmpty();
}

QByteArray
join_file(const MapTileMeta& meta, const QByteArray& png) {
    const QByteArray json = QJsonDocument(meta_to_json(meta)).toJson(QJsonDocument::Compact);
    QByteArray out;
    out.reserve(kMagicLen + kMetaLenBytes + json.size() + png.size());
    out.append(kMagic, kMagicLen);
    const quint32 len = static_cast<quint32>(json.size());
    out.append(static_cast<char>((len >> 24) & 0xFFU));
    out.append(static_cast<char>((len >> 16) & 0xFFU));
    out.append(static_cast<char>((len >> 8) & 0xFFU));
    out.append(static_cast<char>(len & 0xFFU));
    out.append(json);
    out.append(png);
    return out;
}

/* The opaque-tag of an entity tag, for the weak comparison (RFC 9110 §8.8.3.2). */
QByteArray
opaque_tag(const QByteArray& etag) {
    return etag.startsWith("W/") ? etag.mid(2) : etag;
}

bool
same_date(const QByteArray& a, const QByteArray& b, qint64 nowSecs) {
    qint64 ta = 0;
    qint64 tb = 0;
    if (map_tile_http::parseHttpDate(a, nowSecs, &ta) && map_tile_http::parseHttpDate(b, nowSecs, &tb)) {
        return ta == tb;
    }
    return a == b;
}

/* Whether a 304 selects the stored response (RFC 9111 §4.3.4): its validators, when it has any, must match. ETags are
 * compared weakly (W/ ignored) on purpose: the cache holds one response per tile, the one the request just named in
 * If-None-Match, so a weak/strong difference on the same opaque tag does not pick between stored responses. */
bool
validators_select(const MapTileMeta& stored, const dsd_tile_fetch_response& r, qint64 nowSecs) {
    const auto sent_valid = [&r](unsigned int bit) {
        return (r.header_present & bit) != 0U && (r.header_invalid & bit) == 0U;
    };
    if (sent_valid(DSD_TILE_FETCH_HDR_ETAG)) {
        return !stored.etag.isEmpty() && opaque_tag(stored.etag) == opaque_tag(QByteArray(r.etag));
    }
    if (sent_valid(DSD_TILE_FETCH_HDR_LAST_MODIFIED)) {
        return !stored.lastModified.isEmpty() && same_date(stored.lastModified, QByteArray(r.last_modified), nowSecs);
    }
    return true;
}

QList<QPair<QByteArray, QByteArray>>
nominated_values(const QByteArray& vary, const MapTileRequestFields& sent) {
    QList<QPair<QByteArray, QByteArray>> values;
    bool star = false;
    const QList<QByteArray> names = map_tile_http::parseVary(vary, &star);
    if (!star) {
        for (const QByteArray& name : names) {
            values.append(qMakePair(name, sent.value(name)));
        }
    }
    return values;
}

/* Whether the stored nominated values equal what the app sends now (RFC 9111 §4.1). */
bool
vary_matches(const MapTileMeta& meta, const MapTileRequestFields& sent) {
    bool star = false;
    const QList<QByteArray> names = map_tile_http::parseVary(meta.vary, &star);
    if (star) {
        return false;
    }
    for (const QByteArray& name : names) {
        const auto stored = std::find_if(meta.varyValues.cbegin(), meta.varyValues.cend(),
                                         [&name](const QPair<QByteArray, QByteArray>& p) { return p.first == name; });
        if (stored == meta.varyValues.cend() || stored->second != sent.value(name)) {
            return false;
        }
    }
    return true;
}

/* Replace a stored field with the 304's (RFC 9111 §3.2), or clear it when @p clearIfAbsent and the 304 lacks it. */
void
merge_field(MapTileMeta* meta, const dsd_tile_fetch_response& r, unsigned int bit, QByteArray* field, const char* value,
            bool clearIfAbsent) {
    if ((r.header_present & bit) != 0U) {
        *field = QByteArray(value);
        meta->present |= bit;
        if ((r.header_invalid & bit) != 0U) {
            meta->invalid |= bit;
        } else {
            meta->invalid &= ~bit;
        }
    } else if (clearIfAbsent) {
        field->clear();
        meta->present &= ~bit;
        meta->invalid &= ~bit;
    }
}

void
merge_not_modified(MapTileMeta* meta, const dsd_tile_fetch_response& r, qint64 requestTime, qint64 responseTime,
                   const MapTileRequestFields& sent) {
    merge_field(meta, r, DSD_TILE_FETCH_HDR_ETAG, &meta->etag, r.etag, false);
    merge_field(meta, r, DSD_TILE_FETCH_HDR_LAST_MODIFIED, &meta->lastModified, r.last_modified, false);
    merge_field(meta, r, DSD_TILE_FETCH_HDR_CACHE_CONTROL, &meta->cacheControl, r.cache_control, false);
    merge_field(meta, r, DSD_TILE_FETCH_HDR_EXPIRES, &meta->expires, r.expires, false);
    merge_field(meta, r, DSD_TILE_FETCH_HDR_VARY, &meta->vary, r.vary, false);
    merge_field(meta, r, DSD_TILE_FETCH_HDR_DATE, &meta->date, r.date, true);
    merge_field(meta, r, DSD_TILE_FETCH_HDR_AGE, &meta->age, r.age, true);
    meta->requestTime = requestTime;
    meta->responseTime = responseTime;
    meta->varyValues = nominated_values(meta->vary, sent);
}

/* Whether a response's directives forbid keeping it at all. */
bool
must_not_store(const MapTileMeta& meta) {
    const map_tile_http::CacheControl cc = map_tile_http::parseCacheControl(meta.cacheControl);
    bool star = false;
    (void)map_tile_http::parseVary(meta.vary, &star);
    return cc.noStore || cc.syntaxError || star;
}

} // namespace

bool
MapTileKey::valid() const {
    if (z < 0 || z > kMaxZoom) {
        return false;
    }
    const qint64 side = qint64{1} << z;
    return x >= 0 && y >= 0 && x < side && y < side;
}

QByteArray
MapTileRequestFields::value(const QByteArray& lowerName) const {
    if (lowerName == "user-agent") {
        return userAgent;
    }
    if (lowerName == "host") {
        return host;
    }
    if (lowerName == "accept") {
        return QByteArrayLiteral("*/*");
    }
    if (lowerName == "if-none-match") {
        return ifNoneMatch;
    }
    if (lowerName == "if-modified-since") {
        return ifModifiedSince;
    }
    return QByteArray();
}

MapTileFreshness
mapTileFreshness(const MapTileMeta& meta, qint64 nowSecs) {
    MapTileFreshness f;
    const map_tile_http::CacheControl cc = map_tile_http::parseCacheControl(meta.cacheControl);
    f.malformed = cc.syntaxError || cc.maxAgeMalformed || (meta.invalid & kFreshnessFields) != 0U;
    qint64 dateValue = meta.responseTime;
    if ((meta.present & DSD_TILE_FETCH_HDR_DATE) != 0U
        && !map_tile_http::parseHttpDate(meta.date, nowSecs, &dateValue)) {
        f.malformed = true;
    }
    qint64 ageValue = 0;
    if ((meta.present & DSD_TILE_FETCH_HDR_AGE) != 0U && !map_tile_http::parseDeltaSeconds(meta.age, &ageValue)) {
        f.malformed = true;
    }
    if (cc.hasMaxAge) {
        f.lifetime = cc.maxAge;
    } else if ((meta.present & DSD_TILE_FETCH_HDR_EXPIRES) != 0U) {
        qint64 expires = 0;
        if (map_tile_http::parseHttpDate(meta.expires, nowSecs, &expires)) {
            f.lifetime = std::max<qint64>(0, expires - dateValue);
        } else {
            f.malformed = true;
        }
    } else if (!cc.maxAgeMalformed) {
        f.lifetime = MapTileCache::kDefaultFreshnessSecs;
    }
    const qint64 apparentAge = std::max<qint64>(0, meta.responseTime - dateValue);
    const qint64 responseDelay = std::max<qint64>(0, meta.responseTime - meta.requestTime);
    const qint64 correctedInitialAge = std::max(apparentAge, ageValue + responseDelay);
    const qint64 residentTime = std::max<qint64>(0, nowSecs - meta.responseTime);
    f.currentAge = correctedInitialAge + residentTime;
    f.fresh = !f.malformed && f.lifetime > f.currentAge;
    return f;
}

MapTileMeta
mapTileMetaFromResponse(const dsd_tile_fetch_response& response, qint64 requestTime, qint64 responseTime,
                        const MapTileRequestFields& sent) {
    MapTileMeta meta;
    meta.etag = QByteArray(response.etag);
    meta.lastModified = QByteArray(response.last_modified);
    meta.cacheControl = QByteArray(response.cache_control);
    meta.expires = QByteArray(response.expires);
    meta.date = QByteArray(response.date);
    meta.age = QByteArray(response.age);
    meta.vary = QByteArray(response.vary);
    meta.present = response.header_present & kStoredFields;
    meta.invalid = response.header_invalid & kStoredFields;
    meta.requestTime = requestTime;
    meta.responseTime = responseTime;
    meta.varyValues = nominated_values(meta.vary, sent);
    return meta;
}

bool
mapTileReusable(const MapTileMeta& meta) {
    const map_tile_http::CacheControl cc = map_tile_http::parseCacheControl(meta.cacheControl);
    bool star = false;
    const QList<QByteArray> nominated = map_tile_http::parseVary(meta.vary, &star);
    /* A representation chosen by a conditional field (Vary: If-None-Match) was selected for that request only: a
     * lookup by an unconditional request does not match it, and neither may the canvas show it for another view. */
    const bool conditional = nominated.contains(QByteArrayLiteral("if-none-match"))
                             || nominated.contains(QByteArrayLiteral("if-modified-since"));
    return !(cc.noStore || cc.noCache || cc.mustRevalidate || cc.syntaxError || star || conditional);
}

QImage
mapTileDecodePng(const QByteArray& png) {
    /* The PNG signature first, so a body that is not a PNG (an HTML error page) never reaches the decoder. */
    static const char kSignature[] = {'\x89', 'P', 'N', 'G', '\r', '\n', '\x1a', '\n'};
    if (!png.startsWith(QByteArray::fromRawData(kSignature, sizeof(kSignature)))
        || static_cast<size_t>(png.size()) > DSD_TILE_FETCH_BODY_CAP_BYTES) {
        return QImage();
    }
    QBuffer buffer;
    buffer.setData(png);
    if (!buffer.open(QIODevice::ReadOnly)) {
        return QImage();
    }
    QImageReader reader(&buffer, QByteArrayLiteral("png"));
    reader.setDecideFormatFromContent(false);
    const QSize size = reader.size();
    if (!size.isValid() || size.isEmpty() || size.width() > MapTileCache::kMaxTileSide
        || size.height() > MapTileCache::kMaxTileSide) {
        return QImage();
    }
    QImage image;
    if (!reader.read(&image) || image.size() != size) {
        return QImage();
    }
    return image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

MapTileCache::MapTileCache(QString root, const QString& urlTemplate, WallClock wallSecs)
    : m_root(std::move(root)), m_wallSecs(std::move(wallSecs)) {
    if (!m_root.isEmpty()) {
        m_dir = m_root + QLatin1Char('/') + namespaceFor(urlTemplate);
    }
}

QString
MapTileCache::defaultRoot() {
    const QString cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    return cache.isEmpty() ? QString() : cache + QStringLiteral("/map-tiles");
}

QString
MapTileCache::namespaceFor(const QString& urlTemplate) {
    return QString::fromLatin1(
        QCryptographicHash::hash(urlTemplate.toUtf8(), QCryptographicHash::Sha256).toHex().left(32));
}

QString
MapTileCache::directory() const {
    return m_dir;
}

QString
MapTileCache::pathFor(const MapTileKey& key) const {
    return m_dir.isEmpty() ? QString() : m_dir + QLatin1Char('/') + tile_file_name(key);
}

qint64
MapTileCache::now() const {
    return m_wallSecs ? m_wallSecs() : realtimeSecsSinceEpoch();
}

MapTileCache::Lookup
MapTileCache::lookup(const MapTileKey& key, const MapTileRequestFields& sent, Storage storage) {
    Lookup out;
    if (!key.valid()) {
        return out;
    }
    MemoryEntry entry;
    const auto found = m_memory.constFind(key);
    if (found != m_memory.cend()) {
        entry = found.value();
        remember(key, entry);
        if (storage == Storage::DiskAndMemory && entry.persistable) {
            /* A view by hand reuses it: its file counts as used too, so the disk LRU does not evict a tile looked at
             * often in favour of one fetched once. A device-placed view touches nothing on disk. */
            renewDiskStamp(key);
        }
    } else if (storage == Storage::MemoryOnly || !loadFromDisk(key, &entry)) {
        return out;
    }
    if (must_not_store(entry.meta)) {
        drop(key, storage);
        return out;
    }
    if (!vary_matches(entry.meta, sent)) {
        return out;
    }
    out.ifNoneMatch = entry.meta.etag;
    out.ifModifiedSince = entry.meta.lastModified;
    const map_tile_http::CacheControl cc = map_tile_http::parseCacheControl(entry.meta.cacheControl);
    const bool fresh = mapTileFreshness(entry.meta, now()).fresh;
    if (cc.noCache || (!fresh && cc.mustRevalidate)) {
        out.reuse = Reuse::NeedsValidation;
    } else {
        out.reuse = fresh ? Reuse::Fresh : Reuse::StaleUsable;
        out.image = entry.image;
        out.reusable = mapTileReusable(entry.meta);
    }
    return out;
}

bool
MapTileCache::store(const MapTileKey& key, const dsd_tile_fetch_response& response, const QByteArray& png,
                    const QImage& image, qint64 requestTime, qint64 responseTime, const MapTileRequestFields& sent,
                    Storage storage, bool* reusable) {
    if (reusable != nullptr) {
        *reusable = false;
    }
    if (!key.valid() || image.isNull() || png.isEmpty()) {
        return false;
    }
    MemoryEntry entry;
    entry.image = image;
    entry.meta = mapTileMetaFromResponse(response, requestTime, responseTime, sent);
    if (reusable != nullptr) {
        *reusable = mapTileReusable(entry.meta);
    }
    if (must_not_store(entry.meta)) {
        drop(key, storage);
        return false;
    }
    /* A failed write leaves an older file, whose image this one is not: never write a later 304 over it. */
    entry.persistable = storage == Storage::DiskAndMemory && writeEntry(key, entry.meta, png);
    remember(key, entry);
    return true;
}

QImage
MapTileCache::refresh(const MapTileKey& key, const dsd_tile_fetch_response& response, qint64 requestTime,
                      qint64 responseTime, const MapTileRequestFields& sent, Storage storage, bool* reusable) {
    if (reusable != nullptr) {
        *reusable = false;
    }
    if (!key.valid()) {
        return QImage();
    }
    MemoryEntry entry;
    const auto found = m_memory.constFind(key);
    if (found != m_memory.cend()) {
        entry = found.value();
    } else if (storage == Storage::MemoryOnly || !loadFromDisk(key, &entry)) {
        return QImage();
    }
    if (!validators_select(entry.meta, response, now())) {
        drop(key, storage);
        return QImage();
    }
    merge_not_modified(&entry.meta, response, requestTime, responseTime, sent);
    if (reusable != nullptr) {
        *reusable = mapTileReusable(entry.meta);
    }
    if (must_not_store(entry.meta)) {
        drop(key, storage);
        return entry.image;
    }
    /* Metadata a device-placed view merged (its Date, Expires, ETag) is never written, now or by a later 304. */
    entry.persistable = entry.persistable && storage == Storage::DiskAndMemory;
    remember(key, entry);
    QByteArray png;
    if (entry.persistable && readPng(key, &png)) {
        (void)writeEntry(key, entry.meta, png);
    }
    return entry.image;
}

void
MapTileCache::remove(const MapTileKey& key) {
    forget(key);
    const QString path = pathFor(key);
    if (!path.isEmpty()) {
        dropFile(path);
    }
}

void
MapTileCache::invalidate(const MapTileKey& key, Storage storage) {
    drop(key, storage);
}

void
MapTileCache::drop(const MapTileKey& key, Storage storage) {
    if (storage == Storage::MemoryOnly) {
        forget(key);
    } else {
        remove(key);
    }
}

qint64
MapTileCache::diskBytes() {
    ensureScanned();
    return m_diskBytes;
}

bool
MapTileCache::inMemory(const MapTileKey& key) const {
    return m_memory.contains(key);
}

int
MapTileCache::memoryCount() const {
    return static_cast<int>(m_memory.size());
}

void
MapTileCache::setDiskCapForTest(qint64 bytes) {
    m_diskCap = bytes;
}

bool
MapTileCache::loadFromDisk(const MapTileKey& key, MemoryEntry* entry) {
    const QString path = pathFor(key);
    if (path.isEmpty()) {
        return false;
    }
    QFile file(path);
    /* ExistingOnly: a miss must not create an empty file (two directory writes, and a stray file if interrupted). */
    if (!file.open(QIODevice::ReadWrite | QIODevice::ExistingOnly)) {
        /* An entry that exists but cannot be opened (its permissions damaged) would fail every lookup: drop it. */
        if (file.exists()) {
            ensureScanned();
            dropFile(path);
        }
        return false;
    }
    const qint64 cap = kMagicLen + kMetaLenBytes + kMetaMaxBytes + static_cast<qint64>(DSD_TILE_FETCH_BODY_CAP_BYTES);
    const QByteArray bytes = file.read(cap + 1);
    QByteArray png;
    MapTileMeta meta;
    QImage image;
    if (bytes.size() <= cap && split_file(bytes, &meta, &png)) {
        image = mapTileDecodePng(png);
    }
    if (image.isNull()) {
        file.close();
        ensureScanned();
        dropFile(path);
        return false;
    }
    const qint64 stamp = now();
    (void)file.setFileTime(QDateTime::fromSecsSinceEpoch(stamp), QFileDevice::FileModificationTime);
    file.close();
    ensureScanned();
    noteFile(path, bytes.size(), stamp);
    entry->image = std::move(image);
    entry->meta = std::move(meta);
    entry->persistable = true;
    remember(key, *entry);
    return true;
}

void
MapTileCache::renewDiskStamp(const MapTileKey& key) {
    const QString path = pathFor(key);
    if (path.isEmpty()) {
        return;
    }
    ensureScanned();
    const auto found = m_disk.constFind(QDir::cleanPath(path));
    const qint64 stamp = now();
    /* Coarse on purpose: a pan re-asks every tile in view, and one write an hour per tile is enough for the LRU. */
    if (found == m_disk.cend() || stamp - found.value().stamp < kRecencyStepSecs) {
        return;
    }
    const qint64 bytes = found.value().bytes;
    QFile file(path);
    if (!file.open(QIODevice::ReadWrite | QIODevice::ExistingOnly)
        || !file.setFileTime(QDateTime::fromSecsSinceEpoch(stamp), QFileDevice::FileModificationTime)) {
        return;
    }
    file.close();
    noteFile(path, bytes, stamp);
}

bool
MapTileCache::readPng(const MapTileKey& key, QByteArray* png) const {
    QFile file(pathFor(key));
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const qint64 cap = kMagicLen + kMetaLenBytes + kMetaMaxBytes + static_cast<qint64>(DSD_TILE_FETCH_BODY_CAP_BYTES);
    const QByteArray bytes = file.read(cap + 1);
    return bytes.size() <= cap && split_file(bytes, nullptr, png);
}

bool
MapTileCache::writeEntry(const MapTileKey& key, const MapTileMeta& meta, const QByteArray& png) {
    if (m_dir.isEmpty() || !QDir().mkpath(m_dir)) {
        return false;
    }
    ensureScanned();
    const QString path = pathFor(key);
    const QByteArray bytes = join_file(meta, png);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        file.cancelWriting();
        return false;
    }
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        file.cancelWriting();
        return false;
    }
    /* The LRU stamp is the file's modification time, on the cache's clock like every renewal. */
    const qint64 stamp = now();
    QFile written(path);
    if (written.open(QIODevice::ReadWrite)) {
        (void)written.setFileTime(QDateTime::fromSecsSinceEpoch(stamp), QFileDevice::FileModificationTime);
    }
    noteFile(path, bytes.size(), stamp);
    evictToCap();
    return true;
}

void
MapTileCache::remember(const MapTileKey& key, const MemoryEntry& entry) {
    m_memory.insert(key, entry);
    m_memoryOrder.removeOne(key);
    m_memoryOrder.prepend(key);
    while (m_memoryOrder.size() > kMemoryTiles) {
        m_memory.remove(m_memoryOrder.takeLast());
    }
}

void
MapTileCache::forget(const MapTileKey& key) {
    m_memory.remove(key);
    m_memoryOrder.removeOne(key);
}

void
MapTileCache::ensureScanned() {
    if (m_scanned || m_root.isEmpty()) {
        return;
    }
    m_scanned = true;
    const QFileInfoList dirs = QDir(m_root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
    for (const QFileInfo& info : dirs) {
        if (is_namespace_name(info.fileName())) {
            scanNamespace(info.absoluteFilePath());
        }
    }
}

void
MapTileCache::scanNamespace(const QString& dirPath) {
    const QFileInfoList files =
        QDir(dirPath).entryInfoList(QDir::Files | QDir::Hidden | QDir::System | QDir::NoSymLinks);
    for (const QFileInfo& info : files) {
        if (is_tile_file_name(info.fileName())) {
            noteFile(info.absoluteFilePath(), info.size(), info.lastModified().toSecsSinceEpoch());
        } else {
            /* A temporary file a killed write left behind, or anything else that is not an entry. */
            (void)QFile::remove(info.absoluteFilePath());
        }
    }
}

void
MapTileCache::noteFile(const QString& path, qint64 bytes, qint64 stamp) {
    const QString canonical = QDir::cleanPath(path);
    const auto found = m_disk.constFind(canonical);
    if (found != m_disk.cend()) {
        m_diskBytes -= found.value().bytes;
    }
    DiskRecord record;
    record.bytes = bytes;
    record.stamp = stamp;
    m_disk.insert(canonical, record);
    m_diskBytes += bytes;
}

void
MapTileCache::dropFile(const QString& path) {
    const QString canonical = QDir::cleanPath(path);
    (void)QFile::remove(canonical);
    const auto found = m_disk.constFind(canonical);
    if (found != m_disk.cend()) {
        m_diskBytes -= found.value().bytes;
        m_disk.remove(canonical);
    }
}

void
MapTileCache::evictToCap() {
    if (m_diskBytes <= m_diskCap) {
        return;
    }
    std::vector<std::pair<qint64, QString>> order;
    order.reserve(static_cast<size_t>(m_disk.size()));
    for (auto it = m_disk.cbegin(); it != m_disk.cend(); ++it) {
        order.emplace_back(it.value().stamp, it.key());
    }
    std::sort(order.begin(), order.end());
    for (const auto& item : order) {
        if (m_diskBytes <= m_diskCap) {
            break;
        }
        dropFile(item.second);
    }
}

} // namespace dsd_qt
