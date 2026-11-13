#!/bin/bash
# Build the libdrm stand-in as arm64e, strip PAC + ad-hoc sign with renegade.
#   gpu/freedreno-sweet/build.sh [OUTDIR]     default: gpu/freedreno-sweet/out/libdrm.2.dylib
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$HERE/out}"
SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
mkdir -p "$OUT"
# TARGET=ios16-arm64 (Moto G6 Plus, iOS 16.7, plain arm64): no PAC to strip; ad-hoc signed.
if [ "${TARGET:-}" = ios16-arm64 ]; then
	xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0 -isysroot "$SDK" -O2 -g -Wall -Wextra -fvisibility=hidden \
	    -dynamiclib -install_name @rpath/libdrm.2.dylib -current_version 2.4.0 -compatibility_version 2.0.0 \
	    "$HERE/sweet_drm.c" -o "$OUT/libdrm.2.dylib" -framework IOKit -framework CoreFoundation
	cp "$OUT/libdrm.2.dylib" "$OUT/libdrm.2.arm64.dylib"
	codesign --force -s - "$OUT/libdrm.2.dylib"
	ls -l "$OUT/libdrm.2.dylib"
	exit 0
fi
xcrun --sdk iphoneos clang -arch arm64e -miphoneos-version-min=18.0 -isysroot "$SDK" -O2 -g -Wall -Wextra -fvisibility=hidden \
    -dynamiclib -install_name @rpath/libdrm.2.dylib -current_version 2.4.0 -compatibility_version 2.0.0 \
    "$HERE/sweet_drm.c" -o "$OUT/libdrm.2.arm64e.dylib" -framework IOKit -framework CoreFoundation
"$HERE/../../tools/renegade/renegade" strip --sign -o "$OUT/libdrm.2.dylib" "$OUT/libdrm.2.arm64e.dylib"
ls -l "$OUT/libdrm.2.dylib"
