#!/usr/bin/env bash
# build.sh — configure, build, stage and zip the Magisk module.
# Needs: Android NDK r26+, CMake 3.22+, Ninja, python3 (for zipping if `zip` is absent).
set -euo pipefail

ROOT=$(cd "$(dirname "$0")" && pwd)
cd "$ROOT"

NDK="${ANDROID_NDK_HOME:-${ANDROID_NDK:-}}"
if [ -z "$NDK" ]; then
    for base in "${ANDROID_SDK_ROOT:-}" "${ANDROID_HOME:-}" \
                "$HOME/Android/Sdk" "$HOME/Library/Android/sdk" /opt/android-sdk; do
        [ -n "$base" ] || continue
        for cand in "$base"/ndk/*; do
            [ -d "$cand" ] && NDK="$cand"
        done
    done
fi
if [ -z "$NDK" ] || [ ! -d "$NDK" ]; then
    echo "no NDK found — export ANDROID_NDK_HOME=/path/to/ndk" >&2
    exit 1
fi
echo "NDK: $NDK"

BUILD=build
rm -rf "$BUILD" out
mkdir -p out

# EXTRA_CMAKE_ARGS is word-split on purpose (CI passes -DFETCHCONTENT_BASE_DIR=...)
# shellcheck disable=SC2086
cmake -S . -B "$BUILD" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-24 \
    -DCMAKE_BUILD_TYPE=Release \
    ${EXTRA_CMAKE_ARGS:-}

cmake --build "$BUILD" --parallel

STAGE=out/module
mkdir -p "$STAGE/bin" "$STAGE/assets"
cp -a magisk/. "$STAGE/"
cp "$BUILD/out/codm-inject" "$STAGE/bin/codm-inject"
cp "$BUILD/out/libcodm.so" "$STAGE/assets/libcodm.so"
chmod 0755 "$STAGE/bin/codm-inject" "$STAGE"/service.sh "$STAGE"/post-fs-data.sh "$STAGE/codm.sh"

ZIP=out/codm_overlay.zip
rm -f "$ZIP"
if command -v zip >/dev/null 2>&1; then
    (cd "$STAGE" && zip -qr "$ROOT/$ZIP" .)
else
    python3 - "$STAGE" "$ZIP" <<'PY'
import os, sys, zipfile
stage, out = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for root, _, files in os.walk(stage):
        for f in files:
            p = os.path.join(root, f)
            z.write(p, os.path.relpath(p, stage))
PY
fi

ls -lh "$ZIP"
echo "flash $ZIP in Magisk, reboot, launch CODM"
