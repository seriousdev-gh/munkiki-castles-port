// logic_test.c - headless smoke tests for the engine port (no raylib).
// Exercises: MBF asset load, all-level decode, spawn placement, walking
// advances the monkey + move counter, save/resume round-trip.
#include <stdio.h>
#include <string.h>
// Plus the shell (game.c) lifecycle: soft-key routing, New Game hint/map/
// card flow, pause save, app restart + Continue resume.
#include "engine.h"
#include "render.h"
#include "game.h"

void platform_sound(int id) { (void)id; }   // engine->shell sound hook stub

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); \
    printf("  [%s:%d]\n", __FILE__, __LINE__); fails++; } } while (0)

// ---- shell helpers: phone keys arrive as press+release pairs; game_paint
// is a state transition in this port (pause lives there). Repaint before the
// timer tick, like the real app's frame loop: a canvas repaint is guaranteed
// between a key press and the tick whose rel-clear could swallow it.
static unsigned char lcd_fb[96 * 65];
static void run(int n) { for (int t = 0; t < n; t++) { game_paint(lcd_fb); game_tick25(); } }
static void chord(const int *keys, int n, int hold)
{
    for (int i = 0; i < n; i++) game_key(keys[i], 1);
    for (int t = 0; t < hold; t++) game_tick25();
    for (int i = n - 1; i >= 0; i--) game_key(keys[i], 0);
    run(6);
}
static void soft(int hold)  { chord(&((int){ -1 }), 1, hold); }   // select soft key
static void tap(int k)      { chord(&k, 1, 0); }   // instant: soft-key/List keys
// Canvas-polled keys must be held across frames: keyReleased sets the b.d[]
// flag whose rel-clear wipes an[] in the same tick, so a zero-hold tap on
// '2'/'*' would self-clear before any mk_tick100()/paint ever sees the press.
static void tap_h(int k)    { game_key(k, 1); run(4); game_key(k, 0); run(6); }
static void wait_ab(int want, int limit)        // pump until sub-state changes
{
    for (int t = 0; t < limit && game_dbg(0) != want; t++) run(1);
}

static void shell_lifecycle(void)
{
    remove("KKM.rms"); remove("OPT.rms"); remove("KLP.rms"); remove("MKK.rms");
    game_init();
    run(20);
    // ---- PC keyboard remap (live QWERTY): 'r' = pause/restart (phone '*'),
    // 'e' = smash blocks (phone '0'), 'f' = push blocks (phone '5'). The raw
    // phone codes '*' (42) and '0' (48) ALSO route (b.java:101-115): main.c
    // sends '*' for '/' and the raw digit row '0'..'9'.
    game_key('e', 1);
    CHECK(mk_an[MK_KEY_0] == 1, "keymap: 'e' must press the smash key");
    game_key('e', 0); mk_an[MK_KEY_0] = 0;
    game_key('r', 1);
    CHECK(mk_an[MK_KEY_STAR] == 1, "keymap: 'r' must press the pause key");
    game_key('r', 0); mk_an[MK_KEY_STAR] = 0;
    game_key('f', 1);
    CHECK(mk_an[MK_KEY_5] == 1, "keymap: 'f' must press the push key");
    game_key('f', 0); mk_an[MK_KEY_5] = 0;
    game_key('0', 1);
    CHECK(mk_an[MK_KEY_0] == 1, "keymap: raw '0' must press the smash key");
    game_key('0', 0); mk_an[MK_KEY_0] = 0;
    game_key('*', 1);
    CHECK(mk_an[MK_KEY_STAR] == 1, "keymap: raw '*' must press the pause key");
    game_key('*', 0); mk_an[MK_KEY_STAR] = 0;
    soft(0);                                     // splash -> main menu
    CHECK(game_dbg(1) == 25 && game_dbg(6) == 0, "fresh boot: menu/af wrong");
    // Menu nav = S40 List: arrows move highlight (UP wraps), phone digits
    // jump the highlight WITHOUT selecting; only the soft key selects.
    tap(-7);                                     // DOWN
    CHECK(game_dbg(4) == 1, "menu: DOWN did not move highlight (%d)", game_dbg(4));
    tap(-3);                                     // UP
    CHECK(game_dbg(4) == 0, "menu: UP did not move highlight (%d)", game_dbg(4));
    tap(-3);                                     // UP wraps to last (nitems=4 fresh)
    CHECK(game_dbg(4) == 3, "menu: UP did not wrap (%d)", game_dbg(4));
    tap('2');                                    // digit jumps highlight only
    CHECK(game_dbg(4) == 1 && game_dbg(1) == 25,
          "menu: digit must jump, not select (sel=%d ai=%d)", game_dbg(4), game_dbg(1));
    tap(-7); tap(-7); tap(-7);                   // back to item 0
    CHECK(game_dbg(4) == 0, "menu: DOWN wrap failed (%d)", game_dbg(4));
    // Options form (port addition): 3 rows Sounds/Lights/Dithering; arrows move
    // the row cursor (DOWN wraps mod 3), select toggles the row. On == device
    // halftone (1); a fresh boot defaults to smooth gray (dither OFF).
    tap('4');                                    // digit jumps to "Options"
    CHECK(game_dbg(4) == 3, "menu: digit 4 did not jump to Options (sel=%d)", game_dbg(4));
    soft(0);                                     // open the options form
    CHECK(game_dbg(1) == 27, "options form did not open (ai=%d)", game_dbg(1));
    CHECK(game_dither() == 0, "fresh boot: dithering must default OFF (%d)", game_dither());
    tap(-7); tap(-7);                            // DOWN DOWN -> Dithering row
    CHECK(game_dbg(16) == 2, "options: row cursor did not reach Dithering (%d)", game_dbg(16));
    soft(0);                                     // toggle On (device halftone)
    CHECK(game_dither() == 1, "dithering toggle On failed (%d)", game_dither());
    soft(0);                                     // toggle Off (leave state default)
    CHECK(game_dither() == 0, "dithering toggle Off failed (%d)", game_dither());
    tap(-7);                                     // DOWN wraps to row 0 (Sounds)
    CHECK(game_dbg(16) == 0, "options: cursor did not wrap (%d)", game_dbg(16));
    soft(0);                                     // toggle Sounds off...
    soft(0);                                     // ...and back on (leave state default)
    tap(-2);                                     // right soft: back to main menu
    soft(0);                                     // item 0 = New Game (on release)
    CHECK(game_dbg(0) == 0, "new game: expected hint ab=0 got %d", game_dbg(0));
    // STR from player: Enter instead of Space on the hint must act like any
    // other key (advance to map) and must NOT leak into the pause menu.
    // Original quirk: keyReleased(-7) ran r(24) unconditionally (p(2) set
    // aj==2 already at the hint); pause-on-hint + "Return to game" skipped
    // the map's c6++ card -> stats "Level 0" and the same level replayed.
    game_key(-1, 1); game_key(-1, 0); run(6);      // instant press+release pair
    CHECK(game_dbg(1) == 0, "Enter on hint leaked into menu (ai=%d)", game_dbg(1));
    CHECK(game_dbg(0) == 5, "Enter on hint did not advance to map (ab=%d)", game_dbg(0));
    run(5);                                      // stays on the progress map
    CHECK(game_dbg(0) == 5, "expected progress map ab=5 got %d", game_dbg(0));
    wait_ab(1, 200);                             // map auto-exit: level 1 card
    CHECK(game_dbg(0) == 1 && g_dungeon.level_no == 1, "card: ab=%d c6=%d want 1/1", game_dbg(0), g_dungeon.level_no);
    // aw()->ax() checkpoint (Java d.java:1226): the run is saved on every
    // level load, BEFORE the player ever pauses. Losing this loses progress.
    CHECK(mk_save_exists(), "no checkpoint save after level load without pause");
    tap_h('r'); run(30);                         // card -> dungeon
    CHECK(game_dbg(0) == 2 && g_dungeon.level_no == 1, "dungeon: ab=%d c6=%d", game_dbg(0), g_dungeon.level_no);
    int lives0 = g_dungeon.lives;
    int moves0 = g_dungeon.moves, x0 = g_dungeon.pos_x;
    for (int k = 0; k < 3; k++) tap_h('2');   // 3 tiles forward
    CHECK(g_dungeon.moves > moves0 && g_dungeon.pos_x != x0, "walk did nothing (c9=%d x=%d)", g_dungeon.moves, g_dungeon.pos_x >> 16);
    CHECK(g_dungeon.lives == lives0, "walking level 1 forward killed the monkey");
    for (int t = 0; t < 100 && (g_dungeon.action_ticks != 0 || (g_dungeon.pos_x & 0xFFFF)); t++) run(1);  // settle
    int pos_x_saved = g_dungeon.pos_x;                   // save stores the marker cell
    tap_h('r'); run(10);                       // pause (paint) -> mk_save_run save
    CHECK(mk_save_exists(), "pause did not save KKM record");
    tap(-1); run(20);                          // Return (press+release in list)
    CHECK(game_dbg(1) == 0 && game_dbg(0) == 2, "resume from pause failed");
    game_key(-1, 1); run(2); game_key(-1, 0); run(4);  // Enter press+release pauses in dungeon
    CHECK(game_dbg(1) == 24, "Enter no longer opens pause in dungeon (ai=%d)", game_dbg(1));
    tap(-1); run(20);                                  // Return to game
    CHECK(game_dbg(1) == 0 && game_dbg(0) == 2, "resume after Enter pause failed");
    // Options in the pause menu (port addition): 4th item opens the form from
    // the dungeon; Back returns to PAUSE (not the main menu) with the game
    // screen restored; "Return to game" then resumes the dungeon.
    game_key(-1, 1); run(2); game_key(-1, 0); run(4);  // pause
    tap('4'); run(2);                                  // digit jump -> Options item
    CHECK(game_dbg(4) == 3, "pause: digit 4 did not jump to Options (sel=%d)", game_dbg(4));
    soft(0); run(10);                                  // select opens the options form
    CHECK(game_dbg(1) == 27, "pause Options did not open the form (ai=%d)", game_dbg(1));
    tap(-2); run(10);                                  // right soft: back to pause
    CHECK(game_dbg(1) == 24, "options Back did not return to pause (ai=%d)", game_dbg(1));
    soft(0); run(20);                                  // Return to game (item 0)
    CHECK(game_dbg(1) == 0 && game_dbg(0) == 2 && game_dbg(2) == 2,
          "pause-options round-trip lost the dungeon (ai=%d ab=%d screen=%d)",
          game_dbg(1), game_dbg(0), game_dbg(2));
    int level_no_saved = g_dungeon.level_no, moves_saved = g_dungeon.moves, lives_saved = g_dungeon.lives;
    game_init();                                 // app restart with save
    run(20);
    soft(0);
    CHECK(game_dbg(1) == 25 && game_dbg(6) == 1, "restart: Continue missing");
    soft(0);                                     // Continue
    run(60);
    CHECK(g_dungeon.level_no == level_no_saved && g_dungeon.moves == moves_saved && (g_dungeon.pos_x >> 16) == (pos_x_saved >> 16),
          "Continue mismatch c6 %d/%d c9 %d/%d x %d/%d", g_dungeon.level_no, level_no_saved, g_dungeon.moves, moves_saved, g_dungeon.pos_x >> 16, pos_x_saved >> 16);
    for (int k = 0; k < 2; k++) tap_h('8');   // walk back (solid floor)
    CHECK(g_dungeon.lives == lives_saved, "walking back killed the monkey");
    CHECK(g_dungeon.moves > moves_saved, "moves not counted after resume");
    // ---- game over must delete the run record (KKM.java:490).
    // Otherwise the dead run stays resumable: "Continue" resurrects its
    // lives/score and the hiscore can be banked a second time.
    g_dungeon.lives = 0;
    g_dungeon.status = 1;                      // the flag m() sets on a fatal fall
    for (int t = 0; t < 400 && game_dbg(1) != 25; t++) run(1);
    CHECK(game_dbg(1) == 25, "game over did not return to the main menu (ai=%d)", game_dbg(1));
    CHECK(!mk_save_exists(), "game over left KKM.rms behind (zombie Continue)");
    remove("KKM.rms"); remove("OPT.rms"); remove("KLP.rms"); remove("MKK.rms");
    printf("shell lifecycle: done\n");
}

// save->resume must reproduce the whole voxel world exactly: every block
// shape, every collectible (note/key/warp/life), and the falling-channel
// cells (64/128, which carry neither shape nor item bits so a decode-time
// "type present" tally misses them). A regression that flagged each type
// from a stale count dropped the channel cells and desynced the bit-tied
// stream whenever a single-place item had been collected (the "key/heart
// vanishes after Continue" bug). Also exercised with one item picked up.
static void save_resume_integrity(void)
{
    static int8_t cl0[2048];
    int worst = 0, worst_id = 0;
    for (int pass = 0; pass < 2; pass++) {   // 0 = pristine, 1 = after a pickup
        for (int i = 0; i < MK_LEVELS; i++) {
            int id = mk_level_ids[i];
            if (id > 30) continue;           // real levels only (skip 90..95 debug)
            mk_load_level(id);
            int target = -1;
            if (pass == 1) {                 // clear one collectible cell
                for (int c = 0; c < 2048; c++) {
                    int it = mk_cl[c] & MK_CELL_ITEM_MASK;
                    if (it == 16 || it == 24 || it == 32) {   // key / warp / life
                        mk_cl[c] &= (int8_t)~MK_CELL_ITEM_MASK;
                        mk_ck[c] = 0;
                        target = c;
                        break;
                    }
                }
                if (target < 0) continue;    // level has none of those to drop
            }
            memcpy(cl0, mk_cl, sizeof cl0);
            mk_save_run();
            mk_resume_run();
            int diff = 0;
            for (int c = 0; c < 2048; c++)
                if (cl0[c] != mk_cl[c]) diff++;
            if (diff > worst) { worst = diff; worst_id = id; }
            CHECK(diff == 0,
                  "save/resume lost %d cells on level %d (%s)", diff, id,
                  pass ? "after pickup" : "pristine");
            remove("KKM.rms");
        }
    }
    printf("save/resume world integrity: worst=%d cells (level %d)\n", worst, worst_id);
}

int main(void)
{
    mk_load_tables();
    // ---- decode every level in MBF
    int levels_tested = 0;
    for (int i = 0; i < MK_LEVELS; i++) {
        int id = mk_level_ids[i];
        mk_load_level(id);
        int spawn = mk_cell(g_dungeon.spawn_dat_x, g_dungeon.spawn_dat_h, g_dungeon.spawn_dat_z);
        if ((spawn & MK_CELL_SPAWN_MARK) != MK_CELL_SPAWN_MARK) {
            printf("level %d: no spawn marker (cell value %d)\n", id, spawn);
            fails++;
            continue;
        }
        levels_tested++;
        // no monkey-position CHECK here: mk_init_run sets pos straight from
        // spawn_dat, so asserting it after decode is a copy-forward tautology
        // (the real resume-position round-trip is gated in shell_lifecycle)
        CHECK(g_dungeon.spawn_dat_h > 0, "level %d: spawn below floor", id);
    }
    printf("levels decoded: %d/%d\n", levels_tested, MK_LEVELS);

    save_resume_integrity();

    // ---- walking: pressing '2' must complete an action, move the monkey
    //      (or turn on an obstacle) and advance the move counter or anim
    mk_init_run(1, 1);
    mk_load_level(1);
    int x0 = g_dungeon.pos_x, z0 = g_dungeon.pos_z, moves0 = g_dungeon.moves;
    mk_an[MK_KEY_2] = 1;                       // key '2' = forward
    for (int t = 0; t < 12; t++) mk_tick100(); // walk anim fits in 12 * 100ms ticks
    mk_an[MK_KEY_2] = 0;
    for (int t = 0; t < 12; t++) mk_tick100();
    CHECK(g_dungeon.moves > moves0 || g_dungeon.pos_x != x0 || g_dungeon.pos_z != z0,
          "forward press changed nothing (c9 %d->%d)", moves0, g_dungeon.moves);
    printf("walk: moves %d->%d pos (%d,%d)->(%d,%d)\n",
           moves0, g_dungeon.moves, x0 >> 16, z0 >> 16, g_dungeon.pos_x >> 16, g_dungeon.pos_z >> 16);

    // ---- breaking: key '0' (an[0]) smashes ONLY destructible blocks (type 3)
    // Level 12: destructible block with a note riding it at (6,1,8), cell
    // value 11 (3|8); nothing above; solid floor beneath. Standing spot
    // (7,1,8). Pushable box (type 1) at (8,1,9) stands on (8,0,8).
    mk_init_run(1, 1);
    mk_load_level(12);
    for (int t = 0; t < 25; t++) mk_tick100();           // spawn anim + camera settle
    CHECK((mk_cell(6, 1, 8) & MK_CELL_SHAPE_MASK) == 3,
          "break fixture: no destructible at (6,1,8), cell=%d", mk_cell(6, 1, 8));
    g_dungeon.pos_x = 7 * MK_ONE; g_dungeon.pos_h = 1 * MK_ONE; g_dungeon.pos_z = 8 * MK_ONE;
    g_dungeon.facing = 3 * MK_ONE;                            // face -x, straight at it
    int cell_before = mk_cell(6, 1, 8);
    mk_an[MK_KEY_0] = 1;                                // '0' = smash
    int saw_smash_anim = 0;
    for (int t = 0; t < 12; t++) { mk_tick100(); if (g_dungeon.action_code == 9) saw_smash_anim = 1; }
    mk_an[MK_KEY_0] = 0;
    for (int t = 0; t < 12 && g_dungeon.action_ticks != 0; t++) mk_tick100();  // let any repeat swing finish
    CHECK(saw_smash_anim, "break: key '0' never entered smash anim (cp=9)");
    CHECK((mk_cell(6, 1, 8) & MK_CELL_SHAPE_MASK) == 0,
          "break: type-3 block survived (cell %d -> %d)", cell_before, mk_cell(6, 1, 8));
    CHECK(mk_cell(6, 1, 8) == 8,
          "break: note on smashed block must stay collectable (cell %d)", mk_cell(6, 1, 8));
    int notes0 = g_dungeon.notes_left;
    mk_an[MK_KEY_2] = 1; mk_tick100(); mk_an[MK_KEY_2] = 0;           // one step forward
    for (int t = 0; t < 12; t++) mk_tick100();
    CHECK(g_dungeon.pos_x == 6 * MK_ONE && g_dungeon.status == 0,
          "break: monkey could not walk into smashed cell (x=%d c2=%d)", g_dungeon.pos_x >> 16, g_dungeon.status);
    CHECK(g_dungeon.notes_left == notes0 - 1, "break: freed note not collected (c7 %d->%d)", notes0, g_dungeon.notes_left);
    // solid-ish blocks survive '0': type-1 pushable at (8,1,9)
    mk_load_level(12);
    for (int t = 0; t < 25; t++) mk_tick100();
    g_dungeon.pos_x = 8 * MK_ONE; g_dungeon.pos_h = 1 * MK_ONE; g_dungeon.pos_z = 8 * MK_ONE;
    g_dungeon.facing = 2 * MK_ONE;                            // face +z at the box
    mk_an[MK_KEY_0] = 1;
    for (int t = 0; t < 12; t++) mk_tick100();
    mk_an[MK_KEY_0] = 0;
    CHECK(mk_cell(8, 1, 9) == 1,
          "break: type-1 pushable must NOT smash (cell now %d)", mk_cell(8, 1, 9));
    CHECK(g_dungeon.status == 0, "break: smash attempt killed the monkey (c2=%d)", g_dungeon.status);
    // ---- stacked smashables: level 3 (7,1,8) stack = bottom ck=16, falling
    // block ck=19 (ODD id). The fall landing tick in ac() strips the anim
    // flag with java `& -2049` (== ~2048); the port wrote `& ~2049`, which
    // also cleared id bit0 (19 -> 18). The next smash then cleared the WRONG
    // bd slot: the smashed block stayed drawn (ghost over a walkable cell)
    // and an intact block elsewhere vanished. Invariant checked after every
    // smash: each solid cell must point at a live (drawn) bd entry.
    mk_load_level(3);
    for (int t = 0; t < 25; t++) mk_tick100();
    g_dungeon.pos_x = 6 * MK_ONE; g_dungeon.pos_h = 1 * MK_ONE; g_dungeon.pos_z = 8 * MK_ONE;
    g_dungeon.facing = 1 * MK_ONE;                            // face +x at the stack
    { int lower_cell = 7 + 8 * MK_CELL_STEP_Z + 1 * MK_CELL_STEP_H, upper_cell = lower_cell + MK_CELL_STEP_H;
      CHECK((mk_cl[lower_cell] & MK_CELL_SHAPE_MASK) == 3 && (mk_cl[upper_cell] & MK_CELL_SHAPE_MASK) == 3,
            "stack fixture: lvl3 (7,1,8)/(7,2,8) not both smashable (%d,%d)",
            mk_cl[lower_cell], mk_cl[upper_cell]);
      int upper_block_id = mk_ck[upper_cell] & MK_CK_ID_MASK;
      CHECK((upper_block_id & 1) == 1,
            "stack fixture: falling block id must be odd to expose the bug (id=%d)", upper_block_id);
      int bd_slots[2] = { (mk_ck[lower_cell] & MK_CK_ID_MASK) - 1, upper_block_id - 1 };
      CHECK(r_block_type(bd_slots[0]) == 3 && r_block_type(bd_slots[1]) == 3,
            "stack fixture: bd entries not drawn");
      for (int sm = 0; sm < 2; sm++) {          // two presses of '0'
          mk_an[MK_KEY_0] = 1; for (int t = 0; t < 10; t++) mk_tick100(); mk_an[MK_KEY_0] = 0;
          for (int t = 0; t < 80; t++) mk_tick100();
          int bad = 0;                          // solid cell -> live bd slot
          for (int y = 0; y < MK_GRID_H; y++) for (int z = 0; z < MK_GRID_D; z++) for (int x = 0; x < MK_GRID_W; x++) {
              int idx = x + z * MK_CELL_STEP_Z + y * MK_CELL_STEP_H;
              if ((mk_cl[idx] & MK_CELL_SHAPE_MASK) && (mk_ck[idx] & MK_CK_ID_MASK) &&
                  r_block_type((mk_ck[idx] & MK_CK_ID_MASK) - 1) == 0) bad++; }
          CHECK(bad == 0, "stack smash%d: %d solid cells with cleared render slot", sm + 1, bad); }
      CHECK(mk_cl[lower_cell] == 8,
            "stack: after two smashes (7,1,8) must be the freed note (8), got %d", mk_cl[lower_cell]);
      CHECK(r_block_type(bd_slots[0]) == 0 && r_block_type(bd_slots[1]) == 0,
            "stack: ghost render entries survived the smashes");
      mk_an[MK_KEY_2] = 1; mk_tick100(); mk_an[MK_KEY_2] = 0;        // walk into the cleared cell
      for (int t = 0; t < 16; t++) mk_tick100();
      CHECK(g_dungeon.pos_x == 7 * MK_ONE && g_dungeon.status == 0,
            "stack: monkey could not walk into cleared cell (x=%d c2=%d)", g_dungeon.pos_x >> 16, g_dungeon.status); }

    // ---- save/resume round-trip
    mk_save_run();
    CHECK(mk_save_exists(), "save not visible after mk_save_run");
    int pos_x_saved = g_dungeon.pos_x, pos_z_saved = g_dungeon.pos_z, score_saved = g_dungeon.score;
    mk_an[MK_KEY_4] = 1;                       // key '4': strafe left (may turn only)
    for (int t = 0; t < 8; t++) mk_tick100();
    mk_an[MK_KEY_4] = 0;
    for (int t = 0; t < 8; t++) mk_tick100();
    CHECK(mk_resume_run(), "resume failed");
    CHECK(g_dungeon.pos_x == pos_x_saved && g_dungeon.pos_z == pos_z_saved && g_dungeon.score == score_saved,
          "resume mismatch pos (%d,%d)->(%d,%d) score %d->%d",
          pos_x_saved >> 16, pos_z_saved >> 16, g_dungeon.pos_x >> 16, g_dungeon.pos_z >> 16, score_saved, g_dungeon.score);

    // ---- font table: every printable ASCII glyph must carry ink (the
    // lowercase block once had corrupted rows: 'f' empty, 'k'/'t'/'c' mangled)
    CHECK(mk_font_glyph(' ')[0] == 0, "font: space row0 not blank");
    for (int cp = 0x21; cp <= 0x7E; cp++) {
        int ink = 0;
        const unsigned char *glyph = mk_font_glyph(cp);
        for (int r = 0; r < 7; r++)
            for (int b = 3; b <= 7; b++) ink += (glyph[r] >> b) & 1;
        CHECK(ink >= 2, "font: glyph '%c' has only %d pixels", cp, ink);
    }
    // ---- proportional metrics: title "Munkiki's Castles" must fit 96 px
    int title_w = 0;
    for (const char *q = "Munkiki's Castles"; *q; q++) title_w += mk_font_adv((unsigned char)*q);
    CHECK(title_w <= 94, "font: menu title width %d exceeds screen", title_w);
    for (int cp = 0x21; cp <= 0x7E; cp++)
        CHECK(mk_font_adv(cp) >= 3 && mk_font_adv(cp) <= 6,
              "font: adv('%c')=%d out of 3..6", cp, mk_font_adv(cp));
    CHECK(mk_font_adv('i') < mk_font_adv('m'), "font: not proportional (i>=m)");
    CHECK(mk_font_adv('0') == mk_font_adv('9'), "font: digits not tabular");
    shell_lifecycle();
    printf(fails ? "LOGIC TEST: %d FAILURES\n" : "LOGIC TEST: OK (%d failures)\n", fails);
    return fails != 0;
}
