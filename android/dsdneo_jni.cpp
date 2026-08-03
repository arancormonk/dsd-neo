// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief JNI lifecycle surface for the Android app: init/configure/run/stop/destroy.
 *
 * There is no data path here. The Qt UI links the engine in-process and reads
 * app-control directly; JNI only carries what has to cross into Java: the engine
 * lifetime (owned by the foreground service) and platform glue.
 *
 * One engine instance per process (ground rule 4), guarded by a single mutex.
 * Nothing throws across JNI — every entry point returns a status code.
 */

#include <jni.h>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include <android/log.h>
#include <pthread.h>
#include <unistd.h>

#include <dsd-neo/app_control/frontend_runtime.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/engine/engine.h>
#include <dsd-neo/runtime/bootstrap.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/shutdown.h>
#ifdef USE_RADIO
#include <dsd-neo/io/rtl_device.h>
#endif

#include "dsdneo_jni.h"

namespace {

constexpr const char* kLogTag = "dsd-neo";

/* Status codes shared with DsdNative.kt. Keep the two in sync. */
constexpr jint kStatusOk = 0;
constexpr jint kStatusError = -1;
constexpr jint kStatusBadState = -2;
constexpr jint kStatusConfigExit = 1;

std::mutex g_lock;
dsd_opts* g_opts = nullptr;
dsd_state* g_state = nullptr;
bool g_configured = false;
std::atomic<bool> g_running{false};
std::atomic<bool> g_log_pump_started{false};
/* Mirrors "g_opts/g_state exist" for nativeStop, which must not take g_lock; see
 * there. Written under g_lock by nativeInit/nativeDestroy. */
std::atomic<bool> g_initialized{false};

/**
 * @brief Drains the redirected stdout/stderr pipe into logcat, one line per record.
 *
 * Most decode output is written straight to stderr (not through the LOG_* funnel),
 * and Android drops both streams for an app process. LOG_* messages keep their own
 * severities via the platform log sink, so they must not travel through here twice.
 */
void*
// cppcheck-suppress constParameterCallback -- pthread_create entry point signature
log_pump_thread(void* arg) {
    const int read_fd = static_cast<int>(reinterpret_cast<intptr_t>(arg));
    std::string line;
    char buf[512];

    for (;;) {
        const ssize_t n = read(read_fd, buf, sizeof buf);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;
            }
            break;
        }
        for (ssize_t i = 0; i < n; i++) {
            const char c = buf[i];
            if (c == '\n' || c == '\r') {
                if (!line.empty()) {
                    __android_log_write(ANDROID_LOG_INFO, kLogTag, line.c_str());
                    line.clear();
                }
                continue;
            }
            line.push_back(c);
            if (line.size() >= 1024U) {
                __android_log_write(ANDROID_LOG_INFO, kLogTag, line.c_str());
                line.clear();
            }
        }
    }

    if (!line.empty()) {
        __android_log_write(ANDROID_LOG_INFO, kLogTag, line.c_str());
    }
    (void)close(read_fd);
    return nullptr;
}

void
close_if_open(int fd) {
    if (fd >= 0) {
        (void)close(fd);
    }
}

/** @brief Redirect fds 1 and 2 into logcat. Installed once per process. */
void
start_log_pump(void) {
    bool expected = false;
    if (!g_log_pump_started.compare_exchange_strong(expected, true)) {
        return;
    }

    int fds[2];
    if (pipe(fds) != 0) {
        g_log_pump_started.store(false);
        return;
    }

    /* A pipe whose reader is gone is fatal to the decoder: writes to it raise
     * SIGPIPE, whose default disposition kills the process, and a full pipe with
     * no reader blocks every stderr write forever. Neither is acceptable for a
     * logging convenience, so keep the originals to fall back on and make the
     * failure mode "logs go nowhere" rather than "the app dies". */
    const int saved_out = dup(STDOUT_FILENO);
    const int saved_err = dup(STDERR_FILENO);
    (void)signal(SIGPIPE, SIG_IGN);

    (void)dup2(fds[1], STDOUT_FILENO);
    (void)dup2(fds[1], STDERR_FILENO);
    (void)close(fds[1]);

    /* stdout is not a TTY here, so stdio would buffer it fully and decode lines
     * would sit unseen until the buffer filled. */
    (void)setvbuf(stdout, nullptr, _IONBF, 0);
    (void)setvbuf(stderr, nullptr, _IONBF, 0);

    pthread_t tid;
    if (pthread_create(&tid, nullptr, log_pump_thread, reinterpret_cast<void*>(static_cast<intptr_t>(fds[0]))) != 0) {
        /* Put the process's own streams back before the unread pipe can fill. */
        if (saved_out >= 0) {
            (void)dup2(saved_out, STDOUT_FILENO);
        }
        if (saved_err >= 0) {
            (void)dup2(saved_err, STDERR_FILENO);
        }
        (void)close(fds[0]);
        close_if_open(saved_out);
        close_if_open(saved_err);
        return;
    }
    (void)pthread_detach(tid);
    close_if_open(saved_out);
    close_if_open(saved_err);
}

std::string
jstring_to_utf8(JNIEnv* env, jstring value) {
    if (env == nullptr || value == nullptr) {
        return std::string();
    }
    const char* chars = env->GetStringUTFChars(value, nullptr);
    if (chars == nullptr) {
        return std::string();
    }
    std::string out(chars);
    env->ReleaseStringUTFChars(value, chars);
    return out;
}

void
set_env_from_jstring(JNIEnv* env, const char* name, jstring value) {
    const std::string text = jstring_to_utf8(env, value);
    if (text.empty()) {
        return;
    }
    (void)setenv(name, text.c_str(), 1);
}

/**
 * @brief A CLI-shaped argv plus a stable record of the strings it owns.
 *
 * dsd_runtime_bootstrap compacts @ref argv in place (see bootstrap.h): surviving
 * pointers shift down and the tail keeps stale duplicates of them. Freeing through
 * the compacted array would therefore free some strings twice and leak the rest,
 * so ownership is tracked in @ref owned, which the bootstrap never sees.
 */
struct cli_argv {
    char** argv;
    char** owned;
    int argc;
};

/** @brief Frees an argv built by build_argv(). */
void
free_argv(cli_argv* cli) {
    if (cli == nullptr) {
        return;
    }
    if (cli->owned != nullptr) {
        for (int i = 0; i < cli->argc; i++) {
            free(cli->owned[i]);
        }
        free(cli->owned);
        cli->owned = nullptr;
    }
    free(cli->argv);
    cli->argv = nullptr;
    cli->argc = 0;
}

/**
 * @brief Converts a Java String[] into a CLI-shaped argv with a placeholder argv[0].
 * @return True on success; release with free_argv().
 */
bool
build_argv(JNIEnv* env, jobjectArray args, cli_argv* out) {
    const jsize extra = (args != nullptr) ? env->GetArrayLength(args) : 0;
    const int argc = static_cast<int>(extra) + 1;

    out->argc = argc;
    /* One extra slot: compaction writes a NULL terminator at the new argc. */
    out->argv = static_cast<char**>(calloc(static_cast<size_t>(argc) + 1U, sizeof(char*)));
    out->owned = static_cast<char**>(calloc(static_cast<size_t>(argc), sizeof(char*)));
    if (out->argv == nullptr || out->owned == nullptr) {
        free_argv(out);
        return false;
    }

    out->argv[0] = strdup("dsd-neo");
    out->owned[0] = out->argv[0];
    if (out->argv[0] == nullptr) {
        free_argv(out);
        return false;
    }

    for (jsize i = 0; i < extra; i++) {
        jstring item = static_cast<jstring>(env->GetObjectArrayElement(args, i));
        const std::string text = jstring_to_utf8(env, item);
        if (item != nullptr) {
            env->DeleteLocalRef(item);
        }
        out->argv[i + 1] = strdup(text.c_str());
        out->owned[i + 1] = out->argv[i + 1];
        if (out->argv[i + 1] == nullptr) {
            free_argv(out);
            return false;
        }
    }

    return true;
}

} // namespace

namespace dsd_android {

bool
engine_is_running(void) {
    return g_running.load();
}

} // namespace dsd_android

extern "C" {

JNIEXPORT jint JNICALL
Java_io_github_arancormonk_dsdneo_DsdNative_nativeInit(JNIEnv* env, jclass clazz, jstring config_dir,
                                                       jstring cache_dir) {
    (void)clazz;

    start_log_pump();

    /* The app owns the paths and must not steal the process signal dispositions. */
    set_env_from_jstring(env, "HOME", config_dir);
    set_env_from_jstring(env, "XDG_CONFIG_HOME", config_dir);
    set_env_from_jstring(env, "DSD_NEO_CACHE_DIR", cache_dir);
    (void)setenv("DSD_NEO_NO_SIGNAL_HANDLERS", "1", 1);

    /* Without this the LOG_* funnel writes stderr and the pump above re-logs every
     * message at INFO, losing the severity and duplicating nothing useful. */
    dsd_neo_log_set_sink(DSD_NEO_LOG_SINK_PLATFORM);

    std::lock_guard<std::mutex> guard(g_lock);
    if (g_opts != nullptr || g_state != nullptr) {
        return kStatusBadState;
    }

    g_opts = static_cast<dsd_opts*>(calloc(1, sizeof(dsd_opts)));
    g_state = static_cast<dsd_state*>(calloc(1, sizeof(dsd_state)));
    if (g_opts == nullptr || g_state == nullptr) {
        free(g_opts);
        free(g_state);
        g_opts = nullptr;
        g_state = nullptr;
        return kStatusError;
    }

    initOpts(g_opts);
    initState(g_state);
    g_configured = false;
    g_initialized.store(true);
    return kStatusOk;
}

JNIEXPORT jint JNICALL
Java_io_github_arancormonk_dsdneo_DsdNative_nativeConfigure(JNIEnv* env, jclass clazz, jobjectArray args) {
    (void)clazz;

    std::lock_guard<std::mutex> guard(g_lock);
    if (g_opts == nullptr || g_state == nullptr || g_running.load()) {
        return kStatusBadState;
    }

    cli_argv cli = {};
    if (!build_argv(env, args, &cli)) {
        return kStatusError;
    }

    /* A completed run leaves the shutdown flag raised; main() never restarts, an
     * embedding host always does. */
    dsd_exitflag_store(0);

    int exit_rc = 0;
    const int rc = dsd_runtime_bootstrap(cli.argc, cli.argv, g_opts, g_state, nullptr, &exit_rc);
    free_argv(&cli);

    if (rc == DSD_BOOTSTRAP_CONTINUE) {
        g_configured = true;
        return kStatusOk;
    }

    g_configured = false;
    /* EXIT is not a failure: one-shot flows such as -h take it. */
    return (rc == DSD_BOOTSTRAP_EXIT) ? kStatusConfigExit : kStatusError;
}

JNIEXPORT jint JNICALL
Java_io_github_arancormonk_dsdneo_DsdNative_nativeRun(JNIEnv* env, jclass clazz) {
    (void)env;
    (void)clazz;

    dsd_opts* opts = nullptr;
    dsd_state* state = nullptr;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        if (g_opts == nullptr || g_state == nullptr || !g_configured || g_running.load()) {
            return kStatusBadState;
        }
        opts = g_opts;
        state = g_state;
        g_running.store(true);
    }

    /* Installs the telemetry hooks and control pump the UI's app-control reads and
     * command submits need; the CLI's frontend-none path never does this. */
    dsd_app_frontend_runtime_start(opts, state);
    const int rc = dsd_engine_run_with_lifecycle(opts, state, nullptr);
    dsd_app_frontend_runtime_stop();

    {
        std::lock_guard<std::mutex> guard(g_lock);
        g_running.store(false);
        g_configured = false;
    }
    return (rc == 0) ? kStatusOk : kStatusError;
}

JNIEXPORT jint JNICALL
Java_io_github_arancormonk_dsdneo_DsdNative_nativeStop(JNIEnv* env, jclass clazz) {
    (void)env;
    (void)clazz;

    /* Deliberately lock-free. This is called from the main thread on a USB detach
     * broadcast, and nativeConfigure holds g_lock across the whole bootstrap, which
     * can block on an rtl_tcp connect — waiting for it here would be an ANR. There
     * is nothing to protect: dsd_request_shutdown() ignores both arguments and only
     * raises the process exit flag, so the guard below is purely "has the engine
     * been initialized". */
    if (!g_initialized.load()) {
        return kStatusBadState;
    }
    dsd_request_shutdown(nullptr, nullptr);
    return kStatusOk;
}

JNIEXPORT jint JNICALL
Java_io_github_arancormonk_dsdneo_DsdNative_nativeDestroy(JNIEnv* env, jclass clazz) {
    (void)env;
    (void)clazz;

    std::lock_guard<std::mutex> guard(g_lock);
    if (g_running.load()) {
        return kStatusBadState;
    }
    g_initialized.store(false);
    if (g_state != nullptr) {
        freeState(g_state);
    }
    free(g_opts);
    free(g_state);
    g_opts = nullptr;
    g_state = nullptr;
    g_configured = false;
    return kStatusOk;
}

JNIEXPORT jint JNICALL
Java_io_github_arancormonk_dsdneo_DsdNative_nativeSetUsbFd(JNIEnv* env, jclass clazz, jint sys_fd) {
    (void)env;
    (void)clazz;

#ifdef USE_RADIO
    /* Java owns the descriptor: it comes from a UsbDeviceConnection that the service
     * holds open for the engine's lifetime, and -1 clears the slot on detach. The
     * engine reads it during input setup, so it has to be set before nativeRun. */
    rtl_device_set_preopened_fd(static_cast<int>(sys_fd));
    return kStatusOk;
#else
    (void)sys_fd;
    return kStatusError;
#endif
}

JNIEXPORT jboolean JNICALL
Java_io_github_arancormonk_dsdneo_DsdNative_nativeIsRunning(JNIEnv* env, jclass clazz) {
    (void)env;
    (void)clazz;
    return g_running.load() ? JNI_TRUE : JNI_FALSE;
}

} // extern "C"
