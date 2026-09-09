// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest
Item {
    id: mainRoot
    property bool transitioning: false
    property bool showFailure: false
    width: 420; height: 900
    Loader { id: loader; source: uiDir + "/WizardScreen.qml" }
    Loader { id: home; source: uiDir + "/HomeScreen.qml"; active: false }
    TestCase {
        name: "WizardSites"
        when: windowShown
        function init() { while(savedSystems.count) savedSystems.remove(0); loader.item.openForAdd(false) }
        property string libraryPath: ""
        function cleanup() {
            home.active = false
            while(savedSystems.count) savedSystems.remove(0)
            if (libraryPath.length) importedFiles.remove(importedFiles.rowForPath(libraryPath))
            libraryPath = ""
        }
        function test_legacy_hint_and_manual_rows() {
            var path = testContext.writeFixtureCsv("site-provenance.csv", "Decimal,Mode,Alpha Tag\n123,A,Dispatch\n")
            var result = importedFiles.importGeneratedFile(path, "Site provenance.csv", "group",
                                                          {origin:"radioreference", rrSid:12, rrSiteIds:"16863", rrKind:"group"})
            verify(result.ok)
            libraryPath = result.path
            savedSystems.add({name:"Legacy", groupCsvPath:libraryPath})
            savedSystems.add({name:"Manual"})
            home.active = true
            verify(home.item.needsSiteRefresh(0))
            verify(!home.item.needsSiteRefresh(1))
            compare(savedSystems.siteCount(0), 0)
            compare(savedSystems.siteCount(1), 0)
        }
        function site(id, freq) {
            return {name:"Site " + id, rrSid:12, rrSiteId:id, siteName:"Site " + id,
                    freqMhz:freq, decodeFlag:"-f1", trunking:true, chanCsvPath:"", groupCsvPath:""}
        }
        function test_batch_saves_all_sites_with_source() {
            loader.item.applyRadioReference({ok:true, rows:[site(16863,"851"),site(48391,"852")]})
            loader.item.commit()
            compare(savedSystems.count, 2)
            compare(savedSystems.get(0).rrSiteId, 16863)
            compare(savedSystems.get(1).rrSiteId, 48391)
            compare(savedSystems.get(1).freqMhz, "852")
            compare(savedSystems.get(0).sourceType, savedSystems.get(1).sourceType)
            verify(savedSystems.get(0).sourceType.length > 0)
        }
        function openKeyedBatch() {
            savedSystems.add({name:"Earlier row"})
            var source = site(16863, "851")
            source.sourceType = "rtltcp"
            source.host = "127.0.0.1"
            source.port = 1234
            source.encKeyType = "basic"
            source.encKeyValue = String(17 * 3)
            savedSystems.add(source)
            loader.item.openForEdit(1)
            // Capture the source UID before reindexing and before batch import clears editRow.
            savedSystems.remove(0)
            loader.item.applyRadioReference({ok:true, rows:[site(16863,"851"),site(48391,"852")]})
        }
        function test_batch_from_keyed_row_keeps_private_key() {
            openKeyedBatch()
            loader.item.commit()
            compare(savedSystems.count, 3)
            for (var row = 1; row < 3; ++row) {
                var sys = savedSystems.get(row)
                verify(!("encKeyValue" in sys))
                verify(!("encKeyValue" in savedSystems.getByUid(sys.uid)))
                verify(sys.uid !== savedSystems.get(0).uid)
                var built = sessionArgs.build(sys)
                verify(built.ok, "Imported site's retained key must build a valid session")
                var keyIndex = built.args.indexOf("-b")
                verify(keyIndex >= 0)
                // Boolean only: never print the key or argv on failure.
                verify(built.args[keyIndex + 1] === String(17 * 3), "Imported site retains the source key")
            }
        }
        function test_batch_missing_key_source_requires_replace_or_clear() {
            openKeyedBatch()
            savedSystems.remove(0)
            loader.item.commit()
            compare(savedSystems.count, 0)
            verify(loader.item.csvNoticeIsProblem)
            var editor = findChild(loader.item, "wizardEncryptionEditor")
            editor.keyAction = "clear"
            loader.item.commit()
            compare(savedSystems.count, 2)
            verify(!savedSystems.get(0).encKeyConfigured && !savedSystems.get(1).encKeyConfigured)
        }
        function test_batch_replaces_key_when_requested() {
            openKeyedBatch()
            var editor = findChild(loader.item, "wizardEncryptionEditor")
            editor.keyAction = "replace"
            loader.item.encKeyValue = String(19 * 3)
            loader.item.commit()
            compare(savedSystems.count, 3)
            for (var row = 1; row < 3; ++row) {
                var sys = savedSystems.get(row)
                verify(!("encKeyValue" in sys))
                var built = sessionArgs.build(sys)
                verify(built.ok)
                var keyIndex = built.args.indexOf("-b")
                verify(keyIndex >= 0 && built.args[keyIndex + 1] === String(19 * 3), "Batch honors Replace")
            }
        }
        function test_grouped_edit_clears_provenance() {
            savedSystems.add(site(16863,"851"))
            loader.item.openForEdit(0)
            loader.item.freqText = "853"
            loader.item.commit()
            compare(savedSystems.get(0).rrSid, 0)
            compare(savedSystems.get(0).rrSiteId, 0)
        }
        function test_batch_first_site_edit_is_not_lost() {
            loader.item.applyRadioReference({ok:true, rows:[site(16863,"851"),site(48391,"852")]})
            loader.item.freqText = "853"
            loader.item.commit()
            compare(savedSystems.get(0).freqMhz, "853")
            compare(savedSystems.get(0).rrSiteId, 0)
            compare(savedSystems.get(1).rrSiteId, 48391)
        }
    }
}
