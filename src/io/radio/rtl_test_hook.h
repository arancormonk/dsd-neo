// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef DSD_NEO_SRC_IO_RADIO_RTL_TEST_HOOK_H_
#define DSD_NEO_SRC_IO_RADIO_RTL_TEST_HOOK_H_

#include <atomic>
#include <mutex>

/* Keep a callback and its context together when a test changes them while a worker runs. Invoke outside the lock:
 * a callback can wait for another worker or install its replacement. Callers keep contexts alive until workers join. */
template <typename Callback>
class RtlTestHookSlot {
  public:
    void
    set(Callback callback, void* context) {
        const std::lock_guard<std::mutex> lock(mutex_);
        context_ = context;
        callback_.store(callback, std::memory_order_relaxed);
    }

  protected:
    /* The pair as last set, read together; false while no callback is set. */
    bool
    load(Callback* callback, void** context) const {
        if (!callback_.load(std::memory_order_relaxed)) {
            return false;
        }
        const std::lock_guard<std::mutex> lock(mutex_);
        *callback = callback_.load(std::memory_order_relaxed);
        *context = context_;
        return *callback != nullptr;
    }

  private:
    mutable std::mutex mutex_;
    std::atomic<Callback> callback_{nullptr};
    void* context_ = nullptr;
};

/* A hook whose callback takes the listed arguments (none or one) before its context. */
template <typename... Args>
class RtlTestHook;

template <>
class RtlTestHook<> : public RtlTestHookSlot<void (*)(void*)> {
  public:
    void
    invoke() const {
        void (*callback)(void*) = nullptr;
        void* context = nullptr;
        if (this->load(&callback, &context)) {
            callback(context);
        }
    }
};

template <typename Arg>
class RtlTestHook<Arg> : public RtlTestHookSlot<void (*)(Arg, void*)> {
  public:
    void
    invoke(Arg arg) const {
        void (*callback)(Arg, void*) = nullptr;
        void* context = nullptr;
        if (this->load(&callback, &context)) {
            callback(arg, context);
        }
    }
};

#endif
