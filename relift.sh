#!/bin/bash
# Re-lift from the disassembly stage (needed after seeds change), then rebuild.
# Works from Git Bash on Windows (py -3) and on Linux (python3); PY overrides.
cd "$(dirname "$0")/xboxrecomp"
if [ -z "$PY" ]; then
  if command -v py >/dev/null 2>&1; then PY="py -3"; else PY=python3; fi
fi
$PY scripts/recompile.py ../game_files/default.xbe --from ${FROM:-disasm} --work-dir ../.pipeline --project .. --split 400 \
  --game-name "TimeSplitters Future Perfect" --seeds config/seeds/45410066.json --seeds ../config/seeds.json > ../lift.log 2>&1
