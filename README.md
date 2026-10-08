# TimeSplitters: Future Perfect - static recompilation (Windows)

Toolkit: `xboxrecomp/` is a git submodule of the phobos665 fork of xboxrecomp (D3D8 HLE, XDK symbol
database, TS:FP seeds), pinned to be78f4c. Clone with `git submodule update --init xboxrecomp third_party/XbSymbolDatabase`
(run the second path from inside `xboxrecomp`; xemu/nxdk/Cxbx-Reloaded are not needed). The XbSymbolDatabase CLI must be built once for `tools.xdk_symbols`.
Game files (your own disc) go in `game_files/`.

Re-lift (only when seeds/symbols/toolkit change), from `xboxrecomp/`:

    py -3 -m tools.xdk_symbols ../game_files/default.xbe
    py -3 scripts/recompile.py ../game_files/default.xbe --work-dir ../.pipeline --project .. --split 400

Build (VS 2026 + Ninja): `build_vs.bat`  ->  `build\futureperfect_recomp.exe`
Run from anywhere; it finds `..\game_files` relative to the exe, or set RECOMP_GAME_DIR.

## Toolkit patches

`patches/*.patch` are changes to the xboxrecomp submodule that are not upstream yet (run
`./apply_patches.sh` after a fresh submodule checkout; `FROM=lift ./relift.sh` after changing HLE exports).
`xboxrecomp-fp-changes.patch` makes the D3D8 shadow renderer read the NV2A vertex arrays out of the
push buffer when the title sets them itself (Future Perfect does), which is what makes its 3D geometry draw.

It also stops the game reading the keyboard and pad while another window has focus (set `RECOMP_INPUT_BACKGROUND=1` to restore the old behaviour).
