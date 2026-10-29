// SPDX-License-Identifier: GPL-3.0-or-later
// Intentional rule fixtures; run with semgrep --test, never compile.

struct clock_fixture {
    int time;
    double started_s;
};

struct clock_hooks {
    time_t (*time)(time_t* out);
};

// A prototype names a clock function without reading it.
// ok: dsd-neo.no-direct-clock-read
time_t time(time_t* out);
// ok: dsd-neo.no-direct-clock-read
uint64_t dsd_time_monotonic_ns(void);

void
c_platform_clock_reads(void) {
    time_t stamp = 0;
    struct timespec ts;
    struct timeval tv;
    // ruleid: dsd-neo.no-direct-clock-read
    stamp = time(NULL);
    // ruleid: dsd-neo.no-direct-clock-read
    (void)time(&stamp);
    // ruleid: dsd-neo.no-direct-clock-read
    clock_gettime(CLOCK_MONOTONIC, &ts);
    // ruleid: dsd-neo.no-direct-clock-read
    gettimeofday(&tv, NULL);
    // ruleid: dsd-neo.no-direct-clock-read
    timespec_get(&ts, TIME_UTC);
    // ruleid: dsd-neo.no-direct-clock-read
    uint64_t mono_ns = dsd_time_monotonic_ns();
    // ruleid: dsd-neo.no-direct-clock-read
    uint64_t mono_ms = dsd_time_monotonic_ms();
    // ruleid: dsd-neo.no-direct-clock-read
    uint64_t wall_ns = dsd_time_realtime_ns();
    // ruleid: dsd-neo.no-direct-clock-read
    uint64_t deadline_ns = dsd_time_deadline_ns(10U);
    // ruleid: dsd-neo.no-direct-clock-read
    double mono_s = dsd_time_now_monotonic_s();
    // ruleid: dsd-neo.no-direct-clock-read
    double wall_s = dsd_time_now_realtime_s();
    consume(stamp, mono_ns, mono_ms, wall_ns, deadline_ns, mono_s, wall_s);
}

void
c_os_clock_reads(void) {
    LARGE_INTEGER counter;
    FILETIME file_time;
    // ruleid: dsd-neo.no-direct-clock-read
    QueryPerformanceCounter(&counter);
    // ruleid: dsd-neo.no-direct-clock-read
    ULONGLONG ticks = GetTickCount64();
    // ruleid: dsd-neo.no-direct-clock-read
    DWORD short_ticks = GetTickCount();
    // ruleid: dsd-neo.no-direct-clock-read
    GetSystemTimeAsFileTime(&file_time);
    // ruleid: dsd-neo.no-direct-clock-read
    GetSystemTimePreciseAsFileTime(&file_time);
    // ruleid: dsd-neo.no-direct-clock-read
    uint64_t absolute = mach_absolute_time();
    consume(ticks, short_ticks, absolute);
}

void
c_named_domain_reads(struct clock_fixture* fixture, struct clock_fixture copy, const struct clock_hooks* hooks,
                     char* text, size_t text_size, const struct tm* calendar) {
    // The decode-clock and real-time wrappers name their domain.
    // ok: dsd-neo.no-direct-clock-read
    time_t decoded = dsd_decode_time();
    // ok: dsd-neo.no-direct-clock-read
    time_t real = dsd_realtime_time();
    // ok: dsd-neo.no-direct-clock-read
    fixture->started_s = dsd_decode_now_mono_s();
    // ok: dsd-neo.no-direct-clock-read
    uint64_t decoded_ms = dsd_decode_now_mono_ms();
    // ok: dsd-neo.no-direct-clock-read
    uint64_t real_ms = dsd_realtime_mono_ms();
    // ok: dsd-neo.no-direct-clock-read
    double real_wall_s = dsd_realtime_now_s();
    // Other identifiers that contain "time", and members named time, are not the C library call.
    // ok: dsd-neo.no-direct-clock-read
    strftime(text, text_size, "%H:%M", calendar);
    // ok: dsd-neo.no-direct-clock-read
    int field = fixture->time + copy.time;
    // ok: dsd-neo.no-direct-clock-read
    time_t hooked = hooks->time(NULL);
    // ok: dsd-neo.no-direct-clock-read
    (void)dsd_localtime(&decoded, &(struct tm){0});
    // ok: dsd-neo.no-direct-clock-read
    const char* spelled = "time(NULL)";
    consume(real, decoded_ms, real_ms, real_wall_s, field, hooked, spelled);
}

void
c_environment_writes(char* entry, const char* name) {
    // ruleid: dsd-neo.no-environment-write
    setenv(name, "1", 1);
    // ruleid: dsd-neo.no-environment-write
    unsetenv(name);
    // ruleid: dsd-neo.no-environment-write
    putenv(entry);
    // ruleid: dsd-neo.no-environment-write
    clearenv();
    // ruleid: dsd-neo.no-environment-write
    _putenv_s(name, "1");
    // ruleid: dsd-neo.no-environment-write
    SetEnvironmentVariableA(name, "1");
    // ruleid: dsd-neo.no-environment-write
    dsd_setenv(name, "1", 1);
    // ruleid: dsd-neo.no-environment-write
    dsd_unsetenv(name);
    // An override in the store is not a write.
    // ok: dsd-neo.no-environment-write
    dsd_env_set(DSD_ENV_LAYER_SESSION, name, "1");
    // ok: dsd-neo.no-environment-write
    dsd_env_set(DSD_ENV_LAYER_SESSION, name, NULL);
    // ok: dsd-neo.no-environment-write
    dsd_env_clear_layer(DSD_ENV_LAYER_SESSION);
}

void
c_environment_reads(const char* name, char* out, size_t out_size) {
    size_t required = 0;
    char* copy = NULL;
    // ruleid: dsd-neo.env-read-through-store
    const char* a = getenv(name);
    // ruleid: dsd-neo.env-read-through-store
    const char* b = secure_getenv(name);
    // ruleid: dsd-neo.env-read-through-store
    (void)getenv_s(&required, out, out_size, name);
    // ruleid: dsd-neo.env-read-through-store
    (void)_dupenv_s(&copy, &required, name);
    // ruleid: dsd-neo.env-read-through-store
    (void)GetEnvironmentVariableA(name, out, (DWORD)out_size);
    // The store, and the runtime forwarder over it, apply the overrides.
    // ok: dsd-neo.env-read-through-store
    const char* c = dsd_env_get(name);
    // ok: dsd-neo.env-read-through-store
    const char* d = dsd_neo_env_get(name);
    consume(a, b, c, d, copy);
}
