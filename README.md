# TimeSplitters: Future Perfect — static recompilation (Windows and Linux)

A native Windows and Linux build of TimeSplitters: Future Perfect (Xbox, PAL disc, title `0x45410066`),
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
| `build_vs.bat` | build with Visual Studio + Ninja (Windows) |
| `build.sh` | build with CMake + Ninja/Make (Linux) |
| `play.bat` / `play.sh`, `record.bat` / `record.sh` | start the game, optionally on a scripted route or a recording |
| `run_seq.sh` | scripted test run with frame dumps (Git Bash or Linux) |
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

## Linux

The same sources build natively on Linux (x86-64). The renderer is the same
D3D8 translation as on Windows; underneath it, the Direct3D 11 calls it makes
are implemented on **Vulkan 1.3** (`xboxrecomp/src/d3d/vk`), shaders are
compiled from the generated HLSL with shaderc, and the window, keyboard, mouse,
gamepads and audio go through SDL2. Any SDL-supported pad works (Xbox,
PlayStation, Switch Pro...), not only XInput ones.

Dependencies (Arch / CachyOS shown; the same packages exist elsewhere):

```bash
sudo pacman -S --needed base-devel cmake ninja clang sdl2 vulkan-headers vulkan-icd-loader shaderc openssl freetype2 fontconfig python python-pillow
```

Debian/Ubuntu: `build-essential cmake ninja-build clang libsdl2-dev libvulkan-dev glslc libshaderc-dev libssl-dev libfreetype-dev libfontconfig-dev python3 python3-pil`.
Plus a Vulkan 1.3 driver (Mesa RADV/ANV, or NVIDIA's).

```bash
./relift.sh          # optional: regenerate the C (uses python3 when py is absent)
./build.sh           # -> build-linux/futureperfect_recomp
./play.sh            # or: ./play.sh brain_drain, ./record.sh <name>, ./play.sh <name>
```

`build-linux/` keeps a shared checkout's Windows `build/` untouched. Logs go to the
terminal, or to `futureperfect_recomp.log` beside the binary when started from a
desktop launcher. Saves and the emulated hard-disk images live in
`~/.local/share/xboxrecomp` (the profile in `game_files/UDATA` is shared with
Windows); compiled SPIR-V and the Vulkan pipeline cache in `~/.cache/xboxrecomp`.
A bindings file in `~/.config/xboxrecomp/input_bindings.json` takes precedence over
the one beside the binary. `RECOMP_VK_VALIDATION=1` loads the Vulkan validation
layer; `RECOMP_VK_DEVICE=<n>` picks a GPU. The game needs to map memory at
`0x10000`, which is the default `vm.mmap_min_addr` (65536); a system that raises
it needs `sudo sysctl vm.mmap_min_addr=65536`.

## Controls

Keyboard, from `config/input_bindings.json` (copied next to the exe by the build); an XInput pad
also works (on Linux, any SDL game controller).

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
`%APPDATA%\xboxrecomp\input_bindings.json` (Linux: `~/.config/xboxrecomp/input_bindings.json`)
takes precedence over the one beside the exe.

## Line endings

The repository stores LF and `.gitattributes` checks everything out as LF on every OS
(batch files as CRLF); `.editorconfig` tells editors the same. Leave `core.autocrlf`
unset. A checkout made before this with autocrlf can be normalised with
`git rm --cached -r . && git reset --hard` (after committing your work).

## Toolkit changes

All changes to the toolkit live on the `tsfp` branch of the fork; commit there, push, and bump
the submodule pointer here. See `CLAUDE.md` for the history of fixes and the debugging tools.
