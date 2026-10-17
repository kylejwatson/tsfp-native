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
if [ -n "${NO_VERIFY:-}" ]; then EXTRA+=(--no-verify); fi
# The module owns the guest registers when the build says so (the toolkit's
# XBOXRECOMP_REGS_IN_MODULE); the player's module must be built the same way,
# or the runtime refuses it.
if grep -q '^XBOXRECOMP_REGS_IN_MODULE:BOOL=ON$' "$BUILD_DIR/CMakeCache.txt" 2>/dev/null; then
  EXTRA+=(--define RECOMP_REGS_IN_MODULE)
fi
python3 xboxrecomp/scripts/make_kit.py --project . --xbe game_files/default.xbe \
  --module-name futureperfect_game --game-name "TimeSplitters Future Perfect" \
  --split 400 --seeds xboxrecomp/config/seeds/45410066.json --seeds config/seeds.json \
  --cc "${CC:-clang}" --out "$DIST/kit" "${EXTRA[@]}"
cp "$BUILD_DIR/$EXE" "$BUILD_DIR/input_bindings.json" "$DIST/"
echo "dist: $(du -sh "$DIST" | cut -f1) in $DIST/"
