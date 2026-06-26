// iphone_emu.c -- minimal "iPhone" USB peripheral emulation for DJI goggles.
//
// Purpose: DJI Goggles 3 only expose their video stream when they think they
// are connected to an Apple device. They start as the USB *host*, enumerate
// the attached device, and if they see Apple's VID:PID (05AC:12A8) they send a
// vendor control request (bmRequestType=0x40, bRequest=0x51) and then perform a
// USB host<->peripheral role switch. After the switch the goggles act as a
// peripheral and re-enumerate as 2ca3:1002 ("mobile/MFI" mode), which carries
// video.
//
// This program uses the Linux raw-gadget interface to present that Apple
// identity on a dual-role (OTG) UDC -- e.g. the Pi 4B USB-C port or the Pi
// Zero 2 W "USB" port. When it receives the 0x51 request it ACKs it and then
// flips the controller to host role via the usb_role switch sysfs node, so the
// kernel can enumerate the goggles in mobile mode.
//
// EXPERIMENTAL: the exact role-switch choreography is not publicly proven for
// DJI on a Pi. Treat the role-switch step as the part most likely to need
// iteration on real hardware. Run as root.
//
// Build:  gcc -O2 -Wall iphone_emu.c -o iphone_emu
// Usage:  sudo ./iphone_emu <udc-driver> <udc-device> [role-switch-path]
//   e.g.  sudo ./iphone_emu dwc2 fe980000.usb /sys/class/usb_role/fe980000.usb-role-switch/role
// (setup_pi.sh auto-detects these and calls this for you.)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/usb/ch9.h>

// ---- raw-gadget UAPI (inlined so this builds without kernel headers) -------
#ifndef UDC_NAME_LENGTH_MAX
#define UDC_NAME_LENGTH_MAX 128
#endif

struct usb_raw_init {
    __u8 driver_name[UDC_NAME_LENGTH_MAX];
    __u8 device_name[UDC_NAME_LENGTH_MAX];
    __u8 speed;
};

enum usb_raw_event_type {
    USB_RAW_EVENT_INVALID = 0,
    USB_RAW_EVENT_CONNECT = 1,
    USB_RAW_EVENT_CONTROL = 2,
    USB_RAW_EVENT_SUSPEND = 3,
    USB_RAW_EVENT_RESUME  = 4,
    USB_RAW_EVENT_RESET   = 5,
    USB_RAW_EVENT_DISCONNECT = 6,
};

struct usb_raw_event {
    __u32 type;
    __u32 length;
    __u8  data[0];
};

struct usb_raw_ep_io {
    __u16 ep;
    __u16 flags;
    __u32 length;
    __u8  data[0];
};

#define USB_RAW_IOCTL_INIT          _IOW('U', 0, struct usb_raw_init)
#define USB_RAW_IOCTL_RUN           _IO('U', 1)
#define USB_RAW_IOCTL_EVENT_FETCH   _IOR('U', 2, struct usb_raw_event)
#define USB_RAW_IOCTL_EP0_WRITE     _IOW('U', 3, struct usb_raw_ep_io)
#define USB_RAW_IOCTL_EP0_READ      _IOWR('U', 4, struct usb_raw_ep_io)
#define USB_RAW_IOCTL_CONFIGURE     _IO('U', 9)
#define USB_RAW_IOCTL_VBUS_DRAW     _IOW('U', 10, __u32)
#define USB_RAW_IOCTL_EP0_STALL     _IO('U', 12)

// ---- Apple "iPhone" descriptors -------------------------------------------
// The goggles key off VID:PID only, so a minimal but valid device works.
#define APPLE_VID 0x05AC
#define APPLE_PID 0x12A8

static struct usb_device_descriptor dev_desc = {
    .bLength            = USB_DT_DEVICE_SIZE,
    .bDescriptorType    = USB_DT_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0,
    .bDeviceSubClass    = 0,
    .bDeviceProtocol    = 0,
    .bMaxPacketSize0    = 64,
    .idVendor           = APPLE_VID,
    .idProduct          = APPLE_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 1,
    .iProduct           = 2,
    .iSerialNumber      = 3,
    .bNumConfigurations = 1,
};

struct full_config {
    struct usb_config_descriptor    config;
    struct usb_interface_descriptor iface;
} __attribute__((packed));

static struct full_config cfg_desc = {
    .config = {
        .bLength             = USB_DT_CONFIG_SIZE,
        .bDescriptorType     = USB_DT_CONFIG,
        .wTotalLength        = sizeof(struct full_config),
        .bNumInterfaces      = 1,
        .bConfigurationValue = 1,
        .iConfiguration      = 0,
        .bmAttributes        = USB_CONFIG_ATT_ONE | USB_CONFIG_ATT_SELFPOWER,
        .bMaxPower           = 250, // 500 mA
    },
    .iface = {
        .bLength            = USB_DT_INTERFACE_SIZE,
        .bDescriptorType    = USB_DT_INTERFACE,
        .bInterfaceNumber   = 0,
        .bAlternateSetting  = 0,
        .bNumEndpoints      = 0,
        .bInterfaceClass    = 0xFF, // vendor specific (iAP-like)
        .bInterfaceSubClass = 0xF0,
        .bInterfaceProtocol = 0x00,
        .iInterface         = 4,
    },
};

// USB string descriptors.
static const char* strings[] = {
    /* 1 */ "Apple Inc.",
    /* 2 */ "iPhone",
    /* 3 */ "000000000000000000000000000000000000000",
    /* 4 */ "iAP Interface",
};

static const char* g_role_switch_path = NULL;
static volatile int g_got_magic = 0;

static int build_string_desc(int idx, uint8_t* out) {
    if (idx == 0) { // LANGID
        out[0] = 4; out[1] = USB_DT_STRING; out[2] = 0x09; out[3] = 0x04;
        return 4;
    }
    int n = (int)(sizeof(strings) / sizeof(strings[0]));
    if (idx < 1 || idx > n) return -1;
    const char* s = strings[idx - 1];
    int len = (int)strlen(s);
    out[0] = (uint8_t)(2 + len * 2);
    out[1] = USB_DT_STRING;
    for (int i = 0; i < len; i++) { out[2 + i * 2] = (uint8_t)s[i]; out[3 + i * 2] = 0; }
    return out[0];
}

static int usb_raw_open(void) {
    int fd = open("/dev/raw-gadget", O_RDWR);
    if (fd < 0) { perror("open(/dev/raw-gadget) -- is the raw_gadget module loaded?"); exit(1); }
    return fd;
}

static void usb_raw_init(int fd, const char* driver, const char* device) {
    struct usb_raw_init arg;
    memset(&arg, 0, sizeof(arg));
    strncpy((char*)arg.driver_name, driver, UDC_NAME_LENGTH_MAX - 1);
    strncpy((char*)arg.device_name, device, UDC_NAME_LENGTH_MAX - 1);
    arg.speed = USB_SPEED_HIGH;
    if (ioctl(fd, USB_RAW_IOCTL_INIT, &arg) < 0) { perror("USB_RAW_IOCTL_INIT"); exit(1); }
}

static void ep0_write(int fd, const void* data, uint32_t len) {
    uint8_t buf[1024];
    struct usb_raw_ep_io* io = (struct usb_raw_ep_io*)buf;
    io->ep = 0; io->flags = 0; io->length = len;
    if (len) memcpy(io->data, data, len);
    if (ioctl(fd, USB_RAW_IOCTL_EP0_WRITE, io) < 0) perror("EP0_WRITE");
}

static void ep0_stall(int fd) { ioctl(fd, USB_RAW_IOCTL_EP0_STALL, 0); }

static void trigger_role_switch(void) {
    if (!g_role_switch_path) {
        fprintf(stderr, "[!] no role-switch path given; flip to host manually now\n");
        return;
    }
    fprintf(stderr, "[*] got 0x51 -- switching %s to host\n", g_role_switch_path);
    int rf = open(g_role_switch_path, O_WRONLY);
    if (rf < 0) { perror("open(role switch)"); return; }
    if (write(rf, "host\n", 5) < 0) perror("write(role=host)");
    close(rf);
}

static void handle_control(int fd, struct usb_ctrlrequest* ctrl) {
    int in = (ctrl->bRequestType & USB_DIR_IN) != 0;
    int type = ctrl->bRequestType & USB_TYPE_MASK;

    if (type == USB_TYPE_VENDOR) {
        // The DJI "I see an Apple device" trigger.
        if (ctrl->bRequest == 0x51) {
            ep0_write(fd, NULL, 0);   // ACK
            g_got_magic = 1;
            trigger_role_switch();
            return;
        }
        ep0_write(fd, NULL, 0);       // ACK other vendor pokes
        return;
    }

    if (type == USB_TYPE_STANDARD) {
        switch (ctrl->bRequest) {
        case USB_REQ_GET_DESCRIPTOR: {
            int dt = ctrl->wValue >> 8;
            int di = ctrl->wValue & 0xff;
            uint8_t sbuf[256];
            switch (dt) {
            case USB_DT_DEVICE:
                ep0_write(fd, &dev_desc,
                          ctrl->wLength < USB_DT_DEVICE_SIZE ? ctrl->wLength : USB_DT_DEVICE_SIZE);
                return;
            case USB_DT_CONFIG: {
                uint32_t len = sizeof(cfg_desc);
                ep0_write(fd, &cfg_desc, ctrl->wLength < len ? ctrl->wLength : len);
                return;
            }
            case USB_DT_STRING: {
                int n = build_string_desc(di, sbuf);
                if (n < 0) { ep0_stall(fd); return; }
                ep0_write(fd, sbuf, (uint32_t)(ctrl->wLength < (uint32_t)n ? ctrl->wLength : n));
                return;
            }
            default:
                ep0_stall(fd);
                return;
            }
        }
        case USB_REQ_SET_CONFIGURATION:
            ioctl(fd, USB_RAW_IOCTL_CONFIGURE, 0);
            ep0_write(fd, NULL, 0);
            return;
        case USB_REQ_GET_STATUS: {
            uint16_t st = 1; // self powered
            ep0_write(fd, &st, ctrl->wLength < 2 ? ctrl->wLength : 2);
            return;
        }
        case USB_REQ_SET_INTERFACE:
            ep0_write(fd, NULL, 0);
            return;
        default:
            if (in) ep0_stall(fd); else ep0_write(fd, NULL, 0);
            return;
        }
    }

    // Class or anything else: ACK if OUT, stall if IN.
    if (in) ep0_stall(fd); else ep0_write(fd, NULL, 0);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <udc-driver> <udc-device> [role-switch-path]\n", argv[0]);
        fprintf(stderr, "  e.g. %s dwc2 fe980000.usb /sys/class/usb_role/fe980000.usb-role-switch/role\n", argv[0]);
        return 2;
    }
    const char* driver = argv[1];
    const char* device = argv[2];
    if (argc >= 4) g_role_switch_path = argv[3];

    int fd = usb_raw_open();
    usb_raw_init(fd, driver, device);
    if (ioctl(fd, USB_RAW_IOCTL_RUN, 0) < 0) { perror("USB_RAW_IOCTL_RUN"); return 1; }
    fprintf(stderr, "[*] presenting %04x:%04x on %s/%s -- connect the goggles now\n",
            APPLE_VID, APPLE_PID, driver, device);

    uint8_t ev_buf[sizeof(struct usb_raw_event) + sizeof(struct usb_ctrlrequest)];
    while (1) {
        struct usb_raw_event* ev = (struct usb_raw_event*)ev_buf;
        ev->type = 0;
        ev->length = sizeof(struct usb_ctrlrequest);
        if (ioctl(fd, USB_RAW_IOCTL_EVENT_FETCH, ev) < 0) {
            if (errno == EINTR) continue;
            perror("EVENT_FETCH");
            break;
        }
        switch (ev->type) {
        case USB_RAW_EVENT_CONNECT:
            fprintf(stderr, "[*] connect\n");
            break;
        case USB_RAW_EVENT_CONTROL:
            handle_control(fd, (struct usb_ctrlrequest*)ev->data);
            break;
        case USB_RAW_EVENT_RESET:
            fprintf(stderr, "[*] reset\n");
            break;
        case USB_RAW_EVENT_DISCONNECT:
            fprintf(stderr, "[*] disconnect\n");
            if (g_got_magic) {
                // Expected: after the role switch the gadget side drops. Exit so
                // the wrapper can take over as host and enumerate the goggles.
                fprintf(stderr, "[*] magic already received -- exiting for host takeover\n");
                close(fd);
                return 0;
            }
            break;
        default:
            break;
        }
    }
    close(fd);
    return 0;
}
