#!/bin/bash
# Screenshot of what the GPU-composited render server (backboardd on the GL shim, debug marker) last drew on the
# Moto G6 Plus, over SSH: bglshot.sh OUT.png [HOST]. One third of the panel's resolution. Needs a frame to be drawn
# after the trigger (touch the screen or send input).
set -euo pipefail
out=${1:?usage: bglshot.sh OUT.png [HOST]}; host=${2:-root@192.168.7.2}
o=(-o BatchMode=yes -o ConnectTimeout=8)
ssh "${o[@]}" "$host" 'rm -f /private/var/tmp/bgl-shot-*.ppm; touch /private/var/tmp/bgl-shot; chmod 666 /private/var/tmp/bgl-shot; n=0; while [ -e /private/var/tmp/bgl-shot ] && [ $n -lt 20 ]; do sleep 1; n=$((n+1)); done; cat /private/var/tmp/bgl-shot-*.ppm | base64' | base64 -d > "${out%.png}.ppm"
[ -s "${out%.png}.ppm" ] || { echo "no frame was drawn after the trigger" >&2; exit 1; }
sips -s format png "${out%.png}.ppm" --out "$out" >/dev/null && rm -f "${out%.png}.ppm"
echo "$out"
