// SPDX-License-Identifier: GPL-3.0-or-later
#include <QAbstractListModel>
#include <QByteArray>
#include <QByteArrayView>
#include <QChar>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIODevice>
#include <QList>
#include <QMap>
#include <QObject>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QVariant>
#include <QVariantMap>
#include <Qt>
#include <QtEnvironmentVariables>
#include <QtGlobal>
#include <atomic>
#include <cstdio>
#include <dsd-neo/app_control/frontend_runtime.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/key_set.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/cli.h>
#include <dsd-neo/runtime/log.h>
#include <string.h>
#include <thread>
#include <utility>
#include <vector>
#include "command_bridge.h"
#include "diagnostics_log.h"
extern "C" int dsd_test_diagnostics_drain(dsd_opts*, dsd_state*);
#include "../../src/app_control/commands_internal.h"
#include "session_args.h"
extern "C" int dsd_test_diagnostics_direct_key_applied(dsd_opts*, dsd_state*, int, const char*);
#include <memory>
#include "../test_support/test_support.h"
using namespace dsd_qt;
static int failures = 0;

static void
check(bool ok, const char* reason = "diagnostics assertion failed") {
    if (!ok) {
        ++failures;
        DSD_FPRINTF(stderr, "%s\n", reason); // Never print values, captured output or key-bearing expressions.
    }
}

static QList<QByteArray>
hexRepresentations(const QByteArray& key) {
    QList<QByteArray> representations{key};
    QByteArray spaced;
    for (qsizetype i = 0; i < key.size(); i += 16) {
        const auto segment = key.mid(i, 16);
        if (!spaced.isEmpty()) {
            spaced += ' ';
        }
        spaced += segment;
        representations.append(segment); // Also catch a partial segment leak.
        representations.append(QByteArray::number(segment.toULongLong(nullptr, 16)));
    }
    representations.append(spaced);
    return representations;
}

/* The log's own process line ("[process] [info] pid <pid>, <os>") carries the real pid and kernel version, never a
 * key; left in, a pid such as 1845197 reads as a leak of the basic key 197. */
static QByteArray
withoutProcessFacts(const QByteArray& text) {
    QByteArray kept;
    for (const auto& line : text.split('\n')) {
        if (!line.startsWith("[process] [info] pid ")) {
            kept += line;
            kept += '\n';
        }
    }
    return kept;
}

static void
checkAbsent(const QByteArray& text, const QList<QByteArray>& representations, const char* surface) {
    const auto folded = withoutProcessFacts(text).toLower();
    for (const auto& representation : representations) {
        check(!representation.isEmpty() && !folded.contains(representation.toLower()), surface);
    }
}

static QByteArray
parseKey(dsd_opts* opts, dsd_state* state, const char* flag, QByteArray value, bool accepted,
         const char* errorMarker = nullptr) {
    QByteArray option(flag);
    char name[] = "diagnostics-test";
    char* argv[] = {name, option.data(), value.data(), nullptr};
    dsd_test_capture_stderr captured;
    if (dsd_test_capture_stderr_begin(&captured, "diagnostics_cli") != 0) {
        check(false, "CLI capture setup failed");
        DSD_SECURE_ZERO(value.data(), static_cast<size_t>(value.size()));
        return {};
    }
    int effective = 0, exitCode = 0;
    const int result = dsd_parse_args(3, argv, opts, state, &effective, &exitCode);
    check(result == (accepted ? DSD_PARSE_CONTINUE : DSD_PARSE_ERROR), "CLI key parse result mismatch");
    check(exitCode == (accepted ? 0 : 1), "CLI key exit result mismatch");
    check(effective == 3 && opts->show_keys == 0, "CLI argument count or key-display policy mismatch");
    check(dsd_test_capture_stderr_end(&captured) == 0, "CLI capture restore failed");
    QFile file(QString::fromUtf8(captured.path));
    check(file.open(QIODevice::ReadOnly), "CLI capture read failed");
    const auto text = file.readAll();
    // The shared direct-key helper emits no success diagnostic. State assertions
    // establish successful loading; rejected input has a format-only diagnostic.
    if (accepted) {
        check(text.isEmpty(), "successful CLI key load must be silent");
    } else {
        check(errorMarker && text.contains(errorMarker), "CLI diagnostic outcome marker missing");
    }
    file.close();
    check(file.remove(), "CLI capture cleanup failed");
    DSD_SECURE_ZERO(value.data(), static_cast<size_t>(value.size()));
    return text;
}

static void
checkHexState(const dsd_state* state, const QByteArray& key) {
    const auto first = key.left(16).toULongLong(nullptr, 16);
    const auto second = key.mid(16, 16).toULongLong(nullptr, 16);
    const auto third = key.mid(32, 16).toULongLong(nullptr, 16);
    const auto fourth = key.mid(48, 16).toULongLong(nullptr, 16);
    const int segments = key.size() == 10 ? 1 : key.size() / 16;
    check(state->H == first && state->K1 == first && state->K2 == second && state->K3 == third && state->K4 == fourth,
          "hex key scalars were not loaded");
    check(state->keyloader == 0 && state->hytera_key_segments == segments, "hex key-loader state mismatch");
    for (int slot = 0; slot < 2; ++slot) {
        check(state->A1[slot] == (segments == 1 ? 0 : first) && state->A2[slot] == second && state->A3[slot] == third
                  && state->A4[slot] == fourth,
              "AES slot scalars were not loaded");
        check(state->aes_key_loaded[slot] == (segments > 1)
                  && state->aes_key_segments[slot] == (segments == 1 ? 0 : segments),
              "AES slot loading flags mismatch");
    }
    const auto bytes = segments == 1 ? QByteArray() : QByteArray::fromHex(key);
    check(memcmp(state->aes_key, bytes.constData(), static_cast<size_t>(bytes.size())) == 0,
          "AES bytes were not loaded");
    for (size_t i = static_cast<size_t>(bytes.size()); i < sizeof(state->aes_key); ++i) {
        check(state->aes_key[i] == 0, "unused AES bytes were not cleared");
    }
}

static void
checkMutePolicy(const dsd_opts* opts, int muteLeft, int muteRight) {
    check(opts->dmr_mute_encL == muteLeft && opts->dmr_mute_encR == muteRight,
          "key loading changed the user's encrypted audio mute settings");
    check(opts->unmute_encrypted_p25 == 0, "key loading did not mute undecodable P25 audio");
}

static void
checkStoredDiagnostics(DiagnosticsLog& log, DiagnosticsLogModel& model, const QString& directory,
                       const QList<QByteArray>& representations, const QByteArray& marker) {
    model.refresh();
    log.flush();
    const auto ring = log.snapshot().join(QLatin1Char('\n')).toUtf8();
    check(ring.contains(marker), "ring positive capture marker missing");
    checkAbsent(ring, representations, "ring contains a key representation");
    QStringList displayed;
    for (int row = 0; row < model.rowCount(); ++row) {
        displayed.append(model.data(model.index(row), Qt::DisplayRole).toString());
    }
    check(displayed.join(QLatin1Char('\n')).toUtf8() == ring, "model rows do not show the captured ring");
    checkAbsent(displayed.join(QLatin1Char('\n')).toUtf8(), representations, "model rows contain a key representation");
    const auto text = model.allText().toUtf8();
    check(text == ring, "allText does not contain the captured ring");
    checkAbsent(text, representations, "allText contains a key representation");
    QFile exported(directory + "/export.txt");
    check(exported.open(QIODevice::WriteOnly), "export open failed");
    check(exported.write(text) == text.size(), "export write failed");
    exported.close();
    check(exported.open(QIODevice::ReadOnly), "export read failed");
    const auto exportText = exported.readAll();
    check(exportText == text && exportText.contains(marker), "export positive capture marker missing");
    checkAbsent(exportText, representations, "export contains a key representation");
    QFile tail(directory + "/tail.log");
    check(tail.open(QIODevice::ReadOnly), "tail read failed");
    const auto tailText = tail.readAll();
    check(tailText.contains(marker), "tail positive capture marker missing");
    checkAbsent(tailText, representations, "tail contains a key representation");
}

static void
realKeySources() {
    DiagnosticsLog::installTap();
    auto& log = DiagnosticsLog::instance();
    DiagnosticsLogModel model(&log);
    auto opts = std::make_unique<dsd_opts>();
    auto state = std::make_unique<dsd_state>();
    initOpts(opts.get());
    initState(state.get());
    dsd_app_frontend_runtime_start(opts.get(), state.get());

    struct KeyCase {
        int type = 0;
        const char* flag = nullptr;
        QByteArray value;
        const char* error = nullptr;
    };

    const KeyCase cases[] = {
        {DSD_KEY_TYPE_HEX, "-H", QByteArray(16, 'C') + QByteArray(16, 'D') + QByteArray(16, 'E') + QByteArray(16, 'F'),
         "-H expects 10, 32, or 64 hex digits"},
        {DSD_KEY_TYPE_HEX, "-H", QByteArray(10, 'C'), "-H expects 10, 32, or 64 hex digits"},
        {DSD_KEY_TYPE_HEX, "-H", QByteArray(16, 'A') + QByteArray(16, 'B'), "-H expects 10, 32, or 64 hex digits"},
        {DSD_KEY_TYPE_BASIC, "-b", "197", "-b expects decimal 0..255"},
        {DSD_KEY_TYPE_RC4, "-1", "DADADADADA", "-1 expects 1..16 hex digits"},
        {DSD_KEY_TYPE_SCRAMBLER, "-R", "23456", "-R expects decimal 0..32767"},
    };
    QList<QByteArray> representations;
    for (const auto& entry : cases) {
        if (entry.type == DSD_KEY_TYPE_BASIC || entry.type == DSD_KEY_TYPE_SCRAMBLER) {
            representations.append(entry.value);
            representations.append(QByteArray::number(entry.value.toULongLong(), 16));
        } else {
            representations.append(hexRepresentations(entry.value));
        }
        representations.append(entry.value + "invalid");
    }
    const QByteArray malformed = cases[1].value + "invalid";
    // Exercise wide-to-narrow HEX replacement and preservation across other
    // key types on both paths, preserving each combination of the user's
    // independent slot mute settings. The keyring survives every direct overlay.
    for (int path = 0; path < 2; ++path) {
        for (int muteSettings = 0; muteSettings < 4; ++muteSettings) {
            const int muteLeft = muteSettings & 1;
            const int muteRight = (muteSettings >> 1) & 1;
            QByteArray activeHex;
            state->K = 17;
            state->R = 19;
            state->RR = 23;
            unsigned long long basic = state->K, left = state->R, right = state->RR;
            state->rkey_array[7] = 29;
            state->rkey_array_loaded[7] = 1;
            for (const auto& entry : cases) {
                state->keyloader = 1;
                state->payload_keyid = state->payload_keyidR = 7;
                opts->dmr_mute_encL = muteLeft;
                opts->dmr_mute_encR = muteRight;
                opts->unmute_encrypted_p25 = 1;
                if (path == 0) {
                    const auto captured = parseKey(opts.get(), state.get(), entry.flag, entry.value, true);
                    checkAbsent(captured, representations, "CLI diagnostic contains a key representation");
                    log.submit("stderr", "info", QString::fromUtf8(captured));
                } else {
                    check(dsd_test_diagnostics_direct_key_applied(opts.get(), state.get(), entry.type,
                                                                  entry.value.constData()),
                          "KEY_DIRECT_SET did not apply key and activation flags");
                    checkAbsent(QByteArray(state->ui_msg), representations,
                                "direct-command message contains a key representation");
                    log.submit("host", "info", QString::fromUtf8(state->ui_msg));
                }
                if (entry.type == DSD_KEY_TYPE_HEX) {
                    activeHex = entry.value;
                } else if (entry.type == DSD_KEY_TYPE_BASIC) {
                    basic = entry.value.toULongLong();
                } else if (entry.type == DSD_KEY_TYPE_RC4) {
                    left = right = entry.value.toULongLong(nullptr, 16);
                } else {
                    left = entry.value.toULongLong();
                }
                checkHexState(state.get(), activeHex);
                checkMutePolicy(opts.get(), muteLeft, muteRight);
                check(state->K == basic && state->R == left && state->RR == right, "direct overlay scalar mismatch");
                check(state->rkey_array[7] == 29 && state->rkey_array_loaded[7] == 1, "direct overlay altered keyring");
                check(state->payload_keyid == 0 && state->payload_keyidR == 0, "direct activation retained key IDs");
                if (path == 0) {
                    dsd_key_set before{}, after{};
                    check(dsd_key_set_capture(&before, state.get()) == 0, "key state capture failed");
                    const auto captured =
                        parseKey(opts.get(), state.get(), entry.flag, entry.value + "invalid", false, entry.error);
                    check(dsd_key_set_capture(&after, state.get()) == 0 && dsd_key_set_equal(&before, &after),
                          "refused CLI key altered loaded state");
                    checkMutePolicy(opts.get(), muteLeft, muteRight);
                    dsd_key_set_free(&before);
                    dsd_key_set_free(&after);
                    checkAbsent(captured, representations, "CLI error diagnostic contains a key representation");
                    log.submit("stderr", "info", QString::fromUtf8(captured));
                }
            }
        }
    }
    log.submit("host", "error", "malformed key " + QString::fromUtf8(malformed));
    dsd_neo_log_write(LOG_LEVEL_INFO, "decoder restart marker\n");
    log.submit("host", "info", "host restart marker");
    log.submit("stderr", "info", "stderr restart marker");
    const auto directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/diagnostics";
    model.markSessionStarting();
    checkStoredDiagnostics(log, model, directory, representations, "--- session starting ---");
    check(model.allText().contains("decoder restart marker") && model.allText().contains("host restart marker")
              && model.allText().contains("stderr restart marker")
              && model.allText().count("--- session starting ---") == 1,
          "session divider must retain preceding diagnostics");
    {
        DiagnosticsLog restarted(directory);
        DiagnosticsLogModel previous(&restarted);
        previous.refresh();
        check(previous.allText().contains("previous run") && previous.allText().contains("--- session starting ---")
              && previous.allText().contains("decoder restart marker")
              && previous.allText().contains("host restart marker")
              && previous.allText().contains("stderr restart marker"));
        checkAbsent(previous.allText().toUtf8(), representations, "previous-run text contains a key representation");
    }
    dsd_app_frontend_runtime_stop();
    freeState(state.get());
    DSD_SECURE_ZERO(state.get(), sizeof(*state));
}

static void
clearWithConcurrentSubmit() {
    QTemporaryDir dir;
    DiagnosticsLog log(dir.path());
    DiagnosticsLogModel model(&log);
    log.submit("host", "info", "before clear");
    model.refresh();
    check(model.rowCount() == 1);
    // Deterministically submit after the ring clear and before the model reset
    // finishes. Sampling the generation a second time loses this record.
    const auto connection = QObject::connect(&model, &QAbstractItemModel::modelAboutToBeReset, &model, [&log] {
        std::thread producer([&log] { log.submit("host", "info", "during clear"); });
        producer.join();
    });
    model.clear();
    QObject::disconnect(connection);
    check(model.rowCount() == 0);
    model.refresh();
    check(model.rowCount() == 1);
    check(model.data(model.index(0), Qt::DisplayRole).toString().contains("during clear"));
}

/* Issue #718: the diagnostics model is a global context object, ticked every 250 ms whether or not the
 * screen is open. A full reset of up to 2000 rows per tick with new lines was ~70% of the UI tick's work
 * while the list was hidden. Hidden, it takes no rows; opened, it resets once. Set through the property
 * system so the test reads the same on a model without the property. */
static void
inactiveModelDefersResets() {
    QTemporaryDir dir;
    DiagnosticsLog log(dir.path());
    DiagnosticsLogModel model(&log);
    log.submit("host", "info", "first");
    model.refresh();
    check(model.rowCount() == 1, "an active model takes new lines at once");
    int resets = 0;
    QObject::connect(&model, &QAbstractItemModel::modelReset, &model, [&resets] { ++resets; });

    model.setProperty("active", false);
    for (int i = 0; i < 20; ++i) {
        log.submit("host", "info", QStringLiteral("hidden %1").arg(i));
        model.refresh();
    }
    check(resets == 0, "an inactive model performs no resets across ticks with new lines");
    check(model.pendingCount() == 0, "an inactive, unpaused model counts nothing pending");
    model.setProperty("active", true);
    check(resets == 1, "becoming active resets once");
    check(model.rowCount() == 21 && model.data(model.index(20), Qt::DisplayRole).toString().contains("hidden 19"),
          "becoming active shows every line");
    model.setProperty("active", false);
    model.setProperty("active", true);
    check(resets == 1, "becoming active with nothing new does not reset");

    // Paused while hidden: the view shows the lines as they were at the pause, and counts the rest.
    model.setProperty("active", false);
    log.submit("host", "info", "before pause");
    model.refresh();
    model.setPaused(true);
    log.submit("host", "info", "paused one");
    log.submit("host", "info", "paused two");
    model.refresh();
    check(resets == 1 && model.pendingCount() == 2, "a paused, inactive model only counts new lines");
    model.setProperty("active", true);
    check(resets == 2 && model.rowCount() == 22 && model.pendingCount() == 2,
          "becoming active while paused shows the lines as they were at the pause");
    model.setPaused(false);
    check(model.rowCount() == 24 && model.pendingCount() == 0, "resuming shows the held lines");

    // A clear while hidden: only what came after it shows.
    model.setProperty("active", false);
    log.submit("host", "info", "cleared");
    model.refresh();
    model.clear();
    log.submit("host", "info", "after clear");
    model.refresh();
    model.setProperty("active", true);
    check(model.rowCount() == 1 && model.data(model.index(0), Qt::DisplayRole).toString().contains("after clear"),
          "a clear while inactive keeps only later lines");
}

static void
clearAndReopen() {
    for (const bool flushBeforeClear : {false, true}) {
        for (const bool submitAfterClear : {false, true}) {
            QTemporaryDir dir;
            {
                DiagnosticsLog log(dir.path());
                for (int i = 0; i < 2000; ++i) {
                    log.submit("host", "info", "discarded record");
                }
                if (flushBeforeClear) {
                    log.flush();
                }
                log.clear();
                check(log.snapshot().isEmpty(), "clear empties visible diagnostics");
                if (submitAfterClear) {
                    // Exceed queue capacity too: backpressure must not lose the clear.
                    for (int i = 0; i < 2100; ++i) {
                        log.submit("host", "info", "retained record");
                    }
                }
                log.flush();
                QFile tail(dir.filePath("tail.log"));
                check(tail.open(QIODevice::ReadOnly), "cleared tail is readable");
                const auto text = tail.readAll();
                check(!text.contains("discarded record"), "clear removes persisted and queued diagnostics");
                check(submitAfterClear ? text.contains("retained record") : text.isEmpty(),
                      "clear preserves only subsequent diagnostics");
            }
            DiagnosticsLog reopened(dir.path());
            DiagnosticsLogModel model(&reopened);
            model.refresh();
            check(!model.allText().contains("discarded record"), "cleared diagnostics stay absent after reopen");
            check(submitAfterClear ? model.allText().contains("retained record") : model.rowCount() == 0,
                  "reopened diagnostics preserve only subsequent records");
        }
    }
}

static void
earlyHostCaptureAndBridge() {
    {
        auto& log = DiagnosticsLog::instance();
        DiagnosticsLogModel model(&log);
        model.refresh();
        check(model.allText().count("usb: permission pending before init") == 1, "pre-init USB status captured once");
        log.flush();
        QFile tail(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/diagnostics/tail.log");
        check(tail.open(QIODevice::ReadOnly), "pre-init USB tail uses initialized app location");
        check(tail.readAll().count("usb: permission pending before init") == 1, "pre-init USB status persisted once");
    }
    {
        static dsd_opts opts;
        static dsd_state state;
        initOpts(&opts);
        initState(&state);
        dsd_app_frontend_runtime_start(&opts, &state);
        CommandBridge bridge;
        QString formatted;
        for (int i = 0; i < 32; ++i) {
            if (i) {
                formatted += ' ';
            }
            formatted += "11";
        }
        check(bridge.applyEncryptionKey("hex", formatted), "bridge accepts formatted AES-256 input");
        check(dsd_test_diagnostics_drain(&opts, &state) == 1 && state.K1 == 0x1111111111111111ULL
                  && state.K4 == state.K1,
              "formatted bridge AES installs full width");
        check(bridge.applyEncryptionKey("rc4", "0x 11 22 33 44"), "bridge accepts formatted RC4 input");
        check(dsd_test_diagnostics_drain(&opts, &state) == 1 && state.R == 0x11223344ULL,
              "formatted bridge RC4 installs normalized value");
        dsd_app_frontend_runtime_stop();
        DSD_SECURE_ZERO(formatted.data(), static_cast<size_t>(formatted.size()) * sizeof(QChar));
        freeState(&state);
        DSD_SECURE_ZERO(&state, sizeof state);
        DSD_SECURE_ZERO(&opts, sizeof opts);
    }
}

// Use the production bridge and queue: the QML CommandRecorder cannot exercise
// the UTF-8 payload bound. Report only assertions, never submitted key material.
static void
test_live_formatted_keys(void) {
    const auto expect = [](const char* what, bool ok) { check(ok, what); };
    auto opts = std::make_unique<dsd_opts>();
    auto state = std::make_unique<dsd_state>();
    initOpts(opts.get());
    initState(state.get());
    dsd_app_frontend_runtime_start(opts.get(), state.get());
    const dsd_qt::CommandBridge bridge;

    QStringList pairs;
    for (int i = 0; i < 32; ++i) {
        pairs.append(QString::number(128 + i, 16));
    }
    const QString spaced = pairs.join(QLatin1Char(' '));
    expect("AES-256 byte-pair entry exceeds the wire capacity before normalization", spaced.size() == 95);
    const QString canonical = pairs.join(QString()).toUpper();
    for (const auto& formatted : {spaced, QStringLiteral(" \t0x") + pairs.join(QStringLiteral(" \r\n"))}) {
        QVariantMap sys{{QStringLiteral("sourceType"), QStringLiteral("usb")},
                        {QStringLiteral("freqMhz"), QStringLiteral("851.375")}};
        sys[QStringLiteral("encKeyType")] = QStringLiteral("hex");
        sys[QStringLiteral("encKeyValue")] = formatted;
        const auto args = session_args_build(sys, SessionArgPrefs(), nullptr);
        const auto at = args.indexOf(QStringLiteral("-H"));
        expect("formatted AES-256 starts with a normalized discrete key",
               at >= 0 && at + 1 < args.size() && args[at + 1] == canonical);
        expect("formatted AES-256 accepted through the production bridge", bridge.applyEncryptionKey("hex", formatted));
        expect("formatted AES-256 reaches the real queue", dsd_app_drain_cmds(opts.get(), state.get()) == 1);
        for (int slot = 0; slot < 2; ++slot) {
            expect("formatted AES-256 arms both slots",
                   state->aes_key_loaded[slot] && state->aes_key_segments[slot] == 4);
        }
        bool correct = true;
        for (int i = 0; i < 32; ++i) {
            correct = correct && state->aes_key[i] == 128 + i;
        }
        expect("formatted AES-256 retains every decoded key byte", correct);
    }

    const QString rc4 = QStringLiteral(" \t0x") + pairs.mid(0, 8).join(QString(10, QLatin1Char(' ')));
    expect("formatted RC4 is valid before live submission", dsd_qt::session_args_key_valid("rc4", rc4));
    expect("formatted RC4 accepted through the production bridge", bridge.applyEncryptionKey("rc4", rc4));
    expect("formatted RC4 reaches the real queue", dsd_app_drain_cmds(opts.get(), state.get()) == 1);
    expect("formatted RC4 loads the complete key", state->R == canonical.left(16).toULongLong(nullptr, 16));

    expect("oversized normalized key remains refused",
           !bridge.applyEncryptionKey("hex", QString(80, QLatin1Char('a'))));
    expect("embedded NUL remains refused", !bridge.applyEncryptionKey("hex", spaced + QChar(0) + QStringLiteral(" ")));
    expect("rejected bridge payloads never enter the queue", dsd_app_drain_cmds(opts.get(), state.get()) == 0);
    dsd_app_frontend_runtime_stop();
    freeState(state.get());
    DSD_SECURE_ZERO(state.get(), sizeof(*state));
}

// --- Persistent tail: append-only segments (issue #745) ---

static std::atomic<quint64> g_clock{0};

static DiagnosticsLog::TestOptions
manualOptions() {
    DiagnosticsLog::TestOptions options;
    options.clockMs = [] { return g_clock.load(); };
    options.manualDrain = true;
    return options;
}

static QByteArray
readFile(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

static void
writeFile(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    check(file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size(),
          "fixture write failed");
}

static QStringList
withoutDivider(QStringList rows) {
    if (!rows.isEmpty() && rows.first() == QLatin1String("--- previous run ---")) {
        rows.removeFirst();
    }
    return rows;
}

// Every flush used to replace tail.log with a rewritten copy (one rename per batch). An append-only
// tail keeps the file a reader already holds open. Uses only the long-standing API, so it runs on
// either writer.
static void
appendKeepsTheFile() {
#ifndef _WIN32
    QTemporaryDir dir;
    DiagnosticsLog log(dir.path());
    log.submit("host", "info", "first record");
    log.flush();
    const QString path = dir.filePath("tail.log");
    QFile held(path);
    check(held.open(QIODevice::ReadOnly), "tail exists after the first flush");
    for (int round = 0; round < 20; ++round) {
        for (int i = 0; i < 10; ++i) {
            log.submit("host", "info", QStringLiteral("record %1.%2").arg(round).arg(i));
        }
        log.flush();
        dsd_stat_t heldStat{};
        dsd_stat_t pathStat{};
        check(dsd_fstat(held.handle(), &heldStat) == 0
                  && dsd_stat_path(QFile::encodeName(path).constData(), &pathStat) == 0,
              "stat of the tail failed");
        check(heldStat.st_nlink == 1 && heldStat.st_ino == pathStat.st_ino,
              "the tail was replaced instead of appended to");
    }
#endif
}

static void
writeVolumeIsTheLogItself() {
    QTemporaryDir dir;
    g_clock = 0;
    DiagnosticsLog log(dir.path(), manualOptions());
    quint64 payload = 0;
    for (int i = 0; i < 2000; ++i) {
        const QString text = QStringLiteral("Sync: +P25p1 NAC [0x006] frame %1").arg(i);
        payload += static_cast<quint64>(QStringLiteral("[stderr] [info] %1").arg(text).toUtf8().size()) + 1U;
        log.submit("stderr", "info", text);
        if (i % 50 == 49) {
            g_clock += DiagnosticsLog::kWriteIntervalMs;
            log.pump();
        }
    }
    check(log.flush(), "flush after a write burst");
    const auto stats = log.ioStats();
    check(stats.bytes <= payload + payload / 20, "write volume exceeds the log's own bytes");
    check(stats.appends <= 41, "appends were not coalesced");
    check(stats.syncs <= 2 + static_cast<quint64>(g_clock.load() / DiagnosticsLog::kSyncIntervalMs),
          "syncs exceed one per interval");
}

static void
rotationKeepsTheBound() {
    QTemporaryDir dir;
    g_clock = 0;
    int written = 0;
    {
        DiagnosticsLog log(dir.path(), manualOptions());
        // Two queue-sized bursts of maximum-length records, then mixed sizes.
        for (int burst = 0; burst < 2; ++burst) {
            for (int i = 0; i < 2000; ++i) {
                log.submit("host", "info", QStringLiteral("seq %1 ").arg(written++) + QString(600, QLatin1Char('m')));
            }
            check(log.flush(), "burst flush");
            check(QFileInfo(dir.filePath("tail.log")).size() <= DiagnosticsLog::kSegmentLimit, "tail over the cap");
            check(QFileInfo(dir.filePath("tail.prev.log")).size() <= DiagnosticsLog::kSegmentLimit,
                  "previous segment over the cap");
        }
        for (int i = 0; i < 4000; ++i) {
            log.submit("host", "info", QStringLiteral("seq %1 ").arg(written++) + QString(i % 300, QLatin1Char('v')));
            if (i % 100 == 99) {
                g_clock += DiagnosticsLog::kWriteIntervalMs;
                log.pump();
            }
        }
        check(log.flush(), "mixed flush");
        const auto stats = log.ioStats();
        check(stats.rotations > 0 && stats.syncs >= stats.rotations, "a segment rotated before its data was synced");
    }
    check(QFileInfo(dir.filePath("tail.log")).size() <= DiagnosticsLog::kSegmentLimit, "tail over the cap");
    check(QFileInfo(dir.filePath("tail.prev.log")).size() <= DiagnosticsLog::kSegmentLimit, "previous over the cap");
    DiagnosticsLog reopened(dir.path(), manualOptions());
    const auto rows = withoutDivider(reopened.snapshot());
    check(!rows.isEmpty() && rows.last().contains(QStringLiteral("seq %1 ").arg(written - 1)),
          "reload lost the newest record");
    int last = -1;
    static const QRegularExpression seq(QStringLiteral("seq (\\d+) "));
    for (const auto& row : rows) {
        const auto match = seq.match(row);
        if (match.hasMatch()) {
            check(match.captured(1).toInt() > last, "reload out of order");
            last = match.captured(1).toInt();
        }
    }
}

static void
legacyTailIsSplit() {
    QTemporaryDir dir;
    QByteArray legacy;
    QStringList lines;
    for (int i = 0; legacy.size() < 250LL * 1024; ++i) {
        const QString line = QStringLiteral("[host] [info] legacy %1 ").arg(i) + QString(200, QChar(0x03bb));
        lines.append(line);
        legacy += line.toUtf8() + '\n';
    }
    writeFile(dir.filePath("tail.log"), legacy);
    g_clock = 0;
    {
        DiagnosticsLog log(dir.path(), manualOptions());
        check(QFileInfo(dir.filePath("tail.log")).size() <= DiagnosticsLog::kSegmentLimit, "legacy tail not split");
        check(QFileInfo(dir.filePath("tail.prev.log")).size() <= DiagnosticsLog::kSegmentLimit, "legacy prev too big");
        const auto rows = withoutDivider(log.snapshot());
        check(!rows.isEmpty() && rows.size() <= lines.size() && lines.mid(lines.size() - rows.size()) == rows,
              "legacy text changed on load");
        check(QString::fromUtf8(readFile(dir.filePath("tail.prev.log")) + readFile(dir.filePath("tail.log")))
                      .split('\n', Qt::SkipEmptyParts)
                  == rows,
              "segments are not the loaded lines");
        for (int i = 0; i < 300; ++i) {
            log.submit("host", "info", QString(400, QLatin1Char('n')));
        }
        check(log.flush(), "flush after legacy load");
    }
    check(QFileInfo(dir.filePath("tail.log")).size() + QFileInfo(dir.filePath("tail.prev.log")).size() <= 256LL * 1024,
          "legacy tail grew past the bound");
}

static void
crashFragmentIsDropped() {
    QTemporaryDir dir;
    writeFile(dir.filePath("tail.log"), QByteArray("[host] [info] complete\n") + QByteArray(500, 'z'));
    g_clock = 0;
    DiagnosticsLog log(dir.path(), manualOptions());
    check(!log.snapshot().join('\n').contains(QStringLiteral("zzz")), "fragment loaded as a record");
    log.submit("host", "info", "after the crash");
    check(log.flush(), "flush after the crash");
    const auto text = readFile(dir.filePath("tail.log"));
    check(!text.contains("zzz"), "fragment kept in the tail");
    for (const auto& line : text.split('\n')) {
        check(line.size() <= 512, "a record grew past the bound");
    }
    check(text.contains("\n[host] [info] after the crash\n"), "new record not on its own line");
}

// The shape a kernel panic left on an emulator: the tail's size reached the journal before its last unsynced
// second of data, which reads back as NUL bytes.
static void
crashZeroRunIsDropped() {
    QTemporaryDir dir;
    writeFile(dir.filePath("tail.log"), QByteArray("[host] [info] before the crash\n[host] [info] zeroed")
                                            + QByteArray(3000, '\0') + QByteArray("\n[host] [info] after the run\n"));
    g_clock = 0;
    DiagnosticsLog log(dir.path(), manualOptions());
    const auto rows = log.snapshot().join('\n');
    check(rows.contains(QStringLiteral("before the crash")) && rows.contains(QStringLiteral("after the run")),
          "intact lines around a zeroed run lost");
    check(!rows.contains(QChar(0)) && !rows.contains(QStringLiteral("zeroed")), "a zeroed run loaded as a record");
}

static void
flushReportsFailures() {
    QTemporaryDir dir;
    {
        DiagnosticsLog log(dir.path());
        check(log.flush(), "an empty flush completes");
        log.submit("host", "info", "durable record");
        check(log.flush(), "a flush is durable");
        DiagnosticsLog reader(dir.path(), manualOptions());
        check(reader.snapshot().join('\n').contains(QStringLiteral("durable record")), "flushed record not on disk");
        log.setIoFaultForTest(DiagnosticsLog::IoFault::Write);
        log.submit("host", "info", "lost record");
        check(!log.flush(), "a failed write reported durable");
        log.setIoFaultForTest(DiagnosticsLog::IoFault::Sync);
        log.submit("host", "info", "unsynced record");
        check(!log.flush(), "a failed sync reported durable");
        log.noteSessionAttempt(QStringLiteral("rtl"));
        log.setIoFaultForTest(DiagnosticsLog::IoFault::None);
        log.submit("host", "info", "recovered record");
        check(log.flush(), "writing resumes after a fault");
    }
    {
        // A record lost in an ordinary drain makes the next flush non-durable too.
        QTemporaryDir drained;
        g_clock = 0;
        DiagnosticsLog log(drained.path(), manualOptions());
        log.setIoFaultForTest(DiagnosticsLog::IoFault::Write);
        log.submit("host", "info", "lost in a drain");
        g_clock = DiagnosticsLog::kWriteIntervalMs;
        log.pump();
        log.setIoFaultForTest(DiagnosticsLog::IoFault::None);
        check(!log.flush(), "a flush after a failed drain reported durable");
        check(log.flush(), "the failure is reported once");
    }
    QTemporaryDir blocked;
    check(QDir(blocked.path()).mkpath(QStringLiteral("tail.log/inner")), "fixture directory");
    DiagnosticsLog log(blocked.path());
    log.submit("host", "info", "nowhere to go");
    check(!log.flush(), "an unopenable tail reported durable");
}

static void
syncCadenceAndStartupWindow() {
    QTemporaryDir dir;
    g_clock = 0;
    DiagnosticsLog log(dir.path(), manualOptions());
    log.submit("host", "info", "quiet record");
    g_clock = DiagnosticsLog::kWriteIntervalMs;
    log.pump();
    check(log.ioStats().appends == 1 && log.ioStats().syncs == 0, "an append was synced before its interval");
    g_clock = DiagnosticsLog::kSyncIntervalMs;
    log.pump();
    check(log.ioStats().syncs == 1, "dirty data not synced at its deadline");
    log.pump();
    check(log.ioStats().syncs == 1, "a clean tail was synced again");

    g_clock = 5000;
    log.noteSessionAttempt(QStringLiteral("rtl, boot 3"));
    check(readFile(dir.filePath("tail.log")).contains("--- session starting: rtl, boot 3 ---"),
          "the attempt marker is not on disk when the note returns");
    auto syncs = log.ioStats().syncs;
    for (int i = 0; i < 5; ++i) {
        g_clock += 100;
        log.submit("stderr", "info", QStringLiteral("stage %1").arg(i));
        log.pump();
        check(log.ioStats().syncs == ++syncs, "a startup-window record waited for the drain or the sync interval");
    }
    g_clock = 5000 + DiagnosticsLog::kStartupSyncWindowMs + 1;
    syncs = log.ioStats().syncs;
    for (int i = 0; i < 40; ++i) {
        g_clock += 50;
        log.submit("stderr", "info", QStringLiteral("steady %1").arg(i));
        log.pump();
    }
    check(log.ioStats().syncs - syncs <= 3, "steady-state syncs exceed one per second");
}

// One marker per attempt: a USB access request records its own line and only the start records the marker; a start
// note absorbs the plain marker of the Starting transition after it. Every new attempt, the same source again included,
// gets its own.
static void
oneMarkerPerAttempt() {
    QTemporaryDir dir;
    g_clock = 0;
    DiagnosticsLog log(dir.path(), manualOptions());
    const auto count = [&log](const QString& marker) { return log.snapshot().join('\n').count(marker); };
    const QString usb = QStringLiteral("--- session starting: usb, boot 3 ---");
    const QString access = QStringLiteral("usb access requested: usb, boot 3");
    const QString plain = QStringLiteral("--- session starting ---");

    log.noteSessionAttempt(QStringLiteral("usb, boot 3"), DiagnosticsLog::AttemptStage::UsbAccess);
    log.noteSessionAttempt(QStringLiteral("usb, boot 3"));
    log.noteSessionAttempt(QString());
    check(count(access) == 1 && count(usb) == 1 && count(plain) == 0,
          "one attempt through USB access logged other than one marker");

    // Stop, then Play the same source again within the dedupe window: a new attempt.
    g_clock += 5000;
    log.noteSessionAttempt(QStringLiteral("usb, boot 3"), DiagnosticsLog::AttemptStage::UsbAccess);
    log.noteSessionAttempt(QStringLiteral("usb, boot 3"));
    log.noteSessionAttempt(QString());
    check(count(usb) == 2 && count(plain) == 0, "a second Play of the same source lost its marker");

    // A USB access request whose start never came (denied, or abandoned for a backup flow), then a Play of the device
    // it opened: that start is its own attempt and keeps its marker.
    g_clock += 5000;
    log.noteSessionAttempt(QStringLiteral("usb, boot 3"), DiagnosticsLog::AttemptStage::UsbAccess);
    log.submit("host", "info", QStringLiteral("between the attempts"));
    g_clock += 5000;
    log.noteSessionAttempt(QStringLiteral("usb, boot 3"));
    log.noteSessionAttempt(QString());
    const auto rows = log.snapshot();
    check(count(usb) == 3 && count(plain) == 0 && rows.value(rows.size() - 1).contains(usb)
              && rows.value(rows.size() - 2).contains(QStringLiteral("between the attempts")),
          "an abandoned USB access request swallowed a later start's marker");

    // The first Play of a dongle: its label joins the start's context once the access request has opened it.
    g_clock += 5000;
    log.noteSessionAttempt(QStringLiteral("usb, boot 3"), DiagnosticsLog::AttemptStage::UsbAccess);
    log.noteSessionAttempt(QStringLiteral("usb, boot 3, RTL2838UHIDIR 0bda:2838"));
    check(count(QStringLiteral("--- session starting: usb, boot 3, RTL2838UHIDIR 0bda:2838 ---")) == 1,
          "a dongle's first start lost its labelled marker");

    g_clock += 5000;
    log.noteSessionAttempt(QStringLiteral("rtltcp, boot 3"));
    log.noteSessionAttempt(QStringLiteral("rtltcp, boot 3"));
    check(count(QStringLiteral("--- session starting: rtltcp, boot 3 ---")) == 2, "two starts shared one marker");

    // Starting transitions with no start note before them each get the plain marker.
    log.noteSessionAttempt(QString());
    log.noteSessionAttempt(QString());
    log.noteSessionAttempt(QString());
    check(count(plain) == 2, "plain markers miscounted");
}

// A sync that keeps failing is retried after an interval, never in a loop (the writer thread runs the same
// due check pump() does).
static void
failingSyncBacksOff() {
    QTemporaryDir dir;
    g_clock = 0;
    DiagnosticsLog log(dir.path(), manualOptions());
    log.setIoFaultForTest(DiagnosticsLog::IoFault::Sync);
    log.submit("host", "info", "record");
    g_clock = DiagnosticsLog::kSyncIntervalMs;
    check(log.pump() && log.ioStats().syncFailures == 1, "the due sync was not attempted");
    for (int i = 0; i < 100; ++i) {
        check(!log.pump(), "the writer would step again at once after a failed sync");
    }
    check(log.ioStats().syncFailures == 1, "a failed sync was retried at once");
    g_clock += DiagnosticsLog::kSyncIntervalMs;
    log.pump();
    check(log.ioStats().syncFailures == 2, "a failed sync was not retried after its interval");
    log.setIoFaultForTest(DiagnosticsLog::IoFault::None);
    g_clock += DiagnosticsLog::kSyncIntervalMs;
    log.pump();
    check(log.ioStats().syncs == 1, "the sync did not recover");

    // A failure, then a forced sync that succeeds (a Play checkpoint) within the backoff: the startup window's
    // next append is synced at once rather than after the old failure's retry time.
    log.setIoFaultForTest(DiagnosticsLog::IoFault::Sync);
    log.submit("host", "info", "before the failure");
    g_clock += DiagnosticsLog::kSyncIntervalMs;
    log.pump();
    log.setIoFaultForTest(DiagnosticsLog::IoFault::None);
    g_clock += 100;
    log.noteSessionAttempt(QStringLiteral("rtl"));
    const auto syncs = log.ioStats().syncs;
    g_clock += 100;
    log.submit("stderr", "info", "startup stage");
    log.pump();
    check(log.ioStats().syncs == syncs + 1, "a startup append waited out a recovered failure's backoff");
}

static void
segmentsStartWithTheirContext() {
    QTemporaryDir dir;
    g_clock = 0;
    {
        DiagnosticsLog log(dir.path(), manualOptions());
        log.noteProcess(QStringLiteral("1, test os, boot 7"));
        log.noteSessionAttempt(QStringLiteral("rtltcp"));
        for (int i = 0; i < 1200; ++i) {
            log.submit("stderr", "info", QString(300, QLatin1Char('s')));
        }
        check(log.flush(), "header flush");
        check(log.ioStats().rotations >= 2, "the fixture did not rotate");
    }
    for (const char* name : {"tail.log", "tail.prev.log"}) {
        const auto lines = readFile(dir.filePath(QLatin1String(name))).split('\n');
        check(lines.size() > 2 && lines.at(0).startsWith("[process] [info] pid 1, test os, boot 7")
                  && lines.at(1).contains("--- session starting: rtltcp ---"),
              "a segment does not start with its process and session");
    }
}

// A full segment rotates on the run's first record, which is its own process line: written once, not again
// in the header ahead of it.
static void
contextLineWrittenOnce() {
    QTemporaryDir dir;
    QByteArray full;
    while (full.size() < DiagnosticsLog::kSegmentLimit - 40) {
        full += "[stderr] [info] earlier run line\n";
    }
    writeFile(dir.filePath("tail.log"), full);
    g_clock = 0;
    DiagnosticsLog log(dir.path(), manualOptions());
    log.noteProcess(QStringLiteral("1, test os, boot 2"), {QStringLiteral("exit: one")});
    log.noteSessionAttempt(QStringLiteral("usb"));
    check(log.flush(), "context flush");
    const auto tail = readFile(dir.filePath("tail.log"));
    check(log.ioStats().rotations == 1, "the fixture did not rotate on the first record");
    check(tail.count("[process] [info] pid 1, test os, boot 2") == 1 && tail.startsWith("[process] [info] pid 1"),
          "the process line was written twice or not first");
    check(tail.count("--- session starting: usb ---") == 1, "the session line was written twice");
}

// After Clear the saved tail starts again with this process's line, and the cleared session's marker never comes
// back as a later segment's header. A clear that does not reach the disk is reported by the next flush.
static void
clearForgetsTheCleared() {
    QTemporaryDir dir;
    g_clock = 0;
    DiagnosticsLog log(dir.path(), manualOptions());
    log.noteProcess(QStringLiteral("1, test os, boot 4"));
    log.noteSessionAttempt(QStringLiteral("cleared source"));
    check(log.flush(), "pre-clear flush");
    log.clear();
    // About one and a half segments: exactly one rotation, so the previous segment is the first one after Clear.
    for (int i = 0; i < 900; ++i) {
        log.submit("stderr", "info", QString(200, QLatin1Char('q')));
    }
    check(log.flush(), "post-clear flush");
    check(log.ioStats().rotations == 1, "post-clear writes did not rotate exactly once");
    const auto saved = readFile(dir.filePath("tail.prev.log")) + readFile(dir.filePath("tail.log"));
    check(!saved.contains("cleared source"), "a cleared session marker came back in a header");
    check(readFile(dir.filePath("tail.prev.log")).startsWith("[process] [info] pid 1, test os, boot 4"),
          "the first segment after Clear does not start with the process line");
    log.setIoFaultForTest(DiagnosticsLog::IoFault::Sync);
    log.clear();
    check(!log.flush(), "a clear that did not reach the disk reported durable");
}

// The previous run's identity and this run's process and session lines stay at the front of what Copy and Share
// export, however many records arrive after them.
static void
pinnedContextOutlivesTheRing() {
    QTemporaryDir dir;
    QByteArray previous("[process] [info] pid 9, android, boot 4\n[session] [info] --- session starting: rtl ---\n");
    for (int i = 0; i < 2500; ++i) {
        previous += QStringLiteral("[stderr] [info] short %1\n").arg(i).toUtf8();
    }
    writeFile(dir.filePath("tail.log"), previous);
    g_clock = 0;
    DiagnosticsLog log(dir.path(), manualOptions());
    log.noteProcess(QStringLiteral("10, android, boot 4"), {QStringLiteral("exit: one"), QStringLiteral("exit: two")});
    log.noteSessionAttempt(QStringLiteral("rtltcp, boot 4"));
    for (int i = 0; i < 2100; ++i) {
        log.submit("stderr", "info", QStringLiteral("live %1").arg(i));
    }
    const auto rows = log.snapshot();
    check(rows.size() <= 2000, "ring over its limit");
    const auto head = rows.mid(0, 6).join('\n');
    check(rows.value(0) == QLatin1String("--- previous run ---") && head.contains(QStringLiteral("pid 9, android"))
              && head.contains(QStringLiteral("session starting: rtl ---"))
              && head.contains(QStringLiteral("pid 10, android"))
              && head.contains(QStringLiteral("session starting: rtltcp, boot 4")),
          "context lines were evicted from the ring");
    check(rows.last().contains(QStringLiteral("live 2099")), "newest record missing");
}

// A split of an older build's tail that a crash interrupted after writing the previous segment is redone from the
// unsplit tail, without duplicating its records.
static void
interruptedLegacySplit() {
    QTemporaryDir dir;
    QByteArray legacy;
    for (int i = 0; i < 500; ++i) {
        legacy += QStringLiteral("[host] [info] legacy %1 ").arg(i).toUtf8() + QByteArray(400, 'l') + '\n';
    }
    writeFile(dir.filePath("tail.log"), legacy);
    writeFile(dir.filePath("tail.prev.log"), legacy.left(legacy.indexOf('\n', 100LL * 1024) + 1));
    g_clock = 0;
    DiagnosticsLog log(dir.path(), manualOptions());
    const auto text = log.snapshot().join('\n');
    // Record 200 lies inside the interrupted split's prefix, so a reload that joined both would carry it twice.
    check(text.count(QStringLiteral("legacy 499 ")) == 1 && text.count(QStringLiteral("legacy 200 ")) == 1,
          "an interrupted split duplicated records");
    const auto saved = readFile(dir.filePath("tail.prev.log")) + readFile(dir.filePath("tail.log"));
    check(saved.count("legacy 200 ") == 1, "the re-split segments duplicate records");
}

// A split of an older build's tail whose previous segment cannot be written leaves the unsplit tail as it was, so the
// prefix is not lost; the next load splits it again.
static void
failedLegacySplitKeepsTheTail() {
    QTemporaryDir dir;
    QByteArray legacy;
    for (int i = 0; i < 500; ++i) {
        legacy += QStringLiteral("[host] [info] legacy %1 ").arg(i).toUtf8() + QByteArray(400, 'l') + '\n';
    }
    writeFile(dir.filePath("tail.log"), legacy);
    // A directory where the previous segment goes: replacing it fails on every platform.
    check(QDir(dir.path()).mkdir(QStringLiteral("tail.prev.log")), "blocker setup");
    g_clock = 0;
    {
        DiagnosticsLog log(dir.path(), manualOptions());
        check(readFile(dir.filePath("tail.log")).startsWith("[host] [info] legacy 0 "),
              "a failed split truncated the unsplit tail");
    }
    check(QDir(dir.filePath(QStringLiteral("tail.prev.log"))).removeRecursively(), "blocker cleanup");
    DiagnosticsLog log(dir.path(), manualOptions());
    const auto text = log.snapshot().join('\n');
    check(text.count(QStringLiteral("legacy 0 ")) == 1 && text.count(QStringLiteral("legacy 499 ")) == 1,
          "records were lost or duplicated after a failed split");
}

// A rotation whose sync fails and whose new segment cannot be opened leaves no dirty count behind with no file to
// sync it, so the writer does not stay due forever.
static void
lostSegmentDoesNotSpin() {
    QTemporaryDir dir;
    g_clock = 0;
    DiagnosticsLog log(dir.path(), manualOptions());
    for (int i = 0; i < 590; ++i) {
        log.submit("stderr", "info", QString(200, QLatin1Char('k')));
    }
    check(log.flush(), "fill flush");
    log.setIoFaultForTest(DiagnosticsLog::IoFault::SyncAndOpen);
    for (int i = 0; i < 20; ++i) {
        log.submit("stderr", "info", QString(200, QLatin1Char('k')));
    }
    check(!log.flush(), "a lost segment reported durable");
    for (int i = 0; i < 5; ++i) {
        g_clock += 10ULL * DiagnosticsLog::kSyncIntervalMs;
        check(!log.pump(), "the writer stayed due with no segment to sync");
    }
}

// Session lines a newer one replaced give their pinned slot back: after many sessions the front still holds the
// previous run's identity, this process and the current session.
static void
pinsFollowTheCurrentSession() {
    QTemporaryDir dir;
    writeFile(dir.filePath("tail.log"),
              "[process] [info] pid 9, android, boot 4\n[session] [info] --- session starting: old ---\n");
    g_clock = 0;
    DiagnosticsLog log(dir.path(), manualOptions());
    log.noteProcess(QStringLiteral("10, android, boot 4"));
    for (int session = 0; session < 10; ++session) {
        g_clock += 70000;
        log.noteSessionAttempt(QStringLiteral("source %1").arg(session));
        for (int i = 0; i < 2100; ++i) {
            log.submit("stderr", "info", QStringLiteral("live %1.%2").arg(session).arg(i));
        }
    }
    const auto rows = log.snapshot();
    const auto head = rows.mid(0, 5).join('\n');
    check(rows.size() <= 2000, "ring over its limit");
    check(head.contains(QStringLiteral("pid 9, android")) && head.contains(QStringLiteral("session starting: old"))
              && head.contains(QStringLiteral("pid 10, android")) && head.contains(QStringLiteral("source 9")),
          "the current session's line was evicted while older ones stayed pinned");
    check(!rows.join('\n').contains(QStringLiteral("source 3 ")), "a replaced session line stayed pinned");
}

// Repeats of one session line (the same source started again and again) take a single pinned slot, so the current
// process and session lines still fit at the front.
static void
identicalSessionLinesPinOnce() {
    QTemporaryDir dir;
    QByteArray saved("[process] [info] pid 9, android, boot 4\n");
    for (int i = 0; i < 10; ++i) {
        saved += "[session] [info] --- session starting: usb, boot 4 ---\n[stderr] [info] between\n";
    }
    writeFile(dir.filePath("tail.log"), saved);
    g_clock = 0;
    DiagnosticsLog log(dir.path(), manualOptions());
    log.noteProcess(QStringLiteral("10, android, boot 4"));
    log.noteSessionAttempt(QStringLiteral("rtltcp, boot 4"));
    for (int i = 0; i < 2100; ++i) {
        log.submit("stderr", "info", QStringLiteral("live %1").arg(i));
    }
    const auto rows = log.snapshot();
    const auto head = rows.mid(0, 5).join('\n');
    check(head.contains(QStringLiteral("pid 10, android")) && head.contains(QStringLiteral("session starting: rtltcp")),
          "repeated session lines crowded the current context out of the front");
    check(rows.join('\n').count(QStringLiteral("session starting: usb, boot 4")) == 1,
          "a repeated line was pinned twice");
}

// A previous run that never started a session pins no session line: an older run's session line stays with that
// run's process line, so neither is pinned beside the newer process.
static void
pinnedSessionBelongsToThePinnedProcess() {
    QTemporaryDir dir;
    QByteArray saved("[process] [info] pid 7, android, boot 4\n[session] [info] --- session starting: older ---\n"
                     "[process] [info] pid 8, android, boot 4\n");
    for (int i = 0; i < 2500; ++i) {
        saved += QStringLiteral("[stderr] [info] short %1\n").arg(i).toUtf8();
    }
    writeFile(dir.filePath("tail.log"), saved);
    g_clock = 0;
    DiagnosticsLog log(dir.path(), manualOptions());
    log.noteProcess(QStringLiteral("9, android, boot 4"));
    for (int i = 0; i < 2100; ++i) {
        log.submit("stderr", "info", QStringLiteral("live %1").arg(i));
    }
    const auto text = log.snapshot().join('\n');
    check(text.contains(QStringLiteral("pid 8, android")) && text.contains(QStringLiteral("pid 9, android")),
          "a process line was evicted from the front");
    check(!text.contains(QStringLiteral("session starting: older")),
          "an older run's session line was pinned beside a newer process line");
}

// Runs on one boot that start the same source write identical session lines; each run's own line stays pinned after
// its own process line.
static void
identicalSessionLinesAcrossRuns() {
    const QString session = QStringLiteral("--- session starting: usb, boot 4 ---");
    {
        QTemporaryDir dir;
        writeFile(dir.filePath("tail.log"), "[process] [info] pid 9, android, boot 4\n"
                                            "[session] [info] --- session starting: usb, boot 4 ---\n");
        g_clock = 0;
        DiagnosticsLog log(dir.path(), manualOptions());
        log.noteProcess(QStringLiteral("10, android, boot 4"));
        log.noteSessionAttempt(QStringLiteral("usb, boot 4"));
        for (int i = 0; i < 2100; ++i) {
            log.submit("stderr", "info", QStringLiteral("live %1").arg(i));
        }
        const auto rows = log.snapshot();
        const auto text = rows.join('\n');
        const auto at = [&rows](const QString& needle, int from) {
            for (int i = from; i < rows.size(); ++i) {
                if (rows.at(i).contains(needle)) {
                    return i;
                }
            }
            return -1;
        };
        const int current = at(QStringLiteral("pid 10, android"), 0);
        check(text.count(session) == 2 && current >= 0 && at(session, current) > current,
              "the current run's session line was evicted as a repeat of the previous run's");
    }
    {
        QTemporaryDir dir;
        QByteArray saved(
            "[process] [info] pid 7, android, boot 4\n[session] [info] --- session starting: usb, boot 4 ---\n"
            "[process] [info] pid 8, android, boot 4\n[session] [info] --- session starting: usb, boot 4 ---\n");
        // Few enough to stay in the reloaded ring: the live records evict them later, past the pinning.
        for (int i = 0; i < 1000; ++i) {
            saved += QStringLiteral("[stderr] [info] short %1\n").arg(i).toUtf8();
        }
        writeFile(dir.filePath("tail.log"), saved);
        g_clock = 0;
        DiagnosticsLog log(dir.path(), manualOptions());
        log.noteProcess(QStringLiteral("9, android, boot 4"));
        for (int i = 0; i < 2100; ++i) {
            log.submit("stderr", "info", QStringLiteral("live %1").arg(i));
        }
        const auto rows = log.snapshot();
        check(rows.value(1).contains(QStringLiteral("pid 8, android")) && rows.value(2).contains(session)
                  && rows.join('\n').count(session) == 1,
              "the pinned previous session line is not the one after the pinned process line");
    }
}

// A reload that reserves a slot for the process line moves its cutoff past the session line: that line is kept too.
static void
reloadCutoffKeepsBothContextLines() {
    QTemporaryDir dir;
    QByteArray saved("[process] [info] pid 9, android, boot 4\n[process] [info] exit: one\n"
                     "[session] [info] --- session starting: rtl ---\n");
    for (int i = 0; i < 1998; ++i) {
        saved += QStringLiteral("[stderr] [info] decoder %1\n").arg(i).toUtf8();
    }
    writeFile(dir.filePath("tail.log"), saved);
    g_clock = 0;
    DiagnosticsLog log(dir.path(), manualOptions());
    const auto text = log.snapshot().join('\n');
    check(text.contains(QStringLiteral("pid 9, android")) && text.contains(QStringLiteral("session starting: rtl")),
          "a context line fell between the reload cutoffs");
}

static void
previousRunBootFact() {
    const auto run = [](const QByteArray& previous, const QString& facts) {
        QTemporaryDir dir;
        writeFile(dir.filePath("tail.log"), previous);
        g_clock = 0;
        DiagnosticsLog log(dir.path(), manualOptions());
        log.noteProcess(facts);
        return log.snapshot().join('\n');
    };
    const QByteArray before("[process] [info] pid 1, android, boot 5\n[stderr] [info] streaming\n");
    check(run(before, QStringLiteral("2, android, boot 6"))
              .contains(QStringLiteral("previous run: Android has restarted since (boot 5 -> 6)")),
          "boot change not reported");
    check(!run(before, QStringLiteral("2, android, boot 5")).contains(QStringLiteral("previous run:")),
          "same boot reported as a restart");
    check(!run(before, QStringLiteral("2, desktop")).contains(QStringLiteral("previous run:")),
          "unknown boot reported as a restart");
}

static void
contextSurvivesTheRingLimit() {
    QTemporaryDir dir;
    QByteArray previous("[process] [info] pid 9, android, boot 4\n[session] [info] --- session starting: rtl ---\n");
    for (int i = 0; i < 2500; ++i) {
        previous += QStringLiteral("[stderr] [info] short %1\n").arg(i).toUtf8();
    }
    writeFile(dir.filePath("tail.log"), previous);
    g_clock = 0;
    DiagnosticsLog log(dir.path(), manualOptions());
    DiagnosticsLogModel model(&log);
    model.refresh();
    const auto rows = model.allText().split('\n');
    check(rows.size() <= 2000, "ring over its limit");
    check(rows.size() > 3 && rows.at(1).startsWith(QStringLiteral("[process] [info] pid 9"))
              && rows.at(2).contains(QStringLiteral("session starting: rtl")),
          "context dropped by the ring limit");
    check(rows.last().contains(QStringLiteral("short 2499")), "newest previous-run line missing");
    log.noteProcess(QStringLiteral("10, android, boot 5"));
    check(log.snapshot().join('\n').contains(QStringLiteral("boot 4 -> 5")), "boot fact lost to the ring limit");
}

static void
unicodeDirectory() {
    QTemporaryDir root;
    const QString path = root.filePath(QStringLiteral("диагностика-診断"));
    check(QDir().mkpath(path), "unicode directory");
    QByteArray legacy;
    while (legacy.size() < 200LL * 1024) {
        legacy += "[host] [info] legacy line with some length to it\n";
    }
    writeFile(path + QStringLiteral("/tail.log"), legacy);
    g_clock = 0;
    DiagnosticsLog log(path, manualOptions());
    check(QFile::exists(path + QStringLiteral("/tail.prev.log")), "legacy split failed in a unicode directory");
    for (int i = 0; i < 2000; ++i) {
        log.submit("host", "info", QString(200, QLatin1Char('u')));
    }
    check(log.flush() && log.ioStats().rotations > 0, "rotation failed in a unicode directory");
    check(QFileInfo(path + QStringLiteral("/tail.log")).size() <= DiagnosticsLog::kSegmentLimit,
          "unicode tail over cap");
    check(!log.snapshot().join('\n').contains(QStringLiteral("could not be rotated")), "unicode rotation fell back");
}

static void
orphansAndExpiry() {
    QTemporaryDir dir;
    writeFile(dir.filePath("tail.log.AbC123"), "orphan");
    writeFile(dir.filePath("tail.prev.log.Xy9z00"), "orphan");
    writeFile(dir.filePath("tail.log.toolong7"), "kept");
    writeFile(dir.filePath("notes.txt"), "kept");
    writeFile(dir.filePath("tail.prev.log"), "[host] [info] stale\n");
    QFile stale(dir.filePath("tail.prev.log"));
    check(stale.open(QIODevice::ReadWrite)
          && stale.setFileTime(QDateTime::currentDateTimeUtc().addDays(-8), QFileDevice::FileModificationTime));
    stale.close();
    DiagnosticsLog log(dir.path(), manualOptions());
    check(!QFile::exists(dir.filePath("tail.log.AbC123")) && !QFile::exists(dir.filePath("tail.prev.log.Xy9z00")),
          "orphaned temporaries kept");
    check(QFile::exists(dir.filePath("tail.log.toolong7")) && QFile::exists(dir.filePath("notes.txt")),
          "unrelated files removed");
    check(!QFile::exists(dir.filePath("tail.prev.log")), "an expired previous segment kept");
}

static void
rotationFailureKeepsTheBound() {
    QTemporaryDir dir;
    check(QDir(dir.path()).mkpath(QStringLiteral("tail.prev.log/inner")), "fixture directory");
    g_clock = 0;
    DiagnosticsLog log(dir.path(), manualOptions());
    for (int i = 0; i < 3000; ++i) {
        log.submit("host", "info", QStringLiteral("seq %1 ").arg(i) + QString(100, QLatin1Char('r')));
    }
    log.flush();
    check(QFileInfo(dir.filePath("tail.log")).size() <= DiagnosticsLog::kSegmentLimit, "tail grew past the cap");
    check(log.snapshot().join('\n').count(QStringLiteral("could not be rotated")) == 1,
          "rotation failure not noted once");
    check(readFile(dir.filePath("tail.log")).contains("seq 2999 "), "newest record lost after a failed rotation");
}

int
main(int argc, char** argv) {
    QTemporaryDir earlyDir;
    qputenv("XDG_DATA_HOME", earlyDir.path().toUtf8());
    dsd_neo_log_set_sink(DSD_NEO_LOG_SINK_STDERR);
    // USB status may arrive before Qt establishes its app-data location and
    // before nativeInit selects the platform sink or starts the stderr pump.
    DiagnosticsLog::submitHostDiagnostic(QStringLiteral("usb: permission pending before init"));
    QGuiApplication app(argc, argv);
    QTemporaryDir dir;
    qputenv("XDG_DATA_HOME", dir.path().toUtf8());
    DiagnosticsLog::installTap();
    earlyHostCaptureAndBridge();
    clearWithConcurrentSubmit();
    inactiveModelDefersResets();
    clearAndReopen();
    test_live_formatted_keys();
    realKeySources();
    appendKeepsTheFile();
    writeVolumeIsTheLogItself();
    rotationKeepsTheBound();
    legacyTailIsSplit();
    crashFragmentIsDropped();
    crashZeroRunIsDropped();
    flushReportsFailures();
    syncCadenceAndStartupWindow();
    segmentsStartWithTheirContext();
    contextLineWrittenOnce();
    oneMarkerPerAttempt();
    failingSyncBacksOff();
    clearForgetsTheCleared();
    pinnedContextOutlivesTheRing();
    interruptedLegacySplit();
    failedLegacySplitKeepsTheTail();
    lostSegmentDoesNotSpin();
    pinsFollowTheCurrentSession();
    pinnedSessionBelongsToThePinnedProcess();
    identicalSessionLinesAcrossRuns();
    reloadCutoffKeepsBothContextLines();
    identicalSessionLinesPinOnce();
    previousRunBootFact();
    contextSurvivesTheRingLimit();
    unicodeDirectory();
    orphansAndExpiry();
    rotationFailureKeepsTheBound();
    {
        DiagnosticsLog log(dir.path());
        DiagnosticsLogModel model(&log);
        log.submit("tap", "info", "decoder record");
        log.submit("stderr", "info", "stderr record");
        log.submit("host", "info", "host record");
        model.refresh();
        check(model.rowCount() == 3);
        model.setPaused(true);
        log.submit("host", "info", "pending");
        model.refresh();
        check(model.rowCount() == 3 && model.pendingCount() == 1);
        model.setPaused(false);
        check(model.rowCount() == 4 && model.pendingCount() == 0);
        const QString secret = QString(16, QLatin1Char('A')) + QString(16, QLatin1Char('B'));
        log.submit("stderr", "error", "Invalid key: " + secret + "malformed");
        log.submit("tap", "info", secret);
        checkStoredDiagnostics(log, model, dir.path(), hexRepresentations(secret.toUtf8()), "decoder record");
    }
    {
        DiagnosticsLog log(dir.path());
        DiagnosticsLogModel model(&log);
        model.refresh();
        const auto previous = model.allText();
        check(previous.contains("previous run") && previous.contains("decoder record")
              && previous.contains("stderr record") && previous.contains("host record"));
        model.clear();
        std::vector<std::thread> threads;
        threads.reserve(4);
        for (int i = 0; i < 4; ++i) {
            threads.emplace_back([&] {
                for (int j = 0; j < 600; ++j) {
                    log.submit("host", "info", QString(600, QLatin1Char('x')));
                }
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }
        model.refresh();
        check(model.rowCount() == 2000);
        for (const auto& line : model.allText().split('\n')) {
            check(line.toUtf8().size() <= 512);
        }
        log.flush();
        QFile tail(dir.filePath("tail.log"));
        check(tail.size() <= 256LL * 1024);
        model.clear();
        check(model.rowCount() == 0);
        log.submit("host", "info", QString(600, QChar(0x03bb)));
        model.refresh();
        check(model.allText().toUtf8().size() <= 512 && !model.allText().contains(QChar::ReplacementCharacter));
    }
    QFile expired(dir.filePath("tail.log"));
    check(expired.open(QIODevice::ReadWrite));
    check(expired.setFileTime(QDateTime::currentDateTimeUtc().addDays(-8), QFileDevice::FileModificationTime));
    expired.close();
    {
        DiagnosticsLog log(dir.path());
        check(log.snapshot().isEmpty());
        check(!QFile::exists(dir.filePath("tail.log")));
    }
    return failures ? 1 : 0;
}
