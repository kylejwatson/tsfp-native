#!/usr/bin/env bash
# record.sh <name> [route]   (Linux counterpart of record.bat)
#   Plays the game and writes everything player 1 presses (pad, keys, mouse)
#   to recordings/<name>.rec. Get to where you want to be, then close the
#   window; afterwards `./play.sh <name>` replays it. With a route, that route
#   drives the start (and is part of the recording).
HERE="$(cd "$(dirname "$0")" && pwd)"
EXE="$HERE/${BUILD_DIR:-build-linux}/futureperfect_recomp"
if [ -z "$1" ]; then
  echo "usage: record.sh <name> [route]" >&2
  exit 1
fi
mkdir -p "$HERE/recordings"
export RECOMP_INPUT_RECORD="$HERE/recordings/$1.rec"
if [ -n "$2" ] && [ -f "$HERE/seqs/$2.seq" ]; then
  export RECOMP_INPUT_SEQ="@$HERE/seqs/$2.seq"
fi
exec "$EXE"
