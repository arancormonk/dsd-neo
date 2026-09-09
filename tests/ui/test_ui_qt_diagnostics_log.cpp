// SPDX-License-Identifier: GPL-3.0-or-later
#include <QDateTime>
#include <QFile>
#include <QGuiApplication>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <cstdio>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/runtime/cli.h>
#include <dsd-neo/runtime/log.h>
#include <thread>
#include <vector>
#include "diagnostics_log.h"
extern "C" int dsd_test_diagnostics_direct_key(dsd_opts*, dsd_state*, const char*);
#include <memory>
#include "test_support.h"
using namespace dsd_qt;
static int failures = 0;

static void
check(bool ok) {
    if (!ok) {
        ++failures;
        std::fputs("diagnostics assertion failed\n", stderr);
    }
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
    const QString secret = QString(16, QLatin1Char('A')) + QString(16, QLatin1Char('B'));
    for (const auto& pair :
         {qMakePair(QByteArray("-H"), secret.toUtf8()), qMakePair(QByteArray("-b"), QByteArray("197")),
          qMakePair(QByteArray("-b"), secret.toUtf8() + "invalid")}) {
        auto option = pair.first;
        auto value = pair.second;
        char name[] = "diagnostics-test";
        char* argv[] = {name, option.data(), value.data(), nullptr};
        dsd_test_capture_stderr captured;
        check(dsd_test_capture_stderr_begin(&captured, "diagnostics_cli") == 0);
        int effective = 0, exitCode = 0;
        dsd_parse_args(3, argv, opts.get(), state.get(), &effective, &exitCode);
        check(opts->show_keys == 0);
        check(dsd_test_capture_stderr_end(&captured) == 0);
        QFile stderrFile(QString::fromUtf8(captured.path));
        check(stderrFile.open(QIODevice::ReadOnly));
        const auto stderrText = stderrFile.readAll();
        check(!stderrText.contains(value));
        log.submit("stderr", "info", QString::fromUtf8(stderrText));
        stderrFile.close();
        stderrFile.remove();
        DSD_SECURE_ZERO(value.data(), static_cast<size_t>(value.size()));
    }
    const auto bytes = secret.toUtf8();
    check(dsd_test_diagnostics_direct_key(opts.get(), state.get(), bytes.constData()));
    log.submit("host", "error", "malformed key " + secret + "invalid");
    dsd_neo_log_write(LOG_LEVEL_INFO, "decoder restart marker\n");
    log.submit("host", "info", "host restart marker");
    log.submit("stderr", "info", "stderr restart marker");
    model.refresh();
    log.flush();
    check(!model.allText().contains(secret) && !model.allText().contains("197"));
    QFile tail(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/diagnostics/tail.log");
    check(tail.open(QIODevice::ReadOnly));
    check(!tail.readAll().contains(bytes));
    QFile exported(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/diagnostics/export.txt");
    check(exported.open(QIODevice::WriteOnly));
    exported.write(model.allText().toUtf8());
    exported.close();
    check(exported.open(QIODevice::ReadOnly));
    check(!exported.readAll().contains(bytes));
    {
        DiagnosticsLog restarted(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/diagnostics");
        DiagnosticsLogModel previous(&restarted);
        previous.refresh();
        check(previous.allText().contains("previous run") && previous.allText().contains("decoder restart marker")
              && previous.allText().contains("host restart marker")
              && previous.allText().contains("stderr restart marker"));
    }
    freeState(state.get());
    DSD_SECURE_ZERO(state.get(), sizeof(*state));
}

int
main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    QTemporaryDir dir;
    qputenv("XDG_DATA_HOME", dir.path().toUtf8());
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
        model.refresh();
        check(!model.allText().contains(secret));
        QFile exported(dir.filePath("export.txt"));
        check(exported.open(QIODevice::WriteOnly));
        exported.write(model.allText().toUtf8());
        exported.close();
        check(exported.open(QIODevice::ReadOnly));
        check(!exported.readAll().contains(secret.toUtf8()));
        log.flush();
        QFile tail(dir.filePath("tail.log"));
        check(tail.open(QIODevice::ReadOnly));
        check(!tail.readAll().contains(secret.toUtf8()));
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
