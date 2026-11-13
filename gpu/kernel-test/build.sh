#!/bin/bash
# Build adreno_uc_test as arm64e, then strip PAC + ad-hoc sign it with renegade (runs on the PAC-less Kryo 385).
#   gpu/kernel-test/build.sh [OUTDIR]       default: gpu/kernel-test/out
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$HERE/out}"
SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
mkdir -p "$OUT"
# TARGET=ios16-arm64 (Moto G6 Plus, iOS 16.7 plain arm64): no PAC to strip, ad-hoc signed with the SSH tools' entitlements.
if [ "${TARGET:-}" = ios16-arm64 ]; then
	xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0 -isysroot "$SDK" -O1 -g -Wall -Wextra -Wno-unused-parameter \
	    "$HERE/adreno_uc_test.c" -o "$OUT/adreno_uc_test" -framework IOKit -framework CoreFoundation -framework IOSurface
	codesign --force -s - --entitlements "$HERE/../../tools/ssh/entitlements.plist" "$OUT/adreno_uc_test"
	ls -l "$OUT/adreno_uc_test"
	# With a Mesa build (ARCH=arm64 MINOS=16.0 GPU_EAGL_WORK=DIR gpu/eagl/mesa/build-mesa.sh): osmesa_tri and the two
	# libraries it needs, side by side in $OUT/mesa16 (copy the directory to the phone, e.g. /usr/local/mesa16).
	if [ -n "${GPU_EAGL_WORK:-}" ] && [ -f "$GPU_EAGL_WORK/build-hw/src/gallium/targets/osmesa/libOSMesa.8.dylib" ]; then
		M="$OUT/mesa16"; mkdir -p "$M"
		cp -f "$GPU_EAGL_WORK/build-hw/src/gallium/targets/osmesa/libOSMesa.8.dylib" "$M/"
		cp -f "$GPU_EAGL_WORK/drm-standin/libdrm.2.dylib" "$M/"
		xcrun strip -x "$M/libOSMesa.8.dylib"
		for rp in $(otool -l "$M/libOSMesa.8.dylib" | awk '/LC_RPATH/{f=1} f&&/path /{print $2; f=0}'); do
			install_name_tool -delete_rpath "$rp" "$M/libOSMesa.8.dylib" 2>/dev/null || true
		done
		install_name_tool -add_rpath @loader_path "$M/libOSMesa.8.dylib" 2>/dev/null || true
		xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0 -isysroot "$SDK" -O1 -g -Wall -Wextra \
		    -I"$GPU_EAGL_WORK/mesa/include" "$HERE/osmesa_tri.c" -o "$M/osmesa_tri" "$M/libOSMesa.8.dylib" -Wl,-rpath,@executable_path
		# iOS refuses @rpath/@loader_path for these binaries ("security policy does not allow @ path expansion", seen
		# on the G6): absolute install names for where they live on the phone.
		D="${MESA16_DIR:-/usr/local/mesa16}"
		install_name_tool -id "$D/libdrm.2.dylib" "$M/libdrm.2.dylib"
		install_name_tool -id "$D/libOSMesa.8.dylib" -change @rpath/libdrm.2.dylib "$D/libdrm.2.dylib" "$M/libOSMesa.8.dylib"
		install_name_tool -change @rpath/libOSMesa.8.dylib "$D/libOSMesa.8.dylib" "$M/osmesa_tri"
		for f in libdrm.2.dylib libOSMesa.8.dylib osmesa_tri; do
			codesign --force -s - --entitlements "$HERE/../../tools/ssh/entitlements.plist" "$M/$f"
		done
		ls -l "$M"
	fi
	exit 0
fi
xcrun --sdk iphoneos clang -arch arm64e -miphoneos-version-min=18.0 -isysroot "$SDK" -O1 -g -Wall -Wextra -Wno-unused-parameter \
    "$HERE/adreno_uc_test.c" -o "$OUT/adreno_uc_test.arm64e" -framework IOKit -framework CoreFoundation -framework IOSurface
"$HERE/../../tools/renegade/renegade" strip --sign -o "$OUT/adreno_uc_test" "$OUT/adreno_uc_test.arm64e"
chmod +x "$OUT/adreno_uc_test"
ls -l "$OUT/adreno_uc_test"
