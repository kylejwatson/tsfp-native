# TimeSplitters: Future Perfect — static recompilation (Windows)

A native Windows build of TimeSplitters: Future Perfect (Xbox, PAL disc, title `0x45410066`),
lifted to C with the [xboxrecomp](https://github.com/kylejwatson/xboxrecomp/tree/tsfp) toolkit.
You need your own copy of the game.

## Repository layout

| Path | What |
|---|---|
| `xboxrecomp/` | submodule: [kylejwatson/xboxrecomp](https://github.com/kylejwatson/xboxrecomp), branch `tsfp` (a fork of phobos665/xboxrecomp with the D3D8 HLE plus the Future Perfect fixes) |
| `src/main.c` | title entry point |
| `src/recomp_manual.c` | hand-written overrides of lifted functions, each with its reason |
| `config/seeds.json` | extra function seeds for the lifter |
| `config/input_bindings.json` | keyboard bindings, copied next to the exe at build time |
| `relift.sh` | regenerate the C from the XBE |
| `build_vs.bat` | build with Visual Studio + Ninja |
| `run_seq.sh` | scripted test run with frame dumps |
| `game_files/` | your extracted disc (not committed) |

## Setup

```bash
git clone <this repository> mygame && cd mygame
git submodule update --init xboxrecomp
git -C xboxrecomp submodule update --init third_party/XbSymbolDatabase
```

xemu, nxdk and Cxbx-Reloaded are not needed. Build the XbSymbolDatabase CLI once (see
`xboxrecomp/README.md`), then extract the disc into `game_files/` and generate its symbol file:

```bash
cd xboxrecomp && py -3 -m tools.xdk_symbols ../game_files/default.xbe
```

## Lift and build

```bash
./relift.sh          # full lift from disassembly (after seed changes)
FROM=lift ./relift.sh  # after adding/removing an HLE export or a recomp_manual.c function
build_vs.bat         # VS 2026 + Ninja -> build/futureperfect_recomp.exe
```

The exe finds the game at `..\game_files` relative to itself, or in `RECOMP_GAME_DIR`.

## Controls

Keyboard, from `config/input_bindings.json` (copied next to the exe by the build); an XInput pad
also works.

| Key | Xbox |
|---|---|
| WASD | left stick |
| IJKL | right stick |
| Space / Z | A |
| X | B |
| C | X |
| V | Y |
| E / O | left trigger |
| R / U | right trigger |
| Q | White |
| F | Black |
| LShift / P | left / right stick click |
| Arrows | D-pad |
| Enter | Start |
| Backspace | Back |

Input is ignored while the window does not have focus. A config in
`%APPDATA%\xboxrecomp\input_bindings.json` takes precedence over the one beside the exe.

## Toolkit changes

All changes to the toolkit live on the `tsfp` branch of the fork; commit there, push, and bump
the submodule pointer here. See `CLAUDE.md` for the history of fixes and the debugging tools.
