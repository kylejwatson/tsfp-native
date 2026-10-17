#!/usr/bin/env bash
# Make the shareable build in dist/: the runtime executable, its config, and
# the title kit a player's first run builds futureperfect_game.so with from
# their own disc (xboxrecomp/scripts/make_kit.py). Nothing in dist/ comes
# from the game.
#
#   ./build.sh && ./make_kit.sh             # lift first if seeds changed
#
# The kit carries a Zig for this platform only (fetch_zig.py trims it to the
# one target the player's machine builds for): build-zig-linux/ or
# build-zig-windows/, fetched on first use. ZIG=<dir> uses another, ZIG=none
# leaves it out (the player then needs zig, clang or gcc).
#
# It also carries a Python with capstone (fetch_python.py, pinned):
# build-python-linux/ or build-python-windows/, fetched on first use, and
# make_kit.py runs under it, so the reference lift is checked with exactly
# the interpreter that ships. PY=<dir> uses another, PY=none leaves it out
# (the player then needs Python 3 and capstone).
#
# The player starts the executable: the first time, it asks for the disc
# (image or folder) and builds the module, then plays.
set -euo pipefail
cd "$(dirname "$0")"
DIST=${DIST:-dist}

case "$(uname -s)" in
  Linux) OS=linux; BUILD_DIR=${BUILD_DIR:-build-linux}; EXE=futureperfect_recomp ;;
  MINGW*|MSYS*|CYGWIN*) OS=windows; BUILD_DIR=${BUILD_DIR:-build}; EXE=futureperfect_recomp.exe ;;
  *) echo "make_kit.sh: no shareable build for $(uname -s) yet" >&2; exit 1 ;;
esac

if [ "$OS" = windows ]; then
  # The Windows executable is still built with the game linked in
  # (XBOXRECOMP_GAME_MODULE is off there: a DLL cannot import the exe's
  # thread-local guest registers), so it must not be handed out.
  echo "make_kit.sh: the Windows build still contains the game code; it cannot be shared until it loads the game as a module" >&2
  exit 1
fi

if [ -e "$DIST" ]; then
  echo "$DIST exists; move it away first" >&2
  exit 1
fi

# The executable must be the runtime alone: a build with the lifted code
# linked in (or a stale one) would hand the game out.
if nm -D --defined-only "$BUILD_DIR/$EXE" | grep -q ' sub_[0-9A-F]\{8\}$'; then
  echo "make_kit.sh: $BUILD_DIR/$EXE defines lifted functions (sub_*); build with XBOXRECOMP_GAME_MODULE=ON" >&2
  exit 1
fi

ZIG=${ZIG:-build-zig-$OS}
EXTRA=()
if [ "$ZIG" != none ]; then
  if [ ! -e "$ZIG/zig" ] && [ ! -e "$ZIG/zig.exe" ]; then
    python3 xboxrecomp/scripts/fetch_zig.py --os "$OS" --out "$ZIG"
  fi
  EXTRA+=(--zig "$ZIG")
fi
PY=${PY:-build-python-$OS}
PYTHON=python3
if [ "$PY" != none ]; then
  if [ ! -e "$PY/python" ]; then
    python3 xboxrecomp/scripts/fetch_python.py --os "$OS" --out "$PY"
  fi
  EXTRA+=(--python "$PY")
  if [ "$OS" = windows ]; then PYTHON=$PY/python/python.exe; else PYTHON=$PY/python/bin/python3; fi
fi
if [ -n "${NO_VERIFY:-}" ]; then EXTRA+=(--no-verify); fi
# The module owns the guest registers when the build says so (the toolkit's
# XBOXRECOMP_REGS_IN_MODULE); the player's module must be built the same way,
# or the runtime refuses it.
if grep -q '^XBOXRECOMP_REGS_IN_MODULE:BOOL=ON$' "$BUILD_DIR/CMakeCache.txt" 2>/dev/null; then
  EXTRA+=(--define RECOMP_REGS_IN_MODULE)
fi
# With the disc, the kit is made from it and the facts a disc-free build needs
# (XBE hash, generated-file digests, XDK symbol names) are recorded in
# kit_inputs/, which is committed: a change in them shows in git diff. Without
# the disc (CI), the kit is made from kit_inputs/ alone.
if [ -f game_files/default.xbe ] && [ -z "${FROM_INPUTS:-}" ]; then
  SOURCE=(--xbe game_files/default.xbe --record-inputs kit_inputs)
else
  SOURCE=(--inputs kit_inputs)
fi
"$PYTHON" xboxrecomp/scripts/make_kit.py --project . "${SOURCE[@]}" \
  --module-name futureperfect_game --game-name "TimeSplitters Future Perfect" \
  --split 400 --seeds xboxrecomp/config/seeds/45410066.json --seeds config/seeds.json \
  --cc "${CC:-clang}" --out "$DIST/kit" "${EXTRA[@]}"
cp "$BUILD_DIR/$EXE" "$BUILD_DIR/input_bindings.json" "$DIST/"

# Libraries the runtime needs that distributions rarely install, beside it
# (it looks in lib/ first: RUNPATH $ORIGIN/lib).
BUNDLED_LIBS=""
if [ "$OS" = linux ]; then
  mkdir -p "$DIST/lib"
  while read -r name path; do
    cp -L "$path" "$DIST/lib/$name"
    BUNDLED_LIBS="$BUNDLED_LIBS $name"
  done < <(ldd "$BUILD_DIR/$EXE" | awk '/shaderc|glslang|SPIRV/ {print $1, $3}')
fi

# Licences: the runtime's own (GPL-3.0, with the upstream MIT and the
# third-party notices) and every bundled component's, with an index.
L=xboxrecomp/LICENSES
mkdir -p "$DIST/LICENSES"
cp LICENSE "$DIST/LICENSE"
cp xboxrecomp/LICENSE "$DIST/LICENSES/xboxrecomp-GPL-3.0.txt"
cp xboxrecomp/LICENSE.upstream-MIT "$DIST/LICENSES/xboxrecomp-upstream-MIT.txt"
cp xboxrecomp/NOTICE "$DIST/LICENSES/xboxrecomp-NOTICE.txt"
cp "$L/LGPL-2.1.txt" "$DIST/LICENSES/"
kit_field() { python3 -c "import json,sys; d=json.load(open('$DIST/kit/kit.json')); v=d
for k in sys.argv[1].split('.'): v=(v or {}).get(k)
print(v or '')" "$1"; }
{
  echo "Third-party components in this build"
  echo "===================================="
  echo
  echo "This project is free software under the GNU GPL, version 3 or (at your"
  echo "option) any later version (LICENSE). It is unofficial and contains nothing"
  echo "from the game: the game's code is built on your machine from your own disc."
  echo
  echo "futureperfect_recomp is built from xboxrecomp (GPL-3.0; LICENSES/xboxrecomp-*),"
  echo "which credits the code it adapts in LICENSES/xboxrecomp-NOTICE.txt."
  echo
  if [ -n "$BUNDLED_LIBS" ]; then
    cp "$L/Apache-2.0.txt" "$L/glslang-LICENSE.txt" "$DIST/LICENSES/"
    echo "lib/:$BUNDLED_LIBS"
    echo "  shaderc $(pkg-config --modversion shaderc 2>/dev/null) and SPIRV-Tools: Apache-2.0 (LICENSES/Apache-2.0.txt)"
    echo "  glslang: its licence file, LICENSES/glslang-LICENSE.txt"
    echo
  fi
  if [ -d "$DIST/kit/zig" ]; then
    echo "kit/zig/: Zig $(kit_field zig), MIT (kit/zig/LICENSE); its libc headers and"
    echo "  sources keep their own notices under kit/zig/lib/libc."
    echo
  fi
  if [ -d "$DIST/kit/python" ]; then
    echo "kit/python/: CPython $(kit_field python.version) (python-build-standalone),"
    echo "  PSF licence (kit/python/lib/python*/LICENSE.txt, which also lists what it bundles);"
    echo "  capstone $(kit_field python.capstone), BSD-3-Clause"
    echo "  (kit/python/lib/python*/site-packages/capstone-*.dist-info/LICENSE.TXT)."
    echo
  fi
  echo "Used from the system, not included: SDL2, the Vulkan loader, FreeType,"
  echo "fontconfig, OpenSSL (libcrypto), the C and C++ runtimes."
} > "$DIST/THIRD_PARTY.txt"
echo "dist: $(du -sh "$DIST" | cut -f1) in $DIST/"
