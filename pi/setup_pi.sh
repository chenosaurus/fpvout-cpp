#!/usr/bin/env bash
# setup_pi.sh -- one-time prep on a Raspberry Pi (4B or Zero 2 W) to enable the
# dual-role USB controller and raw-gadget so we can emulate an iPhone and then
# switch to host to talk to the DJI goggles.
#
# Run with sudo. A reboot is required after the first run (to apply the dwc2
# overlay). Re-run after reboot to verify and build the emulator.
set -euo pipefail

if [[ $EUID -ne 0 ]]; then echo "run with sudo"; exit 1; fi

# --- locate config.txt (new vs old Raspberry Pi OS layout) ------------------
CONFIG=""
for c in /boot/firmware/config.txt /boot/config.txt; do
    [[ -f "$c" ]] && CONFIG="$c" && break
done
if [[ -z "$CONFIG" ]]; then echo "could not find config.txt"; exit 1; fi
echo "[*] using $CONFIG"

# --- enable dwc2 in dual-role (otg) mode ------------------------------------
if ! grep -q '^dtoverlay=dwc2,dr_mode=otg' "$CONFIG"; then
    # Remove any other dwc2 overlay lines, then append ours.
    sed -i '/^dtoverlay=dwc2/d' "$CONFIG"
    echo 'dtoverlay=dwc2,dr_mode=otg' >> "$CONFIG"
    echo "[*] added dwc2 otg overlay -- REBOOT REQUIRED, then re-run this script"
    NEED_REBOOT=1
else
    echo "[*] dwc2 otg overlay already present"
    NEED_REBOOT=0
fi

# --- load modules -----------------------------------------------------------
modprobe dwc2 2>/dev/null || true
if ! modprobe raw_gadget 2>/dev/null; then
    echo "[!] raw_gadget module not available in this kernel."
    echo "    Check: zcat /proc/config.gz | grep RAW_GADGET   (need CONFIG_USB_RAW_GADGET=m or =y)"
    echo "    On most recent Raspberry Pi OS it is built as a module."
fi

if [[ "${NEED_REBOOT:-0}" -eq 1 ]]; then
    echo "[*] reboot now: sudo reboot"
    exit 0
fi

# --- discover the UDC and role-switch nodes ---------------------------------
UDC_DEV="$(ls /sys/class/udc 2>/dev/null | head -n1 || true)"
if [[ -z "$UDC_DEV" ]]; then
    echo "[!] no UDC found in /sys/class/udc."
    echo "    The dwc2 overlay may not be active yet (did you reboot?), or the"
    echo "    USB data port is cabled to a host. Plug the goggles into the OTG"
    echo "    port (Pi 4B: USB-C; Zero 2 W: the inner 'USB' micro port)."
    exit 1
fi
UDC_DRIVER="$(cat "/sys/class/udc/$UDC_DEV/uevent" 2>/dev/null | sed -n 's/^USB_UDC_DRIVER=//p' | head -n1)"
[[ -z "$UDC_DRIVER" ]] && UDC_DRIVER="dwc2"

ROLE_SWITCH=""
for r in /sys/class/usb_role/*/role; do
    [[ -e "$r" ]] && ROLE_SWITCH="$r" && break
done

echo
echo "[*] detected:"
echo "      UDC driver : $UDC_DRIVER"
echo "      UDC device : $UDC_DEV"
echo "      role switch: ${ROLE_SWITCH:-<none found>}"
echo
echo "export these for run.sh:"
echo "      export UDC_DRIVER='$UDC_DRIVER'"
echo "      export UDC_DEV='$UDC_DEV'"
echo "      export ROLE_SWITCH='${ROLE_SWITCH:-}'"

# --- build the emulator -----------------------------------------------------
HERE="$(cd "$(dirname "$0")" && pwd)"
echo
echo "[*] building iphone_emu"
gcc -O2 -Wall "$HERE/iphone_emu.c" -o "$HERE/iphone_emu"
echo "[*] done. next: sudo $HERE/run.sh"
