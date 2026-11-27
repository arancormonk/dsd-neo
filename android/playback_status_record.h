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

#include <QLatin1Char>
#include <QLatin1String>
#include <QString>
#include <QStringList>
#include <QtGlobal>
#include <algorithm>
#include <iterator>
#include <limits>
#include <stddef.h>
#include <utility>

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
    int refusals = 0;         // focus requests this play was refused: a refusal read again moves it
    int skipNoticeCall = -1;  // the call that played after skips, while it is current; -1: none
    int skipNoticeCount = 0;  // the skips said on it
    int seeksLanded = 0;      // the seeks this play's player confirmed (R56): moves only on a landing
    bool canNext = false;     // Next would act (ACTION_NEXT: a later call not already failed, r13)
};

/** @brief How many tab-separated fields a status record has. */
constexpr int kPlaybackStatusFields = 23;

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

/* The codes each reason field may hold, as RecordingPlayer.kt writes them: its ERROR_*, PAUSED_* and ENDED_* values
 * (never ENDED_REPLACED, which no record carries). run_jvm_tests.py checks they match. */
constexpr const char* kPlaybackErrorCodes[] = {"missing", "unavailable", "failed"};
constexpr const char* kPlaybackPausedCodes[] = {"user", "noisy", "background", "focus", "unavailable"};
constexpr const char* kPlaybackEndedCodes[] = {"finished",   "stopped", "focus", "timeout",
                                               "unplayable", "start",   "failed"};

/** @brief Whether @p text is empty (none) or one of @p codes. */
template <size_t N>
inline bool
playback_status_code(const QString& text, const char* const (&codes)[N]) {
    return text.isEmpty() || std::any_of(std::begin(codes), std::end(codes), [&text](const char* code) {
               return text == QLatin1String(code);
           });
}

} // namespace detail

/**
 * @brief Reads @p text, one status record, into @p out; false, leaving @p out as it was, when it is not one (another
 * number of fields, a number out of its range, a flag other than 0 or 1, a reason that is not one of its field's
 * codes).
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
    static constexpr Range kNumbers[] = {{0, 0, kInt},  {1, 0, 5},      {2, 0, kLong}, {3, 0, kLong}, {5, -1, kInt},
                                         {6, 0, kLong}, {7, -1, kInt},  {8, 0, kInt},  {9, 0, kInt},  {10, 0, kInt},
                                         {11, 0, 1},    {12, 0, 1},     {13, 0, 1},    {16, 0, kInt}, {17, 0, kLong},
                                         {18, 0, kInt}, {19, -1, kInt}, {20, 0, kInt}, {21, 0, kInt}, {22, 0, 1}};
    qint64 n[kPlaybackStatusFields] = {};
    const bool numbers = std::all_of(std::begin(kNumbers), std::end(kNumbers), [&fields, &n](const Range& range) {
        return detail::playback_status_number(fields.at(range.index), range.low, range.high, &n[range.index]);
    });
    // Each reason against its own codes: one the player never writes (a typo, a newer player's) is not a record.
    if (!numbers || !detail::playback_status_code(fields.at(4), detail::kPlaybackErrorCodes)
        || !detail::playback_status_code(fields.at(14), detail::kPlaybackPausedCodes)
        || !detail::playback_status_code(fields.at(15), detail::kPlaybackEndedCodes)) {
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
    record.refusals = static_cast<int>(n[18]);
    record.skipNoticeCall = static_cast<int>(n[19]);
    record.skipNoticeCount = static_cast<int>(n[20]);
    record.seeksLanded = static_cast<int>(n[21]);
    record.canNext = n[22] != 0;
    *out = std::move(record);
    return true;
}

} // namespace dsd_android

#endif /* DSD_NEO_ANDROID_PLAYBACK_STATUS_RECORD_H_ */
