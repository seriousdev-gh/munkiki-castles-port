# Munkiki's Castles (Nokia 3410, 2002) — Formats & Mechanics
Reverse-engineering report from `munkiki-castle-ref/decompiled/` (Vineflower output of `Munkiki-s-Castles_J2ME_EN_v1.jar`). All facts cite `path:line`; inferences are marked **[INFERENCE]**.

## 1. CLASS MAP

| File | Role |
|---|---|
| `KKM.java` | MIDlet. Owns all screens/state: boot (`init()` loads `/en` locale strings → 32×StringBuffer, 12 sounds from `/d`, calls `d.ab()`, `c.ab()`, starts `java.util.Timer` with task `a` at **100 ms** fixed rate, `KKM.java:903-1050`), game flow state machine `p()` (app state `aj`) + `q()` (overlay sub-state `ab`) + per-tick `m()`, all 2D screen drawing `j()`/`k()` (number drawing, bonus screens, progress map, level-clear, game-over %), RecordStores (`OPT` options, `MKK` hiscores), cheat/help cycle `r()` incl. hidden debug levels. |
| `a.java` (30 lines) | `TimerTask`: calls `KKM.m()` then `Thread.sleep(25)`. That's the whole game clock — **one logic tick per 100 ms**. |
| `b.java` | `com.nokia.mid.ui.FullCanvas` (96×65, `b.java:232`). Key routing `keyPressed/Released → boolean an[12]` in `KKM` (mapping in §6), word-wrap text→Image builder `b(String,font)` wrapping at 94 px (`b.java:22-74`), 3 fonts (`b.java:227-229`), `paint()` dispatch: `c.j(g)` (3D + sprites) then `d.j(g)` (HUD) then `KKM.j(g)` (screens) (`b.java:193-207`). Owns the `c` renderer. |
| `c.java` | Render engine (Nokia **M3D** + `DirectGraphics`). Owns the voxel draw lists: `bd[]` = blocks (8 ints each), `ba[]` = item billboards (4 ints each); builds/merges blocks (`x()`, `c.java:309-383`), renders the scene each paint (`j()`, `c.java:92-283`), the monkey "lattice" background tiles (`v()`, `c.java:80-95`), a hand-rolled sine `t()` (`c.java:66-77`), and the **static asset loader** `ab()` reading `/imageV3` and `/d` (`c.java:461-515`) plus the 1bpp image decoder `aa()` (`c.java:427-459`). |
| `d.java` | Game logic per level: the 16×16×8 voxel world `cl`(bytes) + `ck`(shorts), monkey position/facing/movement state machine `m()` (`d.java:828-1173`), movement legality (`ad`/`ae`/`af`), pushing (`aj`), breaking (`ag`), falling-block physics (`ac`, `d.java:60-215`), **level bitstream decoder** `an()` (`d.java:537-640`) and **encoder** `ap()` (`d.java:644-745`), MBF level-blob loader `aq()`/static `ab()` (`d.java:746-765`, `d.java:1343-1381`), HUD `j()` (`d.java:1191-1214`), save/load (`ax`/`ay`), camera transform at end of `m()` (`d.java:1155-1172`). |

## 2. LEVEL DATA

### 2.1 Where levels live
**Not** in resource `d`. `d` (4068 B) = sounds + 3D geometry + sprites + animation tables (§3.2). Levels live in **`/MBF`** (1312 B exactly; embedded as `MK_MBF` in `src/assets.h`), loaded once into static `bw` (`d.java:1357-1376`):

```
MBF layout (byte offsets):
0x000  13 B   b1[13]  — per-type bitmask constants (values OR-ed into cells; see 2.3)
0x00D   1 B   n       — number of level blocks
0x00E   n B   b0[n]   — block LENGTH in bytes (1 byte each, includes the id byte)
0x00E+n ...   n blocks back-to-back: [1 B level id][bitstream … b0[i]-1 bytes …]
```
Lookup (`d.aq`, `d.java:746-765`): linear scan; block's first byte compared to requested level number `c6`. Regular levels are ids **1..30**; the cheat system requests ids **90..95** (`KKM.java:46`, `e = {7,8,9,-90,10,-91,11,-92,12,-93,13,-94,14,-95,15,16}`; `KKM.r(26)` warps to level `-value`, `KKM.java:~770-800`), i.e. hidden debug levels *if present in MBF* — bytes not verifiable here (§7).

### 2.2 Cell grid & cell byte (`cl`) — exact, from both reader and writer
Index: `cell(x,y,z) = cl[x + (z<<4) + (y<<8)]`, **x:0..15, y:0..7 (y = height), z:0..15** → 2048 cells (`d.ab`: `cl=new byte[2048]`, `d.java:1355`; index math `d.java:563, 596, 1104`; bounds check `d.al` `d.java:507-510`).

`cl` byte bits (writer `ar()`+`ap()` at save mirrors the reader, proving LSB-first packing):

| bits | meaning | evidence |
|---|---|---|
| 0–2 | solid block type 1..6 (0 = empty). `ak()`=occupied is `cl&7 != 0` (`d.java:502-505`); type→model/color `bd[..+3]`, palette `a3[type-1]` (`c.java:117-141`) |
| 3 | item bit "note" → value 8 (collect, `d.java:884-890`; count `b8`→`c7`) |
| 4 | item bit "key" → 16 (`d.java:894-897`, `c3++`) |
| 5 | item bit "life" → 32 (`d.java:891-893`, `c5++`; suppressed when already taken, flag `bx`/RMS `KLP`, `d.at/au` `d.java:780-826`) |
| 3+4 = 24 | note+key cell, rendered with `warp.png` texture (`c.java:160` `(item>>3)-1`), collected once (`d.java:898-901`, `c2=24`) |
| 3+5 = 40 | **spawn marker** (start x/y/z recorded, `d.an` single-placed branch `d.java:~560`; `am()` masks 40→0 `d.java:512-515`) |
| 6 | falling/moving block part 1 | bits 6+7 both set (`cl & -64 == -64`) = block currently falling/being pushed/moving (`d.ac` `d.java:60-215`; spawn scan `d.an` `d.java:621-631`) |
| 7 | falling/moving part 2; value 128 (`== -128`) alone = cleared "air channel" that falling debris propagates into (`d.ac:100-140`) |

Item billboards live in `c.ba` (4 ints: x,y,z,type|56), solids in `c.bd` (8 ints: x,y,z,type,merge…); `ck` (short) mirrors the id: bits 0–10 = object index+1, bit 11 (2048) = "in motion", bit 12 (4096) = item entry (`d.an` pass 2, `d.java:612-634`).

### 2.3 Level bitstream (exact; LSB-first within each byte, fields packed contiguously)
Reader `d.an(InputStream)` `d.java:537-640`, bit-reader `as()` `d.java:762-777`; writer `ap()`/`ar()` `d.java:644-760` (used for saves, byte-identical format):

```
cf : 2 bits   initial facing dir (→ cx = cf<<16; dir deltas: 0=(0,-1)N,1=(+1,0)E,2=(0,+1)S,3=(-1,0)W, d.java:877-880)
b7 : 16 bits  "par moves" (drives time-bonus formula, KKM.java:392-408)
then for t = 0..12 (type index into b1[13]):
  present : 1 bit
  if t in {5,6,7,8}   (mask 480 = 0b111100000, d.java:549)  → SINGLE placement:
      x:4, y:3, z:4          cl[cell] |= b1[t]
      if b1[t]==40: record spawn (ce,cd,cc)
      if b1[t]==32 and life already used (bx): placement skipped (bits still consumed)
  else                                 → 8 horizontal LAYERS, y=0..7:
      layer present : 1 bit
      x0:4, dx:4 (x1=x0+dx), z0:4, dz:4 (z1=z0+dz)
      if dx+dz < 2: whole rect filled, no per-cell bits
      else: per-cell 1 bit, z0..z1 outer × x0..x10 inner, ascending
      set cells: cl |= b1[t]   (counts: notes if b1[t]==8 → b8; solids if b1[t]&7; items if b1[t]&56)
final: reader pre-reads the first byte as the bit buffer (b2=read()) — so the bitstream is simply byte-aligned from byte 0.
```
**Worked example (structure, not bytes):** level 1 = `bw[b0 offset of block whose id==1] + 1`. Decode by hand: byte0 bits 0–1 = facing, bits 2–7 + following 14 bits = par moves, then 13 presence bits, etc. The 13 `b1` values are the only unknown (§7).

`b1[13]` constraints from code: 4 entries at indices 5..8 are single-place types and must include 40 (spawn) and 32 (life) (`d.an`); the other 9 are rect-fillable. Semantically the game needs exactly: block types 1..6 (6), note 8, key 16, note+key 24, life 32, spawn 40 = 11, i.e. **2 extra entries** — plausibly block+item composites (e.g. `9` = solid 1 + note) and/or falling-debris values `64`/`192` **[INFERENCE from `cl |= b1[t]` allowing any combo; not confirmed]**. Block semantics confirmed in code:
* **1** = pushable box (pushes 1 cell per push, 16-tick slide), **2** = bouncy (sine squash animation `c.java:120-131`; standing on it makes monkey bounce `bs` flag `d.java:1156`, `c.java:99-103`), **3** = breakable-by-hit (only type destroyed by `ag()` `d.java:404-405`), **4** = sliding (pushed, then keeps re-sliding while path clear, `d.ac` `d.java:196-206`; 8-tick push), **5** = solid, never pushable, **merged into one scaled cuboid when the block array overflows** (`c.x()` `c.java:336-383`), **6** = solid, never pushable (`d.aj` refuses 5/6, `d.java:455-459`).

### 2.4 Save format (RecordStore "KKM") — exact byte map (`d.ax/d.ay`, `d.java:1243-1305`)
```
0 c9 lo, 1 c9 hi   moves taken (c9)      2 c8 deaths/retries    3 c7 notes remaining
4 c6 level num     5 c5 lives            6 c4 lo, 7 c4 hi score
8 c3 keys collected                        9.. level bitstream (2.3, no id byte) + flushed partial byte
```
"OPT": `[sound 0/1, lights 0/1, w=checkpoint level]` (`KKM.g/h`). "MKK": hiscore `[n_hi n_lo m_hi m_lo]` (`KKM.java:425-455`). "KLP": 1-byte records = levels whose life item was consumed (`d.at/au`).

### 2.5 Level-1 ASCII dump — RESOLVED. `MBF` bytes extracted from the jar into `src/assets.h` (`MK_MBF`); the engine's decoder (`d_an` in `src/engine.c`) decodes all 36 levels at startup (`tests/logic_test.c` verifies spawn markers for each). The dumper script in §7 remains for inspection.

## 3. RENDERING (`c.java`, Nokia M3D)

### 3.1 M3D API usage (complete inventory; all args fixed-point 16.16, 65536 = 1 unit)
Setup (`c.java:533-548`): `M3D.createInstance()`; `setupBuffers(0x21, 96, 65)`; `clearColor4ub(white)`; `enable(2929 /*DEPTH_TEST*/, 2884 /*CULL_FACE*/)`; `cullFace(1029 GL_BACK)`; `viewport(0,0,96,65)`; `matrixMode(5889 PROJECTION)`, `frustumxi(-96<<11, 96<<11, -65<<11, 65<<11, 196608, 65536000)` → **half-extents 3.0 × 2.03125 at near = 3.0, far = 1000.0**; `enableClientState(32884 VERTEX_ARRAY, 32888 TEXTURE_COORD_ARRAY)`; textures: `new Texture(3553 GL_TEXTURE_2D, 32832 ?, note|key|warp|life .png)` (`c.java:550-595`).
Per frame (`c.j`, `c.java:92-184`): `loadIdentity` + `clear(0x4100)` (`z()`, `c.java:403-425`); ground/water = one huge light-gray (224,224,224) triangle: vertex array `ap` (9 B), `drawArrays(4 /*TRI_STRIP*/,0,3)` scaled `655360 (=10.0) + bq` (pitch-coupled, `d.java:1155` region + `c.z`). Blocks: per `bd` entry `pushMatrix; translatexi(x*20, y*20, z*20)`; **grid→world scale = 20**; type 2 bouncy: `translatexi(0,±1310720)` + `scalexi(65536, 65536±sine, 65536)` squash; type 5: `scalexi(dx<<16, dy<<16, dz<<16)`; then `color4ub(0,0,0,255)`; `vertexPointerub(3,0,a0); drawElementsub(4,72,a_)` = black **outer shell** (20 verts/72 strip idx), `color4ub(a3[type-1]³)` + `vertexPointerub(3,0,a2); drawElementsub(4,36,a1)` = **inner colored cube** (8 verts/36 idx); alter…

### 3.2 Asset container `/d` (4068 B) — byte map (all offsets from code: `f(0)`, `f(554)`, `f(1945)`, `aa(2116/2632/3284)`)
```
0      12 × { len:u16 LE; OTA/OTT Smart Messaging tone program[len] }  (KKM.init KKM.java:953-968)
       bit-level grammar: cmdlen u8; cmd 0x4A(+7-bit 0x1D)/0x3A -> song type 3 bits
       (1=basic+title, 2=temporary); patterns: hdr 3b + id 2b + loop 4b, then
       [count u8][instr 3b: 1=note(pitch4b over C..B @523Hz, dur3b, dotted2b;
       scale instr x0.5/1/2/4; style natural/1.0/1.1/0.7 gate; tempo 5b table
       25..900 BPM @24 PPQ; volume 4b)]. Decoded+rendered by src/sound.c
       (semantics per NokiaOTTDecoder reference; all 12 verified note-by-note).
       → sounds 0..11: 0 note, 1 key, 2 note+key, 3 break, 4 push, 5 bump/land,
         6 game-over, 7 fall, 8 last-note/win-jingle, 9 life, 10 splash, 11 death scream (from i() call sites)
554    model data, in read order (c.ab c.java:470-500):
       a3:6 palette bytes (gray per block type 1..6); a2:24, a1:36 (inner cube v/t);
       a0:60, a_:72 (shell); ap:9 (ground triangle); skip 12, skip 8 (unused);
       az:12, ay:8, ax:12 (warp mesh); skip 18, skip 12, skip 12, skip 24 (unused);
       ay texcoords are quarter-repeat units: {4,4,4,0,0,0,0,4} on the 16-unit
       quad = the warp texture mapped ONCE (device video: one spiral-ribbon glyph
       across the portal; billboards' {0,1} likewise = one mapping).
       aw:12, av:6, au:18, at:12                            … total 373 → ends 927
927    image "bl" (aa(): w:u16 LE, h:u16 LE, 1bpp rows (w>>3)*h) = HUD atlas:
       digits strip (glyph d at src x=24+4d,y0, 5×7 px — `d.av` d.java:1177-1189),
       note-icon+digit box at (0,0,25,14), monkey icons at src y=53 (progress map,
       KKM.java:647-683) and icons at ~y18..40 used by bonus screens (KKM.java:220-260)
~ends  23 sprite entries bi[0..22], each { w:u8, b:u8, h:u8, b:u8, data (w>>3)*h }
       (c.java:490-495). Used: bi[0] 16×53 wipe wall, bi[1] 8×8 & bi[2] 16×16 fall dust
       (palettes bi[12]/bi[13]), bi[3] 24×26, bi[4] 24×14 monkey-walk, bi[5..11] 16×14
       sparkle frames 0..6 with palettes bi[16..22] (c.java:185-260)
1945   cm[][] animation tables (d.ab d.java:1345-1353): count u8, then per state { len u8, len×u8 monkey-tile indices }
       state index cn = 0 walk?, 1 walk, 2 turn-L, 3 turn-R, 4 step-up/jump, 5 push, 6 smash,
       7/8/9 falls, 10 idle(4 s timeout), 11 victory dance, 12 edge-fall, 13 back-step,
       14 slide-push, 15 spawn, 16 death-fall  [mapping from d.m, d.java:952-1150]
2116   1bpp image (high-score / bonus background)      2632 1bpp image (game-over art)
3284   1bpp image (splash logo)                        file end 4068
```
Hardcoded vertex arrays in `c.java` ctor `c.java:523-528`: item box verts ±9/18, texcoords, strip order, and `as{0,18,0,8,10,0,…}` (float legacy? passed nowhere — dead/unused variant).

### 3.3 `/imageV3` verdict (16400 B)
**No header at all** — file = exactly two raw byte arrays: bytes 0..8199 → `bn`, bytes 8200..16399 → `bm` (`c.java:465-468`). It is data for `DirectGraphics.drawPixels(byte[] data, byte[] alpha, offset, stride40, x, y, 40, 40, opts, bitmapSupport=-1)` (`c.v` `c.java:80-95`): 40×40 dithered **monkey/lattice tiles**, tile size = stride·height = 1600 units, tile index `(bh&63)`; bit 6 of frame index (`bh&64`) draws two 20×40 halves, second mirrored (`processOptions 8192`); bit 7 (`bh&-128`) → invert option. **10 frames fit at 1600 B/frame, or 41 tiles at 400 B/frame → [INFERENCE] 1 bpp, `offset` in pixels (1600 px/frame; 41 tiles = 8200 B), mirrored-half tiles indexed from pixel offset 43200 (= tile 27)**. FreeJ2ME thread confirms: the "background lattice" is d…

### 3.4 Draw order per tick (10 Hz; every tick forces `repaint()+serviceRepaints()`, `b.e`)
1. M3D clear; 2. ground triangle (`z()` called *before* scene build — first-person zoom `bq` recomputed then `z()` again); 3. blocks; 4. textured items; 5. `blit` into `Graphics`; 6. break-sparkles (`a7`, 9× bi[5] radial, 20 frames) / fall-dust (`a6`, 8× bi[1]/bi[2]) (`c.java:185-217`); 7. screen wipe (`a4`: 1=sparkle, 2=monkey/bird scene; height = `a5`, sliding wall pairs from bi[0] mirrored) (`c.java:220-276`); 8. lattice `v()` unless a mid-wipe pass already drew it; 9. `d.j` HUD; 10. `KKM.j` overlays.
Camera (`d.m` end, `d.java:1155-1172`): `translatexi(0,-0.5,-20)` then third-person `rotatexi(1310720 /*+20° X pitch*/,X)` or first-person: no pitch, `rotatexi(c0 /*pitch ±45°*/,X)`, `rotatexi(c1 /*yaw*/,Y)`, dolly `translatexi(0,0,c0>>3)`; then `rotatexi(facingDir*90, Y)` and `translatexi(-x*20, -(y-0.5)*20, -z*20)`. Sine helper `t()` uses **65536 = 1 radian** (`205887 = π·65536`, `c.java:66-77`).

## 4. MECHANICS
* **Grid** 16×16×8; monkey occupies integer cell (x, y≥1, z); falling to y<1 = death (`af` returns −111, `d.java:368-382`).
* **Tick** 100 ms; one action at a time; action duration = `cm[cn].length` ticks; motion is linear (total delta ÷ ticks, `d.java:1088-1097`). Facing ∈ {N,E,S,W}; right=`cn 3`, left=`cn 2`, 90° each.
* **Forward move codes** (`ad/ae` `d.java:216-300,340-…`): 0 walk, 1 step-up 1, 2 jump fwd+up1 (thud s5), 3 fwd2+up1, 4 up2 bounce-launch (from type-2 below, `d.bs`), 5 fwd2+up2, −115/−116 fall 1–2 cells (`cn 7/8/9`), −117 off-edge hop (`cn 12`), −111 fatal fall (scream s11, dust `a6`, then `c2=1` → life lost), 14/15 land-on/ride-moving-block.
* **Push** ('5', `aj` `d.java:431-476`): only if target cell has no item-overhead conflict; types 5,6 immovable; type 4 = slide blocks (8-tick move, then auto-continue sliding each tick until blocked, `d.ac` line 196 ff); types 1–3 pushed exactly 1 cell in 16 ticks; push of a type-4 gives `cp=16/cn=14` "continuous" animation; s4 both.
* **Break** ('0', `cp=9 → ag()` at penultimate tick `d.java:1075-1078`): destroys **type 3 only**; on break: cell's item bits (if any) convert to billboard (`b.w`), blocks above are dropped (`ai`), sparkle `a7=0`, s3.
* **Falling physics** (`ac()` every tick): every cell with bits 6|7 set moves down 1 (or sideways into cleared channels) with per-impact "crush check" — a falling block landing on the monkey triggers the `cp=14/15` recovery; landing spreads debris to one free neighbor (`d.java:96-190`). Level start may contain pre-falling blocks (type values with bits 6|7).
* **Collect**: standing on a cell with item bits auto-picks-up each tick (`am` `d.java:512-530`): note 8 → c7-- (all notes ⇒ `c2=2`, s8); key 16 → `c3++` global keys, `c2=16`, s1; life 32 → `c5++`, s9, remembered in `KLP`; note+key 24 → `c2=24`, s2.
* **Win** level: `c2 ∈ {2,16,24}` → celebration (`ab=3`, monkey dance `cn=11`, backlight blink `(ad&4)*5` ms-scale for option-Lights-on, `KKM.java:413-424`), then bonus payout (`ab=4`, `l()` adds 1/10/100 per tick): time bonus `r = 190·par/(2·moves−par)+10`, capped 200, halved for levels 1–25, ×¾ per death retry (`c8`); key bonus 500 (`c2==16`); warp bonus `(5 − level%5)·400` (`c2==24`); +400 if level 30 (`KKM.java:382-408`).
* **Lose**: fatal fall ⇒ `c2=1` → `n()`: `c5--`; if lives remain: respawn same level (`c8++`); else game-over (`ab=7`), shows `%` = `level·90/30 + score/1000` (`KKM.java:676-683`) and saves hiscores (`MKK`).
* **Idle** 4 s ⇒ `cn=10` idle animation (`d.java:1084-1087`). **Checkpoint**: `w` saved into `OPT` at every bonus screen; 'New game' starts level `w` (with level-select if w>1, `r(28)`).
* **30 levels**, warp levels at every 5th (title screen uses `ao[6]` "…or the Magic key" text when `c6%5==0 && c6<25`, `KKM.java:~608`); keys collected (`c3`) unlock levels 26–30 ("25 + keys" logic `KKM.java:417,461`; progress map draws `c3` tower marks `KKM.java:684-690`).
* **Backlight**: ON at `startApp` iff Lights option (`setLights(0,100)`, `KKM.java:~673`); toggled by Options→Lights; **blinks during level-win celebration** (`setLights(0,(ad&4)*5)`); OFF if option off. (`DeviceControl` = Nokia keypad/display light.)

## 5. UI
Screen: **96×65 px monochrome** (`setupBuffers(96,65)`, `viewport`, `setClip(0,0,96,65)`; the remake renders at exactly 96×65 and upscales by integer scale in the window). Gray = software dither (lattice tiles). The M3D flat grays were halftoned by the LCD itself: measured from real-device footage (autocorrelation of the ground field → lattice vectors (4,0)+(2,2) LCD cells) the device runs an **ordered dither with the 4x4 rank matrix** rows `0,8,3,12 / 10,4,13,7 / 2,14,1,9 / 15,6,11,5`, cell black iff `rank < 16*(255-v)/255` -- ground 224 then lights only ranks 0-1 ((0,0)+(2,2): sparse staggered dots), smashable 192 lights (even,even) = "every other pixel, empty rows", faces 128/140 reach the (even,even)+(odd,odd) checkerboard; all three halftones seen on the phone are one threshold sweep of this matrix. The port reproduces it at display stage (Options -> Dithering, F8; on by default). Fonts: proportional small/medium/large (`b.java:227-229`); score drawn as 5 right-aligned digits at x=63−10i, font2 (`KKM.k`). HUD during play (`d.j`): top-left note counter (bl atlas icon + digits, only while notes remain & not finished), debug keypad (kb.png @73,2 + ky.png arrow @75+6c,4+5r) **only on cheat levels** (`db`).
App states `aj`: −1 none, 0 splash (bl image `d@3284`, s10), 2 game, 3 hiscores (image `d@2116`, walking monkey via `bl` crop), 4 help Form, 5 cheat 3D. Game overlays `ab`: 0 hint "(press *)" scroll, 1 level title ("Level %N" + "Collect %N musical notes…"), 2 play, 3 win celebration, 4 bonus counters, 5 progress map (30 slots, 6 rows × 5 cols serpentine at x=row·13+2, y=42−col·10 with row flip `44−y`, monkey icon hopping), 7 game-complete % screen. Menu (`r(25)`): Continue / New game / High scores / **Instructions** / Options; pause (`r(24)`, via 'C'/GAME_B-release or '*'): "Lives left: %N" + Return to game / Restart level (or End game when 0 lives) / Game menu. Options (`r(27)`): Sounds Off/On + Lights Off/On. `de/fr/it/nl-NL/tr-TR` same format, locale chos…

**`/en` file format** (1961 B): 32 strings, **UTF-16BE, terminated by 0x0000**, read pairwise (`KKM.java:921-933`). Full dump (index → text): 0 "Continue" · 1 "New game" · 2 "High scores" · 3 "Instructions" · 4 "Level %N" (`%N` substituted by `b.a` `b.java:12-19`) · 5 "Collect %N musical notes to complete the level." · 6 "Collect %N musical notes or the Magic key to complete the level." · 7 "Collect every musical note to complete each level." · 8 "To move Munkiki around, press 2 to move forward, 4 and 6 to turn left and right and 8 to step back." · 9 "Munkiki will jump over small gaps and climb up one-block high steps." · 10 "Press 5 to push the blocks." · 11 "There are four types of blocks in this game. Breakable blocks can be broken by hitting them with a …

**Sprite textures** (viewed): `note.png` 32×32 — octagonal badge, white "𝄞"-style note + halftone shading; `key.png` 32×32 — white key on black (no alpha, corners not transparent); `warp.png` 32×32 — black disc + white spiral; `life.png` 16×16 — white monkey arm/fist on black; `ky.png` 5×4 arrow; `kb.png` 21×23 3×4-key keypad icon.

## 6. CONTROLS
Key→slot table (`b.c` `b.java:76-127`, slots = `KKM.an[12]`, cleared by `o()`/`e()`): `'0'..'9'` → slots 0–9; `'#'`→10; `'*'`→11; raw GAME_A(−6)→5; GAME_B(−7) on release → menu (`r(24)` pause / `r(25)`); GameCanvas fallbacks: UP→2, DOWN→4, FIRE→6, GAME_A→8, GAME_C→5.
In-game roles (`d.m` `d.java:895-945`): **2** forward · **4** turn left · **6** turn right · **8** step back · **5** push block · **0** hit/break (stick) · **\*** pause menu (⇒ Restart level per help) · **#** first-person look: free camera, **2/8** pitch ±(5°/tick, clamp ±45°), **4/6** yaw, another **#** exits · **C/menu** softkey pause. Cheat: main-menu → Instructions, press **OK** repeatedly through the help pages: pages 7,8,9 then hidden level 90, page 10, level 91, page 11, 92, 12, 93, 13, 94, 14, 95, then pages 15,16 (`e[]` table `KKM.java:46`, `r(26)` `KKM.java:~760-810`); cheat levels 90–94 enable the debug keypad overlay (`db`), and each auto-simulates a key at tick 15 of its intro (table at `KKM.java:341-344`).

## 7. Open items / blocker + exact finisher (for main agent)
**RESOLVED (remake phase):** `/MBF` (1312 B) and `/d` (4068 B) were extracted byte-exactly from `ref/*.jar` into `src/assets.h` (`MK_MBF`, `MK_D`) and all 36 levels decode cleanly; the original binary-channel blocker below is kept for context. Everything else is complete and byte-accurate.

From `munkiki-castle-ref/decompiled/` run:
```
xxd -l 256 MBF        # header + level-1 bytes → paste into report §2.5
xxd -l 256 imageV3    # contract's first-256-byte dump
python munkiki_dump.py  # script below → full level dumps
```
```python
# munkiki_dump.py — MBF level dumper (format: report §2.2-2.3)
import sys
D=open('MBF','rb').read(); assert len(D)==1312
b1=D[:13]; n=D[13]; L=list(D[14:14+n]); bw=D[14+n:]
print('b1=',[hex(v) for v in b1],'levels=',n,'lens=',L)
pos=0; lv={}
for l in L:
    blk=bw[pos:pos+l]; pos+=l; lv[blk[0]]=blk[1:]
class BR:
    def __init__(s,b): s.b,b_i_=b,0; s.i=0
    def __call__(s,k):
        v=0
        for j in range(k): v|=((s.b[s.i>>3]>>(s.i&7))&1)<<j; s.i+=1
        return v
CH={0:'.',1:'#',2:'B',3:'X',4:'S',5:'M',6:'@',8:'n',16:'k',24:'w',32:'L',40:'P'}
for lid in sorted(lv):
    r=BR(lv[lid]); face=r(2); par=r(16); cells={}
    for t in range(13):
        if not r(1): continue
        m=b1[t]
        if 5<=t<=8:
            x,y,z=r(4),r(3),r(4); cells[(x,y,z)]=cells.get((x,y,z),0)|m
        else:
            for y in range(8):
                if not r(1): continue
                x0=r(4); dx=r(4); z0=r(4); dz=r(4)
                for z in range(z0,z0+dz+1):
                    for x in range(x0,x0+dx+1):
                        if dx+dz<2 or r(1): cells[(x,y,z)]=cells.get((x,y,z),0)|m
    print(f'== level {lid}: facing={face} par={par} start=({cells.get((0,0,0),0)}) cells={len(cells)}')
    for y in sorted({k[1] for k in cells},reverse=True):
        print(f' y={y}');
        for z in range(16): print('  '+''.join(CH.get(cells.get((x,y,z),0)&(7|56),'&') for x in range(16)))
```
(`&` = value needing b1 interpretation; block types 1–6 vs items 8/16/24/32/40 per §2.2.)

**Sources**: all cited decompiled lines in `munkiki-castle-ref/decompiled/{KKM,a,b,c,d}.java`; jar member sizes via `ref/Munkiki-s-Castles_J2ME_EN_v1.jar` listing (MBF 1.3 KB, imageV3 16.0 KB = 2×8200, d 4.0 KB); texture/keypad images viewed; FreeJ2ME issue #45 (github API) for 1bpp-texture, dithered-lattice, #=first-person and SCKL-like-sound confirmations.

[Some lines truncated to 768 chars. Use agent://ReverseMunkiki:raw:89-89 to read line 89 whole; cut lines run to line 143, each read the same way]