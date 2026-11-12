#!/bin/bash
# One unattended test boot of the Moto G6 Plus that watches the Mac's USB bus for the kernel's gadget.
#   usb_boot_watch.sh IMAGE [SECONDS]
# IMAGE must carry sweetselfreset=N (xnu patch 1036) so the phone returns to Android by itself.
# Prints every new USB device / network interface seen while the kernel runs. RAM only.
set -u
IMG=$1; WATCH=${2:-70}
adb devices | grep -qw device || { echo "no adb device"; exit 2; }
before_if=$(ifconfig -l)
adb reboot bootloader
for i in $(seq 1 30); do sleep 2; fastboot devices | grep -q fastboot && break; done
fastboot devices | grep -q fastboot || { echo "no fastboot device"; exit 2; }
fastboot boot "$IMG" 2>&1 | tail -2
seen=""
end=$((SECONDS + WATCH))
while [ $SECONDS -lt $end ]; do
	now=$(ioreg -p IOUSB -w0 | sed -n 's/^[ |+o-]*\(.*\)@.*/\1/p' | sort -u | tr '\n' ';')
	if [ "$now" != "$seen" ]; then echo "t=$SECONDS usb: $now"; seen=$now; fi
	sleep 1
done
echo "interfaces before: $before_if"
echo "interfaces now:    $(ifconfig -l)"
system_profiler SPUSBDataType 2>/dev/null | grep -i -B2 -A12 "sweet" | head -40
for i in $(seq 1 90); do adb devices | grep -qw device && { echo "adb back"; exit 0; }; sleep 2; done
echo "adb did not come back"; exit 1
