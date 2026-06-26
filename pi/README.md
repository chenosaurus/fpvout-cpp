# DJI Goggles 3 video-out via a Raspberry Pi (experimental)

DJI Goggles 3 only stream video over USB when they believe they are connected
to an **iPhone**. Plugged into a normal PC they fall back to "PC mode"
(`2ca3:0020`), which exposes a DUML control channel but **no video**. To get
video you must make the goggles enter "mobile/MFI mode" (`2ca3:1002`), which
requires:

1. Presenting Apple's USB identity (`05AC:12A8`) as a **USB peripheral**.
2. ACKing the goggles' vendor request `bmRequestType=0x40, bRequest=0x51`.
3. Performing a **host<->peripheral role switch** so we become the host.
4. Enumerating the goggles as `2ca3:1002` and pulling the (encapsulated) video.

A Raspberry Pi with a dual-role (OTG) USB port can do this. This is **not a
publicly proven recipe for DJI on a Pi** — the role-switch step (3) is the part
most likely to need iteration on real hardware.

## Hardware / wiring

| Board | OTG (data) port — connect goggles here | Power separately via |
|-------|----------------------------------------|----------------------|
| **Pi 4B** | USB-C | GPIO 5V (pins 4 & 6) or PoE HAT |
| **Pi Zero 2 W** | inner **USB** micro port | **PWR IN** micro port |

The goggles must go to the **OTG** port. The Pi 4B's 4× USB-A ports are
host-only and will only ever give you the video-less PC mode.

## Steps

```bash
# 1. one-time setup (enables dwc2 OTG; reboot when it tells you to)
sudo pi/setup_pi.sh
sudo reboot
# after reboot, run again to detect nodes + build the emulator
sudo pi/setup_pi.sh

# 2. export the values it printed, then run
export UDC_DRIVER=dwc2
export UDC_DEV=fe980000.usb
export ROLE_SWITCH=/sys/class/usb_role/fe980000.usb-role-switch/role
sudo -E pi/run.sh
```

`run.sh` will:
- start the iPhone emulation,
- wait for you to connect the goggles to the OTG port,
- ACK the `0x51` request and flip to host role,
- wait for `2ca3:1002` to appear,
- run `fpvLiberator` and serve the H.264 stream on TCP `5000`.

View it from another machine:

```bash
nc <pi-ip> 5000 | ffplay -fflags nobuffer -flags low_delay -f h264 -i -
```

## Requirements on the Pi

- `gcc`, `cmake`, `pkg-config`, `libusb-1.0-0-dev`, `netcat`
  ```bash
  sudo apt install -y build-essential cmake pkg-config libusb-1.0-0-dev netcat-openbsd
  ```
- Kernel with `CONFIG_USB_RAW_GADGET` (module `raw_gadget`). Verify:
  ```bash
  zcat /proc/config.gz | grep RAW_GADGET
  ```

## Troubleshooting / iteration notes

- **No UDC in `/sys/class/udc`** → the dwc2 overlay isn't active (reboot) or the
  goggles are on a host-only port.
- **`0x51` never arrives** → the goggles didn't accept our Apple identity. Try
  tweaking the descriptors in `iphone_emu.c` (some firmwares want the iAP
  interface populated, or `05AC:12AB`).
- **`0x51` arrives but `2ca3:1002` never appears** → this is the hard part: the
  role switch. Inspect `dmesg` right after the switch. The dwc2 controller may
  need a different role-switch mechanism than the `usb_role` sysfs node, or the
  goggles may expect tighter timing. This is the main thing to iterate on with
  the hardware connected — capture `dmesg -w` and we can adjust.

## Credits

Protocol details from
[samuelsadok/dji_protocol](https://github.com/samuelsadok/dji_protocol) and the
[fpv-wtf](https://github.com/fpv-wtf/voc-poc) / [FPV Out Club](https://github.com/fpvout)
projects.
