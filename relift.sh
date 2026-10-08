#!/bin/bash
# Re-lift from the disassembly stage (needed after seeds change), then rebuild.
cd "$(dirname "$0")/xboxrecomp"
py -3 scripts/recompile.py ../game_files/default.xbe --from ${FROM:-disasm} --work-dir ../.pipeline --project .. --split 400 \
  --game-name "TimeSplitters Future Perfect" --seeds config/seeds/45410066.json --seeds ../config/seeds.json > ../lift.log 2>&1
