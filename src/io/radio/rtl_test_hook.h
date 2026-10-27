// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef DSD_NEO_SRC_IO_RADIO_RTL_TEST_HOOK_H_
#define DSD_NEO_SRC_IO_RADIO_RTL_TEST_HOOK_H_

#include <atomic>
#include <mutex>

/* Keep a callback and its context together when a test changes them while a worker runs. Invoke outside the lock:
 * a callback can wait for another worker or install its replacement. Callers keep contexts alive until workers join. */
template <typename... Args>
class RtlTestHook {
  public:
    using Callback = void (*)(Args..., void*);

    void
    set(Callback callback, void* context) {
        const std::lock_guard<std::mutex> lock(mutex_);
        context_ = context;
        callback_.store(callback, std::memory_order_relaxed);
    }

    void
    invoke(Args... args) const {
        if (!callback_.load(std::memory_order_relaxed)) {
            return;
        }
        Callback callback;
        void* context;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            callback = callback_.load(std::memory_order_relaxed);
            context = context_;
        }
        if (callback) {
            callback(args..., context);
        }
    }

  private:
    mutable std::mutex mutex_;
    std::atomic<Callback> callback_{nullptr};
    void* context_ = nullptr;
};

#endif
