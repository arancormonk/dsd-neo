// SPDX-License-Identifier: GPL-3.0-or-later
// Intentional rule fixtures; run with semgrep --test, never compile.

void
scalar_values(float other_float, double other_double) {
    float value = 0.1f;
    double precise = 0.2;
    // ruleid: dsd-neo.no-floating-point-equality-in-tests
    check(value == other_float);
    // ruleid: dsd-neo.no-floating-point-equality-in-tests
    check(other_float != value);
    // ruleid: dsd-neo.no-floating-point-equality-in-tests
    check(precise != other_double);
    // ruleid: dsd-neo.no-floating-point-equality-in-tests
    check(other_double == precise);
}

void
float_arrays(unsigned i, const float* expected) {
    float output[8] = {};
    // ruleid: dsd-neo.no-floating-point-equality-in-tests
    check(output[i] == expected[i]);
    // ruleid: dsd-neo.no-floating-point-equality-in-tests
    check(output[i] != expected[i + 4]);
    // ruleid: dsd-neo.no-floating-point-equality-in-tests
    check(expected[i] == output[i]);
    // ruleid: dsd-neo.no-floating-point-equality-in-tests
    check(expected[i + 4] != output[i]);
    // Explicit literal sentinels retain the scalar rule's existing exemption.
    // ok: dsd-neo.no-floating-point-equality-in-tests
    check(output[i] == 0.0f);
    // ok: dsd-neo.no-floating-point-equality-in-tests
    check(fabsf(output[i] - expected[i]) < 1e-6f);
}

void
const_arrays(unsigned i, double actual) {
    const double expected[] = {0.1, 0.2};
    const float samples[] = {0.1f, -0.2f};
    // ruleid: dsd-neo.no-floating-point-equality-in-tests
    check(actual == expected[i]);
    // ruleid: dsd-neo.no-floating-point-equality-in-tests
    check(samples[i] != actual);
}

void
uninitialized_arrays(unsigned i, float actual) {
    float output[8];
    double precise[8];
    fill(output, precise);
    // ruleid: dsd-neo.no-floating-point-equality-in-tests
    check(output[i] == actual);
    // ruleid: dsd-neo.no-floating-point-equality-in-tests
    check(actual != precise[i]);
}

void
integer_arrays(unsigned i, int actual) {
    int output[8] = {};
    const unsigned expected[] = {1, 2};
    // ok: dsd-neo.no-floating-point-equality-in-tests
    check(output[i] == actual);
    // ok: dsd-neo.no-floating-point-equality-in-tests
    check(actual != expected[i]);
    // ok: dsd-neo.no-floating-point-equality-in-tests
    check(i == 0);
}
