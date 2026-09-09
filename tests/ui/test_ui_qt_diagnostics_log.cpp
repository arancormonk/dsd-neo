// SPDX-License-Identifier: GPL-3.0-or-later
#include <QDateTime>
#include <QFile>
#include <QGuiApplication>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <cstdio>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/key_set.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/runtime/cli.h>
#include <dsd-neo/runtime/log.h>
#include <thread>
#include <vector>
#include "diagnostics_log.h"
extern "C" int dsd_test_diagnostics_direct_key_refused(dsd_opts*, dsd_state*, const char*);
#include <memory>
#include "test_support.h"
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

static void
checkAbsent(const QByteArray& text, const QList<QByteArray>& representations, const char* surface) {
    const auto folded = text.toLower();
    for (const auto& representation : representations) {
        check(!representation.isEmpty() && !folded.contains(representation.toLower()), surface);
    }
}

static QByteArray
parseKey(dsd_opts* opts, dsd_state* state, const char* flag, QByteArray value, bool accepted) {
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
    check(text.contains(accepted ? "key loaded" : "expected"), "CLI diagnostic outcome marker missing");
    if (accepted) {
        check(text.contains("[redacted]"), "CLI key-load redaction marker missing");
    }
    file.close();
    check(file.remove(), "CLI capture cleanup failed");
    DSD_SECURE_ZERO(value.data(), static_cast<size_t>(value.size()));
    return text;
}

static void
checkHexState(const dsd_opts* opts, const dsd_state* state, const QByteArray& key) {
    const auto first = key.left(16).toULongLong(nullptr, 16);
    const auto second = key.mid(16).toULongLong(nullptr, 16);
    check(state->H == first && state->K1 == first && state->K2 == second && state->K3 == 0 && state->K4 == 0,
          "CLI hex key scalars were not loaded");
    check(state->keyloader == 0 && state->hytera_key_segments == 2, "CLI hex key-loader state mismatch");
    for (int slot = 0; slot < 2; ++slot) {
        check(state->A1[slot] == first && state->A2[slot] == second && state->A3[slot] == 0 && state->A4[slot] == 0,
              "CLI AES slot scalars were not loaded");
        check(state->aes_key_loaded[slot] == 1 && state->aes_key_segments[slot] == 2,
              "CLI AES slot loading flags mismatch");
    }
    const auto bytes = QByteArray::fromHex(key);
    check(memcmp(state->aes_key, bytes.constData(), static_cast<size_t>(bytes.size())) == 0,
          "CLI AES bytes were not loaded");
    for (size_t i = static_cast<size_t>(bytes.size()); i < sizeof(state->aes_key); ++i) {
        check(state->aes_key[i] == 0, "CLI unused AES bytes were not cleared");
    }
    check(opts->dmr_mute_encL == 0 && opts->dmr_mute_encR == 0, "CLI encrypted-slot audio flags mismatch");
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
    const QByteArray hex = QByteArray(16, 'A') + QByteArray(16, 'B');
    const QByteArray basic = QByteArray::number(197);
    // A different key makes a future successful direct setter observable.
    const QByteArray direct = QByteArray(16, 'C') + QByteArray(16, 'D');
    const QByteArray malformed = hex + "invalid";
    auto representations = hexRepresentations(hex);
    representations.append(hexRepresentations(direct));
    representations.append(basic);
    representations.append(QByteArray::number(basic.toUInt(), 16));
    representations.append(malformed);

    state->keyloader = 1;
    state->K3 = state->K4 = 1;
    opts->dmr_mute_encL = opts->dmr_mute_encR = 1;
    auto captured = parseKey(opts.get(), state.get(), "-H", hex, true);
    checkHexState(opts.get(), state.get(), hex);
    checkAbsent(captured, representations, "CLI hex diagnostic contains a key representation");
    log.submit("stderr", "info", QString::fromUtf8(captured));

    state->K = 0;
    opts->dmr_mute_encL = opts->dmr_mute_encR = 1;
    captured = parseKey(opts.get(), state.get(), "-b", basic, true);
    check(state->K == basic.toULongLong(), "CLI basic key was not loaded");
    checkHexState(opts.get(), state.get(), hex);
    checkAbsent(captured, representations, "CLI basic diagnostic contains a key representation");
    log.submit("stderr", "info", QString::fromUtf8(captured));

    dsd_key_set before{};
    dsd_key_set after{};
    check(dsd_key_set_capture(&before, state.get()) == 0, "key state capture failed");
    captured = parseKey(opts.get(), state.get(), "-b", malformed, false);
    check(dsd_key_set_capture(&after, state.get()) == 0 && dsd_key_set_equal(&before, &after),
          "refused CLI key altered loaded state");
    dsd_key_set_free(&before);
    dsd_key_set_free(&after);
    checkAbsent(captured, representations, "CLI error diagnostic contains a key representation");
    log.submit("stderr", "info", QString::fromUtf8(captured));

    check(dsd_test_diagnostics_direct_key_refused(opts.get(), state.get(), direct.constData()),
          "KEY_DIRECT_SET must be refused by the foundation stub; assert successful loading when its handler lands");
    check(state->K == basic.toULongLong(), "refused direct command altered basic key");
    checkHexState(opts.get(), state.get(), hex);
    checkAbsent(QByteArray(state->ui_msg), representations, "direct-command message contains a key representation");
    log.submit("host", "error", "malformed key " + QString::fromUtf8(malformed));
    dsd_neo_log_write(LOG_LEVEL_INFO, "decoder restart marker\n");
    log.submit("host", "info", "host restart marker");
    log.submit("stderr", "info", "stderr restart marker");
    const auto directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/diagnostics";
    checkStoredDiagnostics(log, model, directory, representations, "decoder restart marker");
    {
        DiagnosticsLog restarted(directory);
        DiagnosticsLogModel previous(&restarted);
        previous.refresh();
        check(previous.allText().contains("previous run") && previous.allText().contains("decoder restart marker")
              && previous.allText().contains("host restart marker")
              && previous.allText().contains("stderr restart marker"));
        checkAbsent(previous.allText().toUtf8(), representations, "previous-run text contains a key representation");
    }
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

int
main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    QTemporaryDir dir;
    qputenv("XDG_DATA_HOME", dir.path().toUtf8());
    clearWithConcurrentSubmit();
    realKeySources();
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
        check(tail.size() <= 256 * 1024);
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
