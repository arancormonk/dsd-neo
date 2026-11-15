// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Every configure of the Android service's decoder objects starts fresh (issue #786 B1).
 *
 * DecoderService keeps one dsd_opts/dsd_state pair from nativeInit() to nativeDestroy(), and a stop then a start can
 * reach the same service instance, so one configure follows another on the same objects. This drives
 * dsd_android::SessionObjects, the owner nativeInit(), nativeConfigure(), nativeDiscardConfigured() and nativeDestroy()
 * call, through the real bootstrap and engine, the way those entry points do: each configure after a run, a configure
 * that failed or exited, a configure discarded unrun and an unrun configure takes only its own channel map and options,
 * closes what the earlier one left open, survives an allocation failure in its reset, and a replay after a live run
 * gets the replay decode clock.
 */

#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/engine/engine.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/bootstrap.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/exitflag.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "session_objects.h"
#include "test_support.h"

namespace {

int g_failures = 0;

void
check(const char* label, bool cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", label);
        g_failures++;
    }
}

/* Two made-up channel maps with no channel number in common. */
constexpr long kMapAChannels[] = {101, 102};
constexpr long kMapAHz[] = {851012500, 851512500};
constexpr long kMapBChannels[] = {201, 202, 203};
constexpr long kMapBHz[] = {852012500, 852512500, 853012500};

struct Paths {
    char dir[DSD_TEST_PATH_MAX];
    char map_a[DSD_TEST_PATH_MAX];
    char map_b[DSD_TEST_PATH_MAX];
    char audio[DSD_TEST_PATH_MAX];
    char symbols[DSD_TEST_PATH_MAX];
    char wav[DSD_TEST_PATH_MAX];
};

Paths g_paths;

bool
write_map(const char* path, const long* channels, const long* hz, size_t count) {
    FILE* fp = dsd_fopen_private(path, "wb");
    if (fp == nullptr) {
        return false;
    }
    bool ok = fputs("channel_number,frequency_hz\n", fp) >= 0;
    for (size_t i = 0; i < count; i++) {
        ok = ok && DSD_FPRINTF(fp, "%ld,%ld\n", channels[i], hz[i]) > 0;
    }
    return fclose(fp) == 0 && ok;
}

void
put_le(std::vector<uint8_t>* out, uint32_t value, int bytes) {
    for (int i = 0; i < bytes; i++) {
        out->push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xFFU));
    }
}

/* A tenth of a second of 48 kHz mono 16-bit silence: the input of a live (non-replay) run. */
bool
write_audio(const char* path) {
    constexpr uint32_t kRate = 48000U;
    constexpr uint32_t kDataBytes = 4800U * 2U;
    std::vector<uint8_t> wav;
    const char* riff = "RIFF";
    wav.insert(wav.end(), riff, riff + 4);
    put_le(&wav, 36U + kDataBytes, 4);
    const char* fmt = "WAVEfmt ";
    wav.insert(wav.end(), fmt, fmt + 8);
    put_le(&wav, 16U, 4);
    put_le(&wav, 1U, 2); /* PCM */
    put_le(&wav, 1U, 2); /* mono */
    put_le(&wav, kRate, 4);
    put_le(&wav, kRate * 2U, 4);
    put_le(&wav, 2U, 2);
    put_le(&wav, 16U, 2);
    const char* data = "data";
    wav.insert(wav.end(), data, data + 4);
    put_le(&wav, kDataBytes, 4);
    wav.resize(wav.size() + kDataBytes, 0U);
    FILE* fp = dsd_fopen_private(path, "wb");
    if (fp == nullptr) {
        return false;
    }
    const bool ok = fwrite(wav.data(), 1, wav.size(), fp) == wav.size();
    return fclose(fp) == 0 && ok;
}

long
file_size(const char* path) {
    FILE* fp = dsd_fopen_private(path, "rb");
    if (fp == nullptr) {
        return -1;
    }
    long size = -1;
    if (fseek(fp, 0, SEEK_END) == 0) {
        size = ftell(fp);
    }
    (void)fclose(fp);
    return size;
}

/* Holds stdout in a temporary file while it lives: -h prints the whole usage text. */
class QuietStdout {
  public:
    QuietStdout() : sink_(tmpfile()), saved_(-1) {
        if (sink_ == nullptr) {
            return;
        }
        (void)fflush(stdout);
        saved_ = dsd_dup(dsd_fileno(stdout));
        if (saved_ >= 0) {
            (void)dsd_dup2(dsd_fileno(sink_), dsd_fileno(stdout));
        }
    }

    ~QuietStdout() {
        (void)fflush(stdout);
        if (saved_ >= 0) {
            (void)dsd_dup2(saved_, dsd_fileno(stdout));
            (void)dsd_close(saved_);
        }
        if (sink_ != nullptr) {
            (void)fclose(sink_);
        }
    }

    QuietStdout(const QuietStdout&) = delete;
    QuietStdout& operator=(const QuietStdout&) = delete;

  private:
    FILE* sink_;
    int saved_;
};

/* nativeConfigure's steps after its argv: the bootstrap, then the release a failed or exiting one gets. */
int
configure(dsd_android::SessionObjects* session, std::vector<std::string> args) {
    const QuietStdout quiet;
    dsd_exitflag_store(0);
    args.insert(args.begin(), "dsd-neo");
    std::vector<char*> argv(args.size() + 1U, nullptr);
    std::transform(args.begin(), args.end(), argv.begin(), [](std::string& arg) { return &arg[0]; });
    int exit_rc = 0;
    const int rc = dsd_runtime_bootstrap(static_cast<int>(args.size()), argv.data(), session->opts(), session->state(),
                                         nullptr, &exit_rc);
    if (rc != DSD_BOOTSTRAP_CONTINUE) {
        session->release_outputs();
    }
    return rc;
}

/* The objects nativeConfigure hands the bootstrap, prepared as it prepares them; false when that failed. */
bool
prepare(dsd_android::SessionObjects* session, const char* label) {
    const bool ok = session->prepare_for_configure();
    check(label, ok);
    return ok;
}

/* A configure that leaves the last session's map, Hold TG and relaxed CRC (-F) in the objects. */
std::vector<std::string>
session_a_args(void) {
    return {"-i", g_paths.audio, "-o", "null", "-C", g_paths.map_a, "-I", "1234", "-F"};
}

std::vector<std::string>
session_b_args(void) {
    return {"-i", g_paths.audio, "-o", "null", "-C", g_paths.map_b};
}

/* Session B's objects hold B's channel map alone and the defaults for every option B did not name. */
void
check_only_session_b(const dsd_android::SessionObjects& session, const std::string& when) {
    const dsd_state* state = session.state();
    const dsd_opts* opts = session.opts();
    const std::string prefix = when + ": ";
    check((prefix + "B's three LCN rows only").c_str(), state->lcn_freq_count == 3);
    for (int i = 0; i < 3 && i < state->lcn_freq_count; i++) {
        check((prefix + "LCN row " + std::to_string(i) + " is B's").c_str(),
              *dsd_state_trunk_lcn_slot_const(state, i) == kMapBHz[i]);
    }
    for (size_t i = 0; i < 2; i++) {
        check((prefix + "A's channel " + std::to_string(kMapAChannels[i]) + " is not mapped").c_str(),
              state->trunk_chan_map[kMapAChannels[i]] == 0);
    }
    for (size_t i = 0; i < 3; i++) {
        check((prefix + "B's channel " + std::to_string(kMapBChannels[i]) + " is mapped").c_str(),
              state->trunk_chan_map[kMapBChannels[i]] == kMapBHz[i]);
    }
    check((prefix + "Hold TG back to 0").c_str(), state->tg_hold == 0U);
    check((prefix + "relaxed CRC (-F) back to its default").c_str(), dsd_opts_relaxed_crc(opts) == 0);
    check((prefix + "the state is fresh for its run").c_str(), state->engine_fresh == 1U);
}

/* The test's owner of one SessionObjects, which frees only through destroy(), as nativeDestroy() calls it. */
class Owned {
  public:
    Owned() = default;

    ~Owned() { objects.destroy(); }

    Owned(const Owned&) = delete;
    Owned& operator=(const Owned&) = delete;

    dsd_android::SessionObjects objects;
};

/* What the run's start hook saw: whether it ran, and the decode clock the run chose. */
struct RunView {
    int calls;
    int clock;
};

int
stop_at_start(dsd_opts* opts, dsd_state* state, void* context) {
    (void)opts;
    (void)state;
    auto* view = static_cast<RunView*>(context);
    view->calls++;
    view->clock = static_cast<int>(dsd_decode_clock_source());
    /* Seen what it needed: end the run before the decoder reads a sample. */
    dsd_exitflag_store(1);
    return 0;
}

/* nativeRun's engine call on the configured objects, ended by its start hook. */
int
run_briefly(dsd_android::SessionObjects* session, RunView* view) {
    dsd_engine_lifecycle_hooks hooks = {};
    hooks.start = stop_at_start;
    hooks.context = view;
    return dsd_engine_run_with_lifecycle(session->opts(), session->state(), &hooks);
}

/* Stop, then start another site on the same service instance: B runs on B's files alone. */
void
test_configure_after_a_run(void) {
    Owned owned;
    dsd_android::SessionObjects& session = owned.objects;
    check("create", session.create());
    if (!prepare(&session, "first prepare") || configure(&session, session_a_args()) != DSD_BOOTSTRAP_CONTINUE) {
        check("session A configures", false);
        return;
    }
    check("A's map is in the objects", session.state()->lcn_freq_count == 2);
    check("A's Hold TG is in the objects", session.state()->tg_hold == 1234U);
    check("A's relaxed CRC is in the objects", dsd_opts_relaxed_crc(session.opts()) != 0);
    RunView view = {};
    check("session A runs", run_briefly(&session, &view) == 0 && view.calls == 1);
    check("the run used the state", session.state()->engine_fresh == 0U);

    if (!prepare(&session, "prepare after a run") || configure(&session, session_b_args()) != DSD_BOOTSTRAP_CONTINUE) {
        check("session B configures after A's run", false);
        return;
    }
    check_only_session_b(session, "after a run");
}

/* A configure that failed or exited part way, after it imported its map and set its options, then another. */
void
test_configure_after_a_failed_or_exiting_one(void) {
    struct Case {
        const char* name;
        const char* tail;
        int want;
    };

    /* A second -C is refused after the first is imported; -h is a one-shot that exits after it. */
    const Case cases[] = {{"a failed", "-C", DSD_BOOTSTRAP_ERROR}, {"an exiting", "-h", DSD_BOOTSTRAP_EXIT}};
    for (const Case& c : cases) {
        Owned owned;
        dsd_android::SessionObjects& session = owned.objects;
        check("create", session.create());
        std::vector<std::string> args = session_a_args();
        args.emplace_back(c.tail);
        if (strcmp(c.tail, "-C") == 0) {
            args.emplace_back(g_paths.map_b);
        }
        if (!prepare(&session, "first prepare") || configure(&session, args) != c.want) {
            check((std::string(c.name) + " configure ends as expected").c_str(), false);
            continue;
        }
        check((std::string(c.name) + " configure left A's map behind").c_str(), session.state()->lcn_freq_count == 2);
        if (!prepare(&session, "prepare after an unfinished configure")
            || configure(&session, session_b_args()) != DSD_BOOTSTRAP_CONTINUE) {
            check((std::string("session B configures after ") + c.name + " one").c_str(), false);
            continue;
        }
        check_only_session_b(session, std::string("after ") + c.name + " configure");
    }
}

/* nativeDiscardConfigured(): a configured session given up unrun, then the next configure. */
void
test_configure_after_a_discarded_one(void) {
    Owned owned;
    dsd_android::SessionObjects& session = owned.objects;
    check("create", session.create());
    std::vector<std::string> args = session_a_args();
    args.insert(args.end(), {"-c", g_paths.symbols});
    if (!prepare(&session, "first prepare") || configure(&session, args) != DSD_BOOTSTRAP_CONTINUE) {
        check("session A configures", false);
        return;
    }
    session.release_outputs();
    check("the discard closes A's symbol capture", session.opts()->symbol_out_f == nullptr);
    check("the closed capture holds its header", file_size(g_paths.symbols) == 16);
    if (!prepare(&session, "prepare after a discard")
        || configure(&session, session_b_args()) != DSD_BOOTSTRAP_CONTINUE) {
        check("session B configures after a discarded A", false);
        return;
    }
    check_only_session_b(session, "after a discard");
}

/* A configured session neither run nor discarded holds its output files: the next configure, and destroy, close them. */
void
test_unrun_outputs_closed(void) {
    for (int by_destroy = 0; by_destroy < 2; by_destroy++) {
        (void)remove(g_paths.symbols);
        (void)remove(g_paths.wav);
        Owned owned;
        dsd_android::SessionObjects& session = owned.objects;
        check("create", session.create());
        std::vector<std::string> args = session_a_args();
        args.insert(args.end(), {"-c", g_paths.symbols, "-w", g_paths.wav});
        if (!prepare(&session, "first prepare") || configure(&session, args) != DSD_BOOTSTRAP_CONTINUE) {
            check("session A configures with outputs", false);
            return;
        }
        check("A's configure opened its outputs",
              session.opts()->symbol_out_f != nullptr && session.opts()->wav_out_f != nullptr);
        if (by_destroy != 0) {
            session.destroy();
            check("destroy forgets the objects", !session.exists());
            check("destroy closed A's symbol capture", file_size(g_paths.symbols) == 16);
            continue;
        }
        if (!prepare(&session, "prepare over an unrun configure")) {
            return;
        }
        check("the reset closed A's symbol capture", file_size(g_paths.symbols) == 16);
        check("the reset left no output open",
              session.opts()->symbol_out_f == nullptr && session.opts()->wav_out_f == nullptr);
        check("session B configures over an unrun A", configure(&session, session_b_args()) == DSD_BOOTSTRAP_CONTINUE);
        check_only_session_b(session, "over an unrun configure");
    }
}

/* initState() cannot allocate during a reset: that configure fails, and the next one resets and succeeds. */
void
test_reset_allocation_failure_retries(void) {
    dsd_init_test_alloc_fail_after(0);
    Owned failed_owner;
    dsd_android::SessionObjects& failed = failed_owner.objects;
    check("a create whose initState() fails reports it", !failed.create());
    dsd_init_test_alloc_fail_after(-1);
    check("a failed create keeps nothing", !failed.exists());

    Owned owned;
    dsd_android::SessionObjects& session = owned.objects;
    check("create", session.create());
    if (!prepare(&session, "first prepare") || configure(&session, session_a_args()) != DSD_BOOTSTRAP_CONTINUE) {
        check("session A configures", false);
        return;
    }
    dsd_init_test_alloc_fail_after(0);
    check("a reset whose initState() fails fails the configure", !session.prepare_for_configure());
    dsd_init_test_alloc_fail_after(-1);
    check("the objects stay allocated", session.exists());
    check("the failed reset is marked", session.state()->init_failed != 0U);
    if (!prepare(&session, "the next prepare resets again")
        || configure(&session, session_b_args()) != DSD_BOOTSTRAP_CONTINUE) {
        check("session B configures after a failed reset", false);
        return;
    }
    check("the retried reset is whole", session.state()->init_failed == 0U);
    check_only_session_b(session, "after a failed reset");
}

#ifdef USE_RADIO
/* A live run, then a replay on the same objects: the replay runs on its capture's clock. */
void
test_replay_after_a_live_run(void) {
    Owned owned;
    dsd_android::SessionObjects& session = owned.objects;
    check("create", session.create());
    if (!prepare(&session, "first prepare") || configure(&session, session_b_args()) != DSD_BOOTSTRAP_CONTINUE) {
        check("the live session configures", false);
        return;
    }
    RunView live = {};
    check("the live session runs", run_briefly(&session, &live) == 0 && live.calls == 1);
    check("the live run ran on the system clock", live.clock == DSD_DECODE_CLOCK_SYSTEM);
    check("the live run used the state", session.state()->engine_fresh == 0U);

    char meta[DSD_TEST_PATH_MAX];
    if (dsd_test_path_join(meta, sizeof(meta), DSD_NEO_TEST_IQ_FIXTURE_DIR, "p25p1_cqpsk_cc.iq.json") != 0) {
        check("fixture path", false);
        return;
    }
    if (!prepare(&session, "prepare after the live run")
        || configure(&session, {"-f1", "-mq", "--iq-replay", meta, "-o", "null"}) != DSD_BOOTSTRAP_CONTINUE) {
        check("the replay configures after the live run", false);
        return;
    }
    check("the replay's state is fresh", session.state()->engine_fresh == 1U);
    RunView replay = {};
    check("the replay runs", run_briefly(&session, &replay) == 0 && replay.calls == 1);
    check("the replay runs on its capture's clock", replay.clock == DSD_DECODE_CLOCK_REPLAY);
}
#endif

bool
make_paths(void) {
    if (dsd_test_mkdtemp(g_paths.dir, sizeof(g_paths.dir), "dsdneo_session_objects") == nullptr) {
        return false;
    }
    return dsd_test_path_join(g_paths.map_a, sizeof(g_paths.map_a), g_paths.dir, "map_a.csv") == 0
           && dsd_test_path_join(g_paths.map_b, sizeof(g_paths.map_b), g_paths.dir, "map_b.csv") == 0
           && dsd_test_path_join(g_paths.audio, sizeof(g_paths.audio), g_paths.dir, "silence.wav") == 0
           && dsd_test_path_join(g_paths.symbols, sizeof(g_paths.symbols), g_paths.dir, "symbols.bin") == 0
           && dsd_test_path_join(g_paths.wav, sizeof(g_paths.wav), g_paths.dir, "static.wav") == 0
           && write_map(g_paths.map_a, kMapAChannels, kMapAHz, 2) && write_map(g_paths.map_b, kMapBChannels, kMapBHz, 3)
           && write_audio(g_paths.audio);
}

} // namespace

int
main(void) {
    if (!make_paths()) {
        DSD_FPRINTF(stderr, "FAIL: could not create the test files\n");
        return 1;
    }
    test_configure_after_a_run();
    test_configure_after_a_failed_or_exiting_one();
    test_configure_after_a_discarded_one();
    test_unrun_outputs_closed();
    test_reset_allocation_failure_retries();
#ifdef USE_RADIO
    test_replay_after_a_live_run();
#endif

    static const char* const kFiles[] = {"map_a.csv", "map_b.csv", "silence.wav", "symbols.bin", "static.wav", nullptr};
    if (dsd_test_remove_temp_dir(g_paths.dir, kFiles) != 0) {
        DSD_FPRINTF(stderr, "FAIL: could not remove %s\n", g_paths.dir);
        g_failures++;
    }
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d session object check(s) failed\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "ANDROID_SESSION_OBJECTS: OK\n");
    return 0;
}
