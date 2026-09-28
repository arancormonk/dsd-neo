// SPDX-License-Identifier: GPL-3.0-or-later
#include "dsd-neo/core/safe_api.h"
#include "provoice_frame.h"

#include <dsd-neo/protocol/provoice/provoice_const.h>

#include <assert.h>
#include <stdint.h>

typedef struct {
    int next;
} test_reader;

typedef struct {
    int next;
    int limit;
} limited_reader;

/* A two-level symbol carries one bit; each stream position gets its own bit and reliability, so a cell that
 * took the wrong symbol's value, or only half of it, shows. */
static int
stream_bit(int stream_idx) {
    return (stream_idx ^ (stream_idx >> 2)) & 1;
}

static uint8_t
stream_reliability(int stream_idx) {
    return (uint8_t)(1 + ((stream_idx * 37) % 254));
}

static int
read_counting_bit(void* user, int* out_bit, uint8_t* out_reliability) {
    test_reader* reader = (test_reader*)user;
    assert(reader != 0);
    assert(out_bit != 0);
    assert(out_reliability != 0);
    *out_bit = stream_bit(reader->next);
    *out_reliability = stream_reliability(reader->next);
    reader->next++;
    return 0;
}

static int
read_limited_bit(void* user, int* out_bit, uint8_t* out_reliability) {
    limited_reader* reader = (limited_reader*)user;
    assert(reader != 0);
    assert(out_bit != 0);
    assert(out_reliability != 0);
    if (reader->next >= reader->limit) {
        return -1;
    }
    *out_bit = stream_bit(reader->next);
    *out_reliability = stream_reliability(reader->next);
    reader->next++;
    return 0;
}

static void
assert_interleave_schedule_is_unique(int filled[DSD_PROVOICE_IMBE_ROWS][DSD_PROVOICE_IMBE_COLS]) {
    int count = 0;
    int i;
    for (i = 0; i < 142; i++) {
        int row = provoice_interleave_w[i];
        int col = provoice_interleave_x[i];
        assert(row >= 0 && row < DSD_PROVOICE_IMBE_ROWS);
        assert(col >= 0 && col < DSD_PROVOICE_IMBE_COLS);
        assert(filled[row][col] == 0);
        filled[row][col] = 1;
        count++;
    }
    assert(count == 142);
}

/* Cells the schedule never reaches are bit 0 at reliability 0: a valid soft bit the decoder never reads. */
static void
assert_unscheduled_positions_are_zero(dsd_provoice_imbe_frame frame,
                                      int filled[DSD_PROVOICE_IMBE_ROWS][DSD_PROVOICE_IMBE_COLS]) {
    int row;
    for (row = 0; row < DSD_PROVOICE_IMBE_ROWS; row++) {
        int col;
        for (col = 0; col < DSD_PROVOICE_IMBE_COLS; col++) {
            if (filled[row][col] == 0) {
                assert(frame[row][col].bit == 0);
                assert(frame[row][col].reliability == 0);
            }
        }
    }
}

static void
mark_expected_segment(int expected[142], int* schedule_idx, int* stream_idx, int count) {
    int i;
    for (i = 0; i < count; i++) {
        expected[*schedule_idx] = *stream_idx;
        (*schedule_idx)++;
        (*stream_idx)++;
    }
}

static void
build_expected_stream_indices(int expected1[142], int expected2[142]) {
    int schedule_idx = 0;
    int stream_idx = 0;
    int i;

    for (i = 0; i < 142; i++) {
        expected1[i] = -1;
        expected2[i] = -1;
    }

    for (i = 0; i < 11; i++) {
        mark_expected_segment(expected1, &schedule_idx, &stream_idx, 6);
        schedule_idx -= 6;
        mark_expected_segment(expected2, &schedule_idx, &stream_idx, 6);
    }

    mark_expected_segment(expected1, &schedule_idx, &stream_idx, 6);
    schedule_idx -= 6;
    mark_expected_segment(expected2, &schedule_idx, &stream_idx, 4);
    stream_idx += 2;
    mark_expected_segment(expected2, &schedule_idx, &stream_idx, 2);

    for (i = 0; i < 3; i++) {
        mark_expected_segment(expected1, &schedule_idx, &stream_idx, 6);
        schedule_idx -= 6;
        mark_expected_segment(expected2, &schedule_idx, &stream_idx, 6);
    }

    mark_expected_segment(expected1, &schedule_idx, &stream_idx, 5);
    schedule_idx -= 5;
    mark_expected_segment(expected2, &schedule_idx, &stream_idx, 5);

    for (i = 0; i < 7; i++) {
        mark_expected_segment(expected1, &schedule_idx, &stream_idx, 6);
        schedule_idx -= 6;
        mark_expected_segment(expected2, &schedule_idx, &stream_idx, 6);
    }

    mark_expected_segment(expected1, &schedule_idx, &stream_idx, 5);
    schedule_idx -= 5;
    mark_expected_segment(expected2, &schedule_idx, &stream_idx, 5);

    assert(schedule_idx == 142);
    assert(stream_idx == DSD_PROVOICE_FRAME_PAIR_DIBITS);
}

static void
assert_frame_matches_stream_schedule(dsd_provoice_imbe_frame frame1, dsd_provoice_imbe_frame frame2) {
    int expected1[142];
    int expected2[142];
    int i;

    build_expected_stream_indices(expected1, expected2);
    for (i = 0; i < 142; i++) {
        int row = provoice_interleave_w[i];
        int col = provoice_interleave_x[i];

        assert(expected1[i] >= 0);
        assert(expected2[i] >= 0);
        assert(frame1[row][col].bit == stream_bit(expected1[i]));
        assert(frame1[row][col].reliability == stream_reliability(expected1[i]));
        assert(frame2[row][col].bit == stream_bit(expected2[i]));
        assert(frame2[row][col].reliability == stream_reliability(expected2[i]));
    }
}

static void
test_frame_pair_loader_zero_fills_and_preserves_schedule(void) {
    dsd_provoice_imbe_frame frame1;
    dsd_provoice_imbe_frame frame2;
    int filled[DSD_PROVOICE_IMBE_ROWS][DSD_PROVOICE_IMBE_COLS];
    test_reader reader = {0};
    int rc;

    DSD_MEMSET(frame1, 0x55, sizeof(frame1));
    DSD_MEMSET(frame2, 0x55, sizeof(frame2));
    DSD_MEMSET(filled, 0, sizeof(filled));

    rc = dsd_provoice_load_imbe_frame_pair(read_counting_bit, &reader, frame1, frame2);
    assert(rc == DSD_PROVOICE_FRAME_PAIR_DIBITS);
    assert(reader.next == DSD_PROVOICE_FRAME_PAIR_DIBITS);

    assert_interleave_schedule_is_unique(filled);
    assert_unscheduled_positions_are_zero(frame1, filled);
    assert_unscheduled_positions_are_zero(frame2, filled);
    assert_frame_matches_stream_schedule(frame1, frame2);
}

static void
test_frame_pair_loader_reports_short_input(void) {
    dsd_provoice_imbe_frame frame1;
    dsd_provoice_imbe_frame frame2;
    limited_reader reader = {0, DSD_PROVOICE_FRAME_PAIR_DIBITS - 1};

    assert(dsd_provoice_load_imbe_frame_pair(read_limited_bit, &reader, frame1, frame2) == -1);
    assert(reader.next == DSD_PROVOICE_FRAME_PAIR_DIBITS - 1);
}

static void
test_frame_pair_loader_rejects_invalid_arguments(void) {
    dsd_provoice_imbe_frame frame1;
    dsd_provoice_imbe_frame frame2;
    test_reader reader = {0};

    assert(dsd_provoice_load_imbe_frame_pair(0, &reader, frame1, frame2) == -1);
    assert(dsd_provoice_load_imbe_frame_pair(read_counting_bit, &reader, 0, frame2) == -1);
    assert(dsd_provoice_load_imbe_frame_pair(read_counting_bit, &reader, frame1, 0) == -1);
    assert(reader.next == 0);
}

int
main(void) {
    test_frame_pair_loader_zero_fills_and_preserves_schedule();
    test_frame_pair_loader_reports_short_input();
    test_frame_pair_loader_rejects_invalid_arguments();
    return 0;
}
