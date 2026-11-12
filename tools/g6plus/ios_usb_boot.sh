#!/bin/bash
# One unattended boot of the Moto G6 Plus with the iOS root served over the USB cable.
#   ios_usb_boot.sh BOOT_IMG ROOT_IMG SECS [LOG_PREFIX]
# BOOT_IMG    Enxerto boot image (BOARD=evert enxerto/build.sh PAYLOAD) whose kernel command line has
#             rd=usbroot0 usbrootrw=1 sweetusb=2 sweetselfreset=N (tools/g6plus/bootargs-ios16.txt); SECS should
#             be a little more than N: the kernel resets the SoC after N seconds and Android comes back.
# ROOT_IMG    the root image the Mac serves and iOS writes to (keep it between boots: iOS first-boot work
#             persists). Make a working copy of the pristine image first: cp -c PRISTINE ROOT_IMG.
# LOG_PREFIX  default /tmp/g6-ios: PREFIX.log = kernel console over USB, PREFIX.err = server notes,
#             PREFIX.watch = what the Mac's USB bus saw.
# Nothing is written to the phone: `fastboot boot` loads the image into RAM. Needs adb authorised
# ("always allow from this computer"), fastboot, libusb (brew install libusb).
# Keys and touches for the phone: tools/g6plus/g6input.py (UDP 127.0.0.1:${G6_INPUT_PORT:-5606}).
# Only one usbdisk_server.py may run at a time.
set -u
IMG=$1; ROOT=$2; SECS=$3; P=${4:-/tmp/g6-ios}
HERE=$(cd "$(dirname "$0")" && pwd)
[ -e "$IMG" ] && [ -e "$ROOT" ] || { echo "missing $IMG or $ROOT" >&2; exit 2; }
if pgrep -f "g6plus/usbdisk_server.py" >/dev/null; then echo "a usbdisk_server.py is already running" >&2; exit 2; fi
until adb devices | grep -qw device; do sleep 3; done
rm -f "$P.log"
python3 "$HERE/usbdisk_server.py" "$ROOT" --rw --input-port ${G6_INPUT_PORT:-5606} --log "$P.log" --secs $((SECS + 120)) > /dev/null 2> "$P.err" &
SRV=$!
"$HERE/usb_boot_watch.sh" "$IMG" "$SECS" > "$P.watch" 2>&1
kill $SRV 2>/dev/null
tail -3 "$P.err"
