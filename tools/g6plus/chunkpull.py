#!/usr/bin/env python3
"""Pull a root-readable phone file or block device over a FLAKY adb link, in small verified chunks.

  chunkpull.py REMOTE LOCAL [--chunk KiB] [--su]

Each chunk is read on the phone with dd, sent as base64 text over `adb shell` (short commands survive a
link that stalls long `adb pull` bulk transfers), checked against a SHA-256 computed on the phone, and
retried (with `adb wait-for-device`) until it matches. Resumes: chunks already in LOCAL.part are kept
(their hash is re-checked). At the end the whole file's size and SHA-256 must equal the phone's.
READ-ONLY on the phone.
"""
import argparse, base64, hashlib, os, subprocess, sys, time

ap = argparse.ArgumentParser()
ap.add_argument('remote'); ap.add_argument('local')
ap.add_argument('--chunk', type=int, default=256, help='KiB per chunk')
ap.add_argument('--su', action='store_true', help='read through Magisk su (stdin)')
a = ap.parse_args()

def sh(cmd, timeout=60):
    for attempt in range(1000):
        try:
            if a.su:
                r = subprocess.run(['adb', 'shell', 'su'], input=cmd + '\n', capture_output=True, text=True, timeout=timeout)
            else:
                r = subprocess.run(['adb', 'shell', cmd], capture_output=True, text=True, timeout=timeout)
            if r.returncode == 0:
                return r.stdout
        except subprocess.TimeoutExpired:
            pass
        sys.stderr.write(f'  retry ({attempt + 1}) ...\n')
        subprocess.run(['adb', 'wait-for-device'], timeout=600)
        time.sleep(1)
    raise SystemExit('giving up')

q = a.remote.replace("'", "")
size = int(sh(f"[ -b '{q}' ] && blockdev --getsize64 '{q}' || stat -c %s '{q}'").split()[0])
bs = a.chunk * 1024
n = (size + bs - 1) // bs
part = a.local + '.part'
out = open(part, 'r+b' if os.path.exists(part) else 'w+b')
t0 = time.time()
for i in range(n):
    want_len = min(bs, size - i * bs)
    out.seek(i * bs)
    have = out.read(want_len)
    # dd: bs=4096 granularity so it also works on block devices; chunk is a multiple of 4 KiB
    blocks, skip = (want_len + 4095) // 4096, i * bs // 4096
    rd = f"dd if='{q}' bs=4096 skip={skip} count={blocks} 2>/dev/null | head -c {want_len}"
    if len(have) == want_len:
        if sh(f"{rd} | sha256sum").split()[0] == hashlib.sha256(have).hexdigest():
            continue
    while True:
        txt = sh(f"{rd} | base64 -w 0; echo; {rd} | sha256sum", timeout=120)
        lines = txt.strip().split('\n')
        try:
            data = base64.b64decode(lines[0]); rsum = lines[1].split()[0]
        except Exception:
            data, rsum = b'', '?'
        if len(data) == want_len and hashlib.sha256(data).hexdigest() == rsum:
            break
        sys.stderr.write(f'  chunk {i}: bad ({len(data)} bytes), again\n')
    out.seek(i * bs); out.write(data); out.flush()
    if i % 16 == 15 or i == n - 1:
        sys.stderr.write(f'\r  {min((i + 1) * bs, size) >> 20} / {size >> 20} MiB  {time.time() - t0:.0f}s')
out.truncate(size); out.close()
sys.stderr.write('\n')
whole = sh(f"[ -b '{q}' ] && head -c {size} '{q}' | sha256sum || sha256sum '{q}'", timeout=600).split()[0]
local = hashlib.sha256(open(part, 'rb').read()).hexdigest()
if whole != local:
    raise SystemExit(f'MISMATCH: phone {whole} mac {local} (kept {part})')
os.replace(part, a.local)
print(f'{a.local}: {size} bytes, sha256 {local} (matches the phone)')
