// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The map's tile cache: RFC 9111 private-cache rules, 64 decoded tiles in memory, 50 MB on disk (issue #800).
 *
 * Entries live under `<CacheLocation>/map-tiles/<hash of the URL template>/<z>-<x>-<y>.tile`, one file each holding
 * the response's metadata and its PNG, written whole through QSaveFile so a crash never pairs an image with another
 * response's validators. The directory is named by a hash, never by the template, which may carry a key. An entry
 * that cannot be read back (torn, foreign, not a PNG within 512×512) is deleted. The disk budget is one LRU across
 * every template's directory, stamped by the file's modification time, which a disk hit by hand renews (a memory hit
 * by hand too, at most hourly). CacheLocation is not in the backup allowlist (backup_archive.h), so tiles are never
 * backed up.
 *
 * A view the device's position placed (Locate me, Follow my position, a fit that took in the device's dot) must
 * leave no trace on disk: its lookups, stores and 304 merges take Storage::MemoryOnly. They never read, write, stamp
 * or delete a file (a read alone can stamp the access time), and an entry such a view stored or refreshed is never
 * written later either, until a 200 fetched by hand replaces it.
 *
 * Freshness follows RFC 9111 for a private cache, on the real-time wall clock (HTTP dates are wall time):
 * - lifetime is `max-age`, else `Expires − Date`; with neither the lifetime is seven days (the OpenStreetMap tile
 *   policy's floor). A malformed `max-age`, `Expires`, `Date` or `Age`, or conflicting `max-age` values, make the
 *   entry stale, never fresh (§4.2.1, §5.3);
 * - age is the §4.2.3 corrected age plus the time since it was received;
 * - `no-store`: never written to disk or kept decoded in memory, and it removes the older entry (§5.2.2.5);
 * - `no-cache` (qualified or not; §5.2.2.4 lets a cache treat the qualified form as unqualified): stored, but every
 *   reuse needs a successful revalidation first, even while fresh;
 * - `must-revalidate`: never served once stale (§5.2.2.2). Any other stale entry may be shown while one
 *   revalidation runs;
 * - `Vary: *` is never stored. Otherwise the nominated request fields' values as sent (empty when the app does not
 *   send the field, such as Accept-Encoding) are stored and compared on reuse (§4.1); a mismatch means the entry is
 *   not reused and the request goes out unconditionally;
 * - a 304 updates the stored metadata (§3.2, §4.3.4), the nominated values included, and the entry's times. Date and
 *   Age describe the message that carried them, so a 304 without them clears the stored ones (a recipient records a
 *   missing Date as the time it received the message, RFC 9110 §6.6.1).
 *
 * Not thread-safe: the tile worker's thread owns it. It never logs: a tile's coordinates are the viewed area.
 */

#ifndef DSD_NEO_SRC_UI_QT_MAP_TILE_CACHE_H_
#define DSD_NEO_SRC_UI_QT_MAP_TILE_CACHE_H_

#include <QByteArray>
#include <QHash>
#include <QHashFunctions>
#include <QImage>
#include <QList>
#include <QString>
#include <QtGlobal>
#include <dsd-neo/runtime/tile_fetch.h>
#include <functional>
#include <stddef.h>

namespace dsd_qt {

/** @brief One Web Mercator tile: zoom @c z, column @c x, row @c y. */
struct MapTileKey {
    /** Deepest zoom accepted; 2^30 columns still fit an int. */
    static constexpr int kMaxZoom = 30;

    int z = 0;
    int x = 0;
    int y = 0;

    /** @brief Whether z is within [0, kMaxZoom] and x and y within [0, 2^z). */
    bool valid() const;
};

inline bool
operator==(const MapTileKey& a, const MapTileKey& b) {
    return a.z == b.z && a.x == b.x && a.y == b.y;
}

inline bool
operator!=(const MapTileKey& a, const MapTileKey& b) {
    return !(a == b);
}

inline size_t
qHash(const MapTileKey& key, size_t seed = 0) {
    return qHashMulti(seed, key.z, key.x, key.y);
}

/** @brief The request header fields as the app sends them, for Vary (RFC 9111 §4.1). */
struct MapTileRequestFields {
    QByteArray userAgent;
    QByteArray host;
    /** The conditional request's validators, as sent (empty for an unconditional request). */
    QByteArray ifNoneMatch;
    QByteArray ifModifiedSince;

    /**
     * @brief The value sent for @p lowerName: the User-Agent, the URL's host, libcurl's default Accept (any media
     *        type), If-None-Match and If-Modified-Since as sent, and empty for any field the app does not send
     *        (Accept-Encoding among them).
     */
    QByteArray value(const QByteArray& lowerName) const;
};

/** @brief What the cache keeps about a stored response: its cache-relevant header fields and its times. */
struct MapTileMeta {
    QByteArray etag;
    QByteArray lastModified;
    QByteArray cacheControl;
    QByteArray expires;
    QByteArray date;
    QByteArray age;
    QByteArray vary;
    /** DSD_TILE_FETCH_HDR_* bits: the field was sent, and the field was sent but could not be carried. */
    unsigned int present = 0U;
    unsigned int invalid = 0U;
    /** Wall-clock seconds when the request went out and when its response arrived. */
    qint64 requestTime = 0;
    qint64 responseTime = 0;
    /** Vary's nominated request fields, lowercase, with the values sent. */
    QList<QPair<QByteArray, QByteArray>> varyValues;
};

/** @brief How fresh a stored response is now. */
struct MapTileFreshness {
    bool fresh = false;
    /** A freshness field was malformed, so the entry is stale whatever the numbers say. */
    bool malformed = false;
    qint64 lifetime = 0;
    qint64 currentAge = 0;
};

/** @brief Freshness of @p meta at wall-clock @p nowSecs (RFC 9111 §4.2). */
MapTileFreshness mapTileFreshness(const MapTileMeta& meta, qint64 nowSecs);

/** @brief Build the metadata a response carries, with its times and the nominated values from @p sent. */
MapTileMeta mapTileMetaFromResponse(const dsd_tile_fetch_response& response, qint64 requestTime, qint64 responseTime,
                                    const MapTileRequestFields& sent);

/**
 * @brief Whether an image under @p meta may be shown again for another view without asking the server (the canvas's
 *        held and stand-in tiles): not for `no-store`, `no-cache`, `must-revalidate`, `Vary: *` or an unparseable
 *        Cache-Control. Such an image is shown for the presentation it was delivered for only.
 */
bool mapTileReusable(const MapTileMeta& meta);

/** @brief Decode @p png as a PNG of at most 512×512 pixels; a null image for anything else. */
QImage mapTileDecodePng(const QByteArray& png);

class MapTileCache {
  public:
    static constexpr qint64 kDiskCapBytes = 50LL * 1024 * 1024;
    static constexpr int kMemoryTiles = 64;
    static constexpr qint64 kDefaultFreshnessSecs = 7LL * 24 * 3600;
    /** A memory hit by hand renews its file's LRU stamp at most this often (seconds). */
    static constexpr qint64 kRecencyStepSecs = 3600;
    static constexpr int kMaxTileSide = 512;

    using WallClock = std::function<qint64()>;

    /** @brief Whether an operation may touch the disk. */
    enum class Storage {
        DiskAndMemory, /**< A view placed by hand: the disk is read, written and stamped. */
        MemoryOnly,    /**< A view the device's position placed: the disk is not touched at all. */
    };

    /** @brief How a lookup may reuse what is stored. */
    enum class Reuse {
        Miss,            /**< Nothing reusable: fetch unconditionally. */
        Fresh,           /**< Show the image; no request. */
        StaleUsable,     /**< Show the image while one revalidation runs. */
        NeedsValidation, /**< Show nothing until a revalidation succeeds (no-cache, or stale with must-revalidate). */
    };

    struct Lookup {
        Reuse reuse = Reuse::Miss;
        QImage image;          /**< Set for Fresh and StaleUsable. */
        bool reusable = false; /**< mapTileReusable() of the entry. */
        QByteArray ifNoneMatch;
        QByteArray ifModifiedSince;
    };

    /**
     * @param root        The cache's root, normally defaultRoot(); every template's directory sits under it.
     * @param urlTemplate The tile URL template whose tiles this cache serves.
     * @param wallSecs    Wall-clock seconds; empty reads the real-time clock.
     */
    MapTileCache(QString root, const QString& urlTemplate, WallClock wallSecs = WallClock());

    /** @brief `<CacheLocation>/map-tiles`. */
    static QString defaultRoot();

    /** @brief The directory name for @p urlTemplate: hex of its SHA-256, so the template never appears on disk. */
    static QString namespaceFor(const QString& urlTemplate);

    /** @brief This template's directory. */
    QString directory() const;

    /** @brief Look @p key up, memory first, then disk (memory only for Storage::MemoryOnly). */
    Lookup lookup(const MapTileKey& key, const MapTileRequestFields& sent, Storage storage = Storage::DiskAndMemory);

    /**
     * @brief Take a 200 response whose @p png decoded to @p image.
     *
     * @param reusable Receives mapTileReusable() of the response, when not null.
     * @return whether it was stored (in memory only for Storage::MemoryOnly). `no-store`, `Vary: *` and an
     *         unparseable Cache-Control are not, and remove the older entry (from memory only for MemoryOnly).
     */
    bool store(const MapTileKey& key, const dsd_tile_fetch_response& response, const QByteArray& png,
               const QImage& image, qint64 requestTime, qint64 responseTime, const MapTileRequestFields& sent,
               Storage storage = Storage::DiskAndMemory, bool* reusable = nullptr);

    /**
     * @brief Merge a 304 into the stored entry and return the image it validates.
     *
     * A 304 whose ETag or Last-Modified differs from the stored one selects nothing (§4.3.4): the entry is removed
     * and a null image returned, as when nothing is stored. When the merged fields say `no-store` or `Vary: *` the
     * image is returned for this one use and the entry removed. With Storage::MemoryOnly only the memory entry is
     * merged or removed, and an entry held only on disk selects nothing.
     */
    QImage refresh(const MapTileKey& key, const dsd_tile_fetch_response& response, qint64 requestTime,
                   qint64 responseTime, const MapTileRequestFields& sent, Storage storage = Storage::DiskAndMemory,
                   bool* reusable = nullptr);

    /** @brief The server says @p key has no tile now (a miss): drop it, from memory only for MemoryOnly. */
    void invalidate(const MapTileKey& key, Storage storage);

    /** @brief Drop @p key from memory and disk. */
    void remove(const MapTileKey& key);

    /** @brief Bytes on disk under the root, as last counted. */
    qint64 diskBytes();
    /** @brief Whether @p key's decoded image is held in memory. */
    bool inMemory(const MapTileKey& key) const;
    /** @brief Decoded images held in memory. */
    int memoryCount() const;
    /** @brief The file @p key is stored in. */
    QString pathFor(const MapTileKey& key) const;

    /** @brief Use @p bytes as the disk budget instead of 50 MB. */
    void setDiskCapForTest(qint64 bytes);

  private:
    struct MemoryEntry {
        QImage image;
        MapTileMeta meta;
        /* Everything in it came from a view placed by hand and its image is the one in its file, so a 304 may write
         * the merged metadata back. False once a MemoryOnly store or merge touched it. */
        bool persistable = false;
    };

    struct DiskRecord {
        qint64 bytes = 0;
        qint64 stamp = 0;
    };

    qint64 now() const;
    bool loadFromDisk(const MapTileKey& key, MemoryEntry* entry);
    void renewDiskStamp(const MapTileKey& key);
    bool writeEntry(const MapTileKey& key, const MapTileMeta& meta, const QByteArray& png);
    bool readPng(const MapTileKey& key, QByteArray* png) const;
    void remember(const MapTileKey& key, const MemoryEntry& entry);
    void forget(const MapTileKey& key);
    void drop(const MapTileKey& key, Storage storage);
    void ensureScanned();
    void scanNamespace(const QString& dirPath);
    void noteFile(const QString& path, qint64 bytes, qint64 stamp);
    void dropFile(const QString& path);
    void evictToCap();

    QString m_root;
    QString m_dir;
    WallClock m_wallSecs;
    qint64 m_diskCap = kDiskCapBytes;
    QHash<MapTileKey, MemoryEntry> m_memory;
    QList<MapTileKey> m_memoryOrder; /* Most recently used first. */
    QHash<QString, DiskRecord> m_disk;
    qint64 m_diskBytes = 0;
    bool m_scanned = false;
};

} // namespace dsd_qt

#endif // DSD_NEO_SRC_UI_QT_MAP_TILE_CACHE_H_
