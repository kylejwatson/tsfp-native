# TimeSplitters: Future Perfect, native (unofficial static recompilation)

A native Linux and Windows version of **TimeSplitters: Future Perfect** for the
original Xbox (PAL disc, title `0x45410066`). The game's own code is translated
to C by the [xboxrecomp](https://github.com/kylejwatson/xboxrecomp/tree/tsfp)
toolkit and runs on a PC with a modern renderer, with mouse and keyboard
support, widescreen, HD/4K, MSAA and gamepads. It is not an emulator: the code
is compiled ahead of time.

> **Unofficial fan project.** It is not affiliated with, endorsed by or
> connected to Free Radical Design, Electronic Arts, Deep Silver / Plaion,
> Microsoft or anyone else who owns or published TimeSplitters or the Xbox.
> "TimeSplitters" and "Xbox" are trademarks of their owners.
>
> **This repository and its builds contain nothing from the game**: no code,
> no graphics, sound, text or other data, in source or compiled form. The
> code here is our own, under the GPL. To play, you need your own copy of the
> game (a disc image or the files dumped from your disc). The game's code is
> built on your machine, from your disc, the first time you start it.
> Please don't open issues or pull requests that contain game files, and
> don't ask for them.

## Status

Story and arcade play on Linux (tested through the first missions, Brain Drain
and arcade matches). Fixed and verified so
far includes 3D rendering, fog, skinning, particles, the uplink map, the
colour grading, PAL timing (cutscenes stay in sync), sound effects and music,
and a list of game-logic bugs the translation had introduced (pickups, AI
seats, crashes at particular checkpoints). Additions over the original:

- mouse look (including mounted guns and the moving crosshair) and mouse
  control of the menus;
- 16:9 (the game's own widescreen mode), resolutions up to 4K with sharp
  offscreen effects, MSAA 2x/4x/8x;
- a **PC Settings** screen in the Settings and pause menus (resolution,
  widescreen, antialiasing, texture filtering, frame cap, FPS counter);
- running straight from a disc image (xiso or Redump `.iso`).

Known issues: FMV movies are skipped; a PS5 pad needs Steam Input on Windows;
see the "Open issues" list in [CLAUDE.md](CLAUDE.md).

---

## 1. Players

### Linux (x86-64)

The shareable build is `futureperfect-recomp-linux-x86_64-<version>.tar.gz`,
made by this repository's CI (Actions tab, `dist` workflow). It holds the
runtime, a toolkit "kit" with its own Python and C compiler (Zig), and the
licences. Nothing to install from it besides these system libraries, which
almost every desktop distribution already has:

- a GPU driver with **Vulkan 1.3** (Mesa RADV/ANV, or NVIDIA's);
- SDL2, FreeType, fontconfig, OpenSSL (libcrypto);
- glibc 2.35 or newer (CI builds on Ubuntu 22.04).

Unpack it anywhere and start `futureperfect_recomp`. The first time:

1. A window asks for the game: pick a disc image (`.iso`, xiso or Redump) or a
   folder holding the disc's files (`default.xbe` and `pak/`). Type, paste,
   drop or Browse.
2. It checks that this is the right disc (the PAL release; others are
   refused), then builds the game from it: about 4 minutes, with a progress
   bar. If it fails, "Open log" shows why.
3. The game starts. Later starts load the built game at once.

Where things go: settings in `~/.config/xboxrecomp/titles/45410066.conf`,
saves in `~/.local/share/xboxrecomp`, the built game in
`~/.local/share/xboxrecomp/modules/`, shader caches in `~/.cache/xboxrecomp`.

### Windows

Not shareable yet: the Windows build still links the game's code into the
executable, so for now it has to be built from source (see Contributors).
The plan to bring the Linux-style build to Windows is in
[docs/windows-plan.md](docs/windows-plan.md).

### Controls

Gamepads work as on the Xbox (on Linux any SDL-supported pad: Xbox,
PlayStation, Switch Pro...). Keyboard and mouse, from `input_bindings.json`
beside the executable (a copy in `~/.config/xboxrecomp/` or
`%APPDATA%\xboxrecomp\` takes precedence):

| Input | Xbox |
|---|---|
| Mouse | look (sensitivity, invert in the `"mouse"` section) |
| Left / right mouse button | right trigger (fire) / left trigger |
| Mouse wheel | D-pad left / right (weapon change) |
| Middle mouse | right stick click |
| WASD | left stick (move) |
| IJKL | right stick |
| E / Space | A |
| F | B |
| R | X |
| Q | Y |
| P / Tab | White / Black |
| Left Shift | left stick click |
| Arrows | D-pad |
| Enter | Start |

In menus the mouse is a pointer: hover to select, click for A, right click
for B, the wheel changes values and scrolls lists. **F8** releases the
mouse in game (a click takes it back), **Alt+Enter** toggles fullscreen.
Input is ignored while the window isn't focused.

---

## 2. Contributors (human)

### What's where

| Path | What |
|---|---|
| `xboxrecomp/` | submodule: [kylejwatson/xboxrecomp](https://github.com/kylejwatson/xboxrecomp), branch `tsfp`, the toolkit (lifter, runtime, Xbox kernel, D3D8/DirectSound HLE, Vulkan backend). A fork of phobos665/xboxrecomp. |
| `src/recomp_manual.c` | hand-written replacements and wrappers for lifted game functions (bug fixes, mouse, menus, the PC Settings screen), each with its reason |
| `src/main.c` | entry point: finds the game, the first-run window, loads the game module |
| `config/seeds.json` | extra function entry points for the lifter |
| `config/input_bindings.json` | default bindings, copied beside the executable |
| `re/names.json` | every game function, global and struct we have identified; `re/names.py` turns it into `src/tsfp_names.h` and applies it to a Ghidra project |
| `kit_inputs/` | what the kit needs from the disc, recorded by `make_kit.sh` (hashes and symbol names, no game data); lets CI build the kit without the disc |
| `seqs/`, `play.sh`, `record.sh`, `run_seq.sh` | scripted routes into the game and test runs with frame dumps |
| `make_kit.sh` | makes the shareable build in `dist/` |
| `.github/workflows/dist.yml` | CI: builds `dist/` on a runner with no game files |
| `game_files/` | your extracted disc (ignored, never committed) |

### Building from source (with your disc)

```bash
git clone --recursive https://github.com/kylejwatson/tsfp-native.git && cd tsfp-native
# put your disc's files in game_files/ (default.xbe, pak/, ...)
```

Linux dependencies (Arch/CachyOS; the same packages exist elsewhere):

```bash
sudo pacman -S --needed base-devel cmake ninja clang sdl2 vulkan-headers vulkan-icd-loader shaderc openssl freetype2 fontconfig python python-capstone python-pillow
```

Debian/Ubuntu: `build-essential cmake ninja-build clang libsdl2-dev libvulkan-dev glslc libshaderc-dev libssl-dev libfreetype-dev libfontconfig-dev python3 python3-capstone python3-pil`.

The XDK symbol file is made once (build the XbSymbolDatabase CLI first, see
`xboxrecomp/README.md`):

```bash
git -C xboxrecomp submodule update --init third_party/XbSymbolDatabase
(cd xboxrecomp && python3 -m tools.xdk_symbols ../game_files/default.xbe)
```

Then:

```bash
./relift.sh            # lift the XBE to C in src/recomp/gen (a few minutes)
./build.sh             # -> build-linux/futureperfect_recomp + futureperfect_game.so
./play.sh              # or ./play.sh brain_drain (a scripted route), ./record.sh <name>
```

`FROM=lift ./relift.sh` is enough after adding or removing an `HLE_EXPORT` or a
function in `recomp_manual.c`; a seed change needs the full `./relift.sh`.
Windows: `build_vs.bat` (Visual Studio 2026 + Ninja) builds
`build/futureperfect_recomp.exe` with the game linked in, for local use only.

The executable finds the game at `../game_files`, in `RECOMP_GAME_DIR`, or in
the conf's `game_dir`. The game maps memory at `0x10000`, the default
`vm.mmap_min_addr`; a system that raises it needs
`sudo sysctl vm.mmap_min_addr=65536`.

### The shareable build

`./make_kit.sh` writes `dist/`: the runtime (no game code: it refuses an
executable that contains lifted functions), its libraries, the kit and the
licences. With `game_files/` present it also re-lifts once to check that the
kit reproduces exactly your generated code, and records the result in
`kit_inputs/`. **When a change alters the lift (seeds, lifter, HLE exports),
commit the updated `kit_inputs/` with it**, or CI's kit will refuse players'
lifts. Without the disc (CI) the kit is made from `kit_inputs/` alone.

### How the work is done

- Debugging tools (frame capture and offline replay, RAM dumps, watchpoints,
  push-buffer and shader dumps, swap traces) and the full history of fixes
  with their causes are in [CLAUDE.md](CLAUDE.md); the toolkit's architecture
  is in [xboxrecomp/CLAUDE.md](xboxrecomp/CLAUDE.md).
- Toolkit changes are commits on the submodule's `tsfp` branch: commit and
  push there first, then commit the new submodule pointer here.
- Game-specific fixes go in `src/recomp_manual.c`, never in generated code
  (`src/recomp/gen/` is regenerated and ignored). Name what you find in
  `re/names.json`.
- The repository stores LF line endings (`.gitattributes`; batch files CRLF).
  Leave `core.autocrlf` unset.
- Never commit game files, extracted data, generated C, frame captures
  (`*.d3dcap`) or RAM dumps.

---

## 3. Contributors (AI agents)

Read [CLAUDE.md](CLAUDE.md) and [xboxrecomp/CLAUDE.md](xboxrecomp/CLAUDE.md)
before anything else: they are the working notes (layout, build, run, env
switches, every fix and why, open issues) and are kept current. In short:

- **No game content in any commit, artifact, issue or log you publish.**
  `game_files/`, `src/recomp/gen/`, captures and dumps stay local.
- Overrides only in `src/recomp_manual.c`, each with its reason; identified
  addresses go in `re/names.json` (`python3 re/names.py header` regenerates
  `src/tsfp_names.h`).
- After a seed or lifter change: `./relift.sh`, `./build.sh`, play-test, then
  `./make_kit.sh` and commit `kit_inputs/` with the change.
- Verify by running the game: `./run_seq.sh <name> "<input sequence>" <seconds>`
  (routes in `seqs/` and CLAUDE.md). Windowed runs go inside a nested headless
  compositor (`kwin_wayland --virtual`, see CLAUDE.md), never on the user's
  desktop; don't screenshot the desktop.
- Toolkit commits go to the submodule's `tsfp` branch first, then bump the
  pointer here. Add each fix to CLAUDE.md's numbered list with its cause.

---

## Licence

This project's own code is free software: you can redistribute it and/or
modify it under the terms of the **GNU General Public License, version 3 or
(at your option) any later version** ([LICENSE](LICENSE)). It is built on
xboxrecomp (GPL-3.0, with MIT-licensed upstream code and LGPL-2.1-or-later
parts from xemu, see its NOTICE). The shareable build also carries shaderc and
SPIRV-Tools (Apache-2.0), glslang (BSD/MIT-style), CPython (PSF), capstone
(BSD-3-Clause) and Zig (MIT), all compatible with GPL-3.0; their licences are
in the build's `LICENSES/` and `THIRD_PARTY.txt`.

The licence covers this repository's code only. It grants nothing in the game,
which remains the property of its owners.
