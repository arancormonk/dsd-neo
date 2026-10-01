// SPDX-License-Identifier: GPL-3.0-or-later
// Intentional rule fixtures; run with semgrep --test, never compile.

void
scalar_values(float other_float, double other_double) {
    float value = 0.1f;
    double precise = 0.2;
    // ruleid: dsd-neo.no-floating-point-equality
    check(value == other_float);
    // ruleid: dsd-neo.no-floating-point-equality
    check(other_float != value);
    // ruleid: dsd-neo.no-floating-point-equality
    check(precise != other_double);
    // ruleid: dsd-neo.no-floating-point-equality
    check(other_double == precise);
}

void
float_arrays(unsigned i, const float* expected) {
    float output[8] = {};
    // ruleid: dsd-neo.no-floating-point-equality
    check(output[i] == expected[i]);
    // ruleid: dsd-neo.no-floating-point-equality
    check(output[i] != expected[i + 4]);
    // ruleid: dsd-neo.no-floating-point-equality
    check(expected[i] == output[i]);
    // ruleid: dsd-neo.no-floating-point-equality
    check(expected[i + 4] != output[i]);
    // Explicit literal sentinels retain the scalar rule's existing exemption.
    // ok: dsd-neo.no-floating-point-equality
    check(output[i] == 0.0f);
    // ok: dsd-neo.no-floating-point-equality
    check(fabsf(output[i] - expected[i]) < 1e-6f);
}

struct tuning_fixture {
    double squelch;
    int volume;
};

void
struct_members(const struct tuning_fixture* previous, const struct tuning_fixture* current, dsd_opts* opts) {
    // ruleid: dsd-neo.no-floating-point-equality
    check(previous->squelch != current->squelch);
    // ruleid: dsd-neo.no-floating-point-equality
    check(opts->rtl_squelch_level != previous->squelch);
    // ruleid: dsd-neo.no-floating-point-equality
    check(opts->input_warn_db == current->squelch);
    // Integer members and literal sentinels are outside the rule.
    // ok: dsd-neo.no-floating-point-equality
    check(previous->volume != current->volume);
    // ok: dsd-neo.no-floating-point-equality
    check(previous->squelch == 0.0);
    // ok: dsd-neo.no-floating-point-equality
    check(fabs(previous->squelch - current->squelch) > 1e-12);
}

void
parameters(double seconds, const float level, int count) {
    // ruleid: dsd-neo.no-floating-point-equality
    check(seconds == stored_seconds());
    // ruleid: dsd-neo.no-floating-point-equality
    check(other_level() != level);
    // ok: dsd-neo.no-floating-point-equality
    check(count == other_count());
    // ok: dsd-neo.no-floating-point-equality
    check(seconds == 0.0);
}

void
const_arrays(unsigned i, double actual) {
    const double expected[] = {0.1, 0.2};
    const float samples[] = {0.1f, -0.2f};
    // ruleid: dsd-neo.no-floating-point-equality
    check(actual == expected[i]);
    // ruleid: dsd-neo.no-floating-point-equality
    check(samples[i] != actual);
}

void
uninitialized_arrays(unsigned i, float actual) {
    float output[8];
    double precise[8];
    fill(output, precise);
    // ruleid: dsd-neo.no-floating-point-equality
    check(output[i] == actual);
    // ruleid: dsd-neo.no-floating-point-equality
    check(actual != precise[i]);
}

void
integer_arrays(unsigned i, int actual) {
    int output[8] = {};
    const unsigned expected[] = {1, 2};
    // ok: dsd-neo.no-floating-point-equality
    check(output[i] == actual);
    // ok: dsd-neo.no-floating-point-equality
    check(actual != expected[i]);
    // ok: dsd-neo.no-floating-point-equality
    check(i == 0);
}

// ruleid: dsd-neo.no-direct-clock-read
#include <QElapsedTimer>
// ok: dsd-neo.no-direct-clock-read
#include <chrono>

void
cpp_clock_reads() {
    // ruleid: dsd-neo.no-direct-clock-read
    std::time_t wall = std::time(nullptr);
    // ruleid: dsd-neo.no-direct-clock-read
    std::time_t global = ::time(nullptr);
    // ruleid: dsd-neo.no-direct-clock-read
    auto steady = std::chrono::steady_clock::now();
    // ruleid: dsd-neo.no-direct-clock-read
    auto system = std::chrono::system_clock::now();
    // ruleid: dsd-neo.no-direct-clock-read
    auto precise = std::chrono::high_resolution_clock::now();
    using namespace std::chrono;
    // ruleid: dsd-neo.no-direct-clock-read
    auto unqualified = steady_clock::now();
    // ruleid: dsd-neo.no-direct-clock-read
    auto partly = chrono::system_clock::now();
    consume(wall, global, steady, system, precise, unqualified, partly);
}

void
qt_clock_reads() {
    // ruleid: dsd-neo.no-direct-clock-read
    qint64 ms = QDateTime::currentMSecsSinceEpoch();
    // ruleid: dsd-neo.no-direct-clock-read
    qint64 secs = QDateTime::currentSecsSinceEpoch();
    // ruleid: dsd-neo.no-direct-clock-read
    QString shown = QDateTime::currentDateTime().toString();
    // ruleid: dsd-neo.no-direct-clock-read
    QDateTime utc = QDateTime::currentDateTimeUtc();
    // ruleid: dsd-neo.no-direct-clock-read
    QDate today = QDate::currentDate();
    // ruleid: dsd-neo.no-direct-clock-read
    QTime clock = QTime::currentTime();
    // ruleid: dsd-neo.no-direct-clock-read
    QElapsedTimer timer;
    timer.start();
    // ruleid: dsd-neo.no-direct-clock-read
    auto* heap_timer = new QElapsedTimer();
    // ruleid: dsd-neo.no-direct-clock-read
    qint64 reference = QElapsedTimer::msecsSinceReference();
    consume(ms, secs, shown, utc, today, clock, heap_timer, reference);
}

class ThrottledModel {
    // ruleid: dsd-neo.no-direct-clock-read
    QElapsedTimer m_throttle;
    int m_count = 0;
};

void
cpp_named_domain_reads(const HistoryRow& row, Scheduler& scheduler) {
    // ok: dsd-neo.no-direct-clock-read
    const auto decoded_ms = static_cast<qint64>(dsd_decode_now_realtime_s() * 1000.0);
    // ok: dsd-neo.no-direct-clock-read
    const qint64 real_ms = dsd_qt::realtimeMSecsSinceEpoch();
    // ok: dsd-neo.no-direct-clock-read
    const QDate real_today = dsd_qt::realtimeCurrentDate();
    // Converting a stored stamp, durations, and other things called now() or time() are not clock reads.
    // ok: dsd-neo.no-direct-clock-read
    QDateTime stamped = QDateTime::fromMSecsSinceEpoch(row.when_ms);
    // ok: dsd-neo.no-direct-clock-read
    auto wait = std::chrono::milliseconds(10);
    // ok: dsd-neo.no-direct-clock-read
    auto queued = Scheduler::now();
    // ok: dsd-neo.no-direct-clock-read
    auto tick = scheduler.now();
    // ok: dsd-neo.no-direct-clock-read
    auto when = row.time();
    consume(decoded_ms, real_ms, real_today, stamped, wait, queued, tick, when);
}
