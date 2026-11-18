#!/bin/bash
# Small on-device tools for the Moto G6 Plus (iOS 16 arm64): tools/g6plus/src/*.c -> tools/g6plus/out/.
# Needs Xcode. Copy the results to /usr/local/bin on the running phone (ssh root@192.168.7.2 'cat > ...').
set -euo pipefail
cd "$(dirname "$0")"
export DEVELOPER_DIR=${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
mkdir -p out
for src in src/*.c; do
	name=$(basename "$src" .c)
	xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0 -isysroot "$SDK" -O2 -Wall -Wextra "$src" -o "out/$name"
	codesign --force -s - --entitlements ../ssh/entitlements.plist "out/$name"
	echo "out/$name"
done
