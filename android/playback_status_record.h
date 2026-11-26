// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Reads RecordingPlayback.status() (#909): the call recording player's status record, as RecordingPlayer.kt's
 * Status.record() writes it.
 *
 * Free of JNI so the host test can drive it directly: DecoderHostAndroid::refreshPlayback() only reads the record.
 */

#ifndef DSD_NEO_ANDROID_PLAYBACK_STATUS_RECORD_H_
#define DSD_NEO_ANDROID_PLAYBACK_STATUS_RECORD_H_

#include <QChar>
#include <QLatin1Char>
#include <QString>
#include <QStringList>
#include <QtGlobal>
#include <algorithm>
#include <initializer_list>
#include <iterator>
#include <limits>

namespace dsd_android {

/** @brief One status record's fields, in the record's order. See RecordingPlayer.Status.record(). */
struct PlaybackStatusRecord {
    int generation = 0; // 0: no play
    int state = 0;      // DecoderHost::PlaybackState
    qint64 positionMs = 0;
    qint64 durationMs = 0; // 0 when not known
    QString error;         // "", or why it FAILED
    int itemIndex = -1;    // into the play's items; -1: none
    qint64 token = 0;      // the recording open now; 0: none
    int call = -1;         // -1: none
    int calls = 0;
    int partOrdinal = 0; // 0: none
    int partCount = 0;
    bool gap = false;
    bool interrupted = false;
    bool canSeek = false;
    QString pausedReason;
    QString endedReason;
    int skipped = 0;
    qint64 recheckSerial = 0; // RecordingRecheck.serial: moves when a path waits for takeRecheck()
};

/** @brief How many tab-separated fields a status record has. */
constexpr int kPlaybackStatusFields = 18;

namespace detail {

/** @brief Whether @p text is a decimal within [@p low, @p high], read into @p out. */
inline bool
playback_status_number(const QString& text, qint64 low, qint64 high, qint64* out) {
    bool ok = false;
    const qint64 value = text.toLongLong(&ok, 10);
    if (!ok || value < low || value > high) {
        return false;
    }
    *out = value;
    return true;
}

/** @brief Whether @p text is empty or one of the record's codes: a short run of lower-case letters. */
inline bool
playback_status_code(const QString& text) {
    if (text.size() > 32) {
        return false;
    }
    return std::all_of(text.cbegin(), text.cend(),
                       [](QChar c) { return c >= QLatin1Char('a') && c <= QLatin1Char('z'); });
}

} // namespace detail

/**
 * @brief Reads @p text, one status record, into @p out; false, leaving @p out as it was, when it is not one (another
 * number of fields, a number out of its range, a flag other than 0 or 1, a code that is not one).
 */
inline bool
parse_playback_status(const QString& text, PlaybackStatusRecord* out) {
    const QStringList fields = text.split(QLatin1Char('\t'));
    if (fields.size() != kPlaybackStatusFields) {
        return false;
    }

    struct Range {
        int index = 0;
        qint64 low = 0;
        qint64 high = 0;
    };

    constexpr qint64 kInt = std::numeric_limits<int>::max();
    constexpr qint64 kLong = std::numeric_limits<qint64>::max();
    static constexpr Range kNumbers[] = {{0, 0, kInt},  {1, 0, 4},     {2, 0, kLong}, {3, 0, kLong}, {5, -1, kInt},
                                         {6, 0, kLong}, {7, -1, kInt}, {8, 0, kInt},  {9, 0, kInt},  {10, 0, kInt},
                                         {11, 0, 1},    {12, 0, 1},    {13, 0, 1},    {16, 0, kInt}, {17, 0, kLong}};
    qint64 n[kPlaybackStatusFields] = {};
    const bool numbers = std::all_of(std::begin(kNumbers), std::end(kNumbers), [&fields, &n](const Range& range) {
        return detail::playback_status_number(fields.at(range.index), range.low, range.high, &n[range.index]);
    });
    const auto codes = {4, 14, 15};
    if (!numbers || !std::all_of(codes.begin(), codes.end(), [&fields](int code) {
            return detail::playback_status_code(fields.at(code));
        })) {
        return false;
    }
    PlaybackStatusRecord record;
    record.generation = static_cast<int>(n[0]);
    record.state = static_cast<int>(n[1]);
    record.positionMs = n[2];
    record.durationMs = n[3];
    record.error = fields.at(4);
    record.itemIndex = static_cast<int>(n[5]);
    record.token = n[6];
    record.call = static_cast<int>(n[7]);
    record.calls = static_cast<int>(n[8]);
    record.partOrdinal = static_cast<int>(n[9]);
    record.partCount = static_cast<int>(n[10]);
    record.gap = n[11] != 0;
    record.interrupted = n[12] != 0;
    record.canSeek = n[13] != 0;
    record.pausedReason = fields.at(14);
    record.endedReason = fields.at(15);
    record.skipped = static_cast<int>(n[16]);
    record.recheckSerial = n[17];
    *out = record;
    return true;
}

} // namespace dsd_android

#endif /* DSD_NEO_ANDROID_PLAYBACK_STATUS_RECORD_H_ */
