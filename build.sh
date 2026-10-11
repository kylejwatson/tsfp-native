#!/usr/bin/env bash
# Build on Linux: CMake + Ninja (or Make) into build-linux/, which leaves a
# Windows build/ in a shared checkout alone. Writes build-linux/futureperfect_recomp.
# Needs: cmake, a C compiler (clang or gcc), SDL2, Vulkan headers + loader,
# shaderc, OpenSSL, FreeType and fontconfig (see README.md).
set -euo pipefail
cd "$(dirname "$0")"
BUILD_DIR=${BUILD_DIR:-build-linux}
GEN=()
if command -v ninja >/dev/null 2>&1; then GEN=(-G Ninja); fi
if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
  cmake -S . -B "$BUILD_DIR" "${GEN[@]}" -DCMAKE_BUILD_TYPE="${BUILD_TYPE:-Release}" "$@"
fi
cmake --build "$BUILD_DIR" --parallel
