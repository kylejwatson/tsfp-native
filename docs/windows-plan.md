# Windows: plan for the switch back

The last Windows build and play test was on 2026-10-10/11, just before the
Linux port (toolkit `3e60c97`, title `316cc11`). Since then about 80 toolkit
files changed (+13k lines), 27 of them in `_WIN32` code paths, and the build
system changed from one executable to a runtime plus a game module built from
the player's disc. Work through the steps in order: each one starts from a
build that works.

## 0. Bring the checkout over

- Pull both repos (`git pull`, `git submodule update`). Line endings are LF by
  `.gitattributes`, so nothing should show as modified; if it does, renormalise
  (README, "Line endings").
- Ghidra project: delete a stale `tsfp.lock` and set `tsfp.rep/project.prp`'s
  OWNER back to `mail` (CLAUDE.md, Layout).
- Re-lift on Windows (`relift.sh` from Git Bash, `py -3`). The lift must equal
  `kit_inputs/inputs.json`'s digests (CRLF is ignored): a difference means the
  lift is not deterministic across OSes, which would break kits.

## 1. Static build (game linked in, as before): build and fix

`XBOXRECOMP_GAME_MODULE` stays OFF on Windows for this step. Expect compile
errors in code that was only built on Linux since:

- **Guest register block** (`xbox_memory_layout.h`, `recomp_types.h`): MSVC
  with `__declspec(thread)` on a struct with designated initialisers
  (`RECOMP_REGS_INIT`); `XBOXRECOMP_REGS_IN_MODULE` defaults ON on WIN32 but
  only takes effect with the module, so check the static build defines the
  block in the exe.
- **Disc images** (`kernel_disc.c`, `kernel_path.c`, `kernel_file.c`'s
  `xbox_host_Nt*` rename macro, `kernel_bridge.c` routing): the Windows file
  backend now sits behind the disc layer.
- **First-run code in `src/main.c`** (`_WIN32` branches for the module cache,
  Python path `kit\python\python.exe`, conf `game_dir`).
- **Settings/conf** (`recomp_config.c`), **pointer mode** (`recomp_mouse.c`:
  Windows raw-input path must release/show the cursor and route buttons to the
  menu), **FPS/overlay**, `exit_trace.c`, `recomp_sample.c`, `ohci.c`,
  `xbox_watchpoint.c` (register externs became the block).
- **D3D11 paths written against the Vulkan backend's behaviour**: MSAA
  (`ResolveSubresource`, `CheckMultisampleQualityLevels`), scaled render
  targets and texel sizes, the feedback copy (`hle_d3d8_texture_feedback`),
  frame-buffer keeping, dynamic buffers.
- `-fno-math-errno` is non-MSVC only; nothing to do.

## 2. Play test (static build)

Each was last verified on Linux only. Compare with the numbered fixes in
CLAUDE.md:

| Area | Test |
|---|---|
| Boot, menus, music | title screen to main menu, menu clicks audible |
| Level 1 | `play.bat story1`: geometry, fog, characters whole, intro 120 s in sync (16) |
| Brain Drain | the challenge route in CLAUDE.md |
| Widescreen / HD / 4K | `RECOMP_WIDESCREEN=1`, `RECOMP_RES_SCALE=1080p` and `4k`; graded picture fills 16:9 (28) |
| Uplink map | mission 3: sharp walls at 1080p (32), no corner map/flicker (25-27) |
| Teleport shimmer | mission 3 opening at 1080p: no block noise (34; D3D11 sampled black before) |
| MSAA | `RECOMP_MSAA=4` at 1080p (33) |
| Mouse look | smooth turning while walking (29), mounted guns (21), moving crosshair (31), level 2 camera pan |
| Menu mouse | hover/click/wheel, value rows, list scrolling, briefing click (35) |
| PC Settings | both menus; values written to the conf and applied on restart (36) |
| Disc image | `RECOMP_GAME_DIR=<xiso>` and a Redump image; saves go to `<save dir>\E` (39) |
| Audio | level ambience and stream SetFormat/FlushEx (6, 9), effects (12) |
| Pacing | `RECOMP_FPS=5`: 60 fps, vblank 60 Hz |

## 3. Game module on Windows (the shareable build)

The register block was the blocker and is done (CLAUDE.md 40). Left:

- **CMake** (`xboxrecomp/CMakeLists.txt` line ~81 `FATAL_ERROR`, title
  `CMakeLists.txt` module branch): a `MODULE` DLL target with clang-cl/MSVC
  flags equivalent to `-fwrapv -fno-strict-aliasing` (MSVC has no strict
  aliasing; signed wrap is the default codegen in practice, check `/O2`),
  no `-Bsymbolic` (DLLs bind internally already), `/bigobj`.
- **Module -> runtime imports**: Linux resolves them against the exe at load.
  On Windows the exe must export its runtime (`ENABLE_EXPORTS` +
  `WINDOWS_EXPORT_ALL_SYMBOLS`, or a `.def` generated from the HLE markers)
  and the module links its import library (`futureperfect_recomp.lib`). That
  import library must go into the kit, because the player's build links it.
  Data imports (`HLE_IMPORT_VAR`) need `__declspec(dllimport)` or go through
  the existing per-file slots (`RECOMP_MODULE_HOST`).
- **Loader** (`recomp_module.c` `_WIN32` branch): `LoadLibraryW` /
  `GetProcAddress`, `recomp_regs_attach` on the loading thread, and every
  thread created later gets its block from the module's TLS (TLS in a
  dynamically loaded DLL is fine since Vista).
- **Fault reports**: `SymFromAddr` must find the module's functions (load its
  symbols with `SymLoadModuleEx`, or ship a PDB/DWARF the player builds).
- **Player-side compiler**: the kit's Zig targets `x86_64-windows-gnu`
  (mingw). Check that a mingw-built DLL works in an MSVC exe for what crosses
  the boundary (plain C calls, no `FILE*`/CRT heap ownership crossing; `long`
  is 32-bit on both). Alternative: build the exe with clang/mingw too.
- **`build_module.py` on Windows**: output `futureperfect_game.dll`, link
  against the kit's import library, module cache under `%LOCALAPPDATA%`.
- **First-run window**: `setup_sdl.c` is built only with the SDL host. Either
  link SDL2 on Windows for the setup window only (ship `SDL2.dll`, zlib
  licence) or write a small Win32 version of `recomp_setup.h`. Its build
  child uses `posix_spawn` + process groups: Windows needs `CreateProcessW`
  with pipes and a job object (kill on Escape, die with the parent).
  Browse: `IFileOpenDialog` instead of kdialog/zenity.
- **Kit**: `make_kit.sh` refuses Windows today; remove that, check for lifted
  code with `dumpbin /exports` (or `llvm-nm`) instead of `nm -D`, bundle
  `fetch_python.py --os windows` and `fetch_zig.py --os windows`, no shaderc
  DLLs (D3D11 compiles HLSL itself), and the Windows define
  (`RECOMP_REGS_IN_MODULE`), so the Windows kit id differs from Linux's while
  `reference.json` stays the same.
- Test: first run from an empty `%LOCALAPPDATA%` with no Python or compiler on
  `PATH`, both a folder and an image, then the step 2 table again.

## 4. CI

- Add a `windows` job to `.github/workflows/dist.yml` on `windows-2025`:
  checkout with submodules, MSVC dev shell (`ilammy/msvc-dev-cmd` or
  `vswhere` + `vcvars64.bat`), Ninja, configure with the module ON and no
  generated code (runtime only), `make_kit.sh` under Git Bash (inputs mode),
  zip `dist/` as `futureperfect-recomp-windows-x86_64-<sha>.zip`.
- Make the "no game files" check portable (PowerShell `Test-Path`).
- The toolkit's own CI (`xboxrecomp/.github/workflows/ci.yml`) has a Windows
  MSVC job (windows-2022) besides the Python tests and the Linux build; read its
  result on the latest push first (if Actions is enabled on the fork): it is
  the cheapest early warning for step 1.
- Later: a release workflow on tags that attaches both archives, and
  attestations/checksums for the downloads.
