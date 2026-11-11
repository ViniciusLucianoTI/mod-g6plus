#!/usr/bin/env python3
"""Minimal AOSP mkbootimg (boot image header v0, v1, v2) with the option names Enxerto's build.sh uses.

  mkbootimg.py --header_version N --kernel K --ramdisk R [--dtb D (v2)] [--second S]
               --base B --kernel_offset O --ramdisk_offset O --tags_offset O [--second_offset O]
               [--dtb_offset O] --pagesize P [--os_version A.B.C] [--os_patch_level YYYY-MM]
               [--cmdline STR] [--board NAME] -o OUT

Layout and the id (SHA-1) follow AOSP system/tools/mkbootimg/mkbootimg.py. For hosts without the
AOSP tool (Homebrew has none). Checked 2026-11-11: repacking the kernel + ramdisk + cmdline of the Moto G6
Plus stock boot_a (header v0) gives the first 30724096 bytes of the partition byte-identically (NOTES.md).
"""
import argparse, hashlib, re, struct, sys


def num(s):
    return int(s, 0)


def os_version(ver, patch):
    v = 0
    if ver:
        m = re.fullmatch(r'(\d+)(?:\.(\d+))?(?:\.(\d+))?', ver)
        a, b, c = (int(x or 0) for x in m.groups())
        v = (a << 14 | b << 7 | c) << 11
    if patch:
        y, mth = map(int, patch.split('-')[:2])
        v |= (y - 2000) << 4 | mth
    return v


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--header_version', type=int, default=0)
    ap.add_argument('--kernel', required=True)
    ap.add_argument('--ramdisk')
    ap.add_argument('--second')
    ap.add_argument('--dtb')
    ap.add_argument('--recovery_dtbo')
    ap.add_argument('--base', type=num, default=0x10000000)
    ap.add_argument('--kernel_offset', type=num, default=0x00008000)
    ap.add_argument('--ramdisk_offset', type=num, default=0x01000000)
    ap.add_argument('--second_offset', type=num, default=0x00f00000)
    ap.add_argument('--tags_offset', type=num, default=0x00000100)
    ap.add_argument('--dtb_offset', type=num, default=0x01f00000)
    ap.add_argument('--pagesize', type=num, default=2048)
    ap.add_argument('--os_version')
    ap.add_argument('--os_patch_level')
    ap.add_argument('--cmdline', default='')
    ap.add_argument('--board', default='')
    ap.add_argument('-o', '--output', required=True)
    a = ap.parse_args(argv)
    if a.header_version not in (0, 1, 2):
        sys.exit('only header v0-v2')
    rd = lambda p: open(p, 'rb').read() if p else b''
    kernel, ramdisk, second = rd(a.kernel), rd(a.ramdisk), rd(a.second)
    dtbo, dtb = rd(a.recovery_dtbo), rd(a.dtb)
    if a.header_version < 2 and dtb:
        sys.exit('--dtb needs header v2')
    cmd = a.cmdline.encode()
    if len(cmd) > 512 + 1024 - 1:
        sys.exit('cmdline too long')
    ps = a.pagesize
    sha = hashlib.sha1()
    for blob in [kernel, ramdisk, second] + ([dtbo] if a.header_version >= 1 else []) + ([dtb] if a.header_version >= 2 else []):
        sha.update(blob)
        sha.update(struct.pack('<I', len(blob)))
    hdr = b'ANDROID!' + struct.pack('<10I', len(kernel), a.base + a.kernel_offset, len(ramdisk),
                                    a.base + a.ramdisk_offset, len(second), a.base + a.second_offset,
                                    a.base + a.tags_offset, ps, a.header_version,
                                    os_version(a.os_version, a.os_patch_level))
    hdr += a.board.encode().ljust(16, b'\0')[:16]
    hdr += cmd[:512].ljust(512, b'\0')
    hdr += sha.digest().ljust(32, b'\0')
    hdr += cmd[512:].ljust(1024, b'\0')
    if a.header_version >= 1:
        dtbo_off = 0
        if dtbo:
            pages = lambda n: (n + ps - 1) // ps
            dtbo_off = ps * (1 + pages(len(kernel)) + pages(len(ramdisk)) + pages(len(second)))
        hsize = 1660 if a.header_version == 2 else 1648
        hdr += struct.pack('<IQI', len(dtbo), dtbo_off, hsize)
    if a.header_version >= 2:
        hdr += struct.pack('<IQ', len(dtb), a.base + a.dtb_offset)
    pad = lambda b: b + b'\0' * (-len(b) % ps)
    out = pad(hdr)
    for blob in [kernel, ramdisk, second] + ([dtbo] if a.header_version >= 1 else []) + ([dtb] if a.header_version >= 2 else []):
        out += pad(blob)
    open(a.output, 'wb').write(out)


if __name__ == '__main__':
    main()
