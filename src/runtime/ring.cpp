// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Output ring buffer for demodulated audio samples.
 *
 * Implements blocking producer/consumer operations with timed waits and
 * optional signaling semantics.
 */

#include <cstring>
#include <dsd-neo/runtime/ring.h>
#include <time.h>

extern volatile uint8_t exitflag; // defined in apps/dsd-cli/main.c

/**
 * @brief Write up to count samples, blocking until space is available.
 *
 * Signals data availability only on an empty-to-non-empty transition.
 *
 * @param o     Output ring buffer state.
 * @param data  Source samples to write.
 * @param count Number of samples to write.
 */
void
ring_write(struct output_state* o, const int16_t* data, size_t count) {
    int need_signal = ring_is_empty(o);
    while (count > 0 && !exitflag) {
        size_t free_sp = ring_free(o);
        if (free_sp == 0) {
            /* Wait for space with timeout to avoid indefinite blocking */
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 50L * 1000000L; /* 50ms */
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec += ts.tv_nsec / 1000000000L;
                ts.tv_nsec = ts.tv_nsec % 1000000000L;
            }
            pthread_mutex_lock(&o->ready_m);
            int ret = pthread_cond_timedwait(&o->space, &o->ready_m, &ts);
            pthread_mutex_unlock(&o->ready_m);
            if (ret != 0) {
                if (exitflag) {
                    return;
                }
                /* Metrics: producer timed out waiting for space */
                o->write_timeouts.fetch_add(1);
                continue;
            }
        }
        free_sp = ring_free(o);
        if (free_sp == 0) {
            continue;
        }
        size_t write_now = (count < free_sp) ? count : free_sp;
        size_t h = o->head.load();

        /* First region: from head to end of buffer */
        size_t to_end = o->capacity - h;
        if (to_end >= write_now) {
            memcpy(o->buffer + h, data, write_now * sizeof(int16_t));
            h += write_now;
            if (h >= o->capacity) {
                h = 0;
            }
            o->head.store(h);
            data += write_now;
            count -= write_now;
            continue;
        }

        /* Handle wrap-around case */
        if (to_end > 0) {
            memcpy(o->buffer + h, data, to_end * sizeof(int16_t));
            data += to_end;
            count -= to_end;
        }
        memcpy(o->buffer, data, count * sizeof(int16_t));
        h = count;
        o->head.store(h);
        data += count;
        count = 0;
    }
    if (need_signal) {
        pthread_mutex_lock(&o->ready_m);
        pthread_cond_signal(&o->ready);
        pthread_mutex_unlock(&o->ready_m);
    }
}

/**
 * @brief Write up to count samples, blocking until space is available.
 *
 * Does not signal; caller should decide when to signal.
 *
 * @param o     Output ring buffer state.
 * @param data  Source samples to write.
 * @param count Number of samples to write.
 */
void
ring_write_no_signal(struct output_state* o, const int16_t* data, size_t count) {
    while (count > 0 && !exitflag) {
        size_t free_sp = ring_free(o);
        if (free_sp == 0) {
            /* Wait for space with timeout to avoid indefinite blocking */
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 50L * 1000000L; /* 50ms */
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec += ts.tv_nsec / 1000000000L;
                ts.tv_nsec = ts.tv_nsec % 1000000000L;
            }
            pthread_mutex_lock(&o->ready_m);
            int ret = pthread_cond_timedwait(&o->space, &o->ready_m, &ts);
            pthread_mutex_unlock(&o->ready_m);
            if (ret != 0) {
                if (exitflag) {
                    return;
                }
                /* Metrics: producer timed out waiting for space */
                o->write_timeouts.fetch_add(1);
                continue;
            }
        }
        free_sp = ring_free(o);
        if (free_sp == 0) {
            continue;
        }
        size_t write_now = (count < free_sp) ? count : free_sp;
        size_t h = o->head.load();

        /* First region: from head to end of buffer */
        size_t to_end = o->capacity - h;
        if (to_end >= write_now) {
            memcpy(o->buffer + h, data, write_now * sizeof(int16_t));
            h += write_now;
            if (h >= o->capacity) {
                h = 0;
            }
            o->head.store(h);
            data += write_now;
            count -= write_now;
            continue;
        }

        /* Handle wrap-around case */
        if (to_end > 0) {
            memcpy(o->buffer + h, data, to_end * sizeof(int16_t));
            data += to_end;
            count -= to_end;
        }
        memcpy(o->buffer, data, count * sizeof(int16_t));
        h = count;
        o->head.store(h);
        data += count;
        count = 0;
    }
}

/**
 * @brief Write samples with signal on empty-to-non-empty transition.
 *
 * @param o     Output ring buffer state.
 * @param data  Source samples to write.
 * @param count Number of samples to write.
 */
void
ring_write_signal_on_empty_transition(struct output_state* o, const int16_t* data, size_t count) {
    int need_signal = ring_is_empty(o);
    ring_write_no_signal(o, data, count);
    if (need_signal) {
        pthread_mutex_lock(&o->ready_m);
        pthread_cond_signal(&o->ready);
        pthread_mutex_unlock(&o->ready_m);
    }
}

/**
 * @brief Read one sample from the output ring, blocking with timeout until available.
 *
 * @param o    Output ring buffer state.
 * @param out  Destination for one sample.
 * @return 0 on success, -1 on exit.
 */
int
ring_read_one(struct output_state* o, int16_t* out) {
    while (ring_is_empty(o)) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 10L * 1000000L; /* 10ms */
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000L;
        }
        pthread_mutex_lock(&o->ready_m);
        int ret = pthread_cond_timedwait(&o->ready, &o->ready_m, &ts);
        pthread_mutex_unlock(&o->ready_m);
        if (ret != 0) {
            if (exitflag) {
                return -1;
            }
            /* Metrics: consumer timed out waiting for data */
            o->read_timeouts.fetch_add(1);
            /* Timeout: check again */
            continue;
        }
    }

    size_t t = o->tail.load();
    *out = o->buffer[t];
    t++;
    if (t == o->capacity) {
        t = 0;
    }
    o->tail.store(t);
    /* Signal space available for producer */
    pthread_mutex_lock(&o->ready_m);
    pthread_cond_signal(&o->space);
    pthread_mutex_unlock(&o->ready_m);
    return 0;
}

/**
 * @brief Read up to max_count samples into out.
 *
 * Blocks until at least one sample is available or exit.
 *
 * @param o         Output ring buffer state.
 * @param out       Destination buffer for samples.
 * @param max_count Maximum number of samples to read.
 * @return Number of samples read (>=1) or -1 on exit.
 */
int
ring_read_batch(struct output_state* o, int16_t* out, size_t max_count) {
    if (max_count == 0) {
        return 0;
    }
    while (ring_is_empty(o)) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 10L * 1000000L; /* 10ms */
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000L;
        }
        pthread_mutex_lock(&o->ready_m);
        int ret = pthread_cond_timedwait(&o->ready, &o->ready_m, &ts);
        pthread_mutex_unlock(&o->ready_m);
        if (ret != 0) {
            if (exitflag) {
                return -1;
            }
            /* Metrics: consumer timed out waiting for data */
            o->read_timeouts.fetch_add(1);
            /* Timeout: check again */
            continue;
        }
    }

    size_t available = ring_used(o);
    size_t read_now = (max_count < available) ? max_count : available;
    size_t t = o->tail.load();
    size_t first = o->capacity - t;
    if (first >= read_now) {
        memcpy(out, o->buffer + t, read_now * sizeof(int16_t));
        t += read_now;
        if (t >= o->capacity) {
            t = 0;
        }
    } else {
        memcpy(out, o->buffer + t, first * sizeof(int16_t));
        memcpy(out + first, o->buffer, (read_now - first) * sizeof(int16_t));
        t = read_now - first;
    }
    o->tail.store(t);
    /* Signal space available once for the whole batch */
    pthread_mutex_lock(&o->ready_m);
    pthread_cond_signal(&o->space);
    pthread_mutex_unlock(&o->ready_m);
    return (int)read_now;
}
