#!/bin/bash
# Screenshot of what the GPU-composited render server (backboardd on the GL shim, debug marker) last drew on the
# Moto G6 Plus, over SSH: bglshot.sh OUT.png [HOST]. One third of the panel's resolution. Needs a frame to be drawn
# while the trigger is there (SECS, default 6: touch the screen or send input). One PNG per display surface: OUT-0.png, OUT-1.png, ...
set -euo pipefail
out=${1:?usage: bglshot.sh OUT.png [HOST]}; host=${2:-root@192.168.7.2}
o=(-o BatchMode=yes -o ConnectTimeout=8)
ssh "${o[@]}" "$host" 'rm -f /private/var/tmp/bgl-shot-*.ppm; touch /private/var/tmp/bgl-shot; chmod 666 /private/var/tmp/bgl-shot; sleep '"${SECS:-6}"'; rm -f /private/var/tmp/bgl-shot; cd /private/var/tmp; ls bgl-shot-*.ppm >&2; tar cf - bgl-shot-*.ppm | base64' | base64 -d | tar xf - -C "$(dirname "$out")"
n=0
for f in "$(dirname "$out")"/bgl-shot-*.ppm; do
  [ -s "$f" ] || continue
  sips -s format png "$f" --out "${out%.png}-$n.png" >/dev/null && rm -f "$f" && echo "${out%.png}-$n.png"; n=$((n+1))
done
[ $n -gt 0 ] || { echo "no frame was drawn while the trigger was there" >&2; exit 1; }
exit 0
[ -s "${out%.png}.ppm" ] || { echo "no frame was drawn after the trigger" >&2; exit 1; }
sips -s format png "${out%.png}.ppm" --out "$out" >/dev/null && rm -f "${out%.png}.ppm"
echo "$out"
