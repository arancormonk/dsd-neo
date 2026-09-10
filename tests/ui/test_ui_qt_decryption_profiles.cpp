// SPDX-License-Identifier: GPL-3.0-or-later
#include <QByteArray>
#include <QChar>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QJsonDocument>
#include <QList>
#include <QMap>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <cstdio>
#include <cstdlib>
#include <dsd-neo/core/key_set.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/protocol/nxdn/nxdn_lfsr.h>
#include <utility>
#include "../test_support/qt_test_paths.h"
#include "decoder_host.h"
#include "decryption_profiles_model.h"
#include "json_store.h"
#include "saved_systems_model.h"
#include "scan_lists_model.h"
#include "session_args.h"

static void
check(bool condition) {
    if (!condition) {
        std::fputs("Decryption profile regression failed\n", stderr);
        std::abort();
    }
}

void
LFSRN(const char*, char*, dsd_state*) {}

namespace {
class ReplayTestHost : public dsd_qt::DecoderHost {
  public:
    bool
    isRunning() const override {
        return false;
    }

    QString
    statusText() const override {
        return {};
    }

    bool
    start(const QStringList&) override {
        return false;
    }

    void
    stop() override {}
};
} // namespace

static void
test_replay_migration(dsd_qt::SavedSystemsModel& systems) {
    ReplayTestHost host;
    const QString cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    check(QDir().mkpath(cache));
    const QString oldPath = cache + "/legacy.wav";
    QFile file(oldPath);
    const QByteArray contents("test replay copy");
    check(file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size());
    file.close();
    check(systems.add({{"name", "Replay migration"}, {"sourceType", "file"}, {"filePath", oldPath}}));
    const int index = systems.count() - 1;
    check(systems.migrateCachedReplayFiles(&host));
    const QString durable = systems.get(index).value("filePath").toString();
    check(durable != oldPath && QFile::exists(oldPath));
    check(host.documentInfo(durable).value("sizeBytes").toLongLong() == contents.size());
    check(QFile::remove(oldPath));
    dsd_qt::SavedSystemsModel reopened;
    check(reopened.get(index).value("filePath").toString() == durable && QFile::exists(durable));
    check(systems.migrateCachedReplayFiles(&host));
    check(systems.get(index).value("filePath").toString() == durable);
}

int
main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    dsd_test_qt_isolate_paths();
    dsd_qt::SavedSystemsModel systems;
    dsd_qt::ScanListsModel scans;
    dsd_qt::DecryptionProfilesModel profiles;
    profiles.setReferences(&systems, &scans);
    const QString secret = QStringLiteral("1234567890ABCDEF1122334455667788");
    const QVariantMap entry{
        {"uid", "key-a"}, {"label", "Dispatch"}, {"keyId", 17}, {"kind", "aes128"}, {"material", secret}};
    auto saved = profiles.saveProfile({{"uid", "profile-a"},
                                       {"label", "System A"},
                                       {"protocol", "p25"},
                                       {"mode", "automatic"},
                                       {"keys", QVariantList{entry}}});
    check(saved.value("ok").toBool());
    QString error;
    const auto config = profiles.configuration("profile-a", &error);
    check(error.isEmpty());
    const QByteArray metadata = QJsonDocument::fromVariant(profiles.get("profile-a")).toJson();
    check(!metadata.contains(secret.toUtf8()) && !metadata.contains("material"));
    dsd_key_set keys = {};
    check(dsd_key_set_load_csv(&keys, config.value("keysHexCsvPath").toString().toUtf8().constData(), nullptr, 0) == 0);
    check(keys.count == 2 && keys.entries[0].index == 17 && keys.entries[1].index == 17 + 0x101);
    dsd_key_set_free(&keys);
    const QVariantMap collision{
        {"uid", "key-b"}, {"label", "Other"}, {"keyId", 17 + 0x101}, {"kind", "scalar"}, {"material", "1234"}};
    saved = profiles.saveProfile({{"uid", "profile-a"}, {"keys", QVariantList{entry, collision}}});
    check(!saved.value("ok").toBool());
    check(profiles.get("profile-a").value("keyCount").toInt() == 1);
    check(profiles.configuration("profile-a", &error) == config);

    systems.add({{"name", "Legacy"},
                 {"sourceType", "rtltcp"},
                 {"host", "127.0.0.1"},
                 {"port", 1234},
                 {"freqMhz", "851.5"},
                 {"decodeFlag", "-ft"},
                 {"encKeyType", "hex"},
                 {"encKeyValue", secret},
                 {"encForceKey", 0}});
    const QString systemUid = systems.get(0).value("uid").toString();
    check(profiles.migrateLegacySystems());
    check(systems.get(0).value("decryptionProfileUid").toString() == systemUid);
    check(systems.keyValueForUid(systemUid).isEmpty());
    check(profiles.configuration(systemUid, &error).value("encKeyValue").toString() == secret);
    const int migratedCount = profiles.count();
    check(profiles.migrateLegacySystems() && profiles.count() == migratedCount);
    QFile store(dsd_qt::json_store_path("saved_systems.json"));
    check(store.open(QIODevice::ReadOnly) && !store.readAll().contains(secret.toUtf8()));
    check(!profiles.removeProfile(systemUid).value("ok").toBool());
    dsd_qt::SessionArgsBuilder builder(nullptr);
    builder.setSavedSystems(&systems);
    builder.setDecryptionProfiles(&profiles);
    check(builder.build(systems.get(0)).value("ok").toBool());
    check(!QJsonDocument::fromVariant(builder.build(systems.get(0))).toJson().contains(secret.toUtf8()));

    const QString file = dsd_qt::json_store_path("decryption_profiles.json");
    check(QFile::rename(file, file + ".saved"));
    check(QDir().mkdir(file));
    saved = profiles.saveProfile({{"uid", "profile-a"}, {"label", "Failed rename"}});
    check(!saved.value("ok").toBool() && profiles.get("profile-a").value("label").toString() == "System A");
    check(profiles.configuration("profile-a", &error) == config);
    check(QDir().rmdir(file) && QFile::rename(file + ".saved", file));
    dsd_qt::DecryptionProfilesModel reopened;
    check(reopened.count() == profiles.count());
    check(reopened.configuration(systemUid, &error).value("encKeyValue").toString() == secret);
    saved = profiles.saveProfile({{"uid", "vendor-test"},
                                  {"label", "Vendor"},
                                  {"protocol", "dmr"},
                                  {"mode", "vendor"},
                                  {"vendorType", "tytEp"},
                                  {"vendorValue", secret}});
    check(saved.value("ok").toBool());
    const auto vendor = profiles.configuration("vendor-test", &error);
    check(vendor.value("decryptionVendorArgs").toStringList()
          == (QStringList{"-5", secret.left(16) + ' ' + secret.mid(16)}));
    check(!QJsonDocument::fromVariant(profiles.get("vendor-test")).toJson().contains(secret.left(16).toUtf8()));
    check(!profiles.saveProfile({{"uid", "vendor-test"}, {"vendorType", "csi"}}).value("ok").toBool());
    check(!profiles.saveProfile({{"uid", "vendor-test"}, {"vendorType", "kenwood"}, {"vendorValue", "32768"}})
               .value("ok")
               .toBool());
    check(profiles.configuration("vendor-test", &error) == vendor);
    check(!dsd_qt::session_args_profile_compatible({{"decryptionProtocol", "m17"}, {"decodeFlag", "-fs"}}));
    check(dsd_qt::session_args_profile_compatible({{"decryptionProtocol", "dmr"}, {"decodeFlag", "-fs"}}));
    test_replay_migration(systems);
    return 0;
}
