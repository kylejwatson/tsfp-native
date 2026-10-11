#!/usr/bin/env bash
# play.sh [route]   (Linux counterpart of play.bat)
#   No route: just start the game.
#   seqs/<route>.seq exists: drive the menus with it (RECOMP_INPUT_SEQ), then play.
#   recordings/<route>.rec exists: replay that recording (RECOMP_INPUT_REPLAY);
#   the pad and mouse are yours again when it ends.
HERE="$(cd "$(dirname "$0")" && pwd)"
EXE="$HERE/${BUILD_DIR:-build-linux}/futureperfect_recomp"
if [ -n "$1" ]; then
  if [ -f "$HERE/seqs/$1.seq" ]; then
    export RECOMP_INPUT_SEQ="@$HERE/seqs/$1.seq"
  elif [ -f "$HERE/recordings/$1.rec" ]; then
    export RECOMP_INPUT_REPLAY="$HERE/recordings/$1.rec"
  else
    echo "No seqs/$1.seq or recordings/$1.rec" >&2
    exit 1
  fi
fi
exec "$EXE"
