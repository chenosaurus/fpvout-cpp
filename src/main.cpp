#include <vector>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <unistd.h>
#include <libusb.h>

using namespace std;

// ---- Device config -------------------------------------------------------
// Two USB personalities for DJI Goggles 3:
//
//   * Mobile / MFI mode (FPV_MOBILE_MODE=1, the default): the goggles enumerate
//     as 2ca3:1002 with a single muxed interface "com.dji.logiclink"
//     (interface 1, alt setting 1, EP 0x02 OUT / 0x82 IN). This mode carries
//     the video stream and is the only one proven to work. Reaching it requires
//     hardware that spoofs an iPhone USB role switch (e.g. a Cynthion running
//     samuelsadok/dji_protocol's emulate_iphone.py).
//
//   * PC mode (FPV_MOBILE_MODE=0): the goggles enumerate as 2ca3:0020 with
//     vendor "BULK Interface" descriptors. Only interface 4 (EP 0x04 OUT /
//     0x85 IN) accepts writes, but it only returns a DUML status heartbeat and
//     never the video port. Kept for diagnostics only -- it does NOT produce
//     video. Build with: cmake -S . -B build -DMOBILE_MODE=OFF
//
// Override the mode at build time via the CMake option MOBILE_MODE.
#ifndef FPV_MOBILE_MODE
#define FPV_MOBILE_MODE 1
#endif

#if FPV_MOBILE_MODE
static const uint16_t VID         = 0x2ca3;
static const uint16_t PID         = 0x1002;
static const int      INTERFACE   = 1;
static const int      ALT_SETTING = 1;
static const uint8_t  EP_OUT      = 0x02;
static const uint8_t  EP_IN       = 0x82;
#else
static const uint16_t VID         = 0x2ca3;
static const uint16_t PID         = 0x0020;
static const int      INTERFACE   = 4;
static const int      ALT_SETTING = -1; // no alternate setting needed
static const uint8_t  EP_OUT      = 0x04;
static const uint8_t  EP_IN       = 0x85;
#endif

// Video stream encapsulation port (little-endian 0x574A == 22346).
static const uint8_t  VIDEO_PORT_LO = 0x4A;
static const uint8_t  VIDEO_PORT_HI = 0x57;

// ---- DUML "start video" commands for DJI Goggles N3 ----------------------
// Source: samuelsadok/dji_protocol (usb_mobile_protocol.md), verified on the
// Goggles N3. These already include the 0x55 0xCC USB encapsulation header,
// so they are sent to the bulk OUT endpoint verbatim. Re-sent periodically as
// a keep-alive (the stream otherwise stops after ~11 seconds).
static const uint8_t START1[] = {
    0x55,0xcc,0x49,0x57,0x2d,0x00,0x00,0x00,
    0x55,0x2d,0x04,0xf2,0x02,0x28,0xf3,0xfe,0x40,0x00,0x99,
    0x02,0x02,0x00,0x00,0xd5,0x07,0x00,0x00,0x00,0x00,0x00,0x13,0x00,0x0d,0x00,
    0x63,0x61,0x6d,0x63,0x61,0x70,0x5f,0x63,0x6f,0x6d,0x6d,0x6f,0x6e,0x00,0x00,0x00,0x00,0xd0,0x93,
    0x92,0x3a
};
static const uint8_t START2[] = {
    0x55,0xcc,0x49,0x57,0x1b,0x00,0x00,0x00,
    0x55,0x1b,0x04,0x75,0x02,0x3c,0xf4,0xfe,0x40,0x00,0x88,
    0x17,0x00,0x00,0x23,0x00,0x41,0x50,0x50,0x00,0x00,0x00,0x00,0x00,0x02,0x58,0xa6,
    0x34,0x18
};

static int fail(const char* msg, int code) {
    fprintf(stderr, "%s %d (%s)\n", msg, code, libusb_strerror((libusb_error)code));
    return code;
}

static void send_start(libusb_device_handle* dh) {
    int tx = 0;
    int r1 = libusb_bulk_transfer(dh, EP_OUT, (unsigned char*)START1, sizeof(START1), &tx, 1000);
    int r2 = libusb_bulk_transfer(dh, EP_OUT, (unsigned char*)START2, sizeof(START2), &tx, 1000);
    if (r1 < 0 || r2 < 0)
        fprintf(stderr, "start cmd write failed: %d / %d\n", r1, r2);
}

int main() {
    int err = 0;
    fprintf(stderr, "initializing\n");
    if ((err = libusb_init(NULL)) < 0) return fail("initialize fail", err);

    fprintf(stderr, "opening device %04x:%04x\n", VID, PID);
    libusb_device_handle* dh = libusb_open_device_with_vid_pid(NULL, VID, PID);
    if (!dh) return fail("open device fail", 1);

    libusb_set_auto_detach_kernel_driver(dh, 1);

    fprintf(stderr, "claiming interface %d\n", INTERFACE);
    if ((err = libusb_claim_interface(dh, INTERFACE)) < 0)
        return fail("claim interface fail", err);

    if (ALT_SETTING >= 0) {
        fprintf(stderr, "selecting alt setting %d\n", ALT_SETTING);
        if ((err = libusb_set_interface_alt_setting(dh, INTERFACE, ALT_SETTING)) < 0)
            return fail("set alt setting fail", err);
    }

    fprintf(stderr, "sending start commands to EP 0x%02x\n", EP_OUT);
    send_start(dh);

    // Rolling buffer to reassemble encapsulation packets that span transfers.
    vector<uint8_t> acc;
    acc.reserve(1 << 20);
    vector<uint8_t> in(65536);

    auto last_ka  = chrono::steady_clock::now();
    auto last_log = chrono::steady_clock::now();
    long long video_bytes = 0;
    bool seen_video = false;

    while (true) {
        int rx = 0;
        int r = libusb_bulk_transfer(dh, EP_IN, in.data(), (int)in.size(), &rx, 500);
        if (r < 0 && r != LIBUSB_ERROR_TIMEOUT) {
            fail("read error", r);
            break;
        }
        if (rx > 0)
            acc.insert(acc.end(), in.begin(), in.begin() + rx);

        // Parse complete encapsulation packets: 55 cc | port(2) | len(2) | 00 00 | payload(len)
        size_t off = 0;
        while (acc.size() - off >= 8) {
            if (acc[off] != 0x55 || acc[off + 1] != 0xcc) {
                // Not aligned to a header; skip a byte and resync.
                off++;
                continue;
            }
            uint16_t len = (uint16_t)acc[off + 4] | ((uint16_t)acc[off + 5] << 8);
            size_t total = 8 + (size_t)len;
            if (acc.size() - off < total) break; // wait for the rest

            bool is_video = (acc[off + 2] == VIDEO_PORT_LO && acc[off + 3] == VIDEO_PORT_HI);
            if (is_video && len > 0) {
                fwrite(acc.data() + off + 8, 1, len, stdout);
                fflush(stdout);
                video_bytes += len;
                if (!seen_video) {
                    fprintf(stderr, "video stream started (port 0x%02x%02x)\n",
                            VIDEO_PORT_HI, VIDEO_PORT_LO);
                    seen_video = true;
                }
            } else if (!seen_video) {
                // Diagnostic: report other ports seen until video shows up.
                fprintf(stderr, "rx packet port=0x%02x%02x len=%u\n",
                        acc[off + 3], acc[off + 2], len);
            }
            off += total;
        }
        if (off > 0) acc.erase(acc.begin(), acc.begin() + off);

        auto nowt = chrono::steady_clock::now();
        if (chrono::duration_cast<chrono::seconds>(nowt - last_ka).count() >= 5) {
            send_start(dh);
            last_ka = nowt;
        }
        if (chrono::duration_cast<chrono::seconds>(nowt - last_log).count() >= 2) {
            double secs = chrono::duration<double>(nowt - last_log).count();
            fprintf(stderr, "rx %.1f kb/s\n", (video_bytes / secs) / 1000.0);
            video_bytes = 0;
            last_log = nowt;
        }
    }

    fprintf(stderr, "closing\n");
    libusb_release_interface(dh, INTERFACE);
    libusb_close(dh);
    libusb_exit(NULL);
    return 0;
}
