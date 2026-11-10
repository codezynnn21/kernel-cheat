#!/usr/bin/env bash
# local build wrapper: pins the local NDK + cmake/ninja on PATH
set -euo pipefail
export PATH="/c/Users/Eroll/AppData/Local/Packages/PythonSoftwareFoundation.Python.3.13_qbz5n2kfra8p0/LocalCache/local-packages/Python313/Scripts:$PATH"
export ANDROID_NDK_HOME="C:/Users/Eroll/AppData/Local/Temp/opencode/ndk/android-ndk-r27c"
cd "$(dirname "$0")"
exec ./build.sh "$@"
