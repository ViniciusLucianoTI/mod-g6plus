#!/bin/bash
# Install the SSH server, /bin/sh and toybox (tools/ssh/build.sh, docs/SSH_OVER_USB.md) into an existing iOS
# root image that the Mac serves to the Moto G6 Plus over USB. Only the ssh() step of
# tools/sweet-root-tweaks.py runs; nothing else in the image is touched. Nothing is written to the phone.
#   sudo bash tools/g6plus/install-ssh.sh [ROOT_IMG]      (default research/ipsw-x/ios16-root-g6-work.img)
# Needs root so that owners and modes inside the image are kept. No usbdisk_server.py may be running.
set -euo pipefail
cd "$(dirname "$0")/../.."
img=${1:-research/ipsw-x/ios16-root-g6-work.img}
[[ $(id -u) == 0 ]] || { echo "run with sudo: owners inside the image must be preserved" >&2; exit 1; }
[[ -f $img ]] || { echo "no image at $img" >&2; exit 1; }
if pgrep -f "g6plus/usbdisk_server.py" >/dev/null; then echo "a usbdisk_server.py is serving an image: stop the boot first" >&2; exit 1; fi
[[ -x research/ssh-build/out/dropbear ]] || { echo "build first: tools/ssh/build.sh" >&2; exit 1; }

mnt=$(mktemp -d /tmp/g6root.XXXXXX)
dev=$(hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage "$img" | awk 'NR==1{print $1}')
cleanup() {
	umount "$mnt" 2>/dev/null || true
	hdiutil detach "$dev" >/dev/null 2>&1 || true
	rmdir "$mnt" 2>/dev/null || true
	chown "${SUDO_USER:-$(id -un)}" "$img"
}
trap cleanup EXIT
# the last boot ended with a reset: replay/repair the journal before mounting
fsck_hfs -fy "${dev/disk/rdisk}" || true
mount -t hfs "$dev" "$mnt"
mount | grep " on $mnt " | grep -q noowners && { echo "mounted noowners" >&2; exit 1; }
[[ -d $mnt/System/Library/xpc ]] || { echo "$img does not look like the iOS root" >&2; exit 1; }

python3 - "$mnt" <<'EOF'
import importlib.util, sys
spec = importlib.util.spec_from_file_location('tweaks', 'tools/sweet-root-tweaks.py')
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
m.ssh(sys.argv[1])
EOF

rm -rf "$mnt"/.{Spotlight-V100,fseventsd,Trashes}
ls -la "$mnt/usr/local/bin/dropbear" "$mnt/bin/sh" "$mnt/private/var/root/.ssh/authorized_keys" \
	"$mnt/System/Library/LaunchDaemons/org.sweet.dropbear.plist"
grep '^root:' "$mnt/private/etc/passwd" || true
umount "$mnt"
fsck_hfs -fn "${dev/disk/rdisk}"
echo "OK: SSH instalado em $img"
