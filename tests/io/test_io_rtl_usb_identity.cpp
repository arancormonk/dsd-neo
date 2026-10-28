// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Which USB device an RTL-SDR runs on, and a reset of only that device, over a fake device list (the real dongles are
 * never touched). Identity takes exactly one vendor-specific-class device with the receiver's string triple, refuses
 * any duplicate or unreadable candidate (a twin of another VID included; a device this process may not open is not
 * unreadable), and needs one librtlsdr index with the triple. A reset re-reads the strings at the recorded port and
 * leaves a replacement alone; a device that re-enumerates is found again by its triple within the bound. Without
 * libusb every call reports it.
 */

#include <assert.h>
#include <dsd-neo/core/safe_api.h>
#include <numeric>
#include <stdint.h>
#include <string.h>
#include <vector>
#include "rtl_usb_identity.h"

// LLVM 22+ misclassifies these runtime test oracles as compile-time assertions.
// NOLINTBEGIN(cert-dcl03-c,misc-static-assert)

namespace {

struct fake_dev {
    rtl_usb_device_info info{};
    const char* manufacturer = nullptr;
    const char* product = nullptr;
    const char* serial = nullptr;
    int open_rc = 0;     /* RTL_USB_OP_* from usb_open */
    int read_rc = 0;     /* RTL_USB_OP_* from usb_read_strings */
    int rtl_read_rc = 0; /* RTL_USB_OP_* from librtlsdr's string read for its index (and, nonzero, of its handle) */
    int reset_rc = 0;    /* RTL_USB_OP_* from usb_reset */
    int resets = 0;      /* reset calls that reached this device */
    int hidden = 0;      /* enumerations it is absent from */
    int gone_hidden = 0; /* after a GONE reset: enumerations to stay absent, -1 for ever */
    int gone_port = 0;   /* after a GONE reset: the port it comes back on (0 keeps its port) */
    int deny_opens = 0;  /* opens to refuse with ACCESS, -1 for ever (udev not yet done) */
    int describe_rc = 0; /* RTL_USB_OP_* from usb_describe (info still filled, as the real op does) */
    int gone_deny = 0;   /* after a GONE reset: deny_opens */
};

struct fake_world {
    std::vector<fake_dev> devs;
    std::vector<int> rtl_order; /* librtlsdr index -> devs index */
    uint64_t now = 1000;
    int sessions = 0;
    int cancel_after_sessions = -1;
};

fake_world g_world;

struct fake_session {
    std::vector<int> visible;
};

void
copy_str(char* dst, const char* src) {
    DSD_MEMSET(dst, 0, 256);
    size_t n = strlen(src);
    if (n > 255) {
        n = 255;
    }
    DSD_MEMCPY(dst, src, n);
}

uint32_t
fake_rtl_count(void) {
    return (uint32_t)g_world.rtl_order.size();
}

int
fake_rtl_index_strings(uint32_t index, char* m, char* p, char* s) {
    if (index >= g_world.rtl_order.size()) {
        return RTL_USB_OP_ERROR;
    }
    const fake_dev& d = g_world.devs[(size_t)g_world.rtl_order[index]];
    if (d.rtl_read_rc != RTL_USB_OP_OK) {
        return d.rtl_read_rc;
    }
    copy_str(m, d.manufacturer);
    copy_str(p, d.product);
    copy_str(s, d.serial);
    return 0;
}

int
fake_rtl_handle_strings(rtlsdr_dev_t* dev, char* m, char* p, char* s) {
    const fake_dev* d = reinterpret_cast<fake_dev*>(dev);
    if (d->rtl_read_rc != 0) {
        return -1;
    }
    copy_str(m, d->manufacturer);
    copy_str(p, d->product);
    copy_str(s, d->serial);
    return 0;
}

int
fake_open_session(void** session, int* count) {
    auto* sess = new fake_session();
    g_world.sessions++;
    for (size_t i = 0; i < g_world.devs.size(); i++) {
        fake_dev& d = g_world.devs[i];
        if (d.hidden != 0) {
            if (d.hidden > 0) {
                d.hidden--;
            }
            continue;
        }
        sess->visible.push_back((int)i);
    }
    *session = sess;
    *count = (int)sess->visible.size();
    return RTL_USB_OP_OK;
}

void
fake_close_session(void* session) {
    delete static_cast<fake_session*>(session);
}

int
fake_describe(void* session, int i, rtl_usb_device_info* out) {
    const auto* sess = static_cast<fake_session*>(session);
    const fake_dev& d = g_world.devs[(size_t)sess->visible[(size_t)i]];
    *out = d.info;
    return d.describe_rc;
}

int
fake_open(void* session, int i, void** handle) {
    const auto* sess = static_cast<fake_session*>(session);
    fake_dev& d = g_world.devs[(size_t)sess->visible[(size_t)i]];
    if (d.deny_opens != 0) {
        if (d.deny_opens > 0) {
            d.deny_opens--;
        }
        return RTL_USB_OP_ACCESS;
    }
    if (d.open_rc != RTL_USB_OP_OK) {
        return d.open_rc;
    }
    *handle = &d;
    return RTL_USB_OP_OK;
}

void
fake_close(void* handle) {
    (void)handle;
}

int
fake_read_strings(void* handle, char* m, char* p, char* s) {
    const fake_dev* d = static_cast<fake_dev*>(handle);
    if (d->read_rc != RTL_USB_OP_OK) {
        return d->read_rc;
    }
    copy_str(m, d->manufacturer);
    copy_str(p, d->product);
    copy_str(s, d->serial);
    return RTL_USB_OP_OK;
}

int
fake_reset(void* handle) {
    fake_dev* d = static_cast<fake_dev*>(handle);
    d->resets++;
    if (d->reset_rc == RTL_USB_OP_GONE) {
        d->hidden = d->gone_hidden;
        d->deny_opens = d->gone_deny;
        if (d->gone_port != 0) {
            d->info.ports[d->info.port_depth - 1] = (uint8_t)d->gone_port;
        }
    }
    return d->reset_rc;
}

uint64_t
fake_now(void) {
    return g_world.now;
}

void
fake_sleep(unsigned int ms) {
    g_world.now += ms;
}

const rtl_usb_driver_ops k_fake_ops = {
    fake_rtl_count,
    fake_rtl_index_strings,
    fake_rtl_handle_strings,
    fake_open_session,
    fake_close_session,
    fake_describe,
    fake_open,
    fake_close,
    fake_read_strings,
    fake_reset,
    fake_now,
    fake_sleep,
};

fake_dev
make_dev(uint16_t vid, uint16_t pid, uint8_t bus, uint8_t port, const char* serial, int vendor_specific) {
    fake_dev d;
    DSD_MEMSET(&d, 0, sizeof(d));
    d.info.vid = vid;
    d.info.pid = pid;
    d.info.bus = bus;
    d.info.ports[0] = 1;
    d.info.ports[1] = port;
    d.info.port_depth = 2;
    d.info.vendor_specific = vendor_specific;
    d.manufacturer = "Realtek";
    d.product = "RTL2838UHIDIR";
    d.serial = serial;
    d.open_rc = RTL_USB_OP_OK;
    d.read_rc = RTL_USB_OP_OK;
    d.rtl_read_rc = 0;
    d.reset_rc = RTL_USB_OP_OK;
    d.describe_rc = RTL_USB_OP_OK;
    d.gone_hidden = 0;
    return d;
}

/* The workstation's layout: dongle A (bus 1) runs, dongle B (bus 3) belongs to another program, plus a HID device
 * nobody may open (not a candidate: no vendor-specific class). */
void
reset_world(void) {
    g_world = fake_world();
    g_world.devs.push_back(make_dev(0x0bda, 0x2838, 1, 2, "56435444", 1));
    g_world.devs.push_back(make_dev(0x0bda, 0x2838, 3, 1, "00000001", 1));
    fake_dev hid = make_dev(0x046d, 0xc52b, 1, 3, "", 0);
    hid.manufacturer = "Logitech";
    hid.product = "USB Receiver";
    hid.open_rc = RTL_USB_OP_ACCESS;
    g_world.devs.push_back(hid);
    g_world.rtl_order = {0, 1};
    rtl_usb_identity_set_driver_ops(&k_fake_ops);
}

int
total_resets(void) {
    return std::accumulate(g_world.devs.begin(), g_world.devs.end(), 0,
                           [](int n, const fake_dev& d) { return n + d.resets; });
}

#if defined(DSD_HAVE_LIBUSB) && defined(USE_RTLSDR)

int
cancel_check(void* ctx) {
    (void)ctx;
    return g_world.cancel_after_sessions >= 0 && g_world.sessions >= g_world.cancel_after_sessions;
}

rtl_usb_identity
resolve_a(void) {
    rtl_usb_identity id;
    assert(rtl_usb_identity_resolve(0, &id) == RTL_USB_ID_OK);
    assert(id.valid == 1);
    return id;
}

void
unique_match_resolves(void) {
    reset_world();
    rtl_usb_identity id;
    DSD_MEMSET(&id, 0x5a, sizeof(id));
    assert(rtl_usb_identity_resolve(0, &id) == RTL_USB_ID_OK);
    assert(id.valid == 1);
    assert(id.vid == 0x0bda && id.pid == 0x2838);
    assert(id.bus == 1);
    assert(id.port_depth == 2 && id.ports[0] == 1 && id.ports[1] == 2);
    assert(strcmp(id.manufacturer, "Realtek") == 0);
    assert(strcmp(id.product, "RTL2838UHIDIR") == 0);
    assert(strcmp(id.serial, "56435444") == 0);
    assert(rtl_usb_identity_index(&id) == 0);

    rtl_usb_identity other;
    assert(rtl_usb_identity_resolve(1, &other) == RTL_USB_ID_OK);
    assert(other.bus == 3 && strcmp(other.serial, "00000001") == 0);
    assert(rtl_usb_identity_index(&other) == 1);

    /* An index librtlsdr does not have. */
    assert(rtl_usb_identity_resolve(2, &other) == RTL_USB_ID_NOT_FOUND);
    assert(other.valid == 0);
    assert(rtl_usb_identity_resolve(-1, &other) == RTL_USB_ID_FAILED);
    assert(other.valid == 0);
    assert(total_resets() == 0);
}

void
duplicate_serial_refused(void) {
    /* Two dongles librtlsdr lists with the same triple. */
    reset_world();
    g_world.devs[1].serial = "56435444";
    rtl_usb_identity id;
    assert(rtl_usb_identity_resolve(0, &id) == RTL_USB_ID_DUPLICATE);
    assert(id.valid == 0);
    assert(rtl_usb_identity_resolve(1, &id) == RTL_USB_ID_DUPLICATE);
    assert(id.valid == 0);

    /* A readable twin of another VID that librtlsdr does not list still makes the triple ambiguous. */
    reset_world();
    fake_dev twin = make_dev(0x1d50, 0x604b, 2, 4, "56435444", 1);
    g_world.devs.push_back(twin);
    assert(rtl_usb_identity_resolve(0, &id) == RTL_USB_ID_DUPLICATE);
    assert(id.valid == 0);
    assert(total_resets() == 0);
}

void
unreadable_vendor_candidate_refused(void) {
    /* A vendor-specific device whose open fails with an I/O-style error might be a twin. */
    reset_world();
    fake_dev locked = make_dev(0x27c6, 0x5110, 1, 5, "", 1);
    locked.open_rc = RTL_USB_OP_ERROR;
    g_world.devs.push_back(locked);
    rtl_usb_identity id;
    assert(rtl_usb_identity_resolve(0, &id) == RTL_USB_ID_UNREADABLE);
    assert(id.valid == 0);

    /* One that opens but whose strings cannot be read. */
    g_world.devs.back().open_rc = RTL_USB_OP_OK;
    g_world.devs.back().read_rc = RTL_USB_OP_ERROR;
    assert(rtl_usb_identity_resolve(0, &id) == RTL_USB_ID_UNREADABLE);
    assert(id.valid == 0);

    /* Another librtlsdr index whose strings cannot be read. */
    reset_world();
    g_world.devs[1].rtl_read_rc = RTL_USB_OP_ERROR;
    assert(rtl_usb_identity_resolve(0, &id) == RTL_USB_ID_UNREADABLE);
    assert(id.valid == 0);

    /* The unreadable HID device in every world above is not a candidate. */
    reset_world();
    assert(rtl_usb_identity_resolve(0, &id) == RTL_USB_ID_OK);
    assert(total_resets() == 0);
}

/* A vendor-specific device this process may not open cannot be the dongle librtlsdr is about to open with the same
 * permissions, so it does not block the identity: a root-only fingerprint reader beside the dongles. */
void
access_denied_candidate_ignored(void) {
    reset_world();
    fake_dev locked = make_dev(0x27c6, 0x5110, 1, 5, "", 1);
    locked.open_rc = RTL_USB_OP_ACCESS;
    g_world.devs.push_back(locked);
    rtl_usb_identity id;
    assert(rtl_usb_identity_resolve(0, &id) == RTL_USB_ID_OK);
    assert(id.valid == 1 && id.bus == 1 && id.ports[1] == 2);
    assert(strcmp(id.serial, "56435444") == 0);
    assert(rtl_usb_identity_resolve(1, &id) == RTL_USB_ID_OK);
    assert(id.bus == 3);

    /* One whose configuration descriptor read is refused the same way. */
    fake_dev cfg_locked = make_dev(0x1a86, 0x7523, 1, 7, "", 1);
    cfg_locked.describe_rc = RTL_USB_OP_ACCESS;
    g_world.devs.push_back(cfg_locked);
    assert(rtl_usb_identity_resolve(0, &id) == RTL_USB_ID_OK);
    g_world.devs.back().describe_rc = RTL_USB_OP_UNSUPPORTED;
    assert(rtl_usb_identity_resolve(0, &id) == RTL_USB_ID_OK);
    /* Any other describe failure still refuses. */
    g_world.devs.back().describe_rc = RTL_USB_OP_ERROR;
    assert(rtl_usb_identity_resolve(0, &id) == RTL_USB_ID_UNREADABLE);
    g_world.devs.pop_back();

    /* The reset still re-reads the triple at the recorded port and touches nothing else. */
    id = resolve_a();
    assert(rtl_usb_identity_reset(&id, nullptr, nullptr) == RTL_USB_ID_OK);
    assert(g_world.devs[0].resets == 1 && total_resets() == 1);
    g_world.devs[0].serial = "99999999";
    assert(rtl_usb_identity_reset(&id, nullptr, nullptr) == RTL_USB_ID_MISMATCH);
    assert(total_resets() == 1);
}

/* The same for a device with no driver this process can open it through (Windows without WinUSB). */
void
not_supported_candidate_ignored(void) {
    reset_world();
    fake_dev wifi = make_dev(0x0bda, 0x8179, 1, 6, "00E04C0001", 1);
    wifi.open_rc = RTL_USB_OP_UNSUPPORTED;
    g_world.devs.push_back(wifi);
    rtl_usb_identity id;
    assert(rtl_usb_identity_resolve(0, &id) == RTL_USB_ID_OK);
    assert(id.valid == 1 && id.bus == 1 && strcmp(id.serial, "56435444") == 0);

    /* A device that opens but whose descriptors cannot be read still refuses. */
    g_world.devs.back().open_rc = RTL_USB_OP_OK;
    g_world.devs.back().read_rc = RTL_USB_OP_ERROR;
    assert(rtl_usb_identity_resolve(0, &id) == RTL_USB_ID_UNREADABLE);
    assert(id.valid == 0);
    assert(total_resets() == 0);
}

/* A receiver librtlsdr lists but this process may not open (a second dongle still on its Windows TV driver: librtlsdr
 * hands back libusb_open()'s ACCESS or NOT_SUPPORTED for its index) cannot be the running dongle, as for the libusb scan
 * above: the identity resolves, and the reopen finds the usable dongle's index wherever the refused one is listed. Any
 * other failure to read an index's strings still refuses both. */
void
refused_rtl_index_ignored(void) {
    for (const int refusal : {RTL_USB_OP_ACCESS, RTL_USB_OP_UNSUPPORTED}) {
        reset_world();
        fake_dev tv = make_dev(0x0bda, 0x2832, 1, 4, "00000002", 1);
        tv.open_rc = refusal;
        tv.rtl_read_rc = refusal;
        g_world.devs.push_back(tv); /* devs[3], after the HID device */
        g_world.rtl_order = {3, 0, 1};
        rtl_usb_identity id;
        assert(rtl_usb_identity_resolve(1, &id) == RTL_USB_ID_OK);
        assert(id.valid == 1 && id.bus == 1 && id.ports[1] == 2 && strcmp(id.serial, "56435444") == 0);
        assert(rtl_usb_identity_index(&id) == 1);
        /* Listed after the dongle once the receivers re-enumerate. */
        g_world.rtl_order = {0, 1, 3};
        assert(rtl_usb_identity_index(&id) == 0);
        assert(total_resets() == 0);
    }
    for (const int failure : {RTL_USB_OP_ERROR, RTL_USB_OP_GONE}) {
        reset_world();
        g_world.devs.push_back(make_dev(0x0bda, 0x2832, 1, 4, "00000002", 1));
        g_world.rtl_order = {3, 0, 1};
        rtl_usb_identity id;
        assert(rtl_usb_identity_resolve(1, &id) == RTL_USB_ID_OK);
        g_world.devs[3].rtl_read_rc = failure;
        assert(rtl_usb_identity_index(&id) == -1);
        rtl_usb_identity again;
        assert(rtl_usb_identity_resolve(1, &again) == RTL_USB_ID_UNREADABLE);
        assert(again.valid == 0);
    }
}

void
cross_vid_twin_with_active_unreadable_refused(void) {
    /* librtlsdr reads the running dongle, but libusb cannot; a twin of another VID reads fine. Taking the one
     * readable match would record the twin's port. */
    reset_world();
    g_world.devs[0].open_rc = RTL_USB_OP_ERROR;
    fake_dev twin = make_dev(0x1d50, 0x604b, 2, 4, "56435444", 1);
    g_world.devs.push_back(twin);
    rtl_usb_identity id;
    assert(rtl_usb_identity_resolve(0, &id) == RTL_USB_ID_UNREADABLE);
    assert(id.valid == 0);
    assert(rtl_usb_identity_index(&id) == -1);
    assert(rtl_usb_identity_reset(&id, nullptr, nullptr) == RTL_USB_ID_FAILED);
    assert(total_resets() == 0);

    /* The same with the running dongle's strings unreadable rather than its open. */
    g_world.devs[0].open_rc = RTL_USB_OP_OK;
    g_world.devs[0].read_rc = RTL_USB_OP_ERROR;
    assert(rtl_usb_identity_resolve(0, &id) == RTL_USB_ID_UNREADABLE);
    assert(id.valid == 0);
}

void
empty_strings_refused(void) {
    reset_world();
    g_world.devs[0].serial = "";
    rtl_usb_identity id;
    assert(rtl_usb_identity_resolve(0, &id) == RTL_USB_ID_NO_STRINGS);
    assert(id.valid == 0);

    /* librtlsdr cannot read the receiver's strings at all. */
    reset_world();
    g_world.devs[0].rtl_read_rc = RTL_USB_OP_ERROR;
    assert(rtl_usb_identity_resolve(0, &id) == RTL_USB_ID_UNREADABLE);
    assert(id.valid == 0);

    /* librtlsdr's triple matches no USB device (strings changed between the reads). */
    reset_world();
    g_world.devs[0].read_rc = RTL_USB_OP_OK;
    g_world.devs[0].info.vendor_specific = 0;
    assert(rtl_usb_identity_resolve(0, &id) == RTL_USB_ID_NOT_FOUND);
    assert(id.valid == 0);
}

void
handle_mismatch_detected(void) {
    reset_world();
    rtl_usb_identity id = resolve_a();
    auto* handle_a = reinterpret_cast<rtlsdr_dev_t*>(&g_world.devs[0]);
    auto* handle_b = reinterpret_cast<rtlsdr_dev_t*>(&g_world.devs[1]);
    assert(rtl_usb_identity_verify_handle(handle_a, &id) == RTL_USB_ID_OK);
    assert(rtl_usb_identity_verify_handle(handle_b, &id) == RTL_USB_ID_MISMATCH);

    /* A product string differs, same serial. */
    g_world.devs[1].serial = "56435444";
    g_world.devs[1].product = "Blog V4";
    assert(rtl_usb_identity_verify_handle(handle_b, &id) == RTL_USB_ID_MISMATCH);

    g_world.devs[0].rtl_read_rc = RTL_USB_OP_ERROR;
    assert(rtl_usb_identity_verify_handle(handle_a, &id) == RTL_USB_ID_UNREADABLE);
    assert(rtl_usb_identity_verify_handle(nullptr, &id) == RTL_USB_ID_FAILED);

    rtl_usb_identity invalid = id;
    invalid.valid = 0;
    g_world.devs[0].rtl_read_rc = 0;
    assert(rtl_usb_identity_verify_handle(handle_a, &invalid) == RTL_USB_ID_FAILED);
    assert(total_resets() == 0);
}

void
reset_refuses_replacement(void) {
    /* The running dongle in place: only it is reset. */
    reset_world();
    rtl_usb_identity id = resolve_a();
    assert(rtl_usb_identity_reset(&id, nullptr, nullptr) == RTL_USB_ID_OK);
    assert(g_world.devs[0].resets == 1);
    assert(total_resets() == 1);

    /* Another dongle (same VID:PID) now sits on its port: left alone. */
    reset_world();
    id = resolve_a();
    g_world.devs[0].serial = "99999999";
    assert(rtl_usb_identity_reset(&id, nullptr, nullptr) == RTL_USB_ID_MISMATCH);
    assert(total_resets() == 0);

    /* The bystander takes over the port path on another bus: not the recorded port. */
    reset_world();
    id = resolve_a();
    g_world.devs[0].hidden = -1;
    g_world.devs[1].serial = "56435444";
    g_world.devs[1].info.ports[1] = 2;
    assert(rtl_usb_identity_reset(&id, nullptr, nullptr) == RTL_USB_ID_NOT_FOUND);
    assert(total_resets() == 0);

    /* Same port, other VID:PID, same strings: not the recorded device. */
    reset_world();
    id = resolve_a();
    g_world.devs[0].info.pid = 0x2832;
    assert(rtl_usb_identity_reset(&id, nullptr, nullptr) == RTL_USB_ID_NOT_FOUND);
    assert(total_resets() == 0);

    /* Its strings cannot be read, or it cannot be opened. */
    reset_world();
    id = resolve_a();
    g_world.devs[0].read_rc = RTL_USB_OP_ERROR;
    assert(rtl_usb_identity_reset(&id, nullptr, nullptr) == RTL_USB_ID_UNREADABLE);
    g_world.devs[0].read_rc = RTL_USB_OP_OK;
    g_world.devs[0].open_rc = RTL_USB_OP_ACCESS;
    assert(rtl_usb_identity_reset(&id, nullptr, nullptr) == RTL_USB_ID_ACCESS);
    g_world.devs[0].open_rc = RTL_USB_OP_ERROR;
    assert(rtl_usb_identity_reset(&id, nullptr, nullptr) == RTL_USB_ID_FAILED);
    assert(total_resets() == 0);

    /* Cancelled on entry; cancelled after the strings were re-read, just before the reset; a failing reset. */
    reset_world();
    id = resolve_a();
    g_world.cancel_after_sessions = 0;
    assert(rtl_usb_identity_reset(&id, cancel_check, nullptr) == RTL_USB_ID_FAILED);
    assert(total_resets() == 0);
    g_world.cancel_after_sessions = g_world.sessions + 1;
    assert(rtl_usb_identity_reset(&id, cancel_check, nullptr) == RTL_USB_ID_FAILED);
    assert(total_resets() == 0);
    g_world.cancel_after_sessions = -1;
    g_world.devs[0].reset_rc = RTL_USB_OP_ERROR;
    assert(rtl_usb_identity_reset(&id, cancel_check, nullptr) == RTL_USB_ID_FAILED);
    assert(g_world.devs[0].resets == 1 && total_resets() == 1);

    rtl_usb_identity invalid = id;
    invalid.valid = 0;
    assert(rtl_usb_identity_reset(&invalid, nullptr, nullptr) == RTL_USB_ID_FAILED);
    assert(rtl_usb_identity_reset(nullptr, nullptr, nullptr) == RTL_USB_ID_FAILED);
    assert(total_resets() == 1);
}

void
reset_not_found_rediscovers(void) {
    /* The reset re-enumerates the dongle; it is back three enumerations later on another port. */
    reset_world();
    rtl_usb_identity id = resolve_a();
    g_world.devs[0].reset_rc = RTL_USB_OP_GONE;
    g_world.devs[0].gone_hidden = 3;
    g_world.devs[0].gone_port = 4;
    uint64_t start = g_world.now;
    assert(rtl_usb_identity_reset(&id, nullptr, nullptr) == RTL_USB_ID_OK);
    assert(g_world.devs[0].hidden == 0); /* looked for until it was back */
    assert(g_world.now - start < 5000);
    assert(g_world.devs[0].resets == 1 && total_resets() == 1);
    assert(rtl_usb_identity_index(&id) == 0);

    /* Back at once but unreadable for a while (permissions not yet applied): waited for. */
    reset_world();
    id = resolve_a();
    g_world.devs[0].reset_rc = RTL_USB_OP_GONE;
    g_world.devs[0].gone_deny = 4;
    assert(rtl_usb_identity_reset(&id, nullptr, nullptr) == RTL_USB_ID_OK);
    assert(g_world.devs[0].deny_opens == 0);

    /* Never back: NOT_FOUND once the bound passes, not before. */
    reset_world();
    id = resolve_a();
    g_world.devs[0].reset_rc = RTL_USB_OP_GONE;
    g_world.devs[0].gone_hidden = -1;
    start = g_world.now;
    assert(rtl_usb_identity_reset(&id, nullptr, nullptr) == RTL_USB_ID_NOT_FOUND);
    assert(g_world.now - start >= 5000);
    assert(g_world.now - start < 5500);
    assert(total_resets() == 1);

    /* Back but never readable: UNREADABLE at the bound. */
    reset_world();
    id = resolve_a();
    g_world.devs[0].reset_rc = RTL_USB_OP_GONE;
    g_world.devs[0].gone_deny = -1;
    start = g_world.now;
    assert(rtl_usb_identity_reset(&id, nullptr, nullptr) == RTL_USB_ID_UNREADABLE);
    assert(g_world.now - start >= 5000);
    assert(g_world.now - start < 5500);

    /* A twin shows up while waiting: refused at once. */
    reset_world();
    id = resolve_a();
    g_world.devs[0].reset_rc = RTL_USB_OP_GONE;
    g_world.devs[0].gone_hidden = 0;
    g_world.devs[1].serial = "56435444";
    assert(rtl_usb_identity_reset(&id, nullptr, nullptr) == RTL_USB_ID_DUPLICATE);
    assert(g_world.devs[1].resets == 0);

    /* Cancelled while waiting. */
    reset_world();
    id = resolve_a();
    g_world.devs[0].reset_rc = RTL_USB_OP_GONE;
    g_world.devs[0].gone_hidden = -1;
    g_world.cancel_after_sessions = 3;
    start = g_world.now;
    assert(rtl_usb_identity_reset(&id, cancel_check, nullptr) == RTL_USB_ID_NOT_FOUND);
    assert(g_world.now - start < 1000);
    assert(total_resets() == 1);
}

/* After a reset that re-enumerates, devices that refuse to open (or to give their configuration) and have another
 * VID:PID cannot be the dongle: the wait ends as soon as it is back and readable. */
void
rediscovery_ignores_refused_other_devices(void) {
    reset_world();
    fake_dev reader = make_dev(0x27c6, 0x5110, 1, 5, "", 1);
    reader.open_rc = RTL_USB_OP_ACCESS;
    g_world.devs.push_back(reader);
    fake_dev wifi = make_dev(0x0bda, 0x8179, 1, 6, "", 1);
    wifi.open_rc = RTL_USB_OP_UNSUPPORTED;
    g_world.devs.push_back(wifi);
    fake_dev cfg_locked = make_dev(0x1a86, 0x7523, 1, 7, "", 1);
    cfg_locked.describe_rc = RTL_USB_OP_ACCESS;
    g_world.devs.push_back(cfg_locked);
    rtl_usb_identity id = resolve_a();
    g_world.devs[0].reset_rc = RTL_USB_OP_GONE;
    g_world.devs[0].gone_hidden = 2;
    const uint64_t start = g_world.now;
    assert(rtl_usb_identity_reset(&id, nullptr, nullptr) == RTL_USB_ID_OK);
    assert(g_world.devs[0].hidden == 0);
    assert(g_world.now - start <= 300);
    assert(g_world.devs[0].resets == 1 && total_resets() == 1);

    /* Any other failure on such a device is still waited out, then reported. */
    reset_world();
    fake_dev flaky = make_dev(0x27c6, 0x5110, 1, 5, "", 1);
    flaky.open_rc = RTL_USB_OP_ERROR;
    id = resolve_a();
    g_world.devs.push_back(flaky);
    g_world.devs[0].reset_rc = RTL_USB_OP_GONE;
    assert(rtl_usb_identity_reset(&id, nullptr, nullptr) == RTL_USB_ID_UNREADABLE);
}

/* A refused device with the recorded VID:PID may be the dongle before udev has applied its permissions: waited for,
 * and the identity is found once it opens; one that never opens ends the wait as UNREADABLE at the bound. */
void
rediscovery_waits_for_refused_recorded_vidpid(void) {
    reset_world();
    rtl_usb_identity id = resolve_a();
    g_world.devs[0].reset_rc = RTL_USB_OP_GONE;
    g_world.devs[0].gone_port = 4;
    g_world.devs[0].gone_deny = 5;
    uint64_t start = g_world.now;
    assert(rtl_usb_identity_reset(&id, nullptr, nullptr) == RTL_USB_ID_OK);
    assert(g_world.devs[0].deny_opens == 0);
    assert(g_world.now - start >= 400 && g_world.now - start < 5000);

    /* A refused device with the recorded VID:PID that never opens, beside the readable dongle. */
    reset_world();
    id = resolve_a();
    fake_dev locked = make_dev(0x0bda, 0x2838, 2, 3, "", 1);
    locked.open_rc = RTL_USB_OP_ACCESS;
    g_world.devs.push_back(locked);
    g_world.devs[0].reset_rc = RTL_USB_OP_GONE;
    start = g_world.now;
    assert(rtl_usb_identity_reset(&id, nullptr, nullptr) == RTL_USB_ID_UNREADABLE);
    assert(g_world.now - start >= 5000 && g_world.now - start < 5500);

    /* The same with its configuration read refused. */
    reset_world();
    id = resolve_a();
    locked.open_rc = RTL_USB_OP_OK;
    locked.describe_rc = RTL_USB_OP_UNSUPPORTED;
    g_world.devs.push_back(locked);
    g_world.devs[0].reset_rc = RTL_USB_OP_GONE;
    assert(rtl_usb_identity_reset(&id, nullptr, nullptr) == RTL_USB_ID_UNREADABLE);
    assert(total_resets() == 1);
}

void
index_follows_identity_after_reorder(void) {
    reset_world();
    rtl_usb_identity id = resolve_a();
    assert(rtl_usb_identity_index(&id) == 0);

    /* After the reset librtlsdr lists the dongles the other way round. */
    g_world.rtl_order = {1, 0};
    assert(rtl_usb_identity_index(&id) == 1);

    /* Missing, duplicated, or another index unreadable: no index. */
    g_world.rtl_order = {1};
    assert(rtl_usb_identity_index(&id) == -1);
    g_world.rtl_order = {0, 1};
    g_world.devs[1].serial = "56435444";
    assert(rtl_usb_identity_index(&id) == -1);
    g_world.devs[1].serial = "00000001";
    g_world.devs[1].rtl_read_rc = RTL_USB_OP_ERROR;
    assert(rtl_usb_identity_index(&id) == -1);
    g_world.devs[1].rtl_read_rc = 0;
    assert(rtl_usb_identity_index(&id) == 0);

    rtl_usb_identity invalid = id;
    invalid.valid = 0;
    assert(rtl_usb_identity_index(&invalid) == -1);
    assert(rtl_usb_identity_index(nullptr) == -1);
}

#else

void
no_libusb_stub(void) {
    /* A world in which the receiver would resolve: the stub still reports no libusb and resets nothing. */
    reset_world();
    rtl_usb_identity id;
    DSD_MEMSET(&id, 0x5a, sizeof(id));
    assert(rtl_usb_identity_resolve(0, &id) == RTL_USB_ID_NO_LIBUSB);
    assert(id.valid == 0);
    id.valid = 1;
    copy_str(id.manufacturer, "Realtek");
    copy_str(id.product, "RTL2838UHIDIR");
    copy_str(id.serial, "56435444");
    assert(rtl_usb_identity_verify_handle(reinterpret_cast<rtlsdr_dev_t*>(&g_world.devs[0]), &id)
           == RTL_USB_ID_NO_LIBUSB);
    assert(rtl_usb_identity_reset(&id, nullptr, nullptr) == RTL_USB_ID_NO_LIBUSB);
    assert(rtl_usb_identity_index(&id) == -1);
    assert(total_resets() == 0);
    assert(g_world.sessions == 0);
}

#endif

void
off_reasons(void) {
    assert(strcmp(rtl_usb_identity_off_reason(RTL_USB_ID_DUPLICATE),
                  "another device has the same serial (set a unique one with rtl_eeprom)")
           == 0);
    assert(strcmp(rtl_usb_identity_off_reason(RTL_USB_ID_UNREADABLE),
                  "could not read the USB strings of a candidate device")
           == 0);
    assert(strcmp(rtl_usb_identity_off_reason(RTL_USB_ID_NO_STRINGS), "the receiver reports no serial") == 0);
    assert(strcmp(rtl_usb_identity_off_reason(RTL_USB_ID_NO_LIBUSB), "built without libusb") == 0);
    assert(strcmp(rtl_usb_identity_off_reason(RTL_USB_ID_OK), "") == 0);
    const rtl_usb_id_result others[] = {RTL_USB_ID_NOT_FOUND, RTL_USB_ID_MISMATCH, RTL_USB_ID_ACCESS,
                                        RTL_USB_ID_FAILED};
    for (rtl_usb_id_result r : others) {
        assert(rtl_usb_identity_off_reason(r)[0] != '\0');
    }
    assert(rtl_usb_identity_off_reason((rtl_usb_id_result)99)[0] != '\0');
}

} // namespace

int
main(void) {
#if defined(DSD_HAVE_LIBUSB) && defined(USE_RTLSDR)
    unique_match_resolves();
    duplicate_serial_refused();
    unreadable_vendor_candidate_refused();
    access_denied_candidate_ignored();
    not_supported_candidate_ignored();
    refused_rtl_index_ignored();
    cross_vid_twin_with_active_unreadable_refused();
    empty_strings_refused();
    handle_mismatch_detected();
    reset_refuses_replacement();
    reset_not_found_rediscovers();
    rediscovery_ignores_refused_other_devices();
    rediscovery_waits_for_refused_recorded_vidpid();
    index_follows_identity_after_reorder();
#else
    no_libusb_stub();
#endif
    off_reasons();
    rtl_usb_identity_set_driver_ops(nullptr);
    return 0;
}

// NOLINTEND(cert-dcl03-c,misc-static-assert)
