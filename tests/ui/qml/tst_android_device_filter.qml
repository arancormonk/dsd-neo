// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest

TestCase {
    name: "AndroidDeviceFilter"

    function matches(filters, vendor, product) {
        return filters.some(function (filter) {
            return (filter["vendor-id"] === -1 || filter["vendor-id"] === vendor)
                && (filter["product-id"] === -1 || filter["product-id"] === product);
        });
    }

    function test_usb_filter_matches_only_supported_devices() {
        var filters = testContext.androidUsbDeviceFilters();
        compare(filters.length, 43);
        verify(matches(filters, 0x1d50, 0x60a1), "Airspy R2 / Mini must match");
        verify(matches(filters, 0x0bda, 0x2832), "RTL2832U must match");
        verify(!matches(filters, 0x1234, 0x5678), "Unrelated USB devices must not match");
        verify(!matches(filters, 0x1d50, 0x5678), "Vendor alone must not match");
        verify(!matches(filters, 0x1234, 0x60a1), "Product alone must not match");
        for (var i = 0; i < filters.length; ++i) {
            verify(filters[i]["vendor-id"] >= 0, "Missing vendor-id at entry " + i);
            verify(filters[i]["product-id"] >= 0, "Missing product-id at entry " + i);
        }
    }
}
