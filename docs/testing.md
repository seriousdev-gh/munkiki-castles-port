# Tests and verification

Everything except `munkiki` itself runs headless: no window, no assets on
disk, rc=0 means pass. Build them with `nob` (see [building.md](building.md)).

## Regression gates

```powershell
build-nob\logic_test.exe       # headless engine tests: all 36 levels, walking,
                               # checkpoint/resume, game-over deletes the run
build-nob\scene_shot.exe       # headless scene renders -> sc_*.bmp for visual regression
```

| Mode (`scene_shot ...`) | what it proves |
|---|---|
| (none) | full shell path: asserts spawn reached and walking moves the monkey, then dumps `sc_*.bmp` (dumps diffed manually against the last accepted set) |
| `inblock` | camera inside a block cell renders transparent (GL_BACK winding cull); rc=1 if a wall appears |
| `warp` | showroom portal close-up: front/edge-on/back dumps (`warp_ph*.bmp`); rc=1 if the quad stops contributing pixels (front-vs-edge diff <140; 169 with portal, 119 without) |
| `notes` | note billboard spin-rate + round alpha-mask pass (key/warp/life pixels are gated by `diag`) |
| `diag` | MK_DIAG showroom boots and renders; save/resume round-trip: zeroed spawn_dat + `mk_resume_run` must restore the grid cell-identically AND respawn the monkey at its pre-save cell (a showroom missing the (40) marker in cl resumes at (0,0,0) - the "Continue spawned me somewhere else" bug); key/warp/life billboards must contribute pixels (hide-and-diff across 4 spin phases; rc=1 if they vanish - a regression once left all but notes invisible). The showroom carries at most one key/warp/life: those cell types are single-placement in the world bitstream, multiples cannot round-trip |
| `pr` | pause -> save -> resume boot round-trip |

Under WSL2 the headless frames are byte-identical to the MSVC build
(verified via md5 of all `scene_shot` dumps), which is what makes the Linux
runs a valid regression gate.

## Sanitizers and save-file fuzzing

The save-format decoder (`KKM.rms` -> `mk_decode_level`) and the integer
rasterizer are exercised under ASan/UBSan; `tests/fuzz_loader.c` feeds
arbitrary bytes through BOTH save-file entries - the raw level-stream
decoder and the product path (bytes -> `KKM.rms` -> `mk_resume_run`, header
included) - then runs physics ticks and a full raster frame (`r_frame`) on
the crafted world (libFuzzer build, or a plain file driver for repros;
`--gen` seeds one real KKM.rms per all 36 level ids):

```sh
F="-fsanitize=address,undefined -g -O1 -std=c11 -Isrc"
cc $F tests/logic_test.c src/engine.c src/game.c src/render.c src/m3d.c -lm && ./a.out
cc $F tests/scene_shot.c src/engine.c src/game.c src/render.c src/m3d.c -lm && ./a.out
clang -fsanitize=fuzzer,address,undefined -O1 -std=c11 -Isrc -DLIBFUZZER \
     tests/fuzz_loader.c src/engine.c src/game.c src/render.c src/m3d.c -lm -o flf
./flf --gen corpus   # via the non-libFuzzer build: fuzz_loader --gen corpus
./flf -max_total_time=600 corpus
```

Hardening found this way (all invisible to shipped data - frame md5s
unchanged): grid clamping of decoded rectangle coords, the 64-entry cap on
the moving-block list, bounds-safe neighbor probes in the block physics,
out-of-grid monkey guards in the spawn-marker fall probe and item pickup,
and the capacity bound on the save encoder (`KKM.rms` round-trip of a
crafted world truncates instead of smashing the stack).

## Live verification on Windows

The game runs from the repo root (save files land there). Keyboard input and
screenshots can be scripted against the window from PowerShell
(`SendKeys` + `PrintWindow`); note that the window must be the foreground
process for key delivery, and raylib's `Esc` exit key must never be sent
accidentally. In-app debug helpers: `MK_LIVEDUMP=1` dumps the exact LCD
framebuffer to `mk_live.bmp` once (~6 s after launch); `MK_DIAG=1` skips the
menu and spawns the monkey in a showroom level that shows every block shape
(1-7) and every collectible billboard (note/key/warp/life) on one row at
z=10, facing it (`facing` is quarter-turns of 65536 and 0 looks toward -z) -
the same view `scene_shot diag` prints as ASCII; setting `MK_AUTONAV=1` before
launch plays a scripted navigation (it also dumps the LCD frame); `MK_SCR=1`
traces key input and state to stderr, `MK_TRI=1` the full M3D triangle
pipeline (projection, winding-cull skips, `NPKILL` lines for near-plane
drops); `F8` toggles the LCD halftone dither (default off — smooth gray),
`F7` toggles the backlight tint (device "Lights").
