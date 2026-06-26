#!/usr/bin/env bash
# run.sh -- orchestrate the iPhone emulation -> role switch -> video capture.
#
# Sequence:
#   1. Start as a USB peripheral pretending to be an iPhone (iphone_emu).
#   2. Goggles enumerate us, send the 0x51 vendor request; we ACK and flip the
#      controller to host role.
#   3. Wait for the goggles to re-enumerate as 2ca3:1002 (mobile mode).
#   4. Run fpvLiberator (built in mobile mode) and restream the H.264 over TCP.
#
# Run with sudo. Configure via env (setup_pi.sh prints these):
#   UDC_DRIVER  (default dwc2)
#   UDC_DEV     (e.g. fe980000.usb)
#   ROLE_SWITCH (e.g. /sys/class/usb_role/fe980000.usb-role-switch/role)
#   PORT        TCP port to serve the stream on (default 5000)
set -uo pipefail

if [[ $EUID -ne 0 ]]; then echo "run with sudo"; exit 1; fi

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"

UDC_DRIVER="${UDC_DRIVER:-dwc2}"
UDC_DEV="${UDC_DEV:-$(ls /sys/class/udc 2>/dev/null | head -n1)}"
if [[ -z "${ROLE_SWITCH:-}" ]]; then
    for r in /sys/class/usb_role/*/role; do [[ -e "$r" ]] && ROLE_SWITCH="$r" && break; done
fi
PORT="${PORT:-5000}"

if [[ -z "$UDC_DEV" ]]; then echo "[!] no UDC found; run setup_pi.sh first (and reboot)"; exit 1; fi
echo "[*] UDC=$UDC_DRIVER/$UDC_DEV  role-switch=${ROLE_SWITCH:-<none>}  port=$PORT"

modprobe raw_gadget 2>/dev/null || true

# Make sure we start in peripheral role so the goggles (host) can connect.
if [[ -n "${ROLE_SWITCH:-}" ]]; then echo device > "$ROLE_SWITCH" 2>/dev/null || true; fi

# Build the capture binary (mobile mode is the default).
if [[ ! -x "$REPO/build/fpvLiberator" ]]; then
    echo "[*] building fpvLiberator (mobile mode)"
    cmake -S "$REPO" -B "$REPO/build" >/dev/null
    cmake --build "$REPO/build" >/dev/null
fi

echo "[*] starting iPhone emulation -- connect the goggles to the OTG port now"
"$HERE/iphone_emu" "$UDC_DRIVER" "$UDC_DEV" "${ROLE_SWITCH:-}"
echo "[*] emulator exited (role switch should have occurred)"

# Wait for the goggles to come up as host-side device 2ca3:1002.
echo "[*] waiting for 2ca3:1002 to enumerate..."
for i in $(seq 1 20); do
    if lsusb | grep -qi '2ca3:1002'; then echo "[*] goggles in mobile mode!"; break; fi
    sleep 0.5
done
if ! lsusb | grep -qi '2ca3:1002'; then
    echo "[!] 2ca3:1002 did not appear. Check role switch / cabling."
    echo "    Current 2ca3 devices:"; lsusb | grep -i 2ca3 || echo "    (none)"
    exit 1
fi

echo "[*] capturing + serving H.264 on tcp/$PORT"
echo "    view from another machine:"
echo "      nc <pi-ip> $PORT | ffplay -fflags nobuffer -flags low_delay -f h264 -i -"
while true; do
    "$REPO/build/fpvLiberator" | nc -l -p "$PORT" || true
    echo "[*] client disconnected; waiting for next connection"
    sleep 1
done
