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
