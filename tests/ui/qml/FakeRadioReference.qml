// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

// Offline account and import responses; production QML owns the navigation.
// Deliberately limited to these flows: credentialsReady follows the password
// alone, without validating the username or application key. Imports record the
// savedRow argument but omit it from the result. Location request allocation,
// nearby/zip lookups and place-browsing load methods are not modeled here.
QtObject {
    signal locationRequestAllocated(double requestId)
    property bool available: true
    property bool hasAppKey: true
    property bool buildHasAppKey: true
    property bool credentialsReady: false
    property bool busy: false
    property string statusText: ""
    property string errorText: ""
    property int errorKind: 0
    property bool errorIsAuth: false
    property bool errorIsSubscription: false
    property bool conventional: false
    property bool trunked: true
    property var countries: []
    property var states: []
    property var counties: []
    property var systems: []
    property var sites: []
    property var systemDetails: ({})
    property var talkgroupSummary: ({})
    property string sessionPassword: ""
    property int accountChecks: 0
    property int imports: 0
    property int savedRow: -2

    function setPassword(value) {
        sessionPassword = value;
        credentialsReady = value.length > 0;
    }
    function checkAccount() {
        accountChecks++;
        errorText = "";
        errorIsAuth = false;
        errorIsSubscription = false;
    }
    function cancel() {
        busy = false;
    }
    function closeSystem() {
        systemDetails = ({});
        sites = [];
    }
    function loadSystem(sid) {
        systemDetails = {sid: sid, name: "County radio", typeDescr: "Project 25"};
        sites = [{siteNumber: 1, descr: "Central", freqCount: 1,
                  controlFreqMhz: "851.0125", freqMhz: "851.0125", simulcast: false, colorCode: ""}];
    }
    function buildImportPlan(selected, options) {
        return {ok: selected.length > 0, awaitingSelection: selected.length === 0,
                freqMhz: "851.0125", decodeFlag: "-f1", trunking: true,
                warnings: [], scanList: false};
    }
    function performImport(plan, name, row) {
        imports++;
        savedRow = row;
        return {ok: true, name: name, freqMhz: plan.freqMhz,
                decodeFlag: plan.decodeFlag, trunking: plan.trunking,
                rrSid: systemDetails.sid, rrSiteId: 1};
    }
}
