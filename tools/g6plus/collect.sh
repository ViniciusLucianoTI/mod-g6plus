#!/bin/sh
# Collect Moto G6 Plus (evert) hardware facts over adb. READ-ONLY on the phone.
#
#   tools/g6plus/collect.sh            facts only (needs adb root: TWRP/OrangeFox, or rooted Android)
#   tools/g6plus/collect.sh --backup   also dd every partition except the big OS/user ones
#
# Raw output goes to backup/evert/<date>/ (git-ignored: it holds serials, IMEI, calibration).
# summary.txt there has serial numbers redacted and is the file to read/quote in NOTES.md.
# The running device tree goes to dtb/evert/live.dtb (also git-ignored); pass it to
# tools/extract-dtb.py --live together with the stock boot.img.
set -eu
cd "$(dirname "$0")/../.."

command -v adb >/dev/null || { echo "adb not found: brew install --cask android-platform-tools" >&2; exit 1; }
[ "$(adb get-state 2>/dev/null)" = device ] || [ "$(adb get-state 2>/dev/null)" = recovery ] || {
    echo "no phone over adb (boot TWRP/OrangeFox, or Android with USB debugging)" >&2; exit 1; }
# Root: recovery/userdebug adbd is already uid 0; otherwise Magisk su. No `adb root`: on
# user builds it restarts adbd and the next commands race the reconnect.
if [ "$(adb shell id -u | tr -d '\r')" = 0 ]; then ROOT=adbd
elif [ "$(echo 'id -u' | adb shell su 2>/dev/null | tr -d '\r')" = 0 ]; then ROOT=su
else ROOT=; echo "warning: no root; /proc/iomem, partitions and some sysfs will be empty" >&2; fi

OUT=backup/evert/$(date +%Y%m%d-%H%M%S)
mkdir -p "$OUT" dtb/evert
echo "-> $OUT"

sh_() { adb shell "$1" 2>/dev/null | tr -d '\r'; }
# root shell: the command goes to su on stdin, so no quoting layers
rsh() { if [ "$ROOT" = su ]; then printf '%s\n' "$1" | adb shell su 2>/dev/null | tr -d '\r'; else sh_ "$1"; fi; }
grab() { echo "### $1"; rsh "$2"; echo; }
# Root copy of a phone file or block device to the Mac: tools/g6plus/chunkpull.py (small dd chunks as base64
# over `adb shell`, each SHA-256 checked against the phone, retried across USB drops; whole-file size + hash
# must match at the end). `adb exec-out "su -c 'cat ...'"` silently TRUNCATED the live DT on 2026-11-10
# (232960 of 317131 bytes) and plain `adb pull` stalls on this phone's flaky USB link.
rpull() {
    if [ "$ROOT" = su ]; then python3 tools/g6plus/chunkpull.py --su "$1" "$2" --chunk 512
    else python3 tools/g6plus/chunkpull.py "$1" "$2" --chunk 512; fi
}

{
grab "getprop" "getprop"
grab "cmdline" "cat /proc/cmdline"
grab "cpuinfo" "cat /proc/cpuinfo"
grab "cpu topology" "for c in /sys/devices/system/cpu/cpu[0-9]*; do echo \$c \$(cat \$c/topology/physical_package_id \$c/topology/core_id \$c/regs/identification/midr_el1 2>/dev/null) max=\$(cat \$c/cpufreq/cpuinfo_max_freq 2>/dev/null); done"
grab "meminfo" "head -3 /proc/meminfo"
grab "iomem" "cat /proc/iomem"
grab "interrupts" "cat /proc/interrupts"
grab "partitions (by-name)" "ls -l /dev/block/by-name/ /dev/block/bootdevice/by-name/ 2>/dev/null"
grab "partition sizes" "cat /proc/partitions"
grab "storage hosts" "ls -l /sys/class/mmc_host/ /sys/bus/platform/drivers/*ufs* /sys/bus/platform/drivers/sdhci_msm 2>/dev/null; cat /sys/class/mmc_host/mmc0/mmc0:*/{name,type,manfid,date} 2>/dev/null"
grab "i2c devices" "for d in /sys/bus/i2c/devices/*; do echo \$d \$(cat \$d/name 2>/dev/null) \$(readlink \$d/driver); done"
grab "spi devices" "for d in /sys/bus/spi/devices/*; do echo \$d \$(cat \$d/modalias 2>/dev/null) \$(readlink \$d/driver); done"
grab "input devices" "cat /proc/bus/input/devices"
grab "display" "cat /sys/class/graphics/fb0/msm_fb_panel_info /sys/class/graphics/fb0/modes /sys/class/graphics/fb0/virtual_size /sys/class/graphics/fb0/msm_fb_type 2>/dev/null; ls /sys/class/drm 2>/dev/null; grep -i -E 'panel|mdss|dsi' /proc/cmdline"
grab "gpu" "cat /sys/class/kgsl/kgsl-3d0/gpu_model /sys/class/kgsl/kgsl-3d0/max_gpuclk 2>/dev/null; ls /vendor/firmware /firmware/image /vendor/firmware_mnt/image 2>/dev/null | grep -i -E 'a5|a6|zap|pfp|pm4|gpmu'"
grab "usb" "ls /sys/class/udc 2>/dev/null; ls /sys/bus/platform/devices | grep -i -E 'usb|dwc|phy'"
grab "power supply" "for p in /sys/class/power_supply/*; do echo == \$p; cat \$p/uevent; done"
grab "regulators" "for r in /sys/class/regulator/regulator.*; do echo \$(cat \$r/name) \$(cat \$r/microvolts 2>/dev/null); done"
grab "pinctrl/gpio" "cat /sys/kernel/debug/gpio 2>/dev/null | head -200"
grab "platform devices" "ls /sys/bus/platform/devices"
grab "kernel version" "cat /proc/version"
grab "modules" "cat /proc/modules"
grab "dmesg" "dmesg"
grab "bootloader vars" "getprop | grep -E 'ro.boot|bootloader|baseband'"
} > "$OUT/raw.txt"

# also compare with the FDT header's totalsize (catches a short read on the phone side too)
rpull /sys/firmware/fdt dtb/evert/live.dtb 2>/dev/null || true
if [ -s dtb/evert/live.dtb ]; then
    tot=$(python3 -c "import struct,sys; print(struct.unpack('>I', open(sys.argv[1],'rb').read()[4:8])[0])" dtb/evert/live.dtb)
    got=$(wc -c < dtb/evert/live.dtb | tr -d ' ')
    [ "$tot" = "$got" ] && echo "live DT: $got bytes, complete" || echo "warning: live DT is $got bytes, header says $tot: TRUNCATED" >&2
fi
[ -s dtb/evert/live.dtb ] && command -v dtc >/dev/null && dtc -I dtb -O dts -o "$OUT/live.dts" dtb/evert/live.dtb 2>/dev/null || true

# redacted summary: drop serials, IMEI-like numbers, MACs
SERIAL=$(sh_ 'getprop ro.serialno')
SERIAL=$SERIAL perl -ne 'last if /^### dmesg/; my $s=$ENV{SERIAL};
    s/\Q$s\E/<serial>/g if length $s; s/(androidboot\.serialno=)\S+/$1<serial>/g; s/^(Serial\s*:\s*)\S+/$1<serial>/;
    s/(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}/<mac>/g; s/\b\d{15}\b/<imei>/g; print' \
    "$OUT/raw.txt" > "$OUT/summary.txt"
echo "summary: $OUT/summary.txt   live DT: dtb/evert/live.dtb"

if [ "${1:-}" = --backup ]; then
    mkdir -p "$OUT/parts"
    for p in $(rsh 'ls /dev/block/by-name/ 2>/dev/null || ls /dev/block/bootdevice/by-name/'); do
        case "$p" in userdata|system*|vendor*|product*|super|oem*|cache|mmcblk*) echo "skip $p"; continue;; esac
        echo "dd $p"
        rpull "/dev/block/by-name/$p" "$OUT/parts/$p.img" || { echo "  FAILED: $p"; FAILED="${FAILED:-} $p"; }
    done
    (cd "$OUT/parts" && shasum -a 256 *.img > SHA256SUMS)
    [ -z "${FAILED:-}" ] || { echo "partitions NOT backed up correctly:$FAILED" >&2; exit 1; }
    echo "partition backups: $OUT/parts (keep a copy off this Mac too)"
fi
