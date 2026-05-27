#!/usr/bin/env bash
set -euo pipefail

BUILD_TYPE="${1:-release}"
BUILD_DIR="$(dirname "$0")/build"

case "$BUILD_TYPE" in
    debug|Debug|DEBUG)
        BUILD_TYPE="Debug"
        ;;
    release|Release|RELEASE)
        BUILD_TYPE="Release"
        ;;
    *)
        echo "Usage: $0 [debug|release]"
        echo "  (default: release)"
        exit 1
        ;;
esac

echo "=== Build: $BUILD_TYPE ==="

cmake -S "$(dirname "$0")" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

cmake --build "$BUILD_DIR" -j "$(nproc)"

echo "=== Done: $BUILD_TYPE ==="
