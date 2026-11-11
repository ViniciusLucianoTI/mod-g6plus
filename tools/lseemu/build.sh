#!/bin/bash
# Build lseemu_test for the phone (iOS arm64, ad-hoc signed). On the G6 Plus (ARMv8.0, kernel with
# xnu patch 1020) the "native" LSE forms trap into the kernel emulation, so `lseemu_test unit` there
# checks the kernel against the user-space LL/SC loops, and `lseemu_test bench` shows the cost of a trap.
#   tools/lseemu/build.sh [OUTDIR]       default: tools/lseemu/out
# Copy to the phone: scp -O tools/lseemu/out/lseemu_test root@192.168.7.2:/var/root/
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$HERE/out}"
GEN="$OUT/gen"
SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
mkdir -p "$OUT"
python3 -I "$HERE/check.py" --emit "$GEN"
# arm64, not arm64e: no PAC words, so only LSE/LDAPR reach the kernel's undefined-instruction path
xcrun --sdk iphoneos clang -arch arm64 -march=armv8.5-a -miphoneos-version-min=18.0 -isysroot "$SDK" \
    -std=c11 -O2 -g -Wall -Wextra -I "$GEN" \
    "$HERE/lseemu_test.c" "$HERE/lse_host.S" "$GEN/forms_gen.S" "$GEN/forms_gen.c" -o "$OUT/lseemu_test"
codesign -f -s - --entitlements "$HERE/entitlements.plist" "$OUT/lseemu_test"
ls -l "$OUT/lseemu_test"
