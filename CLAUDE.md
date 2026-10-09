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
6. Audio too fast/high: streams are created at 44.1 kHz but FP calls `IDirectSoundStream_SetFormat` per file (menu music 32 kHz stereo, level audio 22042 Hz). `CDirectSoundStream_SetFormat` is now replaced (`hle_dsound_stream.c`, `[DSOUND] ... SetFormat:` log lines).
7. Vertex constants written straight into the push buffer (0x1EA4 load + 0x0B80 data, ~4M registers/min in level 1) are replayed to the host by the push walker (`hle_d3d8_push.c`), and the walk now runs before *every* shadow draw (`shadow_sync_push` in `hle_d3d8.c`), not only at EndPush.
8. Keyboard sticks: `config/input_bindings.json`, copied beside the exe (`build/input_bindings.json`); the exe now also looks beside itself, not just the working directory. WASD = left stick, IJKL = right stick, Space/Z = A, X = B, C = X, V = Y, E/O = L trigger, R/U = R trigger, Q = White, F = Black, LShift/P = stick clicks. (A copy written to `%APPDATA%` from a Claude shell did not reach the user's real profile.)
9. Stale audio: FP stops streams with Pause + `FlushEx` (not intercepted before), so old packets played after the next resume. `CDirectSoundStream_FlushEx` is replaced. `RECOMP_DSOUND_TRACE=1` logs every stream call with timestamps.
10. Blown-out particles (space smoke, dust): NV2A `EXP`/`LOG` are expp/logp (four different components); the translator returned `exp2(x)` everywhere. FP's particle program uses `EXP.y` as frac() for a spin angle. Fixed in `d3d8_vsh.c` and the CPU interpreter `kernel/nv2a_vsh.c`.
11. Dust storm covering the level ("sepia"): vertex programs output `float4 oT0..3` but both pixel shaders take `float3` texcoords with `float fog` packed into TEXCOORD3's `.w`, so every program draw read its fog from `oT3.w` (a constant ~3 → fully fogged). Program VS_OUT texcoords are float3 now.

12. No sound effects (menu clicks, guns, damage): XDK 5849 moved the DirectSound buffer settings 4 bytes later (data +0xBC, size +0xC0, loop +0xCC/+0xD0; read from the game's own `CDirectSoundBufferSettings_SetBufferData`/`SetLoopRegion`). With the 5344 offsets every ADPCM buffer's "size" was a pointer, so none got a model. `hle_dsound.c` now reads the DSOUND build from the XBE header's library table (`[DSOUND] library build 5849: buffer settings at +0xBC`).
13. Push-buffer gather (`hle_d3d8_push.c`) is used whenever a declared attribute's NV2A array is not where the stream 0 view says, not only when the stride is too small.
14. Unfed vertex attributes: NV2A leaves each enabled attribute's current value at the last vertex a draw fetched (xemu models it the same way); the host left them at (0,0,0,1). FP's programs take colour from v2 and many draws feed no v2, so visors/glows were black blobs and whole character bodies were missing. `hle_d3d8_push_after_draw` now forwards those values; the walker also decodes `SET_VERTEX_DATA*`. Side effect seen once: a bright rectangle in the first fade-in frame of level 1.

15. Characters in pieces, grey first-person shotgun, missing character-select models: FP loads its skinning programs into the NV2A program slots by writing the push buffer itself (`0x1E9C`/`0x0B00`/`0x1EA0`), so the host drew palette-skinned vertices (v0 = bone offset -24/-28/-32) with the last XDK-selected rigid program. The walker mirrors program memory and `shadow_sync_program` (`hle_d3d8.c`) binds the push buffer's program before each draw (`RECOMP_HLE_D3D8_PUSH_PROGRAM=0` to disable).

Replay tool additions: `--list-draws` prints fog states, raw vertex dwords and recorded vertex data; `REPLAY_ALL_CONSTANTS=1`, `REPLAY_TEX_DUMP=<id>` (writes `tex_<id>.bin`), `REPLAY_NO_CULL=1`, `REPLAY_NO_Z=1`; `RECOMP_D3D8_PS_SHOW=fograw`. `RECOMP_HLE_D3D8_ARRAY_CHECK=1` compares declarations with NV2A arrays (they agreed in level 1). `RECOMP_HLE_D3D8_ARRAY_TRACE=<stride>|1` (+ `_AFTER=<ms>`) prints declaration vs hardware arrays per draw. Replay: `--list-draws` shows each draw's program hash (`prog`, matches `RECOMP_D3D8_VS_DUMP=1` output), `REPLAY_SHOW_CONSTANTS=<first>[,<n>]`, `--skip-draw a-b`. Push dump reports hardware draw count (it matched intercepted draws).

## Open issues (next steps)

- Cutscene/in-game audio "mis-timed" (user, before fixes 12-15). Measured: the cutscene stream starts at the game's un-pause and completes 2048 ms packets 2.03-2.06 s apart; swaps steady at 60/s (30 fps logic, swaps in pairs); runtime shader compiles in the cutscene ~50 ms. Dialogue/SFX are ADPCM buffers, silent until fix 12, so re-check by ear first. `RECOMP_DSOUND_TRACE=1` lines the stream up against time.
- Black screen after some checkpoints (user). Not reproduced; a candidate is a full-screen pass coloured from an unfed register (black before fix 14).
- Scrolling floor/wallpaper in Challenge "Brain Drain" (user). Not visible in a 1.5 s still-camera capture after fix 15; ask for where/when.
- Stray triangles at a marine's shoulders in level 1 (seen once, before fix 15); bright rectangle in the first fade-in frame of level 1 (fix 14 side effect).
- Main-menu animated background goes black after the profile screen (not re-checked).
- XMV movies are skipped (toolkit built without `third_party/ffmpeg`).
- PS5 controller: XInput only; needs Steam Input/DS4Windows, or a HID backend in `src/input`.

Inputs for the challenge: `14000:a,17000:down+a,21000:down,22000:down,23500:a,27000:a,30000:a,33000:a,36000:a,39000:a` reaches Brain Drain gameplay (~45 s). Arcade character select: `14000:a,17000:down+a,21000:down,22500:a,26000:a,30000:a,34000:a,38000:a` (Cortez at ~42 s).

## Gotchas

- Many toolkit sources are CRLF; edit with the Edit tool or preserve line endings. Python heredocs writing C with `\n` inside string literals have turned into real newlines here — check with `cat -A` after scripted edits.
- `rm -rf` of directories is blocked by the harness; leave scratch dirs (they're gitignored).
- Scripted runs create/modify saves in `game_files/UDATA/45410066` (a throwaway "bbbb…" profile exists).
- Don't commit game data, captures (`*.d3dcap`), dumps or generated C.
