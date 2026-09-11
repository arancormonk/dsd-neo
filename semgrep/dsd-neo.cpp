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
