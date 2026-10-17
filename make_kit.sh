#!/usr/bin/env bash
# Make the shareable build in dist/: the runtime executable, its config, and
# the title kit a player builds futureperfect_game.so with from their own
# disc (xboxrecomp/scripts/make_kit.py). Nothing in dist/ comes from the game.
#
#   ./build.sh && ./make_kit.sh             # lift first if seeds changed
#   ZIG=build-zig ./make_kit.sh             # include a Zig (fetch_zig.py)
#
# The player then runs:
#   python3 kit/toolkit/scripts/build_module.py --xbe <disc>/default.xbe
set -euo pipefail
cd "$(dirname "$0")"
DIST=${DIST:-dist}
BUILD_DIR=${BUILD_DIR:-build-linux}
if [ -e "$DIST" ]; then
  echo "$DIST exists; move it away first" >&2
  exit 1
fi
EXTRA=()
if [ -n "${ZIG:-}" ]; then EXTRA+=(--zig "$ZIG"); fi
if [ -n "${NO_VERIFY:-}" ]; then EXTRA+=(--no-verify); fi
python3 xboxrecomp/scripts/make_kit.py --project . --xbe game_files/default.xbe \
  --module-name futureperfect_game --game-name "TimeSplitters Future Perfect" \
  --split 400 --seeds xboxrecomp/config/seeds/45410066.json --seeds config/seeds.json \
  --cc "${CC:-clang}" --out "$DIST/kit" "${EXTRA[@]}"
cp "$BUILD_DIR/futureperfect_recomp" "$BUILD_DIR/input_bindings.json" "$DIST/"
echo "dist: $(du -sh "$DIST" | cut -f1) in $DIST/"
