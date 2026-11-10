#!/usr/bin/env bash
# mirrors the CI assertions: zip layout, aarch64 ELFs, no external shadowhook
set -euo pipefail
cd "$(dirname "$0")"
READELF="C:/Users/Eroll/AppData/Local/Temp/opencode/ndk/android-ndk-r27c/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe"
echo "--- zip layout ---"
unzip -l out/codm_overlay.zip
echo "--- elf headers ---"
file build/out/libcodm.so build/out/codm-inject
echo "--- NEEDED ---"
"$READELF" -d build/out/libcodm.so | grep NEEDED
echo "--- shadowhook must not be a NEEDED ---"
if "$READELF" -d build/out/libcodm.so | grep -q libshadowhook; then
  echo "FAIL: libshadowhook.so is a NEEDED entry"
  exit 1
fi
echo "OK"
