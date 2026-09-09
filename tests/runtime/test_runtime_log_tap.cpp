// SPDX-License-Identifier: GPL-3.0-or-later
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <dsd-neo/runtime/log.h>
#include <thread>
#include <vector>
static std::atomic<int> calls{0};

static void
tap(dsd_neo_log_level_t level, const char* text, void* ctx) {
    if (level != LOG_LEVEL_WARN || !text || ctx != &calls) {
        std::abort();
    }
    ++calls;
    dsd_neo_log_write(LOG_LEVEL_INFO, "nested\n");
}

int
main() {
    dsd_neo_log_set_tap(tap, &calls);
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([] {
            for (int j = 0; j < 100; ++j) {
                dsd_neo_log_write(LOG_LEVEL_WARN, "record %d\n", j);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    return calls == 400 ? 0 : 1;
}
