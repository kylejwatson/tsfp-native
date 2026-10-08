# TimeSplitters: Future Perfect — static recompilation (working notes)

Native Windows build of TS:FP (Xbox, title 0x45410066, XDK 5849, entry 0x0038024D),
lifted to C by the xboxrecomp toolkit. Read `xboxrecomp/CLAUDE.md` too: it is the
toolkit's own context (architecture, debug table, env switches) and is not repeated here.

## Layout

| Path | What |
|---|---|
| `xboxrecomp/` | submodule: **phobos665/xboxrecomp** (a fork with D3D8 HLE, FP seeds), pinned to `be78f4c`. Not sp00nznet upstream — that one has no D3D8 HLE and cannot render this game. |
| `patches/xboxrecomp-fp-changes.patch` | **our changes to the toolkit** (we can't push to the fork). `./apply_patches.sh` after a fresh submodule checkout. Regenerate after editing the submodule: `git -C xboxrecomp add -N <new files>; git -C xboxrecomp diff --binary HEAD > patches/xboxrecomp-fp-changes.patch` |
| `src/main.c`, `src/recomp_manual.c` | title project (from `xboxrecomp/titles/futureperfect`). Overrides live only in `recomp_manual.c`, each with its reason. |
| `config/seeds.json` | our extra function seeds (toolkit's `config/seeds/45410066.json` is also passed) |
| `game_files/` | the user's extracted disc (ignored). `game_files/default_xdk_symbols.json` comes from `py -3 -m tools.xdk_symbols` (needs the XbSymbolDatabase CLI built once, see README). |
| `src/recomp/gen/`, `.pipeline/`, `build/` | generated (ignored) |
| `ghidra/` | user's Ghidra project (ignored, unused so far) |

## Build / lift / run

- Build: `cmd.exe //c build_vs.bat` (VS 2026 "18" + Ninja; writes `build/futureperfect_recomp.exe`). ~10 min full, seconds incremental. Builds are long: run in background and poll `tasklist` for ninja/cl.
- Re-lift: `./relift.sh` (from disasm; needed after seed changes). `FROM=lift ./relift.sh` after adding/removing an `HLE_EXPORT` in `xboxrecomp/src/hle` or a function in `recomp_manual.c`.
- The exe finds the game at `..\game_files` relative to itself (or `RECOMP_GAME_DIR`).
- Scripted test run with frame dumps + contact sheet: `./run_seq.sh <outdir> "<RECOMP_INPUT_SEQ>" <seconds> [dump_every]` → `<outdir>/montage.png`, log `<outdir>.log`. `MINDRAWS=n` and `EXTRA="ENV=1 ..."` are honoured. Always muted.
- Input sequences (needs an existing profile in `game_files/UDATA`, i.e. "Player 1" at index 1):
  - story level 1 (gameplay in ~40 s): `14000:a,17000:down+a,20000:a,23000:a,26000:a,29000:a,36000:start` (the `start` skips the cutscene; a second `start` pauses)
  - arcade game-type menu: `14000:a,17000:down+a,21000:down,22500:a,26000:a`
- Input is ignored while the window is unfocused (our change); `RECOMP_INPUT_SEQ` is unaffected. Keyboard: Z=A, X=B, arrows, Enter=Start.

## Debugging tools that paid off

- Frame capture + offline replay: `RECOMP_D3D8_CAPTURE=caps/c.d3dcap RECOMP_D3D8_CAPTURE_SWAP=3000 RECOMP_D3D8_CAPTURE_EVERY=400 RECOMP_D3D8_CAPTURE_MINDRAWS=50`, then `build/xboxrecomp/src/replay/d3d8_replay.exe caps/c_N.d3dcap --out rep/x_` (`--list-draws`, `--no-combiners`, `--skip-draw n`). With `RECOMP_D3D8_PS_SHOW=t0|v0|r0|fog|foga|fogon` to view one combiner register, `RECOMP_D3D8_PS_DUMP=1` for HLSL (+ raw final-combiner words). `xboxrecomp/scripts/bmp_to_png.py` to view BMPs.
- `RECOMP_HLE_D3D8_DECL_DUMP=1` (vertex declarations, stride-skip details), `RECOMP_HLE_D3D8_PUSH_DUMP=1` (methods in the push buffer), `RECOMP_AUDIO_DUMP=<dir>` (raw PCM per host voice), `RECOMP_DSOUND_STREAM_RATE=<Hz>` (force stream rate).
- The `[HLE-D3D8] shadow: ... skipped ...` stats line is the first thing to read for rendering problems.
- Unresolved `[ICALL]` target → `py -3 -m tools.seed_from_log <log> ../game_files/default.xbe --functions ../.pipeline/disasm/functions.json --seeds ../config/seeds.json` (from `xboxrecomp/`), keep only new entries, `./relift.sh`.

## What has been fixed (and why) — details in git log

1. Level-load crash: unseeded function 0x5FCF0; MSVC `memcpy` (sub_000245C0) jumps into its own middle → native override in `recomp_manual.c`.
2. No 3D geometry: FP writes its NV2A vertex arrays itself (sub_0001EB00 via BeginPush/EndPush, one array per attribute). `hle_d3d8_push.c` follows the push buffer and gathers those arrays into the declaration layout when the stream-0 view can't hold the vertex.
3. Sepia world: FP's final combiner is `fog.a, fog.rgb, r0` (0x13030C00), opposite to the XDK default; fog-disabled now means "no fog" for either lerp form (`d3d8_combiners.c`).
4. Green/yellow icons: L8/A8L8/AL8/L16 textures are expanded to BGRA (L,L,L,A) at upload (`d3d8_resources.c`).
5. Background input: keyboard/pad gated on window focus (`input_bindings.c`).

## Open issues (next steps)

- **Audio "sped up"** (user report). Measured: everything audible is ONE stereo 44.1 kHz Xbox-ADPCM DirectSound stream (music+dialog), decoded per spec, fed in real time at the rate the game declares; the game never calls stream SetFrequency. So the data path looks right. Unconfirmed by ear — ask the user to compare `audio_check_menu_music.wav` with the real game, or try `RECOMP_DSOUND_STREAM_RATE=32000`. The emulated APU (`src/apu`) is effectively silent (its XAudio2 path only outputs the unused software mixer); the looping ADPCM *buffers* (`format 04010069`, "not modelled", garbage size from `setting(SET_SIZE)`) are never voiced — dialog/SFX may be missing because of that.
- Canyon walls in level 1 look flat/untextured (ground and guns are fine) — likely a second texture stage or a combiner input; start with a capture + `PS_SHOW`.
- Main-menu animated background goes black after the profile screen; arcade character-select 3D models (user report, not re-checked since the vertex-array fix).
- Arcade mode load crash (user report, not re-tested after the level-load fixes).
- XMV movies are skipped (toolkit built without `third_party/ffmpeg`).
- PS5 controller: XInput only; needs Steam Input/DS4Windows, or a HID backend in `src/input`.

## Gotchas

- Many toolkit sources are CRLF; edit with the Edit tool or preserve line endings. Python heredocs writing C with `\n` inside string literals have turned into real newlines here — check with `cat -A` after scripted edits.
- `rm -rf` of directories is blocked by the harness; leave scratch dirs (they're gitignored).
- Scripted runs create/modify saves in `game_files/UDATA/45410066` (a throwaway "bbbb…" profile exists).
- Don't commit game data, captures (`*.d3dcap`), dumps or generated C.
