// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Issue #806 X12: samples a producer loses before they reach the input ring (an Airspy driver overflow, a full ring
 * dropping the rest of a transfer) leave a gap mark that travels with the ring's data:
 *
 * - however the producer splits its writes and the consumer its reads (arbitrary short blocks), no read the consumer
 *   clips at the next mark crosses a gap, and the read that starts at each gap's first surviving sample, and only it,
 *   reports the gap; a producer and consumer on two threads find the same;
 * - a second mark at the same place is the same gap;
 * - a purge drops the marks before the samples it discarded and keeps one at the new read position;
 * - read_block() and a purge move the consumer's position as read_commit() does;
 * - a producer with every slot pending moves the newest mark rather than overwrite one the consumer has yet to reach.
 */

#include <atomic>
#include <cmath>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/runtime/input_ring.h>
#include <stdint.h>
#include <stdio.h>
#include <vector>

#include "dsd-neo/core/safe_api.h"

extern "C" int
dsd_rtl_stream_should_exit(void) {
    return 0;
}

namespace {

int
expect_true(const char* label, bool cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", label);
        return 1;
    }
    return 0;
}

int
expect_u64(const char* label, uint64_t got, uint64_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s: got=%llu want=%llu\n", label, (unsigned long long)got, (unsigned long long)want);
        return 1;
    }
    return 0;
}

uint32_t
next_lcg(uint32_t* state) {
    *state = (*state * 1664525U) + 1013904223U;
    return *state >> 8;
}

/* Write @p pairs pairs, sample k of the stream holding the float value of its stream position. */
size_t
produce(input_ring_state* r, uint64_t* position, size_t pairs) {
    float *p1 = nullptr, *p2 = nullptr;
    size_t n1 = 0U, n2 = 0U;
    const int granted = input_ring_reserve(r, pairs * 2U, &p1, &n1, &p2, &n2);
    const size_t take = ((size_t)granted / 2U) * 2U;
    for (size_t i = 0; i < take; i++) {
        float* dst = (i < n1) ? &p1[i] : &p2[i - n1];
        *dst = (float)(*position + i);
    }
    input_ring_commit(r, take);
    *position += take;
    return take;
}

/* The consumer as the demod reads (demod_input_span_stop_at_gap()): reserve up to @p max_count, stop at the next mark,
 * check every sample is the next stream position, commit. Records each read's start position and gap flag. */
struct Reader {
    uint64_t next = 0U;
    std::vector<uint64_t> gaps_found;
    int crossed = 0;
    int out_of_order = 0;
};

size_t
consume(input_ring_state* r, Reader* rd, size_t max_count) {
    if (input_ring_is_empty(r)) {
        return 0U;
    }
    float *p1 = nullptr, *p2 = nullptr;
    size_t n1 = 0U, n2 = 0U;
    size_t got = (size_t)input_ring_read_reserve(r, max_count, &p1, &n1, &p2, &n2);
    int at_read = 0;
    const size_t until = input_ring_gap_pending(r) ? input_ring_take_gap(r, &at_read) : SIZE_MAX;
    if (at_read) {
        rd->gaps_found.push_back(rd->next);
    }
    if (until < got) {
        got = until;
    }
    for (size_t i = 0; i < got; i++) {
        const float v = (i < n1) ? p1[i] : p2[i - n1];
        /* Positions stay far below 2^24, where every integer is a float: half a unit apart is another sample. */
        if (std::fabs(v - (float)(rd->next + i)) > 0.5f) {
            rd->out_of_order = 1;
        }
    }
    input_ring_read_commit(r, got);
    rd->next += got;
    return got;
}

int
test_arbitrary_blocks_single_thread(void) {
    int rc = 0;
    input_ring_state r;
    rc |= expect_true("init", input_ring_init(&r, 1024U) == 0);
    uint32_t lcg = 0x5eedU;
    uint64_t produced = 0U;
    std::vector<uint64_t> gaps_noted;
    Reader rd;
    for (int step = 0; step < 4000; step++) {
        const uint32_t pick = next_lcg(&lcg);
        if ((pick & 3U) != 0U) {
            const size_t pairs = 1U + (next_lcg(&lcg) % 37U);
            (void)produce(&r, &produced, pairs);
            if ((next_lcg(&lcg) % 5U) == 0U) {
                /* The producer lost samples after this block: the next one starts after a gap. */
                input_ring_note_gap(&r);
                if (gaps_noted.empty() || gaps_noted.back() != produced) {
                    gaps_noted.push_back(produced);
                }
            }
        } else {
            (void)consume(&r, &rd, (size_t)2U * (1U + (next_lcg(&lcg) % 53U)));
        }
    }
    while (consume(&r, &rd, 64U) > 0U) {}
    /* A gap at the very end has no sample after it yet: the next read would carry it. */
    if (!gaps_noted.empty() && gaps_noted.back() == produced) {
        gaps_noted.pop_back();
    }
    rc |= expect_u64("every sample read once, in order", rd.next, produced);
    rc |= expect_true("samples in stream order", rd.out_of_order == 0);
    rc |= expect_u64("gaps found", rd.gaps_found.size(), gaps_noted.size());
    rc |= expect_true("each gap found at its first surviving sample", rd.gaps_found == gaps_noted);
    input_ring_destroy(&r);
    return rc;
}

struct ThreadRun {
    input_ring_state* r;
    std::atomic<int> done{0};
    std::vector<uint64_t> gaps_noted;
    uint64_t produced = 0U;
};

DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
__stdcall
#endif
producer_thread(void* arg) {
    auto* run = static_cast<ThreadRun*>(arg);
    uint32_t lcg = 0xabcdU;
    for (int block = 0; block < 20000; block++) {
        const size_t pairs = 1U + (next_lcg(&lcg) % 29U);
        while (input_ring_free(run->r) < pairs * 2U) {
            dsd_thread_yield();
        }
        (void)produce(run->r, &run->produced, pairs);
        if ((next_lcg(&lcg) % 7U) == 0U) {
            input_ring_note_gap(run->r);
            run->gaps_noted.push_back(run->produced);
        }
    }
    run->done.store(1, std::memory_order_release);
    DSD_THREAD_RETURN;
}

/* The same over two threads: the producer never lets the consumer fall INPUT_RING_GAP_SLOTS marks behind (it waits for
 * space in a ring far smaller than that many blocks), so every gap is found where it was noted. */
int
test_arbitrary_blocks_two_threads(void) {
    int rc = 0;
    input_ring_state r;
    rc |= expect_true("init", input_ring_init(&r, 128U) == 0);
    ThreadRun run;
    run.r = &r;
    dsd_thread_t producer;
    rc |= expect_true("producer started", dsd_thread_create(&producer, producer_thread, &run) == 0);
    Reader rd;
    uint32_t lcg = 0x1357U;
    for (;;) {
        const int finished = run.done.load(std::memory_order_acquire);
        if (consume(&r, &rd, (size_t)2U * (1U + (next_lcg(&lcg) % 41U))) == 0U) {
            if (finished) {
                break;
            }
            dsd_thread_yield();
        }
    }
    dsd_thread_join(producer);
    if (!run.gaps_noted.empty() && run.gaps_noted.back() == run.produced) {
        run.gaps_noted.pop_back();
    }
    rc |= expect_u64("two threads: every sample read", rd.next, run.produced);
    rc |= expect_true("two threads: in order", rd.out_of_order == 0);
    rc |= expect_true("two threads: each gap at its first surviving sample", rd.gaps_found == run.gaps_noted);
    input_ring_destroy(&r);
    return rc;
}

int
test_duplicate_purge_and_positions(void) {
    int rc = 0;
    input_ring_state r;
    rc |= expect_true("init", input_ring_init(&r, 64U) == 0);
    uint64_t produced = 0U;
    (void)produce(&r, &produced, 4U);
    input_ring_note_gap(&r);
    input_ring_note_gap(&r); /* a ring-full drop right after an overflow: the same gap */
    (void)produce(&r, &produced, 4U);
    float out[64];
    /* read_block() moves the read position as a commit does: 6 floats in, the mark is 2 ahead. */
    rc |= expect_true("read_block", input_ring_read_block(&r, out, 6U) == 6);
    int at_read = -1;
    rc |= expect_u64("mark 2 floats ahead", input_ring_take_gap(&r, &at_read), 2U);
    rc |= expect_true("not at the read position yet", at_read == 0);
    rc |= expect_true("read up to it", input_ring_read_block(&r, out, 2U) == 2);
    rc |= expect_u64("nothing after it", input_ring_take_gap(&r, &at_read), SIZE_MAX);
    rc |= expect_true("one gap at the read position", at_read == 1);
    rc |= expect_true("taken", !input_ring_gap_pending(&r));

    /* Marks inside samples a purge discards are dropped; one at the purge's end stays at the new read position. */
    input_ring_note_gap(&r);
    (void)produce(&r, &produced, 3U);
    input_ring_note_gap(&r);
    input_ring_discard_all_consumer(&r);
    rc |= expect_u64("purge moves the read position", input_ring_take_gap(&r, &at_read), SIZE_MAX);
    rc |= expect_true("the mark at the purge's end is at the read position", at_read == 1);
    rc |= expect_true("none left", !input_ring_gap_pending(&r));
    input_ring_note_gap(&r);
    (void)input_ring_take_gap(&r, &at_read);
    rc |= expect_true("a mark before any new sample is at the read position", at_read == 1);
    input_ring_reset_positions(&r);
    rc |= expect_true("reset clears the marks", !input_ring_gap_pending(&r));
    input_ring_destroy(&r);
    return rc;
}

int
test_full_queue_moves_newest(void) {
    int rc = 0;
    input_ring_state r;
    rc |= expect_true("init", input_ring_init(&r, 1024U) == 0);
    uint64_t produced = 0U;
    for (unsigned i = 0; i < INPUT_RING_GAP_SLOTS + 3U; i++) {
        (void)produce(&r, &produced, 1U);
        input_ring_note_gap(&r);
    }
    Reader rd;
    while (consume(&r, &rd, 1024U) > 0U) {}
    int at_read = 0;
    (void)input_ring_take_gap(&r, &at_read);
    if (at_read) {
        rd.gaps_found.push_back(rd.next);
    }
    rc |= expect_u64("every slot's gap found", rd.gaps_found.size(), INPUT_RING_GAP_SLOTS);
    rc |= expect_u64("the first gaps where they were", rd.gaps_found.front(), 2U);
    rc |= expect_u64("the newest moved to the last gap", rd.gaps_found.back(), produced);
    input_ring_destroy(&r);
    return rc;
}

} // namespace

int
main(void) {
    int rc = 0;
    rc |= test_arbitrary_blocks_single_thread();
    rc |= test_arbitrary_blocks_two_threads();
    rc |= test_duplicate_purge_and_positions();
    rc |= test_full_queue_moves_newest();
    if (rc == 0) {
        printf("RUNTIME_INPUT_RING_GAP: OK\n");
    }
    return rc;
}
