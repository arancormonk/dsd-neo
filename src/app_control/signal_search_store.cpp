// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <dsd-neo/app_control/signal_search.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/runtime/signal_search.h>
#include <dsd-neo/runtime/signal_search_avoids.h>
#include <mutex>
#include <numeric>
#include <stdint.h>
#include <stdlib.h>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#ifdef DSD_NEO_TEST_HOOKS
#include "signal_search_store_test_hooks.h"
#endif

namespace {
using Hit = dsd_signal_search_hit;
using Bytes = std::vector<unsigned char>;
using FileLoad = dsd_app_search_file_load;
constexpr int history_file = DSD_APP_SEARCH_STORE_HISTORY;
constexpr int avoids_file = DSD_APP_SEARCH_STORE_AVOIDS;
constexpr int v1_file = DSD_APP_SEARCH_STORE_V1;
constexpr int file_count = 3;
/* A file the store could not read is kept as "<name>.unreadable-1" .. "-99", the first name that is free. */
constexpr int kept_limit = 99;

#ifdef DSD_NEO_TEST_HOOKS
struct TestHooks {
    int fail_open[file_count] = {0, 0, 0};
    long fail_read[file_count] = {-1, -1, -1};
    int fail_probe = 0;
    void (*after_aside)(const char*, const char*) = nullptr;
};

TestHooks test_hooks;
#endif
/* Record magics, little endian. Version 2 appends the checked site, call slot and encryption to version 1's body.
   Each version has its own file: version 2 records live in the version 2 journal, and the version 1 journal an
   earlier release keeps writing is only ever read (migrated from while no version 2 journal exists). */
constexpr uint32_t magic_v1 = 0x31535344; // DSS1
constexpr uint32_t magic = 0x32535344;    // DSS2

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

/* The text up to its NUL, then zeros: bytes left behind a shorter text are neither persisted nor compared. The last
   byte is always NUL. */
template <size_t N>
void
put_text(Bytes& b, const char (&text)[N]) {
    size_t i = 0;
    for (; i + 1 < N && text[i]; ++i) {
        b.push_back((unsigned char)text[i]);
    }
    b.insert(b.end(), N - i, 0);
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
    b.reserve(2048); // one record (1,652 bytes) without regrowing: this runs on every publication
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
    put_text(b, h.site);
    put(b, h.slot >= 0 && h.slot <= 2 ? (unsigned)h.slot : 0U, 1);
    put_text(b, h.crypt);
    return b;
}

/* @p b holds one whole record of @p version (its size was checked against the magic). Nothing loaded is trusted:
   every text ends at its own last byte and the slot is 0, 1 or 2. */
Hit
decode(const Bytes& b, uint32_t version) {
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
    if (version >= 2) {
        take_text(b, at, h.site);
        h.slot = (int)take(b, at, 1);
        if (h.slot > 2) {
            h.slot = 0;
        }
        take_text(b, at, h.crypt);
    }
    return h;
}

/* The body size of a version 1 record: the version 2 body without what version 2 appended. */
size_t
v1_size() {
    Hit h{};
    return encode(h).size() - sizeof(h.site) - 1 - sizeof(h.crypt);
}

/* The version 2 journal beside the version 1 journal @p base: a trailing ".v1" becomes ".v2", else ".v2" is
   appended. */
std::string
v2_journal_path(const std::string& base) {
    const std::string v1 = ".v1";
    if (base.size() > v1.size() && base.compare(base.size() - v1.size(), v1.size(), v1) == 0) {
        return base.substr(0, base.size() - v1.size()) + ".v2";
    }
    return base + ".v2";
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

/* @p text is the file's @p n bytes followed by a NUL. A NUL inside them is not a list the store takes (issue #828: the
   text after it would be dropped), nor are more than DSD_SEARCH_MAX_AVOIDS different frequencies; a frequency the file
   repeats is kept once (issue #828), so one removal lifts it. */
bool
parse_avoids(const char* text, size_t n, std::vector<uint32_t>& result) {
    if (std::memchr(text, '\0', n) != nullptr || text[n] != '\0') {
        return false;
    }
    const char* p = skip_space(text);
    if (*p++ != '[') {
        return false;
    }
    p = skip_space(p);
    if (*p == ']') {
        return !*skip_space(p + 1);
    }
    for (;;) {
        uint32_t frequency = 0;
        if (!parse_avoid_frequency(p, frequency)) {
            return false;
        }
        if (std::find(result.begin(), result.end(), frequency) == result.end()) {
            if (result.size() == DSD_SEARCH_MAX_AVOIDS) {
                return false;
            }
            result.push_back(frequency);
        }
        if (*p == ']') {
            return !*skip_space(p + 1);
        }
        if (*p++ != ',') {
            return false;
        }
        p = skip_space(p);
    }
}

/* The name after @p path's last folder separator. */
size_t
name_start(const std::string& path) {
#if DSD_PLATFORM_WIN_NATIVE
    const size_t cut = path.find_last_of("/\\");
#else
    const size_t cut = path.find_last_of('/');
#endif
    return cut == std::string::npos ? 0 : cut + 1;
}

std::string
base_name(const std::string& path) {
    return path.substr(name_start(path));
}

std::string
kept_path(const std::string& path, int n) {
    return path + ".unreadable-" + std::to_string(n);
}

bool
confirmed_absent(int err) {
    return err == ENOENT || err == ENOTDIR;
}

/* Every load opens through here: a regular file only, without following a final symbolic link and without waiting on a
   FIFO (issue #828). */
FILE*
open_existing(const std::string& path, int which) {
#ifdef DSD_NEO_TEST_HOOKS
    if (test_hooks.fail_open[which]) {
        errno = test_hooks.fail_open[which];
        return nullptr;
    }
#else
    (void)which;
#endif
    errno = 0;
    return dsd_fopen_existing_regular_file(path.c_str(), "rb");
}

/* What an open_existing() that just failed says about the file: only a confirmed absence is missing. */
FileLoad
open_failure() {
    return confirmed_absent(errno) ? DSD_APP_SEARCH_FILE_MISSING : DSD_APP_SEARCH_FILE_UNREADABLE;
}

/* Reads one store file, telling a read error from its end: a short fread() is either, and only ferror() says which
   (issue #828). */
class Reader {
  public:
    Reader(FILE* file, int which) : f_(file) {
#ifdef DSD_NEO_TEST_HOOKS
        fail_after_ = test_hooks.fail_read[which];
#else
        (void)which;
#endif
    }

    size_t
    read(void* out, size_t n) {
        size_t want = n;
#ifdef DSD_NEO_TEST_HOOKS
        if (fail_after_ >= 0 && at_ + n > (size_t)fail_after_) {
            want = (size_t)fail_after_ > at_ ? (size_t)fail_after_ - at_ : 0;
            failed_ = true;
        }
#endif
        const size_t got = want ? fread(out, 1, want, f_) : 0;
#ifdef DSD_NEO_TEST_HOOKS
        at_ += got;
#endif
        if (ferror(f_)) {
            failed_ = true;
        }
        return got;
    }

    bool
    failed() const {
        return failed_;
    }

  private:
    FILE* f_;
    bool failed_ = false;
#ifdef DSD_NEO_TEST_HOOKS
    size_t at_ = 0;
    long fail_after_ = -1;
#endif
};

/* Whether kept name @p n of a version 2 journal exists: 1, 0, or -1 when that cannot be told. */
int
probe_kept(const std::string& name, int n) {
#ifdef DSD_NEO_TEST_HOOKS
    if (test_hooks.fail_probe == n) {
        errno = EACCES;
        return -1;
    }
#else
    (void)n;
#endif
    return dsd_path_exists_nofollow(name.c_str());
}

/* What renaming a marked file aside found. */
enum class Aside { None, Kept, Gone, Refused };

/* Renames the marked @p path, never following it, to its first free kept name, which @p kept receives as a base name.
   A file or a link (whatever it names: the link itself moves) is moved. Gone when the path is confirmed absent
   (removed behind the store's back: nothing is left to keep). Refused, leaving everything as it was, when it cannot be
   told whether the path exists, when it is a real folder, FIFO, socket or device, which is never moved or replaced, or
   when no kept name is free. A no-replace rename that made the new name but could not remove the old one (its
   out_source_left) has still kept the file: the replacement then takes the old name. */
Aside
keep_aside(const std::string& path, std::string& kept) {
    dsd_path_type type = DSD_PATH_TYPE_OTHER;
    const int present = dsd_path_type_nofollow(path.c_str(), &type);
    if (present == 0) {
        return Aside::Gone;
    }
    if (present < 0 || (type != DSD_PATH_TYPE_FILE && type != DSD_PATH_TYPE_LINK)) {
        return Aside::Refused;
    }
    for (int n = 1; n <= kept_limit; ++n) {
        const std::string to = kept_path(path, n);
        if (dsd_rename_noreplace(path.c_str(), to.c_str(), nullptr) == 0) {
#ifdef DSD_NEO_TEST_HOOKS
            if (test_hooks.after_aside) {
                test_hooks.after_aside(path.c_str(), to.c_str());
            }
#endif
            kept = base_name(to);
            return Aside::Kept;
        }
        if (errno != EEXIST) {
            return Aside::Refused;
        }
    }
    return Aside::Refused;
}

/* Whether @p target may take a replacement, never following it: nothing there (moved aside, or gone), or a file or a
   link (the link itself is replaced). A folder, FIFO, socket or device is never replaced, and neither is a path that
   cannot be told: a rename over a FIFO would delete it. */
bool
replaceable(const std::string& target) {
    dsd_path_type type = DSD_PATH_TYPE_OTHER;
    const int present = dsd_path_type_nofollow(target.c_str(), &type);
    return present == 0 || (present == 1 && (type == DSD_PATH_TYPE_FILE || type == DSD_PATH_TYPE_LINK));
}

/* How one file's write went, and what renaming it aside first found. */
struct Replaced {
    bool ok = false;
    Aside aside = Aside::None;
    std::string kept;
};

/* Replaces @p target with what @p write puts in a temporary file beside it, in the order that keeps a file the store
   could not read (@p marked): (1) write and sync the temporary file, (2) rename the marked target aside, (3) replace
   the target. A failure at (1) or (2) moves nothing; one at (3) leaves the file kept. (3) replaces only nothing, a
   file or a link (replaceable()), so the retry after a kept file, which is no longer marked, never replaces what the
   move would have refused. */
template <typename Write>
Replaced
replace_file(const std::string& target, bool marked, Write write) {
    Replaced out;
    char temp[4096];
    FILE* f = dsd_fopen_private_temp_for_replace(target.c_str(), temp, sizeof(temp), "wb");
    if (!f) {
        return out;
    }
    bool ok = write(f);
    if (fflush(f) != 0 || dsd_fsync(dsd_fileno(f)) != 0) {
        ok = false;
    }
    if (fclose(f) != 0) {
        ok = false;
    }
    if (ok && marked) {
        out.aside = keep_aside(target, out.kept);
        ok = out.aside == Aside::Kept || out.aside == Aside::Gone;
    }
    if (ok) {
        ok = replaceable(target) && dsd_replace_file_with_temp(temp, target.c_str()) == 0;
    }
    if (!ok) {
        (void)dsd_remove_file(temp);
    }
    out.ok = ok;
    return out;
}

/* The temporary files of the store's own targets ("<target>.tmp.XXXXXX", dsd_fopen_private_temp_for_replace()) that a
   crash between their creation and their rename left behind (issue #828). */
struct TempSweep {
    std::vector<std::string> prefixes, found;
};

int
collect_temp(const char* name, void* user) {
    auto* sweep = static_cast<TempSweep*>(user);
    const std::string entry(name);
    if (std::any_of(sweep->prefixes.begin(), sweep->prefixes.end(), [&entry](const std::string& prefix) {
            return entry.size() == prefix.size() + 6 && entry.compare(0, prefix.size(), prefix) == 0;
        })) {
        sweep->found.push_back(entry);
    }
    return 0;
}

/* The folder holding @p path, to list it: "." for a bare name, else the path before its name without the separator,
   except at a root ("/", "C:\\"), which keeps it: "C:" alone names that drive's current folder, not its root. */
std::string
folder_of(const std::string& path) {
    const size_t start = name_start(path);
    if (start == 0) {
        return ".";
    }
    const bool root = start == 1 || (start == 3 && path[1] == ':');
    return path.substr(0, root ? start : start - 1);
}

/* Removes those of @p targets, which share one folder. Nothing else there is touched. */
void
sweep_temps(const std::vector<std::string>& targets) {
    if (targets.empty()) {
        return;
    }
    const std::string prefix = targets.front().substr(0, name_start(targets.front()));
    const std::string folder = folder_of(targets.front());
    TempSweep sweep;
    for (const std::string& target : targets) {
        sweep.prefixes.push_back(base_name(target) + ".tmp.");
    }
    (void)dsd_dir_list(folder.c_str(), collect_temp, &sweep);
    for (const std::string& name : sweep.found) {
        (void)dsd_remove_file((prefix + name).c_str());
    }
}

/* Appends @p batch to the journal. Only a regular file is appended to, opened without waiting on it: a FIFO or anything
   else that took the journal's name since it loaded fails the write, which stays owed and reported, and is never
   replaced. */
bool
append_history(const std::string& path, const std::vector<Hit>& batch) {
    FILE* f = dsd_fopen_private_append_regular(path.c_str());
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

/* One store file's state (issue #828): how the open found it, whether it is marked (it did not load whole, so its
   first replacement renames it aside first; only that clears the mark, or its confirmed absence then), the base name
   it was kept as, and whether its write is owed. */
struct StoreFile {
    FileLoad load = DSD_APP_SEARCH_FILE_MISSING;
    bool marked = false;
    bool write_failed = false;
    std::string kept;

    void
    loaded(FileLoad state) {
        load = state;
        marked = state == DSD_APP_SEARCH_FILE_UNREADABLE;
    }

    /* Under the lock, after this file's write: a move clears the mark for good, so a replacement that fails after it
       leaves the file kept and the write owed, and the retry only writes. */
    void
    written(const Replaced& result) {
        write_failed = !result.ok;
        if (result.aside == Aside::Kept) {
            marked = false;
            load = DSD_APP_SEARCH_FILE_KEPT;
            kept = result.kept;
        } else if (result.aside == Aside::Gone) {
            marked = false;
            load = DSD_APP_SEARCH_FILE_MISSING;
        }
    }
};

struct Store {
    std::mutex mutex;
    std::condition_variable wake, flushed;
    std::thread worker;
    /* `base` is the path the store was opened with: the version 1 journal, and the avoid list's prefix (unchanged, so
       every release shares one avoid list). `path` is the version 2 journal this store reads and writes. */
    std::string base, path, avoid_path;
    std::vector<Hit> history, pending;
    /* The permanent avoids. The runtime's permanent list, which the running search reads, is set from it under the
       mutex at every change, so the two never disagree and only this store writes that list (issue #828). */
    std::vector<uint32_t> avoids;
    dsd_signal_search_status status{};
    bool stop = false, dirty_avoids = false, busy = false, compact = true;
    /* `retry` wakes the worker for one more attempt at a write owed (flush, close) without looping on storage that
       keeps failing. */
    bool retry = false;
    /* Indexed by dsd_app_search_store_file. A file whose write failed stays owed until its own write succeeds; every
       worker pass retries it. */
    StoreFile files[file_count];
    size_t records = 0;
    /* Bumped on every change a frontend shows: history rows (upserts, trims, reloads), the permanent avoids, and
       which session's avoid list is shown (session_avoids_shown_locked()); the runtime's session revision adds the
       rest. */
    uint64_t history_revision = 1, avoids_revision = 1;

    ~Store() { close(); }

    bool
    owed() const {
        return files[history_file].write_failed || files[avoids_file].write_failed;
    }

    void
    close() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stop = true;
            if (owed()) {
                retry = true;
            }
            wake.notify_all();
        }
        if (worker.joinable()) {
            worker.join();
        }
    }

    /* Reads every record of @p version from @p f: whole at a clean end; unreadable at a read error or at the first
       record that is incomplete, damaged or of any other version, keeping the records before it (a damaged or torn
       journal is kept aside too, so nothing after the damage is lost; issue #828). */
    FileLoad
    read_journal(FILE* f, uint32_t version, int which) {
        const size_t size = version == 2 ? encode(Hit{}).size() : v1_size();
        Reader in{f, which};
        for (;;) {
            Bytes header(12);
            const size_t got = in.read(header.data(), header.size());
            if (in.failed()) {
                return DSD_APP_SEARCH_FILE_UNREADABLE;
            }
            if (!got) {
                return DSD_APP_SEARCH_FILE_OK;
            }
            size_t at = 0;
            if (got != header.size() || take(header, at, 4) != (version == 2 ? magic : magic_v1)
                || take(header, at, 4) != size) {
                return DSD_APP_SEARCH_FILE_UNREADABLE;
            }
            uint32_t sum = (uint32_t)take(header, at, 4);
            Bytes bytes(size);
            if (in.read(bytes.data(), size) != size || checksum(bytes) != sum) {
                return DSD_APP_SEARCH_FILE_UNREADABLE;
            }
            Hit h = decode(bytes, version);
            if (h.session_id && h.hit_id && h.frequency_hz) {
                h.ended = 1;
                upsert(history, h);
            }
            ++records;
        }
    }

    /* With no version 2 journal, whether the version 1 rows may load: not while a kept version 2 journal exists, which
       a crash between renaming it aside and replacing it leaves alone. Every kept name is checked (one may have been
       removed), and one that cannot be checked counts as kept. The history then reports the newest kept name, or
       stays unreadable when only unknowns were found. */
    bool
    v1_allowed() {
        bool unknown = false;
        std::string newest;
        for (int n = 1; n <= kept_limit; ++n) {
            const std::string name = kept_path(path, n);
            const int found = probe_kept(name, n);
            if (found > 0) {
                newest = name;
            } else if (found < 0) {
                unknown = true;
            }
        }
        if (!newest.empty()) {
            files[history_file].load = DSD_APP_SEARCH_FILE_KEPT;
            files[history_file].kept = base_name(newest);
            return false;
        }
        if (unknown) {
            files[history_file].loaded(DSD_APP_SEARCH_FILE_UNREADABLE);
            return false;
        }
        return true;
    }

    /* The version 2 journal when it exists, even empty or damaged; otherwise the rows of the version 1 journal, which
       the first write after opening (a compaction) copies into a new version 2 journal. The version 1 journal is never
       written, replaced, moved or removed: one that cannot be read whole is only reported (issue #828). */
    void
    load_history() {
        FILE* f = open_existing(path, history_file);
        if (f) {
            files[history_file].loaded(read_journal(f, 2, history_file));
            fclose(f);
            return;
        }
        files[history_file].loaded(open_failure());
        if (files[history_file].load != DSD_APP_SEARCH_FILE_MISSING || !v1_allowed()) {
            return;
        }
        f = open_existing(base, v1_file);
        if (!f) {
            files[v1_file].load = open_failure();
            return;
        }
        files[v1_file].load = read_journal(f, 1, v1_file);
        fclose(f);
    }

    /* All or nothing: a list that cannot be read whole loads empty and is marked. */
    void
    load_avoids() {
        FILE* f = open_existing(avoid_path, avoids_file);
        if (!f) {
            files[avoids_file].loaded(open_failure());
            return;
        }
        char buf[16385];
        Reader in{f, avoids_file};
        const size_t n = in.read(buf, sizeof(buf) - 1);
        const bool whole = !in.failed() && n < sizeof(buf) - 1;
        buf[n] = '\0';
        fclose(f);
        std::vector<uint32_t> parsed;
        if (whole && parse_avoids(buf, n, parsed)) {
            avoids = std::move(parsed);
            files[avoids_file].loaded(DSD_APP_SEARCH_FILE_OK);
        } else {
            files[avoids_file].loaded(DSD_APP_SEARCH_FILE_UNREADABLE);
        }
    }

    void
    load() {
        history.clear();
        avoids.clear();
        records = 0;
        ++history_revision;
        ++avoids_revision;
        std::fill(files, files + file_count, StoreFile{});
        load_history();
        load_avoids();
    }

    Replaced
    compact_history(const std::vector<Hit>& all, bool marked) const {
        return replace_file(path, marked, [&all](FILE* f) {
            return std::all_of(all.begin(), all.end(), [f](const Hit& h) { return write_hit(f, h); });
        });
    }

    Replaced
    save_avoids(const std::vector<uint32_t>& list, bool marked) const {
        return replace_file(avoid_path, marked, [&list](FILE* f) {
            bool ok = DSD_FPRINTF(f, "[") >= 0;
            for (size_t i = 0; i < list.size(); ++i) {
                if (DSD_FPRINTF(f, "%s%u", i ? "," : "", list[i]) < 0) {
                    ok = false;
                }
            }
            return DSD_FPRINTF(f, "]\n") >= 0 && ok;
        });
    }

    /* Saving one file settles nothing about the other. A failed history write leaves `compact` set, so its retry
       rewrites every retained record once instead of appending again. */
    void
    record_write_result(bool write_history, const Replaced& history_result, bool write_avoids,
                        const Replaced& avoids_result, size_t saved_records) {
        if (write_history) {
            files[history_file].written(history_result);
            compact = !history_result.ok;
            if (history_result.ok) {
                records = saved_records;
            }
        }
        if (write_avoids) {
            files[avoids_file].written(avoids_result);
        }
    }

    /* One worker pass, entered and left holding @p lock: takes the pending detections and every file still owed,
       writes them unlocked, and records how each write went. A marked history is always replaced, never appended to. */
    void
    write_pass(std::unique_lock<std::mutex>& lock) {
        std::vector<Hit> batch;
        batch.swap(pending);
        auto all = history;
        auto avoid_copy = avoids;
        const bool write_history = !batch.empty() || files[history_file].write_failed;
        const bool write_avoids = dirty_avoids || files[avoids_file].write_failed;
        const bool history_marked = files[history_file].marked, avoids_marked = files[avoids_file].marked;
        dirty_avoids = false;
        retry = false;
        bool replace = compact || history_marked || records + batch.size() > 4000;
        busy = true;
        lock.unlock();
        Replaced history_result{true, Aside::None, {}};
        if (write_history) {
            history_result =
                replace ? compact_history(all, history_marked) : Replaced{append_history(path, batch), Aside::None, {}};
        }
        Replaced avoids_result{true, Aside::None, {}};
        if (write_avoids) {
            avoids_result = save_avoids(avoid_copy, avoids_marked);
        }
        lock.lock();
        record_write_result(write_history, history_result, write_avoids, avoids_result,
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

/* Hand the permanent avoids to the runtime list the search reads. Under the store's mutex, so the runtime takes every
   change in the order the store made it. */
void
avoids_publish_locked() {
    dsd_signal_search_avoids_permanent_set(store.avoids.data(), store.avoids.size());
}

/* The session whose own avoid list the frontends are shown: the store's running or paused search, none once it failed
   or stopped. The list itself is the engine's, in the runtime. */
uint64_t
session_avoids_shown_locked() {
    const int phase = store.status.phase;
    return phase == DSD_SEARCH_STOPPED || phase == DSD_SEARCH_ERROR ? 0 : store.status.session_id;
}

/* Replace the published status, moving the avoid revision when that changes which session's avoids are shown. */
void
status_set_locked(const dsd_signal_search_status& status) {
    const uint64_t shown = session_avoids_shown_locked();
    store.status = status;
    if (session_avoids_shown_locked() != shown) {
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
        if (store.worker.joinable() && store.base == path && !store.stop) {
            return 0;
        }
    }
    store.close();
    std::lock_guard<std::mutex> lock(store.mutex);
    store.base = path;
    store.path = v2_journal_path(store.base);
    store.avoid_path = store.base + ".avoids.json";
    store.stop = false;
    store.compact = true;
    store.retry = false;
    store.pending.clear();
    sweep_temps({store.path, store.avoid_path});
    store.load();
    avoids_publish_locked();
    store.worker = std::thread([] { store.run(); });
    return 0;
}

/* A closed store keeps no permanent avoids, and a search reads none from it; the next open loads the file again. */
extern "C" void
dsd_app_signal_search_store_close(void) {
    store.close();
    std::lock_guard<std::mutex> lock(store.mutex);
    if (!store.avoids.empty()) {
        store.avoids.clear();
        ++store.avoids_revision;
    }
    avoids_publish_locked();
}

extern "C" void
dsd_app_signal_search_store_flush(void) {
    std::unique_lock<std::mutex> lock(store.mutex);
    if (!store.worker.joinable()) {
        return;
    }
    if (store.owed()) {
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
    status_set_locked(*status);
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
        DSD_SNPRINTF(error, capacity, "%s",
                     store.owed() ? "Signal search history or avoids could not be saved; retained in memory" : "");
    }
}

extern "C" int
dsd_app_signal_search_store_file_state(int which, dsd_app_search_file_load* state, int* write_failed, char* kept_name,
                                       size_t cap) {
    if (which < 0 || which >= file_count) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(store.mutex);
    const StoreFile& file = store.files[which];
    if (state) {
        *state = file.load;
    }
    if (write_failed) {
        *write_failed = file.write_failed ? 1 : 0;
    }
    if (kept_name && cap) {
        DSD_SNPRINTF(kept_name, cap, "%s", file.load == DSD_APP_SEARCH_FILE_KEPT ? file.kept.c_str() : "");
    }
    return 0;
}

#ifdef DSD_NEO_TEST_HOOKS
extern "C" void
dsd_app_signal_search_store_test_fail_open(int which, int err) {
    if (which >= 0 && which < file_count) {
        test_hooks.fail_open[which] = err;
    }
}

extern "C" void
dsd_app_signal_search_store_test_fail_read(int which, long after) {
    if (which >= 0 && which < file_count) {
        test_hooks.fail_read[which] = after;
    }
}

extern "C" void
dsd_app_signal_search_store_test_fail_probe(int n) {
    test_hooks.fail_probe = n;
}

extern "C" void
dsd_app_signal_search_store_test_folder_of(const char* path, char* out, size_t cap) {
    DSD_SNPRINTF(out, cap, "%s", folder_of(path).c_str());
}

extern "C" void
dsd_app_signal_search_store_test_after_aside(void (*fn)(const char* path, const char* kept)) {
    std::lock_guard<std::mutex> lock(store.mutex);
    test_hooks.after_aside = fn;
}
#endif

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

/* Both counters only grow, so their sum moves whenever either does. */
extern "C" uint64_t
dsd_app_signal_search_avoids_revision(void) {
    std::lock_guard<std::mutex> lock(store.mutex);
    return store.avoids_revision + dsd_signal_search_avoids_session_revision();
}

extern "C" size_t
dsd_app_signal_search_session_avoids_get(uint32_t* frequencies, size_t capacity) {
    std::lock_guard<std::mutex> lock(store.mutex);
    const uint64_t shown = session_avoids_shown_locked();
    if (!frequencies || !capacity || !shown) {
        return 0;
    }
    uint64_t session = 0;
    const size_t n = dsd_signal_search_avoids_session_get(&session, frequencies, capacity);
    return session == shown ? n : 0;
}

extern "C" void
dsd_app_signal_search_session_ended(void) {
    std::lock_guard<std::mutex> lock(store.mutex);
    /* A rejected start leaves an error with no coordinator to publish STOPPED. Keep its message for the idle screen,
       but nothing the next session sees may claim the tuner or a running phase. */
    if (store.status.phase != DSD_SEARCH_STOPPED || store.status.owned) {
        dsd_signal_search_status ended = store.status;
        ended.phase = DSD_SEARCH_STOPPED;
        ended.owned = 0;
        ended.identifying = 0;
        ended.live_power_valid = 0;
        ++ended.revision;
        status_set_locked(ended);
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

extern "C" int
dsd_app_signal_search_permanent_avoid(uint32_t frequency) {
    if (frequency == 0) {
        return DSD_APP_SEARCH_AVOID_INVALID;
    }
    std::lock_guard<std::mutex> lock(store.mutex);
    if (std::find(store.avoids.begin(), store.avoids.end(), frequency) != store.avoids.end()) {
        return DSD_APP_SEARCH_AVOID_DUPLICATE;
    }
    if (store.avoids.size() >= DSD_SEARCH_MAX_AVOIDS) {
        return DSD_APP_SEARCH_AVOID_FULL;
    }
    store.avoids.push_back(frequency);
    ++store.avoids_revision;
    avoids_publish_locked();
    store.dirty_avoids = true;
    store.wake.notify_all();
    return DSD_APP_SEARCH_AVOID_ADDED;
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
    avoids_publish_locked();
    store.dirty_avoids = true;
    store.wake.notify_all();
    return 0;
}
