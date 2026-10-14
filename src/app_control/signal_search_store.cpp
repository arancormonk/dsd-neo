// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <dsd-neo/app_control/signal_search.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/signal_search.h>
#include <mutex>
#include <numeric>
#include <stdint.h>
#include <stdlib.h>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
using Hit = dsd_signal_search_hit;
using Bytes = std::vector<unsigned char>;
constexpr uint32_t magic = 0x31535344; // DSS1, version 1, little endian

void
put(Bytes& out, uint64_t value, unsigned n) {
    for (unsigned i = 0; i < n; ++i) {
        out.push_back((unsigned char)(value >> (8 * i)));
    }
}

uint64_t
take(const Bytes& data, size_t& at, unsigned n) {
    uint64_t value = 0;
    for (unsigned i = 0; i < n; ++i) {
        value |= (uint64_t)data[at++] << (8 * i);
    }
    return value;
}

uint32_t
checksum(const Bytes& b) {
    return std::accumulate(b.begin(), b.end(), uint32_t{2166136261U},
                           [](uint32_t sum, unsigned char c) { return (sum ^ c) * 16777619U; });
}

template <size_t N>
void
put_text(Bytes& b, const char (&text)[N]) {
    for (size_t i = 0; i < N; ++i) {
        b.push_back((unsigned char)text[i]);
    }
}

template <size_t N>
void
take_text(const Bytes& b, size_t& at, char (&text)[N]) {
    DSD_MEMCPY(text, b.data() + at, N);
    text[N - 1] = '\0';
    at += N;
}

Bytes
encode(const Hit& h) {
    Bytes b;
    put(b, h.session_id, 8);
    put(b, h.hit_id, 8);
    put(b, h.started_ms, 8);
    put(b, h.ended_ms, 8);
    put(b, h.frequency_hz, 4);
    put(b, h.bandwidth_hz, 4);
    put(b, h.step_num, 4);
    put(b, h.step_den, 4);
    put(b, h.range_index, 4);
    put(b, (uint32_t)(int32_t)(h.power_dbfs * 1000), 4);
    put(b, (uint32_t)(int32_t)(h.noise_dbfs * 1000), 4);
    put(b, h.confirmed, 1);
    put(b, h.ended, 1);
    put(b, h.am, 1);
    put(b, h.unresolved, 1);
    put(b, (uint32_t)h.gain_tenth_db, 4);
    put(b, h.gain_auto, 1);
    put(b, h.target, 4);
    put(b, h.source, 4);
    put_text(b, h.range_name);
    put_text(b, h.protocol);
    put_text(b, h.access);
    put_text(b, h.tone);
    put_text(b, h.target_text);
    put_text(b, h.source_text);
    put_text(b, h.input_spec);
    put(b, (uint32_t)h.ppm, 4);
    put(b, h.bias_tee, 1);
    put_text(b, h.airspy.serial);
    put(b, h.airspy.sample_rate, 4);
    for (int v : {h.airspy.gain_mode, h.airspy.sensitivity_gain, h.airspy.linearity_gain, h.airspy.lna_gain,
                  h.airspy.mixer_gain, h.airspy.vga_gain, h.airspy.lna_agc, h.airspy.mixer_agc, h.airspy.bias_tee}) {
        put(b, (uint32_t)v, 4);
    }
    return b;
}

Hit
decode(const Bytes& b) {
    Hit h{};
    size_t at = 0;
    h.session_id = take(b, at, 8);
    h.hit_id = take(b, at, 8);
    h.started_ms = take(b, at, 8);
    h.ended_ms = take(b, at, 8);
    h.frequency_hz = (uint32_t)take(b, at, 4);
    h.bandwidth_hz = (uint32_t)take(b, at, 4);
    h.step_num = (uint32_t)take(b, at, 4);
    h.step_den = (uint32_t)take(b, at, 4);
    h.range_index = (uint32_t)take(b, at, 4);
    h.power_dbfs = (int32_t)take(b, at, 4) / 1000.0f;
    h.noise_dbfs = (int32_t)take(b, at, 4) / 1000.0f;
    h.confirmed = (int)take(b, at, 1);
    h.ended = (int)take(b, at, 1);
    h.am = (int)take(b, at, 1);
    h.unresolved = (int)take(b, at, 1);
    h.gain_tenth_db = (int32_t)take(b, at, 4);
    h.gain_auto = (int)take(b, at, 1);
    h.target = (uint32_t)take(b, at, 4);
    h.source = (uint32_t)take(b, at, 4);
    take_text(b, at, h.range_name);
    take_text(b, at, h.protocol);
    take_text(b, at, h.access);
    take_text(b, at, h.tone);
    take_text(b, at, h.target_text);
    take_text(b, at, h.source_text);
    take_text(b, at, h.input_spec);
    h.ppm = (int32_t)take(b, at, 4);
    h.bias_tee = (int)take(b, at, 1);
    take_text(b, at, h.airspy.serial);
    h.airspy.sample_rate = (uint32_t)take(b, at, 4);
    for (int* v :
         {&h.airspy.gain_mode, &h.airspy.sensitivity_gain, &h.airspy.linearity_gain, &h.airspy.lna_gain,
          &h.airspy.mixer_gain, &h.airspy.vga_gain, &h.airspy.lna_agc, &h.airspy.mixer_agc, &h.airspy.bias_tee}) {
        *v = (int32_t)take(b, at, 4);
    }
    return h;
}

bool
same(const Hit& a, const Hit& b) {
    return a.session_id == b.session_id && a.hit_id == b.hit_id;
}

void
upsert(std::vector<Hit>& list, const Hit& h) {
    auto found = std::find_if(list.begin(), list.end(), [&](const Hit& x) { return same(x, h); });
    if (found != list.end()) {
        *found = h;
    } else {
        list.push_back(h);
        if (list.size() > DSD_SEARCH_HISTORY_LIMIT) {
            list.erase(list.begin());
        }
    }
}

bool
write_hit(FILE* f, const Hit& h) {
    Bytes b = encode(h), header;
    put(header, magic, 4);
    put(header, b.size(), 4);
    put(header, checksum(b), 4);
    return fwrite(header.data(), 1, header.size(), f) == header.size() && fwrite(b.data(), 1, b.size(), f) == b.size();
}

const char*
skip_space(const char* p) {
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') {
        ++p;
    }
    return p;
}

bool
parse_avoid_frequency(const char*& p, uint32_t& frequency) {
    if (*p < '0' || *p > '9') {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    unsigned long value = strtoul(p, &end, 10);
    if (errno || end == p || !value || value > UINT32_MAX) {
        return false;
    }
    frequency = (uint32_t)value;
    p = skip_space(end);
    return true;
}

bool
parse_avoids(const char* p, std::vector<uint32_t>& result) {
    p = skip_space(p);
    if (*p++ != '[') {
        return false;
    }
    p = skip_space(p);
    if (*p == ']') {
        return !*skip_space(p + 1);
    }
    while (result.size() < DSD_SEARCH_MAX_AVOIDS) {
        uint32_t frequency = 0;
        if (!parse_avoid_frequency(p, frequency)) {
            return false;
        }
        result.push_back(frequency);
        if (*p == ']') {
            return !*skip_space(p + 1);
        }
        if (*p++ != ',') {
            return false;
        }
        p = skip_space(p);
    }
    return false;
}

bool
append_history(const std::string& path, const std::vector<Hit>& batch) {
    FILE* f = dsd_fopen_private(path.c_str(), "ab");
    if (!f) {
        return false;
    }
    bool ok = std::all_of(batch.begin(), batch.end(), [f](const Hit& h) { return write_hit(f, h); });
    if (fflush(f) != 0 || dsd_fsync(dsd_fileno(f)) != 0) {
        ok = false;
    }
    if (fclose(f) != 0) {
        ok = false;
    }
    return ok;
}

struct Store {
    std::mutex mutex;
    std::condition_variable wake, flushed;
    std::thread worker;
    std::string path, avoid_path, error;
    std::vector<Hit> history, pending;
    std::vector<uint32_t> avoids, session_avoids;
    dsd_signal_search_status status{};
    bool stop = false, dirty_avoids = false, busy = false, compact = true;
    /* A file whose write failed stays owed until its own write succeeds; every worker pass retries it. `retry` wakes
       the worker for one more attempt (flush, close) without looping on storage that keeps failing. */
    bool history_failed = false, avoids_failed = false, retry = false;
    size_t records = 0;
    /* Bumped on every change a frontend shows: history rows (upserts, trims, reloads) and either avoid list. */
    uint64_t history_revision = 1, avoids_revision = 1;
    uint64_t session_avoids_session = 0;

    ~Store() { close(); }

    void
    close() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stop = true;
            if (history_failed || avoids_failed) {
                retry = true;
            }
            wake.notify_all();
        }
        if (worker.joinable()) {
            worker.join();
        }
    }

    void
    load_history() {
        FILE* f = dsd_fopen_private(path.c_str(), "rb");
        if (f) {
            const size_t size = encode(Hit{}).size();
            for (;;) {
                Bytes header(12);
                size_t got = fread(header.data(), 1, header.size(), f);
                if (!got) {
                    break;
                }
                size_t at = 0;
                if (got != header.size() || take(header, at, 4) != magic || take(header, at, 4) != size) {
                    error = "History journal has an incomplete or unsupported record";
                    break;
                }
                uint32_t sum = (uint32_t)take(header, at, 4);
                Bytes bytes(size);
                if (fread(bytes.data(), 1, size, f) != size || checksum(bytes) != sum) {
                    error = "History journal has an incomplete or damaged record";
                    break;
                }
                Hit h = decode(bytes);
                if (h.session_id && h.hit_id && h.frequency_hz) {
                    h.ended = 1;
                    upsert(history, h);
                }
                ++records;
            }
            fclose(f);
        }
    }

    void
    load() {
        history.clear();
        avoids.clear();
        records = 0;
        ++history_revision;
        ++avoids_revision;
        load_history();
        FILE* f = dsd_fopen_private(avoid_path.c_str(), "rb");
        if (!f) {
            return;
        }
        char buf[16385];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        bool ok = !ferror(f) && n < sizeof(buf) - 1;
        buf[n] = '\0';
        fclose(f);
        std::vector<uint32_t> parsed;
        if (ok && parse_avoids(buf, parsed)) {
            avoids = std::move(parsed);
        } else {
            error = "Invalid permanent avoid store";
        }
    }

    bool
    compact_history(const std::vector<Hit>& all) const {
        char temp[4096];
        FILE* f = dsd_fopen_private_temp_for_replace(path.c_str(), temp, sizeof(temp), "wb");
        if (!f) {
            return false;
        }
        bool ok = std::all_of(all.begin(), all.end(), [f](const Hit& h) { return write_hit(f, h); });
        if (fflush(f) != 0 || dsd_fsync(dsd_fileno(f)) != 0) {
            ok = false;
        }
        if (fclose(f) != 0) {
            ok = false;
        }
        if (ok) {
            ok = dsd_replace_file_with_temp(temp, path.c_str()) == 0;
        }
        if (!ok) {
            remove(temp);
        }
        return ok;
    }

    bool
    save_avoids(const std::vector<uint32_t>& list) const {
        char temp[4096];
        FILE* f = dsd_fopen_private_temp_for_replace(avoid_path.c_str(), temp, sizeof(temp), "wb");
        if (!f) {
            return false;
        }
        bool ok = DSD_FPRINTF(f, "[") >= 0;
        for (size_t i = 0; i < list.size(); ++i) {
            if (DSD_FPRINTF(f, "%s%u", i ? "," : "", list[i]) < 0) {
                ok = false;
            }
        }
        if (DSD_FPRINTF(f, "]\n") < 0 || fflush(f) != 0 || dsd_fsync(dsd_fileno(f)) != 0) {
            ok = false;
        }
        if (fclose(f) != 0) {
            ok = false;
        }
        if (ok) {
            ok = dsd_replace_file_with_temp(temp, avoid_path.c_str()) == 0;
        }
        if (!ok) {
            remove(temp);
        }
        return ok;
    }

    /* Saving one file settles nothing about the other: the error clears only once neither is owed. A failed history
       write leaves `compact` set, so its retry rewrites every retained record once instead of appending again. */
    void
    record_write_result(bool write_history, bool history_ok, bool write_avoids, bool avoids_ok, size_t saved_records) {
        const bool owed = history_failed || avoids_failed;
        if (write_history) {
            history_failed = !history_ok;
            compact = !history_ok;
            if (history_ok) {
                records = saved_records;
            }
        }
        if (write_avoids) {
            avoids_failed = !avoids_ok;
        }
        if (history_failed || avoids_failed) {
            error = "Signal search history or avoids could not be saved; retained in memory";
        } else if (owed) {
            error.clear();
        }
    }

    /* One worker pass, entered and left holding @p lock: takes the pending detections and every file still owed,
       writes them unlocked, and records how each write went. */
    void
    write_pass(std::unique_lock<std::mutex>& lock) {
        std::vector<Hit> batch;
        batch.swap(pending);
        auto all = history;
        auto avoid_copy = avoids;
        const bool write_history = !batch.empty() || history_failed;
        const bool write_avoids = dirty_avoids || avoids_failed;
        dirty_avoids = false;
        retry = false;
        bool replace = compact || records + batch.size() > 4000;
        busy = true;
        lock.unlock();
        bool history_ok = true;
        if (write_history) {
            history_ok = replace ? compact_history(all) : append_history(path, batch);
        }
        const bool avoids_ok = !write_avoids || save_avoids(avoid_copy);
        lock.lock();
        record_write_result(write_history, history_ok, write_avoids, avoids_ok,
                            replace ? all.size() : records + batch.size());
        busy = false;
        flushed.notify_all();
    }

    void
    run() {
        std::unique_lock<std::mutex> lock(mutex);
        for (;;) {
            wake.wait(lock, [&] { return stop || retry || !pending.empty() || dirty_avoids; });
            if (stop && !retry && pending.empty() && !dirty_avoids) {
                break;
            }
            write_pass(lock);
        }
        flushed.notify_all();
    }
};

Store store;

void
session_avoids_clear_locked() {
    if (!store.session_avoids.empty() || store.session_avoids_session) {
        store.session_avoids.clear();
        store.session_avoids_session = 0;
        ++store.avoids_revision;
    }
}
} // namespace

extern "C" int
dsd_app_signal_search_store_open(const char* path) {
    if (!path || !*path) {
        return -1;
    }
    {
        std::lock_guard<std::mutex> lock(store.mutex);
        if (store.worker.joinable() && store.path == path && !store.stop) {
            return 0;
        }
    }
    store.close();
    std::lock_guard<std::mutex> lock(store.mutex);
    store.path = path;
    store.avoid_path = store.path + ".avoids.json";
    store.stop = false;
    store.compact = true;
    store.history_failed = false;
    store.avoids_failed = false;
    store.retry = false;
    store.error.clear();
    store.pending.clear();
    store.session_avoids.clear();
    store.session_avoids_session = 0;
    store.load();
    store.worker = std::thread([] { store.run(); });
    return 0;
}

extern "C" void
dsd_app_signal_search_store_close(void) {
    store.close();
}

extern "C" void
dsd_app_signal_search_store_flush(void) {
    std::unique_lock<std::mutex> lock(store.mutex);
    if (!store.worker.joinable()) {
        return;
    }
    if (store.history_failed || store.avoids_failed) {
        store.retry = true;
    }
    store.wake.notify_all();
    store.flushed.wait(lock,
                       [] { return !store.retry && store.pending.empty() && !store.dirty_avoids && !store.busy; });
}

extern "C" void
dsd_app_signal_search_publish(const dsd_signal_search_status* status, const Hit* hit, void* context) {
    (void)context;
    std::lock_guard<std::mutex> lock(store.mutex);
    store.status = *status;
    if (status->phase == DSD_SEARCH_STOPPED) {
        session_avoids_clear_locked();
    }
    if (hit && hit->hit_id) {
        auto found =
            std::find_if(store.history.begin(), store.history.end(), [&](const Hit& h) { return same(h, *hit); });
        if (found != store.history.end() && encode(*found) == encode(*hit)) {
            return;
        }
        upsert(store.history, *hit);
        upsert(store.pending, *hit);
        ++store.history_revision;
        store.wake.notify_all();
    }
}

extern "C" void
dsd_app_signal_search_status_get(dsd_signal_search_status* status, char* error, size_t capacity) {
    std::lock_guard<std::mutex> lock(store.mutex);
    if (status) {
        *status = store.status;
    }
    if (error && capacity) {
        DSD_SNPRINTF(error, capacity, "%s", store.error.c_str());
    }
}

static size_t
history_copy_locked(Hit* hits, size_t capacity) {
    size_t n = hits ? std::min(capacity, store.history.size()) : 0;
    for (size_t i = 0; i < n; ++i) {
        hits[i] = store.history[store.history.size() - 1 - i];
    }
    return n;
}

extern "C" size_t
dsd_app_signal_search_history_get(Hit* hits, size_t capacity) {
    std::lock_guard<std::mutex> lock(store.mutex);
    return history_copy_locked(hits, capacity);
}

extern "C" uint64_t
dsd_app_signal_search_history_revision(void) {
    std::lock_guard<std::mutex> lock(store.mutex);
    return store.history_revision;
}

extern "C" size_t
dsd_app_signal_search_history_snapshot(Hit* hits, size_t capacity, uint64_t* revision) {
    std::lock_guard<std::mutex> lock(store.mutex);
    if (revision) {
        *revision = store.history_revision;
    }
    return history_copy_locked(hits, capacity);
}

extern "C" uint64_t
dsd_app_signal_search_avoids_revision(void) {
    std::lock_guard<std::mutex> lock(store.mutex);
    return store.avoids_revision;
}

extern "C" size_t
dsd_app_signal_search_session_avoids_get(uint32_t* frequencies, size_t capacity) {
    std::lock_guard<std::mutex> lock(store.mutex);
    size_t n = frequencies ? std::min(capacity, store.session_avoids.size()) : 0;
    for (size_t i = 0; i < n; ++i) {
        frequencies[i] = store.session_avoids[i];
    }
    return n;
}

extern "C" void
dsd_app_signal_search_session_avoid_add(uint64_t session, uint32_t frequency) {
    std::lock_guard<std::mutex> lock(store.mutex);
    if (session != store.session_avoids_session) {
        store.session_avoids.clear();
        store.session_avoids_session = session;
        ++store.avoids_revision;
    }
    if (frequency
        && std::find(store.session_avoids.begin(), store.session_avoids.end(), frequency) == store.session_avoids.end()
        && store.session_avoids.size() < DSD_SEARCH_MAX_AVOIDS) {
        store.session_avoids.push_back(frequency);
        ++store.avoids_revision;
    }
}

extern "C" void
dsd_app_signal_search_session_avoid_remove(uint32_t frequency) {
    std::lock_guard<std::mutex> lock(store.mutex);
    auto found = std::find(store.session_avoids.begin(), store.session_avoids.end(), frequency);
    if (found != store.session_avoids.end()) {
        store.session_avoids.erase(found);
        ++store.avoids_revision;
    }
}

extern "C" void
dsd_app_signal_search_session_avoids_clear(void) {
    std::lock_guard<std::mutex> lock(store.mutex);
    session_avoids_clear_locked();
}

extern "C" void
dsd_app_signal_search_session_ended(void) {
    std::lock_guard<std::mutex> lock(store.mutex);
    session_avoids_clear_locked();
    /* A rejected start leaves an error with no coordinator to publish STOPPED. Keep its message for the idle screen,
       but nothing the next session sees may claim the tuner or a running phase. */
    if (store.status.phase != DSD_SEARCH_STOPPED || store.status.owned) {
        store.status.phase = DSD_SEARCH_STOPPED;
        store.status.owned = 0;
        store.status.identifying = 0;
        store.status.live_power_valid = 0;
        ++store.status.revision;
    }
}

extern "C" size_t
dsd_app_signal_search_avoids_get(uint32_t* frequencies, size_t capacity) {
    std::lock_guard<std::mutex> lock(store.mutex);
    size_t n = frequencies ? std::min(capacity, store.avoids.size()) : 0;
    for (size_t i = 0; i < n; ++i) {
        frequencies[i] = store.avoids[i];
    }
    return n;
}

extern "C" void
dsd_app_signal_search_permanent_avoid(uint32_t frequency) {
    std::lock_guard<std::mutex> lock(store.mutex);
    if (std::find(store.avoids.begin(), store.avoids.end(), frequency) == store.avoids.end()
        && store.avoids.size() < DSD_SEARCH_MAX_AVOIDS) {
        store.avoids.push_back(frequency);
        ++store.avoids_revision;
        store.dirty_avoids = true;
        store.wake.notify_all();
    }
}

extern "C" int
dsd_app_signal_search_avoid_remove(uint32_t frequency) {
    std::lock_guard<std::mutex> lock(store.mutex);
    auto found = std::find(store.avoids.begin(), store.avoids.end(), frequency);
    if (found == store.avoids.end()) {
        return -1;
    }
    store.avoids.erase(found);
    ++store.avoids_revision;
    store.dirty_avoids = true;
    store.wake.notify_all();
    return 0;
}
