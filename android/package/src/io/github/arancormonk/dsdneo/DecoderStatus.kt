// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

package io.github.arancormonk.dsdneo

/** One slot's call identity, mirroring `dsd_app_slot_call`. */
data class SlotCall(
    val state: Int,
    val name: String,
    val tgText: String,
    val srcText: String,
    val tgId: ULong,
    val enc: Boolean,
    val algid: Int,
    val kid: Int,
    val elapsedMs: Long,
) {
    val hasContent: Boolean
        get() = state == DecoderStatus.LINE_ACTIVE || state == DecoderStatus.LINE_ENDED
}

/**
 * The decoder's status as the notification needs it.
 *
 * Parsed from one record rather than assembled from several calls, so every field
 * describes the same moment.
 */
data class DecoderStatus(
    val protocol: String,
    val radioInput: Boolean,
    val trunking: Boolean,
    val trunkTuned: Boolean,
    val ccFreqHz: Long,
    val vcFreqHz: Long,
    val centerFreqHz: Long,
    val slots: List<SlotCall>,
) {
    /** The slot whose call should headline, or null when nothing is on the air. */
    val leadSlot: SlotCall?
        get() = slots.filter { it.hasContent }
            .minWithOrNull(
                // Active outranks merely ended. Among equals the smallest elapsed time is
                // the most recently started, which is the transmission the user just heard
                // begin -- so a plain ascending compare on elapsedMs is the tie-break.
                compareByDescending<SlotCall> { it.state == LINE_ACTIVE }
                    .thenBy { it.elapsedMs },
            )

    companion object {
        const val LINE_NONE = 0
        const val LINE_IDLE = 1
        const val LINE_ACTIVE = 2
        const val LINE_ENDED = 3

        private const val VERSION = "v1"
        private const val HEADER_FIELDS = 8
        private const val SLOT_FIELDS = 9
        private const val SLOT_COUNT = 2
        private const val TOTAL_FIELDS = HEADER_FIELDS + SLOT_FIELDS * SLOT_COUNT

        /**
         * @return the parsed status, or null when [record] is absent, a version this build
         *   does not know, or the wrong shape. A caller that gets null renders the plain
         *   phase text rather than a half-read status.
         */
        @JvmStatic
        fun parse(record: String?): DecoderStatus? {
            if (record.isNullOrEmpty()) {
                return null
            }
            // Kotlin's Char-delimited split, unlike Java's regex-based String.split, never
            // drops trailing empty strings -- the default limit (0, meaning unbounded)
            // already keeps the empty name/tgText/srcText a quiet slot produces. (A
            // negative limit is not "unbounded" here; split() throws IllegalArgumentException
            // for one, so this must stay 0/default rather than the -1 a Java habit suggests.)
            val f = record.split('\t')
            if (f.size != TOTAL_FIELDS || f[0] != VERSION) {
                return null
            }
            return try {
                DecoderStatus(
                    protocol = f[1],
                    radioInput = f[2] == "1",
                    trunking = f[3] == "1",
                    trunkTuned = f[4] == "1",
                    ccFreqHz = f[5].toLong(),
                    vcFreqHz = f[6].toLong(),
                    centerFreqHz = f[7].toLong(),
                    slots = (0 until SLOT_COUNT).map { slot ->
                        val b = HEADER_FIELDS + slot * SLOT_FIELDS
                        SlotCall(
                            state = f[b].toInt(),
                            name = f[b + 1],
                            tgText = f[b + 2],
                            srcText = f[b + 3],
                            // tg_id is uint64_t on the wire (0..2^64-1), a range Long cannot
                            // fully cover; ULong covers it exactly. A bad or oversized string
                            // still throws NumberFormatException here (confirmed empirically,
                            // not assumed toULong() behaves like toLong()), so the catch below
                            // still applies -- this only changes behavior for the values above
                            // Long.MAX_VALUE that toLong() could never have represented anyway.
                            tgId = f[b + 4].toULong(),
                            enc = f[b + 5] == "1",
                            algid = f[b + 6].toInt(),
                            kid = f[b + 7].toInt(),
                            elapsedMs = f[b + 8].toLong(),
                        )
                    },
                )
            } catch (e: NumberFormatException) {
                // A malformed record is a bug on the native side, not something to render.
                null
            }
        }
    }
}
