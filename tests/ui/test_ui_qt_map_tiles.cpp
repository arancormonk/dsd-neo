// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Issue #800, PR2: the map's tile cache and tile worker, against a fake transport (no network).
 *
 * The cache, driven directly on a fake wall clock: the HTTP-date, delta-seconds, Cache-Control and Vary parsers;
 * freshness from max-age, Expires − Date, Age and the seven-day default, with every malformed field reading stale;
 * a fresh no-cache entry that still needs a revalidation; a stale must-revalidate entry that is never shown; Vary: *
 * never stored; nominated Vary values compared on reuse (an app upgrade's new User-Agent goes out unconditionally);
 * no-store kept out of disk and memory and removing the older entry; the 304 merge (new fields, Date and Age
 * cleared when absent, the nominated values, a mismatched validator selecting nothing); unreadable entries deleted;
 * oversized PNGs refused; the disk LRU across template directories and the 64-image memory bound.
 *
 * The worker, through the fake transport and the real thread: nothing at all while tiles are off; one tile at a
 * time in the order asked, a memory hit answered without a request; a stale tile shown, then revalidated with its
 * ETag and shown fresh on the 304; generations (a tile no longer in view is cancelled, its queue mates are never
 * requested, a tile still in view is kept); the backoff (1 s doubling, to 5 min, Retry-After as seconds or a date,
 * cache hits served while waiting, reset by a success); https-only templates; a build without the transport; the
 * User-Agent stored for Vary; and a teardown during a stalled transfer that returns at once and whose thread ends
 * after the cancel. */

#include <QBuffer>
#include <QByteArray>
#include <QColor>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHash>
#include <QIODevice>
#include <QImage>
#include <QList>
#include <QLocale>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QtGlobal>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/tile_fetch.h>
#include <functional>
#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <thread>
#include <vector>

#include "../test_support/qt_test_paths.h"
#include "map_tile_cache.h"
#include "map_tile_worker.h"

using dsd_qt::MapTileCache;
using dsd_qt::MapTileKey;
using dsd_qt::MapTileMeta;
using dsd_qt::MapTileRequestFields;
using dsd_qt::MapTileWorker;
namespace http = dsd_qt::map_tile_http;

namespace {

int g_failures = 0;

/* Fake clocks: wall seconds for HTTP freshness, monotonic milliseconds for the backoff. */
std::atomic<qint64> g_wall{1700000000};
std::atomic<quint64> g_mono{1000000};

const QString kTemplate = QStringLiteral("https://tiles.example.test/{z}/{x}/{y}.png");
const QByteArray kHost = QByteArrayLiteral("tiles.example.test");

void
expect(const char* what, bool ok) {
    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", what);
        g_failures++;
    }
}

qint64
wall_now() {
    return g_wall.load();
}

quint64
mono_now() {
    return g_mono.load();
}

QByteArray
imf_date(qint64 secs) {
    return QLocale::c()
        .toString(QDateTime::fromSecsSinceEpoch(secs, QTimeZone::utc()),
                  QStringLiteral("ddd, dd MMM yyyy HH:mm:ss 'GMT'"))
        .toLatin1();
}

QByteArray
make_png(int w, int h, QColor color) {
    QImage image(w, h, QImage::Format_ARGB32);
    image.fill(color);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

MapTileKey
key(int z, int x, int y) {
    MapTileKey k;
    k.z = z;
    k.x = x;
    k.y = y;
    return k;
}

MapTileRequestFields
fields(const QByteArray& ua = QByteArrayLiteral("dsd-neo/1.0 (+https://github.com/arancormonk/dsd-neo)")) {
    MapTileRequestFields f;
    f.userAgent = ua;
    f.host = kHost;
    return f;
}

void
copy_field(char* out, size_t cap, const QByteArray& value) {
    DSD_SNPRINTF(out, cap, "%s", value.constData());
}

/* A response as dsd_tile_fetch() leaves it, from "Name: value" lines. */
dsd_tile_fetch_response
response(dsd_tile_fetch_status status, const QList<QByteArray>& lines) {
    dsd_tile_fetch_response r{};
    r.status = status;
    r.http_status = status == DSD_TILE_FETCH_NOT_MODIFIED ? 304 : 200;
    for (const QByteArray& line : lines) {
        const int colon = line.indexOf(':');
        const QByteArray name = line.left(colon).trimmed().toLower();
        const QByteArray value = line.mid(colon + 1).trimmed();
        if (name == "etag") {
            copy_field(r.etag, sizeof(r.etag), value);
            r.header_present |= DSD_TILE_FETCH_HDR_ETAG;
        } else if (name == "last-modified") {
            copy_field(r.last_modified, sizeof(r.last_modified), value);
            r.header_present |= DSD_TILE_FETCH_HDR_LAST_MODIFIED;
        } else if (name == "cache-control") {
            copy_field(r.cache_control, sizeof(r.cache_control), value);
            r.header_present |= DSD_TILE_FETCH_HDR_CACHE_CONTROL;
        } else if (name == "expires") {
            copy_field(r.expires, sizeof(r.expires), value);
            r.header_present |= DSD_TILE_FETCH_HDR_EXPIRES;
        } else if (name == "date") {
            copy_field(r.date, sizeof(r.date), value);
            r.header_present |= DSD_TILE_FETCH_HDR_DATE;
        } else if (name == "age") {
            copy_field(r.age, sizeof(r.age), value);
            r.header_present |= DSD_TILE_FETCH_HDR_AGE;
        } else if (name == "vary") {
            copy_field(r.vary, sizeof(r.vary), value);
            r.header_present |= DSD_TILE_FETCH_HDR_VARY;
        }
    }
    return r;
}

MapTileMeta
meta_of(const QList<QByteArray>& lines, qint64 requestTime, qint64 responseTime) {
    return dsd_qt::mapTileMetaFromResponse(response(DSD_TILE_FETCH_OK, lines), requestTime, responseTime, fields());
}

/* Store one 256×256 tile in @p cache with @p lines as its headers, received now. */
bool
store_tile(MapTileCache& cache, const MapTileKey& k, const QList<QByteArray>& lines,
           const MapTileRequestFields& sent = fields()) {
    const QByteArray png = make_png(256, 256, Qt::darkGreen);
    const QImage image = dsd_qt::mapTileDecodePng(png);
    return cache.store(k, response(DSD_TILE_FETCH_OK, lines), png, image, wall_now(), wall_now(), sent);
}

/* ------------------------------------------------------------------------- */
/* HTTP helpers and freshness                                                 */
/* ------------------------------------------------------------------------- */

void
test_http_dates_and_numbers() {
    qint64 a = 0;
    qint64 b = 0;
    qint64 c = 0;
    const qint64 now = 1700000000;
    expect("IMF-fixdate parses", http::parseHttpDate("Sun, 06 Nov 1994 08:49:37 GMT", now, &a));
    expect("RFC 850 date parses", http::parseHttpDate("Sunday, 06-Nov-94 08:49:37 GMT", now, &b));
    expect("asctime date parses", http::parseHttpDate("Sun Nov  6 08:49:37 1994", now, &c));
    expect("the three forms name the same instant", a == 784111777 && b == a && c == a);
    qint64 x = 0;
    expect("Expires: 0 is not a date", !http::parseHttpDate("0", now, &x));
    expect("a day past the month's end is not a date", !http::parseHttpDate("Fri, 31 Feb 2023 00:00:00 GMT", now, &x));
    expect("a zone other than GMT is not a date", !http::parseHttpDate("Sun, 06 Nov 1994 08:49:37 UTC", now, &x));
    expect("an RFC 850 year more than 50 years ahead is the century before",
           http::parseHttpDate("Thursday, 01-Jan-99 00:00:00 GMT", now, &x) && x == 915148800);
    qint64 d = 0;
    expect("delta-seconds parse", http::parseDeltaSeconds("3600", &d) && d == 3600);
    expect("a huge delta-seconds reads as 2^31",
           http::parseDeltaSeconds("99999999999999999999", &d) && d == 2147483648LL);
    expect("a signed or spaced delta-seconds is malformed", !http::parseDeltaSeconds("-1", &d)
                                                                && !http::parseDeltaSeconds("1 0", &d)
                                                                && !http::parseDeltaSeconds("", &d));
}

void
test_cache_control_and_vary_parsing() {
    http::CacheControl cc =
        http::parseCacheControl("Public, MAX-AGE=600, no-cache=\"Set-Cookie, X-A\", Must-Revalidate");
    expect("directives are case-insensitive", cc.hasMaxAge && cc.maxAge == 600 && cc.mustRevalidate);
    expect("a qualified no-cache counts as no-cache", cc.noCache && !cc.noStore);
    cc = http::parseCacheControl("max-age=\"30\"");
    expect("a quoted max-age is accepted", cc.hasMaxAge && cc.maxAge == 30 && !cc.maxAgeMalformed);
    expect("max-age without a number is malformed", http::parseCacheControl("max-age=soon").maxAgeMalformed);
    expect("max-age without a value is malformed", http::parseCacheControl("max-age").maxAgeMalformed);
    expect("two different max-age values are malformed",
           http::parseCacheControl("max-age=10, max-age=20").maxAgeMalformed);
    expect("a repeated equal max-age is fine", !http::parseCacheControl("max-age=10, max-age=10").maxAgeMalformed);
    expect("an unterminated quote is a syntax error", http::parseCacheControl("private=\"x, no-store").syntaxError);
    expect("no-store is found after a quoted comma", http::parseCacheControl("private=\"a,b\", no-store").noStore);
    bool star = false;
    const QList<QByteArray> names = http::parseVary("User-Agent , accept-encoding", &star);
    expect("Vary names are lowercased",
           !star && names.size() == 2 && names.at(0) == "user-agent" && names.at(1) == "accept-encoding");
    (void)http::parseVary("Accept, *", &star);
    expect("Vary: * is seen", star);
    (void)http::parseVary("Bad Name", &star);
    expect("a Vary element that is not a field name fails closed", star);
}

void
test_freshness_rules() {
    const qint64 t0 = 1700000000;
    dsd_qt::MapTileFreshness f = dsd_qt::mapTileFreshness(meta_of({"Cache-Control: max-age=100"}, t0, t0), t0 + 99);
    expect("max-age keeps an entry fresh until it runs out", f.fresh && f.lifetime == 100);
    expect("max-age runs out",
           !dsd_qt::mapTileFreshness(meta_of({"Cache-Control: max-age=100"}, t0, t0), t0 + 100).fresh);
    const MapTileMeta exp = meta_of({"Date: " + imf_date(t0 - 50), "Expires: " + imf_date(t0 + 50)}, t0, t0);
    f = dsd_qt::mapTileFreshness(exp, t0);
    expect("Expires minus Date is the lifetime, and the apparent age counts", f.lifetime == 100 && f.currentAge == 50);
    expect("an Expires entry goes stale at its date", !dsd_qt::mapTileFreshness(exp, t0 + 50).fresh);
    f = dsd_qt::mapTileFreshness(meta_of({"Cache-Control: max-age=100", "Age: 60"}, t0, t0 + 5), t0 + 30);
    expect("Age plus the response delay plus residence is the age", f.currentAge == 60 + 5 + 25 && f.fresh);
    f = dsd_qt::mapTileFreshness(meta_of({"ETag: \"a\""}, t0, t0), t0 + 6 * 86400);
    expect("no freshness at all gets seven days", f.fresh && f.lifetime == 7 * 86400);
    expect("the seven days run out", !dsd_qt::mapTileFreshness(meta_of({"ETag: \"a\""}, t0, t0), t0 + 7 * 86400).fresh);
    f = dsd_qt::mapTileFreshness(meta_of({"Cache-Control: max-age=soon"}, t0, t0), t0);
    expect("a malformed max-age is stale, not seven days", !f.fresh && f.malformed);
    f = dsd_qt::mapTileFreshness(meta_of({"Expires: 0"}, t0, t0), t0);
    expect("a malformed Expires is stale", !f.fresh && f.malformed);
    f = dsd_qt::mapTileFreshness(meta_of({"Cache-Control: max-age=100", "Date: yesterday"}, t0, t0), t0);
    expect("a malformed Date is stale", !f.fresh && f.malformed);
    f = dsd_qt::mapTileFreshness(meta_of({"Cache-Control: max-age=100", "Age: old"}, t0, t0), t0);
    expect("a malformed Age is stale", !f.fresh && f.malformed);
    dsd_tile_fetch_response r = response(DSD_TILE_FETCH_OK, {"Cache-Control: max-age=100"});
    r.header_present |= DSD_TILE_FETCH_HDR_EXPIRES;
    r.header_invalid |= DSD_TILE_FETCH_HDR_EXPIRES;
    f = dsd_qt::mapTileFreshness(dsd_qt::mapTileMetaFromResponse(r, t0, t0, fields()), t0);
    expect("an Expires the fetch could not carry is malformed, not missing", !f.fresh && f.malformed);
}

/* ------------------------------------------------------------------------- */
/* Cache                                                                      */
/* ------------------------------------------------------------------------- */

void
test_namespace_and_round_trip(const QString& root) {
    MapTileCache cache(root, kTemplate, wall_now);
    expect("the directory is the template's hash under the root",
           cache.directory() == root + QLatin1Char('/') + MapTileCache::namespaceFor(kTemplate));
    expect("the template never appears in the path", !cache.directory().contains(QStringLiteral("example")));
    const MapTileKey k = key(3, 2, 1);
    expect("a tile with freshness is stored", store_tile(cache, k, {"Cache-Control: max-age=600", "ETag: \"r1\""}));
    expect("the entry is on disk", QFileInfo::exists(cache.pathFor(k)));
    expect("the entry file is private",
           QFile(cache.pathFor(k)).permissions()
               == (QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadUser | QFileDevice::WriteUser));
    MapTileCache reopened(root, kTemplate, wall_now);
    const MapTileCache::Lookup hit = reopened.lookup(k, fields());
    expect("a reopened cache serves the entry fresh from disk",
           hit.reuse == MapTileCache::Reuse::Fresh && hit.image.width() == 256 && hit.ifNoneMatch == "\"r1\"");
    expect("an invalid key is a miss", reopened.lookup(key(1, 2, 0), fields()).reuse == MapTileCache::Reuse::Miss);
    MapTileCache other(root, kTemplate + QStringLiteral("?v=2"), wall_now);
    expect("another template does not see it", other.lookup(k, fields()).reuse == MapTileCache::Reuse::Miss);
}

void
test_no_cache_and_must_revalidate(const QString& root) {
    MapTileCache cache(root, kTemplate, wall_now);
    const MapTileKey k = key(4, 1, 1);
    expect("a no-cache tile is stored", store_tile(cache, k, {"Cache-Control: no-cache, max-age=600", "ETag: \"n\""}));
    MapTileCache::Lookup found = cache.lookup(k, fields());
    expect("a fresh no-cache entry still needs a revalidation, and is not shown before it",
           found.reuse == MapTileCache::Reuse::NeedsValidation && found.image.isNull() && found.ifNoneMatch == "\"n\"");
    MapTileCache reopened(root, kTemplate, wall_now);
    expect("the same from disk", reopened.lookup(k, fields()).reuse == MapTileCache::Reuse::NeedsValidation);

    const MapTileKey m = key(4, 2, 2);
    expect("a must-revalidate tile is stored", store_tile(cache, m, {"Cache-Control: max-age=10, must-revalidate"}));
    expect("fresh, it is served", cache.lookup(m, fields()).reuse == MapTileCache::Reuse::Fresh);
    const MapTileKey s = key(4, 3, 3);
    expect("a plain tile is stored", store_tile(cache, s, {"Cache-Control: max-age=10"}));
    g_wall += 11;
    found = cache.lookup(m, fields());
    expect("stale with must-revalidate, it is never served",
           found.reuse == MapTileCache::Reuse::NeedsValidation && found.image.isNull());
    found = cache.lookup(s, fields());
    expect("stale without it, it is shown while it is revalidated",
           found.reuse == MapTileCache::Reuse::StaleUsable && !found.image.isNull());
    const MapTileKey bad = key(4, 4, 4);
    expect("a tile with a malformed max-age is stored", store_tile(cache, bad, {"Cache-Control: max-age=x"}));
    expect("and is stale at once", cache.lookup(bad, fields()).reuse == MapTileCache::Reuse::StaleUsable);
}

void
test_vary(const QString& root) {
    MapTileCache cache(root, kTemplate, wall_now);
    const MapTileKey k = key(5, 1, 1);
    expect("an older entry exists", store_tile(cache, k, {"Cache-Control: max-age=600"}));
    expect("Vary: * is not stored", !store_tile(cache, k, {"Cache-Control: max-age=600", "Vary: *"}));
    expect("and removes the older entry from memory and disk",
           !cache.inMemory(k) && !QFileInfo::exists(cache.pathFor(k)));

    const MapTileKey v = key(5, 2, 2);
    expect("a Vary tile is stored",
           store_tile(cache, v,
                      {"Cache-Control: max-age=600", "ETag: \"e\"", "Vary: User-Agent, Accept-Encoding, Accept"}));
    MapTileCache reopened(root, kTemplate, wall_now);
    expect("the same request fields reuse it", reopened.lookup(v, fields()).reuse == MapTileCache::Reuse::Fresh);
    const MapTileCache::Lookup upgraded =
        reopened.lookup(v, fields("dsd-neo/9.9 (+https://github.com/arancormonk/dsd-neo)"));
    expect("a new User-Agent does not reuse it, and the request is unconditional",
           upgraded.reuse == MapTileCache::Reuse::Miss && upgraded.image.isNull() && upgraded.ifNoneMatch.isEmpty());
    MapTileRequestFields sent = fields();
    expect("Accept-Encoding, never sent, is stored and compared as empty", sent.value("accept-encoding").isEmpty());
    expect("Accept is libcurl's default", sent.value("accept") == "*/*");
}

void
test_no_store(const QString& root) {
    MapTileCache cache(root, kTemplate, wall_now);
    const MapTileKey k = key(6, 1, 1);
    expect("an older entry exists", store_tile(cache, k, {"Cache-Control: max-age=600"}));
    expect("a no-store response is not stored", !store_tile(cache, k, {"Cache-Control: no-store, max-age=600"}));
    expect("not kept decoded in memory", !cache.inMemory(k));
    expect("not on disk, and the older entry is gone", !QFileInfo::exists(cache.pathFor(k)));
    expect("the next lookup is a miss", cache.lookup(k, fields()).reuse == MapTileCache::Reuse::Miss);
    expect("an unparseable Cache-Control is not stored either",
           !store_tile(cache, k, {"Cache-Control: private=\"x, max-age=600"}));
}

void
test_not_modified_merge(const QString& root) {
    MapTileCache cache(root, kTemplate, wall_now);
    const MapTileKey k = key(7, 1, 1);
    expect("a tile is stored",
           store_tile(cache, k,
                      {"Cache-Control: max-age=10", "ETag: W/\"m\"", "Last-Modified: " + imf_date(wall_now() - 1000),
                       "Date: " + imf_date(wall_now()), "Age: 5", "Vary: User-Agent"}));
    g_wall += 60;
    expect("it went stale", cache.lookup(k, fields()).reuse == MapTileCache::Reuse::StaleUsable);
    const MapTileRequestFields newer = fields("dsd-neo/2.0 (+https://github.com/arancormonk/dsd-neo)");
    const QImage image =
        cache.refresh(k, response(DSD_TILE_FETCH_NOT_MODIFIED, {"Cache-Control: max-age=300", "ETag: \"m\""}),
                      wall_now(), wall_now(), newer);
    expect("a 304 with the same (weakly compared) ETag returns the stored image", !image.isNull());
    MapTileCache reopened(root, kTemplate, wall_now);
    MapTileCache::Lookup found = reopened.lookup(k, newer);
    expect("the merged max-age and the new nominated User-Agent are stored on disk",
           found.reuse == MapTileCache::Reuse::Fresh && found.ifNoneMatch == "\"m\""
               && !found.ifModifiedSince.isEmpty());
    expect("the old User-Agent no longer matches", reopened.lookup(k, fields()).reuse == MapTileCache::Reuse::Miss);
    g_wall += 290;
    expect("Date and Age the 304 lacked were cleared: the age starts at the 304",
           reopened.lookup(k, newer).reuse == MapTileCache::Reuse::Fresh);
    g_wall += 20;
    expect("and the merged lifetime runs out", reopened.lookup(k, newer).reuse == MapTileCache::Reuse::StaleUsable);
    const QImage mismatch =
        reopened.refresh(k, response(DSD_TILE_FETCH_NOT_MODIFIED, {"ETag: \"other\""}), wall_now(), wall_now(), newer);
    expect("a 304 for another ETag selects nothing and drops the entry",
           mismatch.isNull() && !QFileInfo::exists(reopened.pathFor(k)));
    expect("a 304 with nothing stored returns nothing",
           reopened.refresh(k, response(DSD_TILE_FETCH_NOT_MODIFIED, {}), wall_now(), wall_now(), newer).isNull());
    const MapTileKey n = key(7, 2, 2);
    expect("another tile is stored", store_tile(cache, n, {"Cache-Control: max-age=10", "ETag: \"q\""}));
    expect("a 304 that says no-store still validates this one use",
           !cache
                .refresh(n, response(DSD_TILE_FETCH_NOT_MODIFIED, {"Cache-Control: no-store"}), wall_now(), wall_now(),
                         fields())
                .isNull());
    expect("but removes the entry", !cache.inMemory(n) && !QFileInfo::exists(cache.pathFor(n)));
}

void
test_unreadable_and_oversized(const QString& root) {
    MapTileCache cache(root, kTemplate, wall_now);
    const MapTileKey k = key(8, 1, 1);
    QDir().mkpath(cache.directory());
    QFile junk(cache.pathFor(k));
    expect("a torn entry is written", junk.open(QIODevice::WriteOnly) && junk.write("DSDTILE1\x00\x00", 10) == 10);
    junk.close();
    expect("an unreadable entry is a miss", cache.lookup(k, fields()).reuse == MapTileCache::Reuse::Miss);
    expect("and is deleted", !QFileInfo::exists(cache.pathFor(k)));
    expect("a PNG over 512 px is refused", dsd_qt::mapTileDecodePng(make_png(513, 256, Qt::red)).isNull());
    expect("a 512 px PNG decodes", dsd_qt::mapTileDecodePng(make_png(512, 512, Qt::red)).width() == 512);
    expect("a JPEG is not a PNG", [] {
        QImage image(16, 16, QImage::Format_RGB32);
        image.fill(Qt::blue);
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "JPG");
        return bytes.isEmpty() || dsd_qt::mapTileDecodePng(bytes).isNull();
    }());
    const QString stray = cache.directory() + QStringLiteral("/1-1-1.tile.Ab12Cd");
    QFile tmp(stray);
    expect("a stray temporary file is written", tmp.open(QIODevice::WriteOnly) && tmp.write("x", 1) == 1);
    tmp.close();
    MapTileCache scanned(root, kTemplate, wall_now);
    (void)scanned.diskBytes();
    expect("the first scan deletes it", !QFileInfo::exists(stray));
}

void
test_disk_lru_and_memory_bound(const QString& root) {
    MapTileCache first(root, QStringLiteral("https://a.example.test/{z}/{x}/{y}.png"), wall_now);
    MapTileCache second(root, QStringLiteral("https://b.example.test/{z}/{x}/{y}.png"), wall_now);
    expect("the old template's tile is stored", store_tile(first, key(2, 0, 0), {"Cache-Control: max-age=600"}));
    g_wall += 10;
    expect("a second tile is stored", store_tile(second, key(2, 1, 0), {"Cache-Control: max-age=600"}));
    g_wall += 10;
    expect("a third tile is stored", store_tile(second, key(2, 2, 0), {"Cache-Control: max-age=600"}));
    const qint64 one = QFileInfo(second.pathFor(key(2, 1, 0))).size();
    MapTileCache reader(root, QStringLiteral("https://b.example.test/{z}/{x}/{y}.png"), wall_now);
    g_wall += 10;
    expect("a disk hit renews the second tile",
           reader.lookup(key(2, 1, 0), fields()).reuse == MapTileCache::Reuse::Fresh);
    const qint64 cap = reader.diskBytes() - one / 2;
    reader.setDiskCapForTest(cap);
    g_wall += 10;
    expect("a fourth tile is stored", store_tile(reader, key(2, 3, 0), {"Cache-Control: max-age=600"}));
    expect("the least recently used tile, in another template's directory, went first",
           !QFileInfo::exists(first.pathFor(key(2, 0, 0))));
    expect("then the oldest of this template's", !QFileInfo::exists(reader.pathFor(key(2, 2, 0))));
    expect("the renewed tile stayed", QFileInfo::exists(reader.pathFor(key(2, 1, 0))));
    expect("the new tile stayed", QFileInfo::exists(reader.pathFor(key(2, 3, 0))));
    expect("the budget holds", reader.diskBytes() <= cap);

    MapTileCache memory(QString(), kTemplate, wall_now);
    for (int i = 0; i < MapTileCache::kMemoryTiles + 6; ++i) {
        (void)store_tile(memory, key(7, i, 0), {"Cache-Control: max-age=600"});
    }
    expect("64 decoded tiles at most", memory.memoryCount() == MapTileCache::kMemoryTiles);
    expect("the oldest left memory", !memory.inMemory(key(7, 0, 0)) && memory.inMemory(key(7, 69, 0)));
    expect("a cache with no root keeps nothing on disk", memory.directory().isEmpty());
}

/* ------------------------------------------------------------------------- */
/* Fake transport                                                             */
/* ------------------------------------------------------------------------- */

struct FakeReply {
    long status = 200;
    QList<QByteArray> headers;
    QByteArray body;
    /* perform() fails with this status instead of answering. */
    dsd_tile_fetch_status failure = DSD_TILE_FETCH_OK;
    /* Hold the transfer until it is cancelled or released. */
    bool stall = false;
};

struct SeenRequest {
    QString url;
    QByteArray ifNoneMatch;
    QByteArray ifModifiedSince;
};

struct FakeServer {
    std::mutex mutex;
    std::condition_variable cv;
    QHash<QString, std::deque<FakeReply>> scripted;
    FakeReply fallback;
    std::vector<SeenRequest> seen;
    int stalled = 0;
    int cancelled = 0;
    bool release = false;

    void
    script(const MapTileKey& k, const FakeReply& reply) {
        std::lock_guard<std::mutex> lock(mutex);
        scripted[MapTileWorker::tileUrl(kTemplate, k)].push_back(reply);
    }

    size_t
    seenCount() {
        std::lock_guard<std::mutex> lock(mutex);
        return seen.size();
    }

    SeenRequest
    seenAt(size_t i) {
        std::lock_guard<std::mutex> lock(mutex);
        return i < seen.size() ? seen[i] : SeenRequest();
    }

    int
    stalledCount() {
        std::lock_guard<std::mutex> lock(mutex);
        return stalled;
    }

    int
    cancelledCount() {
        std::lock_guard<std::mutex> lock(mutex);
        return cancelled;
    }

    void
    releaseStall() {
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
        cv.notify_all();
    }
};

char*
dup_bytes(const QByteArray& bytes, size_t* len) {
    *len = static_cast<size_t>(bytes.size());
    char* out = static_cast<char*>(malloc(*len + 1U));
    if (out != nullptr) {
        DSD_MEMCPY(out, bytes.constData(), *len);
        out[*len] = '\0';
    }
    return out;
}

/* Wait out a stalled reply; @return true when it was cancelled. */
bool
stall(FakeServer* server, std::unique_lock<std::mutex>& lock, const dsd_tile_fetch_request* req) {
    server->stalled++;
    server->release = false;
    while (!server->release) {
        if (dsd_tile_fetch_cancel_requested(req->cancel) != 0) {
            server->cancelled++;
            return true;
        }
        server->cv.wait_for(lock, std::chrono::milliseconds(2));
    }
    return false;
}

int
fake_perform(void* ctx, const dsd_tile_fetch_request* req, dsd_tile_fetch_exchange* ex) {
    auto* server = static_cast<FakeServer*>(ctx);
    std::unique_lock<std::mutex> lock(server->mutex);
    SeenRequest seen;
    seen.url = QString::fromLatin1(req->url);
    seen.ifNoneMatch = req->if_none_match != nullptr ? QByteArray(req->if_none_match) : QByteArray();
    seen.ifModifiedSince = req->if_modified_since != nullptr ? QByteArray(req->if_modified_since) : QByteArray();
    server->seen.push_back(seen);
    std::deque<FakeReply>& queue = server->scripted[seen.url];
    FakeReply reply = server->fallback;
    if (!queue.empty()) {
        reply = queue.front();
        queue.pop_front();
    }
    if (reply.stall && stall(server, lock, req)) {
        ex->status = DSD_TILE_FETCH_ERR_CANCELLED;
        return -1;
    }
    if (reply.failure != DSD_TILE_FETCH_OK) {
        ex->status = reply.failure;
        return -1;
    }
    QByteArray block = "HTTP/1.1 " + QByteArray::number(static_cast<qlonglong>(reply.status)) + " X\r\n";
    for (const QByteArray& line : reply.headers) {
        block += line + "\r\n";
    }
    block += "\r\n";
    ex->http_status = reply.status;
    ex->headers = dup_bytes(block, &ex->headers_len);
    if (!reply.body.isEmpty()) {
        size_t len = 0;
        ex->body = reinterpret_cast<unsigned char*>(dup_bytes(reply.body, &len));
        ex->body_len = len;
    }
    return 0;
}

struct Delivered {
    quint64 generation = 0;
    MapTileKey key;
    bool stale = false;
};

struct Harness {
    FakeServer server;
    dsd_tile_fetch_transport transport{};
    std::vector<Delivered> tiles;
    MapTileWorker* worker = nullptr;

    explicit Harness(const QString& root) {
        transport.perform = fake_perform;
        transport.ctx = &server;
        server.fallback.headers = {"Cache-Control: max-age=600"};
        server.fallback.body = make_png(256, 256, Qt::gray);
        MapTileWorker::Options options;
        options.cacheRoot = root;
        options.transport = &transport;
        options.wallSecs = wall_now;
        options.monoMs = mono_now;
        worker = new MapTileWorker(options);
        worker->setUrlTemplate(kTemplate);
        QObject::connect(worker, &MapTileWorker::tileReady, worker,
                         [this](quint64 gen, int z, int x, int y, const QImage&, bool stale) {
                             Delivered d;
                             d.generation = gen;
                             d.key = key(z, x, y);
                             d.stale = stale;
                             tiles.push_back(d);
                         });
    }

    ~Harness() { delete worker; }

    Harness(const Harness&) = delete;
    Harness& operator=(const Harness&) = delete;
};

bool
wait_for(const std::function<bool()>& done, int timeoutMs = 3000) {
    const uint64_t deadline = dsd_realtime_mono_ms() + static_cast<uint64_t>(timeoutMs);
    while (!done()) {
        if (dsd_realtime_mono_ms() > deadline) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

/* Let queued deliveries land and a would-be request show up. */
void
settle(int ms = 60) {
    const uint64_t deadline = dsd_realtime_mono_ms() + static_cast<uint64_t>(ms);
    while (dsd_realtime_mono_ms() < deadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

bool
wait_threads_gone() {
    return wait_for([] { return MapTileWorker::liveThreads() == 0; });
}

/* ------------------------------------------------------------------------- */
/* Worker                                                                     */
/* ------------------------------------------------------------------------- */

void
test_off_means_nothing(const QString& root) {
    const QString dir = root + QStringLiteral("/off");
    Harness h(dir);
    expect("tiles start off", !h.worker->enabled() && h.worker->state() == MapTileWorker::State::Off);
    const quint64 before = h.worker->generation();
    expect("a request while off is ignored", h.worker->requestTiles({key(1, 0, 0)}) == before);
    settle();
    expect("no request goes out while tiles are off", h.server.seenCount() == 0U && h.tiles.empty());
    expect("no cache directory is made while tiles are off", !QFileInfo::exists(dir));
    expect("no thread runs while tiles are off", MapTileWorker::liveThreads() == 0);
    h.worker->setEnabled(true);
    expect("on, the worker is ready", h.worker->state() == MapTileWorker::State::Ready);
    h.worker->requestTiles({key(1, 0, 0)});
    expect("on, the tile is fetched", wait_for([&h] { return h.tiles.size() == 1U; }));
    h.worker->setEnabled(false);
    expect("off again", h.worker->state() == MapTileWorker::State::Off);
}

void
test_order_and_memory_hit(const QString& root) {
    Harness h(root + QStringLiteral("/order"));
    h.worker->setEnabled(true);
    const MapTileKey a = key(3, 1, 1);
    const MapTileKey b = key(3, 2, 1);
    const quint64 gen = h.worker->requestTiles({a, b, a, key(3, 9, 0)});
    expect("both tiles arrive", wait_for([&h] { return h.tiles.size() == 2U; }));
    expect("one at a time, in the order asked, duplicates and invalid keys skipped",
           h.server.seenCount() == 2U && h.server.seenAt(0).url == MapTileWorker::tileUrl(kTemplate, a)
               && h.server.seenAt(1).url == MapTileWorker::tileUrl(kTemplate, b));
    expect("each carries the request's generation, fresh",
           h.tiles[0].generation == gen && h.tiles[0].key == a && !h.tiles[0].stale && h.tiles[1].key == b);
    expect("a first request is unconditional", h.server.seenAt(0).ifNoneMatch.isEmpty());
    h.tiles.clear();
    h.worker->requestTiles({a});
    expect("a cached tile arrives again", wait_for([&h] { return h.tiles.size() == 1U; }));
    settle();
    expect("from memory, without a request", h.server.seenCount() == 2U);
}

void
test_stale_then_revalidated(const QString& root) {
    Harness h(root + QStringLiteral("/stale"));
    h.worker->setEnabled(true);
    const MapTileKey a = key(4, 5, 6);
    FakeReply first;
    first.headers = {"Cache-Control: max-age=60", "ETag: \"v1\"", "Date: " + imf_date(wall_now())};
    first.body = make_png(256, 256, Qt::green);
    h.server.script(a, first);
    FakeReply notModified;
    notModified.status = 304;
    notModified.headers = {"Cache-Control: max-age=60", "ETag: \"v1\""};
    h.server.script(a, notModified);
    h.worker->requestTiles({a});
    expect("the tile arrives", wait_for([&h] { return h.tiles.size() == 1U; }));
    g_wall += 120;
    h.tiles.clear();
    h.worker->requestTiles({a});
    expect("stale, it is shown, then shown again once revalidated", wait_for([&h] { return h.tiles.size() == 2U; }));
    expect("first as stale, then fresh", h.tiles[0].stale && !h.tiles[1].stale);
    expect("the revalidation sent the stored ETag", h.server.seenAt(1).ifNoneMatch == "\"v1\"");
    h.tiles.clear();
    h.worker->requestTiles({a});
    expect("fresh again after the 304", wait_for([&h] { return h.tiles.size() == 1U; }) && !h.tiles[0].stale);
    settle();
    expect("without another request", h.server.seenCount() == 2U);
}

void
test_not_modified_for_another_etag(const QString& root) {
    Harness h(root + QStringLiteral("/etag"));
    h.worker->setEnabled(true);
    const MapTileKey a = key(4, 7, 7);
    FakeReply first;
    first.headers = {"Cache-Control: max-age=60", "ETag: \"v1\""};
    first.body = make_png(256, 256, Qt::green);
    h.server.script(a, first);
    FakeReply other;
    other.status = 304;
    other.headers = {"ETag: \"v2\""};
    h.server.script(a, other);
    h.worker->requestTiles({a});
    expect("the tile arrives", wait_for([&h] { return h.tiles.size() == 1U; }));
    g_wall += 120;
    h.worker->requestTiles({a});
    expect("a 304 for another ETag is a failure",
           wait_for([&h] { return h.worker->state() == MapTileWorker::State::Retrying; }));
    expect("the revalidation had sent the stored ETag", h.server.seenAt(1).ifNoneMatch == "\"v1\"");
    h.tiles.clear();
    g_mono += 1000;
    h.worker->wakeForTest();
    expect("the retry loads the tile", wait_for([&h] { return h.tiles.size() == 1U; }) && !h.tiles[0].stale);
    expect("unconditionally", h.server.seenCount() == 3U && h.server.seenAt(2).ifNoneMatch.isEmpty());
}

void
test_generations_and_no_prefetch(const QString& root) {
    Harness h(root + QStringLiteral("/gen"));
    h.worker->setEnabled(true);
    const MapTileKey a = key(5, 1, 0);
    const MapTileKey b = key(5, 2, 0);
    const MapTileKey c = key(5, 3, 0);
    const MapTileKey d = key(5, 4, 0);
    FakeReply stalled;
    stalled.stall = true;
    stalled.headers = {"Cache-Control: max-age=600"};
    stalled.body = make_png(256, 256, Qt::yellow);
    h.server.script(a, stalled);
    h.worker->requestTiles({a, b, c});
    expect("the first tile is in flight", wait_for([&h] { return h.server.stalledCount() == 1; }));
    const quint64 panned = h.worker->requestTiles({d});
    expect("a pan away cancels the tile no longer in view", wait_for([&h] { return h.server.cancelledCount() == 1; }));
    expect("the new view's tile arrives", wait_for([&h] { return h.tiles.size() == 1U; }));
    settle();
    expect("only it arrived, with the new generation",
           h.tiles.size() == 1U && h.tiles[0].key == d && h.tiles[0].generation == panned);
    expect("the dropped view's queued tiles were never requested", h.server.seenCount() == 2U);

    const MapTileKey e = key(5, 5, 0);
    const MapTileKey f = key(5, 6, 0);
    const MapTileKey g = key(5, 7, 0);
    h.server.script(e, stalled);
    h.tiles.clear();
    h.worker->requestTiles({e, f});
    expect("the next view's first tile is in flight", wait_for([&h] { return h.server.stalledCount() == 2; }));
    const quint64 kept = h.worker->requestTiles({g, e});
    settle();
    expect("a tile still in view is not cancelled", h.server.cancelledCount() == 1);
    h.server.releaseStall();
    expect("both tiles of the new view arrive", wait_for([&h] { return h.tiles.size() == 2U; }));
    expect("the kept tile carries the new generation", h.tiles[0].key == e && h.tiles[0].generation == kept);
    settle();
    expect("the tile that left the view was never requested", h.server.seenCount() == 4U);
    h.worker->clearRequests();
    expect("clearing starts a new generation", h.worker->generation() == kept + 1U);

    /* A clear cancels the tile in flight; a request for it that lands before the cancel does still gets it. */
    const MapTileKey back = key(5, 8, 0);
    h.server.script(back, stalled);
    h.tiles.clear();
    h.worker->requestTiles({back});
    expect("a tile is in flight again", wait_for([&h] { return h.server.stalledCount() == 3; }));
    h.worker->clearRequests();
    const quint64 returned = h.worker->requestTiles({back});
    expect("the tile asked for again after the clear arrives", wait_for([&h] { return h.tiles.size() == 1U; }));
    expect("for the new request", h.tiles[0].key == back && h.tiles[0].generation == returned);
    expect("after a second request, the first having been cancelled",
           h.server.cancelledCount() == 2 && h.server.seenCount() == 6U);
}

void
test_backoff_and_retry_after(const QString& root) {
    Harness h(root + QStringLiteral("/backoff"));
    h.worker->setEnabled(true);
    const MapTileKey cached = key(6, 0, 0);
    h.worker->requestTiles({cached});
    expect("a tile is cached", wait_for([&h] { return h.tiles.size() == 1U; }));
    const MapTileKey a = key(6, 1, 0);
    FakeReply busy;
    busy.status = 503;
    h.server.script(a, busy);
    h.server.script(a, busy);
    h.worker->requestTiles({a});
    expect("a failure puts the worker in Retrying",
           wait_for([&h] { return h.worker->state() == MapTileWorker::State::Retrying; }));
    expect("the first wait is 1 s", h.worker->backoffRemainingMsForTest() == 1000);
    h.tiles.clear();
    h.worker->requestTiles({cached, a});
    expect("a cache hit is still served while waiting", wait_for([&h] { return h.tiles.size() == 1U; }));
    settle();
    expect("no request goes out before the wait ends", h.server.seenCount() == 2U);
    g_mono += 1000;
    h.worker->wakeForTest();
    expect("the tile is tried again after 1 s", wait_for([&h] { return h.server.seenCount() == 3U; }));
    expect("the second wait is 2 s", wait_for([&h] { return h.worker->backoffRemainingMsForTest() == 2000; }));
    g_mono += 2000;
    h.worker->wakeForTest();
    expect("then it loads", wait_for([&h] { return h.tiles.size() == 2U; }));
    expect("a success makes the worker ready",
           wait_for([&h] { return h.worker->state() == MapTileWorker::State::Ready; }));
    expect("and resets the wait", h.worker->backoffRemainingMsForTest() == 0);

    const MapTileKey r = key(6, 2, 0);
    FakeReply limited;
    limited.status = 429;
    limited.headers = {"Retry-After: 120"};
    h.server.script(r, limited);
    h.worker->requestTiles({r});
    expect("Retry-After in seconds sets the wait",
           wait_for([&h] { return h.worker->backoffRemainingMsForTest() == 120000; }));
    h.worker->setUrlTemplate(QStringLiteral("https://other.example.test/{z}/{x}/{y}.png"));
    expect("a template change resets the wait", h.worker->backoffRemainingMsForTest() == 0);
    expect("and makes the worker ready", h.worker->state() == MapTileWorker::State::Ready);

    qint64 step = 0;
    QList<qint64> steps;
    for (int i = 0; i < 11; ++i) {
        step = MapTileWorker::nextBackoffMs(step);
        steps.append(step);
    }
    expect("the wait doubles from 1 s and stops at 5 min", steps.first() == 1000 && steps.at(1) == 2000
                                                               && steps.at(8) == 256000 && steps.at(9) == 300000
                                                               && steps.last() == 300000);
    qint64 wait = 0;
    expect("Retry-After as a date is the time left until it",
           MapTileWorker::retryAfterMs(imf_date(1700000090), 1700000000, &wait) && wait == 90000);
    expect("a Retry-After date in the past is no wait",
           MapTileWorker::retryAfterMs(imf_date(1699999000), 1700000000, &wait) && wait == 0);
    expect("a malformed Retry-After is ignored", !MapTileWorker::retryAfterMs("soon", 1700000000, &wait));
}

void
test_templates() {
    QString host;
    expect("the default template is valid",
           MapTileWorker::validTemplate(QString::fromLatin1(MapTileWorker::kDefaultTemplate), &host)
               && host == QStringLiteral("tile.openstreetmap.org"));
    expect("a key in the query is fine",
           MapTileWorker::validTemplate(QStringLiteral("https://maps.example.test:8443/t/{z}/{x}/{y}?key=abc"), &host)
               && host == QStringLiteral("maps.example.test:8443"));
    expect("http is refused", !MapTileWorker::validTemplate(QStringLiteral("http://a.example.test/{z}/{x}/{y}.png")));
    expect("a missing placeholder is refused",
           !MapTileWorker::validTemplate(QStringLiteral("https://a.example.test/{z}/{x}.png")));
    expect("an unknown placeholder is refused",
           !MapTileWorker::validTemplate(QStringLiteral("https://{s}.example.test/{z}/{x}/{y}.png")));
    expect("a placeholder in the host is refused",
           !MapTileWorker::validTemplate(QStringLiteral("https://{z}.example.test/{x}/{y}.png")));
    expect("spaces are refused", !MapTileWorker::validTemplate(QStringLiteral("https://a.example.test/{z} /{x}/{y}")));
    expect("non-ASCII is refused",
           !MapTileWorker::validTemplate(QStringLiteral("https://a.example.test/é/{z}/{x}/{y}")));
    expect("no host is refused", !MapTileWorker::validTemplate(QStringLiteral("https:///{z}/{x}/{y}")));
    expect("a template too long for the URL cap is refused",
           !MapTileWorker::validTemplate(QStringLiteral("https://a.example.test/") + QString(2010, QLatin1Char('a'))
                                         + QStringLiteral("/{z}/{x}/{y}")));
    expect("the URL is filled in", MapTileWorker::tileUrl(kTemplate, key(12, 345, 678))
                                       == QStringLiteral("https://tiles.example.test/12/345/678.png"));
    expect("an invalid key fills nothing", MapTileWorker::tileUrl(kTemplate, key(1, 2, 0)).isEmpty());
}

void
test_invalid_template_and_unavailable(const QString& root) {
    Harness h(root + QStringLiteral("/invalid"));
    h.worker->setEnabled(true);
    h.worker->setUrlTemplate(QStringLiteral("http://tiles.example.test/{z}/{x}/{y}.png"));
    expect("an http template is an invalid source", h.worker->state() == MapTileWorker::State::InvalidSource);
    const quint64 gen = h.worker->generation();
    expect("requests are ignored", h.worker->requestTiles({key(1, 0, 0)}) == gen);
    settle();
    expect("nothing goes out", h.server.seenCount() == 0U);
    h.worker->setUrlTemplate(kTemplate);
    FakeReply unsupported;
    unsupported.failure = DSD_TILE_FETCH_ERR_UNSUPPORTED;
    h.server.script(key(1, 1, 0), unsupported);
    h.worker->requestTiles({key(1, 1, 0), key(1, 0, 1)});
    expect("a transport this build lacks makes the worker unavailable",
           wait_for([&h] { return h.worker->state() == MapTileWorker::State::Unavailable; }));
    settle();
    expect("and nothing more is tried", h.server.seenCount() == 1U);
    MapTileWorker builtIn;
    expect("the built-in transport is available exactly when the runtime says so",
           builtIn.available() == (dsd_tile_fetch_available() != 0));
    expect("the default template is the OpenStreetMap one",
           builtIn.urlTemplate() == QStringLiteral("https://tile.openstreetmap.org/{z}/{x}/{y}.png"));
}

void
test_user_agent_stored_for_vary(const QString& root) {
    const QString dir = root + QStringLiteral("/ua");
    Harness h(dir);
    h.worker->setEnabled(true);
    const MapTileKey a = key(9, 3, 3);
    FakeReply varies;
    varies.headers = {"Cache-Control: max-age=600", "Vary: User-Agent"};
    varies.body = make_png(256, 256, Qt::cyan);
    h.server.script(a, varies);
    h.worker->requestTiles({a});
    expect("the tile arrives", wait_for([&h] { return h.tiles.size() == 1U; }));
    char agent[128];
    expect("the User-Agent is known", dsd_tile_fetch_user_agent(agent, sizeof(agent)) == 0);
    const QByteArray ua(agent);
    expect("it names the app and its project page",
           ua.startsWith("dsd-neo/") && ua.endsWith(" (+https://github.com/arancormonk/dsd-neo)"));
    MapTileCache cache(dir, kTemplate, wall_now);
    expect("the stored entry matches the app's own User-Agent",
           cache.lookup(a, fields(ua)).reuse == MapTileCache::Reuse::Fresh);
    MapTileCache upgraded(dir, kTemplate, wall_now);
    expect("and not another version's",
           upgraded.lookup(a, fields("dsd-neo/0.0.1 (+https://github.com/arancormonk/dsd-neo)")).reuse
               == MapTileCache::Reuse::Miss);
}

void
test_teardown_during_stalled_transfer(const QString& root) {
    auto* h = new Harness(root + QStringLiteral("/teardown"));
    h->worker->setEnabled(true);
    FakeReply stalled;
    stalled.stall = true;
    h->server.script(key(2, 1, 1), stalled);
    h->worker->requestTiles({key(2, 1, 1)});
    expect("a transfer stalls", wait_for([h] { return h->server.stalledCount() == 1; }));
    const uint64_t before = dsd_realtime_mono_ms();
    delete h->worker;
    h->worker = nullptr;
    const uint64_t took = dsd_realtime_mono_ms() - before;
    expect("deleting the worker does not wait for the thread", took < 50U);
    expect("the stalled transfer is cancelled", wait_for([h] { return h->server.cancelledCount() == 1; }));
    expect("and the thread ends", wait_threads_gone());
    settle();
    expect("nothing is delivered after the teardown", h->tiles.empty());
    delete h;
}

} // namespace

int
main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    dsd_test_qt_isolate_paths();
    QTemporaryDir dir;
    if (!dir.isValid()) {
        DSD_FPRINTF(stderr, "FAIL: no temporary directory\n");
        return 1;
    }
    const QString root = dir.path() + QStringLiteral("/map-tiles");

    test_http_dates_and_numbers();
    test_cache_control_and_vary_parsing();
    test_freshness_rules();
    test_namespace_and_round_trip(root + QStringLiteral("/c1"));
    test_no_cache_and_must_revalidate(root + QStringLiteral("/c2"));
    test_vary(root + QStringLiteral("/c3"));
    test_no_store(root + QStringLiteral("/c4"));
    test_not_modified_merge(root + QStringLiteral("/c5"));
    test_unreadable_and_oversized(root + QStringLiteral("/c6"));
    test_disk_lru_and_memory_bound(root + QStringLiteral("/c7"));

    test_off_means_nothing(root);
    expect("the off test's thread ended", wait_threads_gone());
    test_order_and_memory_hit(root);
    test_stale_then_revalidated(root);
    test_not_modified_for_another_etag(root);
    test_generations_and_no_prefetch(root);
    test_backoff_and_retry_after(root);
    test_templates();
    test_invalid_template_and_unavailable(root);
    test_user_agent_stored_for_vary(root);
    expect("every worker thread so far ended", wait_threads_gone());
    test_teardown_during_stalled_transfer(root);

    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stdout, "UI_QT_MAP_TILES: all checks passed\n");
    return 0;
}
