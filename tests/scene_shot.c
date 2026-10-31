// scene_shot.c - headless scene dumps: drives the real shell (splash -> menu
// -> New Game/hint -> map -> dungeon -> walk/turn/pause/save) with no window
// and writes 96x65 BMPs. Modes: (none) full path; `pr` pause/resume repro;
// `resume` boot with existing KKM.rms -> Continue -> walk steps; `kb` Enter-only
// new-game start (level card must be fully drawn; rc!=0 on regression).
#ifndef _WIN32
#define _DEFAULT_SOURCE                  // setenv() prototype under -std=c11
#endif
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "engine.h"
#include "game.h"
#include "render.h"
extern int game_paint_count(int);

void platform_sound(int id) { (void)id; }

static unsigned char lcd_fb[96 * 65];
static void run(int n) { for (int t = 0; t < n; t++) { game_tick25(); game_paint(lcd_fb); } }
static void chord(const int *keys, int n, int hold)
{
    for (int i = 0; i < n; i++) game_key(keys[i], 1);
    for (int t = 0; t < hold; t++) game_tick25();
    for (int i = n - 1; i >= 0; i--) game_key(keys[i], 0);
    run(6);
}
static void soft(int hold) { chord(&((int){ -1 }), 1, hold); }
static void tap(int k)     { chord(&k, 1, 0); }
static void tap_h(int k)   { game_key(k, 1); run(4); game_key(k, 0); run(6); }
static void wait_ab(int want, int limit)
{
    for (int t = 0; t < limit && game_dbg(0) != want; t++) run(1);
}
static void dump(const char *path)
{
    static const int W = 96, H = 65;
    int row_bytes = W * 3;
    unsigned char *bmp_hdr, *img;
    int hdr_bytes = 14 + 40;
    FILE *f;
    int x, y;
    img = malloc((size_t)row_bytes * H);
    for (y = 0; y < H; y++)                 /* BMP bottom-up, BGR */
        for (x = 0; x < W; x++) {
            unsigned char gray = lcd_fb[(size_t)y * W + x];
            unsigned char *p = img + (size_t)(H - 1 - y) * row_bytes + x * 3;
            p[0] = p[1] = p[2] = gray;
        }
    bmp_hdr = malloc(hdr_bytes);
    memset(bmp_hdr, 0, hdr_bytes);
    bmp_hdr[0] = 'B'; bmp_hdr[1] = 'M';
    /* unaligned fields: memcpy keeps this UB-free under UBSan */
#define PUT32(off, v) do { int v_ = (v); memcpy(bmp_hdr + (off), &v_, 4); } while (0)
    PUT32(2,  hdr_bytes + row_bytes * H);
    PUT32(10, hdr_bytes);
    PUT32(14, 40);
    PUT32(18, W); PUT32(22, H);
#undef PUT32
    bmp_hdr[26] = 1; bmp_hdr[28] = 24;
    f = fopen(path, "wb");
    fwrite(bmp_hdr, 1, hdr_bytes, f);
    fwrite(img, 1, (size_t)row_bytes * H, f);
    fclose(f);
    free(bmp_hdr); free(img);
    { int q, n = 0; for (q = 0; q < 96 * 65; q++) if (lcd_fb[q] < 250) n++;
      fprintf(stderr, "DUMP %s n_black=%d\n", path, n); }
    printf("dumped %s\n", path);
}

#include "m3d.h"
static void dump_raw(const char *path)
{
    memcpy(lcd_fb, m3d_get()->fb, 96 * 65);
    dump(path);
}


int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "diag") == 0) {   // MK_DIAG showroom headless
        int rc = 0;
        remove("KKM.rms"); remove("OPT.rms"); remove("KLP.rms"); remove("MKK.rms");
#ifdef _MSC_VER
        _putenv("MK_DIAG=1");
#else
        setenv("MK_DIAG", "1", 1);
#endif
        game_init();
        wait_ab(2, 120);
        run(40);                                   // let the spawn dance finish
        printf("diag sub=%d screen=%d pos(%d,%d,%d) face=%d\n",
               game_dbg(0), game_dbg(2), g_dungeon.pos_x >> 16,
               g_dungeon.pos_h >> 16, g_dungeon.pos_z >> 16, g_dungeon.facing);
        printf("diag status=%d action=%d anim=%d\n", g_dungeon.status,
               g_dungeon.action_code, g_dungeon.anim_id);
        { int x, y, z; printf("cells:");
          for (x = 0; x < 16; x++) for (y = 0; y < 4; y++) for (z = 0; z < 16; z++)
              if (mk_cell(x, y, z)) printf(" (%d,%d,%d)=%d", x, y, z, mk_cell(x, y, z));
          printf("\n"); }
        printf("diag bills=%d:", g_scene.bill_n);
        for (int b = 0; b < g_scene.bill_n; b++) {
            int o = b * MK_BILL_STRIDE;
            printf(" (%d,%d,%d)i%d", g_scene.bill_rec[o] >> 16,
                   g_scene.bill_rec[o + 1] >> 16, g_scene.bill_rec[o + 2] >> 16,
                   g_scene.bill_rec[o + 3]);
        }
        printf("\n");
        printf("diag block_n=%d\n", g_scene.block_n);
        /* Save/resume gate for the showroom: mk_diag_level overwrites the
           decoded L1 grid AFTER the level-load checkpoint, so the only world
           that reaches KKM.rms is the showroom - and it must come back
           cell-identical AND at the same cell. A showroom that never placed
           the (40) spawn marker into cl saved no respawn cell: Continue
           then resumed the monkey at stale/zero spawn_dat (the "wrong place
           after Continue" bug). The resume also rebuilds every block/billboard
           record from the decoded grid, so the item gates below run against
           the RESUMED world. */
        {
            static int8_t snap[2048];
            int px = g_dungeon.pos_x, ph = g_dungeon.pos_h, pz = g_dungeon.pos_z;
            memcpy(snap, mk_cl, sizeof snap);
            mk_save_run();
            /* a fresh process resumes with zeroed statics: a save that does
               not carry the (40) marker would respawn the monkey there */
            g_dungeon.spawn_dat_x = g_dungeon.spawn_dat_h = g_dungeon.spawn_dat_z = 0;
            mk_resume_run();
            int dcl = 0;
            for (int i = 0; i < 2048; i++) if (snap[i] != mk_cl[i]) dcl++;
            int same = g_dungeon.pos_x == px && g_dungeon.pos_h == ph &&
                       g_dungeon.pos_z == pz;
            printf("diag save/resume: cells_diff=%d pos_restored=%d pos(%d,%d,%d)\n",
                   dcl, same, g_dungeon.pos_x >> 16, g_dungeon.pos_h >> 16,
                   g_dungeon.pos_z >> 16);
            if (dcl || !same) rc = 1;
            remove("KKM.rms");
        }
        game_paint(lcd_fb);
        dump("diag_spawn.bmp");
        // Teleport 2 cells in front of the row (no ticks: records stay
        // intact, no collision or pickups) for a solid-size item view.
        g_dungeon.pos_z = 7 * MK_ONE;
        printf("diag near pos(%d,%d,%d)\n", g_dungeon.pos_x >> 16,
               g_dungeon.pos_h >> 16, g_dungeon.pos_z >> 16);
        game_paint(lcd_fb);
        dump("diag_near.bmp");
        /* Regression gate (taken from the near view, where the items are
           large enough to matter): every collectible kind must contribute
           pixels.  Billboard records persist between paints (only level
           decode rebuilds them), so zeroing the key/warp/life records and
           repainting isolates their contribution.  billboard_tex[] once
           carried only the note pointer, silently hiding the other three. */
        {
            static unsigned char fb_full[96 * 65];
            int off[12], val[12], nsav = 0, best = 0;
            for (int b = 0; b < g_scene.bill_n && nsav < 12; b++) {
                int o = b * MK_BILL_STRIDE;
                int v = g_scene.bill_rec[o + 3];
                if (v != 0 && v != 8) { off[nsav] = o; val[nsav] = v; nsav++; }
            }
            /* All billboards share one spin angle, so a single paint can
               catch every quad edge-on (zero area, zero contribution).
               Sample 4 phases, 45 deg apart, and take the best. */
            for (int ph = 0; ph < 4; ph++) {
                int diff_px = 0;
                g_scene.frame_no += 8;              // 45 spin degrees
                game_paint(lcd_fb);                 // full billboards
                memcpy(fb_full, lcd_fb, sizeof fb_full);
                for (int k = 0; k < nsav; k++) g_scene.bill_rec[off[k] + 3] = 0;
                game_paint(lcd_fb);                 // same phase, hidden
                for (int i = 0; i < 96 * 65; i++)
                    if (lcd_fb[i] != fb_full[i]) diff_px++;
                for (int k = 0; k < nsav; k++) g_scene.bill_rec[off[k] + 3] = val[k];
                if (diff_px > best) best = diff_px;
            }
            g_scene.frame_no += 8;
            game_paint(lcd_fb);
            printf("diag key/warp/life contribution = %d px (hidden %d bills)\n",
                   best, nsav);
            /* The showroom carries exactly one key/warp/life (single-
               placement types): 3 hidden bills, ~60 px across the 4 spin
               phases; with the billboard_tex regression only ~16 remain. */
            if (best < 40 || nsav < 3) rc = 1;
        }
        for (int y = 0; y < 65; y++) {
            for (int x = 0; x < 96; x++) {
                unsigned char v = lcd_fb[y * 96 + x];
                putchar(v < 64 ? '#' : v < 160 ? '+' : v < 250 ? '-' : '.');
            }
            putchar('\n');
        }
        dump_raw("diag_near_raw.bmp");
        remove("KKM.rms"); remove("OPT.rms"); remove("KLP.rms"); remove("MKK.rms");
        return rc;
    }
    if (argc > 1 && strcmp(argv[1], "warp") == 0) {
        /* Portal close-up: dump warp quad front-on / edge-on / back-on from
         * 2 cells in the MK_DIAG showroom (eye above the support block). The
         * front-vs-edge pixel diff proves the textured quad is present and
         * spins; one-glyph-vs-tiled fidelity is judged by eye from these
         * dumps against device video frames - at 14 LCD px no metric can
         * tell a full glyph from 16 micro-tiles reliably. Frame phases are
         * deterministic because MK_WARP_SPIN is pinned to the original rate. */
        remove("KKM.rms"); remove("OPT.rms"); remove("KLP.rms"); remove("MKK.rms");
#ifdef _MSC_VER
        _putenv("MK_DIAG=1");
        _putenv("MK_WARP_SPIN=1");
#else
        setenv("MK_DIAG", "1", 1);
        setenv("MK_WARP_SPIN", "1", 1);
#endif
        game_init();
        wait_ab(2, 120);
        run(40);
        g_dungeon.pos_x = 4 * MK_ONE + 32768;
        g_dungeon.pos_z = 7 * MK_ONE;   // eye z=8: quad 2 cells ahead, 16 units
        g_dungeon.pos_h = 2 * MK_ONE;   // eye above the support block
        g_dungeon.facing = 2 * MK_ONE;
        static const int ph_frame[3] = { 0, 16, 32 }; // front, edge-on, back
        static unsigned char front[96 * 65];
        int rc = 0;
        for (int ph = 0; ph < 3; ph++) {
            char nm[64];
            g_scene.frame_no = ph_frame[ph];
            game_paint(lcd_fb);
            snprintf(nm, sizeof nm, "warp_ph%d.bmp", ph);
            dump(nm);
            if (ph == 0)
                memcpy(front, lcd_fb, sizeof front);
            if (ph == 1) {           // front vs edge-on: quad must contribute
                int d = 0;
                for (int i = 0; i < 96 * 65; i++)
                    if (front[i] != lcd_fb[i]) d++;
                printf("warp front-vs-edge diff = %d px\n", d);
                if (d < 140) rc = 1;     // measured: 169 with portal, 119 without
            }
            if (ph != 1) {              // ASCII for the front/back facings
                for (int y = 10; y < 30; y++) {
                    for (int x = 30; x < 70; x++)
                        putchar(lcd_fb[y * 96 + x] < 64 ? '#' :
                                lcd_fb[y * 96 + x] < 160 ? '+' :
                                lcd_fb[y * 96 + x] < 250 ? '-' : '.');
                    putchar('\n');
                }
            }
        }
        printf("warp gate %s\n", rc ? "FAIL" : "ok");
        remove("KKM.rms"); remove("OPT.rms"); remove("KLP.rms"); remove("MKK.rms");
        return rc;
    }
    if (argc > 1 && strcmp(argv[1], "lv3") == 0) {   // TEMP repro: level 3 heart on block
        remove("KKM.rms"); remove("OPT.rms"); remove("KLP.rms"); remove("MKK.rms");
        game_init();
        run(20);
        soft(0); soft(0);                        // splash -> menu -> New Game
        tap('2'); run(45); wait_ab(1, 200);      // hint -> map
        tap_h('r'); run(30);                     // card -> dungeon
        mk_load_level(3); run(25);
        int sx = g_dungeon.pos_x >> 16, sz = g_dungeon.pos_z >> 16, sh = g_dungeon.pos_h >> 16;
        printf("lv3 spawn (%d,%d,%d) face=%d\n", sx, sh, sz, g_dungeon.facing);
        printf("cells:");
        for (int x = 0; x < 16; x++) for (int y = 0; y < 4; y++) for (int z = 0; z < 16; z++)
            if (mk_cell(x, y, z)) printf(" (%d,%d,%d)=%d", x, y, z, mk_cell(x, y, z));
        printf("\n");
        for (int f = 0; f < 4; f++) {
            g_dungeon.freelook = 1; g_dungeon.yaw = f * MK_ONE; g_dungeon.pitch = 0;
            game_paint(lcd_fb);
            printf("=== freelook yaw %d ===\n", f);
            for (int y = 0; y < 30; y++) {
                for (int x = 0; x < 96; x++) {
                    unsigned char v = lcd_fb[y * 96 + x];
                    putchar(v < 64 ? '#' : v < 160 ? '+' : v < 250 ? '-' : '.');
                }
                putchar('\n');
            }
        }
        g_dungeon.freelook = 0; g_dungeon.yaw = 0; g_dungeon.pitch = 0;
        g_dungeon.facing = 2 * MK_ONE;
        for (int f = 0; f < 4; f++) {
            g_dungeon.facing = f * MK_ONE;
            game_paint(lcd_fb);
            printf("=== facing %d ===\n", f);
            for (int y = 0; y < 30; y++) {
                for (int x = 0; x < 96; x++) {
                    unsigned char v = lcd_fb[y * 96 + x];
                    putchar(v < 64 ? '#' : v < 160 ? '+' : v < 250 ? '-' : '.');
                }
                putchar('\n');
            }
        }
        g_dungeon.facing = 131072;
        game_paint(lcd_fb);
        dump("lv3_spawn.bmp");
        printf("bill_n=%d\n", g_scene.bill_n);
        for (int b = 0; b < g_scene.bill_n; b++) {
            int o = b * MK_BILL_STRIDE;
            printf("bill %d: (%d.%03d,%d.%03d,%d.%03d) item=%d\n", b,
                   g_scene.bill_rec[o] >> 16, (g_scene.bill_rec[o] & 0xffff) * 1000 >> 16,
                   g_scene.bill_rec[o + 1] >> 16, (g_scene.bill_rec[o + 1] & 0xffff) * 1000 >> 16,
                   g_scene.bill_rec[o + 2] >> 16, (g_scene.bill_rec[o + 2] & 0xffff) * 1000 >> 16,
                   g_scene.bill_rec[o + 3]);
        }
        for (int y = 0; y < 40; y++) {
            for (int x = 0; x < 96; x++) {
                unsigned char v = lcd_fb[y * 96 + x];
                putchar(v < 64 ? '#' : v < 160 ? '+' : v < 250 ? '-' : '.');
            }
            putchar('\n');
        }
        remove("KKM.rms"); remove("OPT.rms"); remove("KLP.rms"); remove("MKK.rms");
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "inblock") == 0) {
        /* CULL_FACE + cullFace(GL_BACK) (c.java:536-537): the original culled
           backfaces, so when the chase camera sits inside a block all its
           faces vanish and the block looks transparent.  Level 12 has a
           column of unit cubes at (8,y,9); the chase eye is 1 cell AHEAD of
           the monkey at (c_, cz+1, cy+1), so monkey (8,8) cz=1 puts the eye
           exactly at the centre of cube (8,2,9).
           Gate: painting that view with culling ON must differ from the
           pre-fix (culling OFF) paint by >1000 px (the enclosing wall faces
           disappear).  Dumps ib_in.bmp / ib_in_nocull.bmp for inspection.
           Exit !=0 on regression. */
        int i, diff_px = 0, rc = 0;
        static unsigned char fb_nocull[96 * 65];
        remove("KKM.rms"); remove("OPT.rms"); remove("KLP.rms"); remove("MKK.rms");
        game_init();
        run(20);
        soft(0); soft(0);                        // splash -> menu -> New Game
        tap('2'); run(45); wait_ab(1, 200);      // hint -> map
        tap_h('r'); run(30);                     // card -> dungeon
        { int x, y, z; printf("lvl1 cells:");
          for (x = 0; x < 16; x++) for (y = 0; y < 4; y++) for (z = 0; z < 16; z++)
              if (mk_cell(x, y, z)) printf(" (%d,%d,%d)=%d", x, y, z, mk_cell(x, y, z));
          printf("\nib spawn c_=%d cy=%d cz=%d cx=%d\n", g_dungeon.pos_x >> 16, g_dungeon.pos_z >> 16, g_dungeon.pos_h >> 16, g_dungeon.facing); }
        mk_load_level(12); run(20);                       // swap to level 12 in dungeon state
        g_dungeon.pos_x = 8 * MK_ONE; g_dungeon.pos_h = 1 * MK_ONE; g_dungeon.pos_z = 8 * MK_ONE; g_dungeon.facing = 0;
        if (mk_cell(8, 2, 9) == 0) { printf("inblock: level corrupt?\n"); return 2; }
        m3d_cullFace(m3d_get(), 0);              /* pre-fix / FreeJ2ME: no cull */
        game_paint(lcd_fb);
        memcpy(fb_nocull, lcd_fb, sizeof fb_nocull);
        dump("ib_in_nocull.bmp");
        { long skips_before = m3d_cull_skips();
          m3d_cullFace(m3d_get(), M3D_BACK);
          game_paint(lcd_fb);
          dump("ib_in.bmp");
          { long skips_after = m3d_cull_skips();
            for (i = 0; i < 96 * 65; i++)
                if ((lcd_fb[i] > fb_nocull[i] ? lcd_fb[i] - fb_nocull[i] : fb_nocull[i] - lcd_fb[i]) > 8) diff_px++;
            printf("inblock inside-view diff (cull vs nocull) = %d px, faces culled = %ld\n",
                   diff_px, skips_after - skips_before);
            if (diff_px < 1000 || skips_after == skips_before) rc = 1; } }
        remove("KKM.rms"); remove("OPT.rms"); remove("KLP.rms"); remove("MKK.rms");
        return rc;
    }
    if (argc > 1 && strcmp(argv[1], "notes") == 0) {
        /* Port deviations on collectible billboards: the note texture has a
           circular alpha mask (the original is an opaque quad: its white
           body + dark corner texels read as a spinning diamond whose
           corners stick out ~40% past the ring), and note/key/life spin at
           half the original rate (period 64 frames, not 32).  Gate: park
           the eye one cell in front of a note, paint 32 frames (half the
           new period, one whole old one) and diff frame i with frame i+16.
           Static HUD/ground cancel out; only the icon differs.  Half-speed:
           i+16 is 90 degrees away (edge-on), so the diff covers the whole
           icon silhouette: area ~= 217 masked / 304 unmasked (diamond
           corners add ~87 px).  Full speed: i+16 is the identical mirrored
           pose: only the glyph mirrors, area ~110-170.  So area < 190 =
           spin regression, area > 260 = the round mask is gone. */
        int lvl, x, y, z, nx = -1, ny = 0, nz = 0, i, imax = 0;
        static unsigned char fr[32][96 * 65];
        int w[32], a[32];
        remove("KKM.rms"); remove("OPT.rms"); remove("KLP.rms"); remove("MKK.rms");
        game_init();
        run(20);
        soft(0); soft(0);
        tap('2'); run(45); wait_ab(1, 200);
        tap_h('r'); run(30);
        for (lvl = 0; lvl < 36 && nx < 0; lvl++) {
            mk_load_level(lvl);
            for (y = 0; y < 4 && nx < 0; y++)
                for (x = 0; x < 16 && nx < 0; x++)
                    for (z = 0; z < 16 && nx < 0; z++)
                        if ((mk_cell(x, y, z) & MK_CELL_ITEM_MASK) == 8)
                            nx = x, ny = y, nz = z;
        }
        if (nx < 0) { printf("notes: no note cell in any level?\n"); return 2; }
        printf("notes: lvl %d cell (%d,%d,%d)\n", lvl - 1, nx, ny, nz);
        mk_load_level(lvl - 1);
        /* facing=0 looks along +x, eye = monkey minus 1 cell (the chase
           offset), +z to the right: monkey at (nx, nz) puts the eye 1 cell
           in front of the note. */
        g_dungeon.pos_x = nx * MK_ONE; g_dungeon.pos_z = nz * MK_ONE;
        g_dungeon.pos_h = (ny - 1) * MK_ONE; g_dungeon.facing = 0;
        for (i = 0; i < 32; i++) { game_paint(lcd_fb); memcpy(fr[i], lcd_fb, sizeof fr[i]); }
        for (i = 0; i < 32; i++) {
            int j = (i + 16) % 32, q, lo = 999, hi = -1;
            w[i] = 0; a[i] = 0;
            for (q = 0; q < 96 * 65; q++) {
                int d = (int)fr[i][q] - (int)fr[j][q];
                if (d > 40 || d < -40) {
                    int c = q % 96;
                    a[i]++;
                    if (c < lo) lo = c;
                    if (c > hi) hi = c;
                }
            }
            if (hi >= lo) w[i] = hi - lo + 1;
            if (w[i] > w[imax]) imax = i;
        }
        printf("notes: front pair (%d,%d) bbox=%d px area=%d\n",
               imax, (imax + 16) % 32, w[imax], a[imax]);
        memcpy(lcd_fb, fr[imax], 96 * 65);
        dump("notes_front.bmp");
        if (w[imax] < 20)  { printf("notes: icon never visible\n"); return 1; }
        if (a[imax] < 190) { printf("notes: full-speed spin (period 32)\n"); return 1; }
        if (a[imax] > 260) { printf("notes: square corners painted (no round mask)\n"); return 1; }
        remove("KKM.rms"); remove("OPT.rms"); remove("KLP.rms"); remove("MKK.rms");
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "mapfix") == 0) {
        /* MIDP Image.createImage() is OPAQUE WHITE: the world-map card ah[1]
           repaints the whole 96x65 area each frame (KKM.m() case 5,
           drawImage(ah[1], 0, 0)), erasing the previous monkey-marker frame
           while it slides (ad 9..18).  A transparent card instead lets the
           marker smear a black streak into the lower-left corner.
           Gate: after the slide ends (ad>=40) the dark px in the 16x12 cell
           below the marker must be <= 25 (final marker + start ticks ~4;
           without the opaque repaint the smear is >= 43).  Dumps
           mf_map.bmp.  Exit != 0 on regression. */
        int x, y, dark_px = 0, rc = 0;
        remove("KKM.rms"); remove("OPT.rms"); remove("KLP.rms"); remove("MKK.rms");
        game_init();
        run(20);
        soft(0); soft(0);                        // splash -> menu -> New Game
        tap('2');                                // hint -> progress map
        { int guard = 0;                      /* shell runs ~10Hz: wait until
                                                 the marker slide is done */
          while (game_dbg(5) < 40 && game_dbg(0) == 5 && guard++ < 400)
              game_tick25(), game_paint(lcd_fb); }
        if (game_dbg(0) != 5) { printf("mapfix: not on map (ab=%d)\n", game_dbg(0)); return 2; }
        /* the slot's tick mark ("V" = (3,51)-(6,54) + (7,54)-(10,51)) must
           survive the sliding marker's clip exactly. The old 16x14 clip
           white-washed its top two rows and bled the next sprite cell's head
           ("###" at row 52) under the monkey. */
        for (y = 51; y <= 54; y++)
            for (x = 0; x < 16; x++)
                if (lcd_fb[y * 96 + x] < 100) dark_px++;
        printf("mapfix tick-mark px under marker = %d\n", dark_px);
        dump("mf_map.bmp");
        if (dark_px != 8) rc = 1;
        /* atlas cols 14.. hold a NEIGHBOURING sprite (blob head, atlas rows 3..6);
           a clip wider than the monkey bbox (cols 1..13) draws it as a stray dash
           right of the monkey -- window rows 42..45 x cols 14..15 must stay empty.
           Conversely col 13 rows 43..44 IS the monkey's own arm: both px required */
        {
            int leak = 0, arm = 0;
            for (y = 42; y <= 45; y++)
                for (x = 14; x <= 15; x++)
                    if (lcd_fb[y * 96 + x] < 100) leak++;
            arm = (lcd_fb[43 * 96 + 13] < 100) + (lcd_fb[44 * 96 + 13] < 100);
            printf("mapfix neighbour-sprite leak px = %d, arm px = %d\n", leak, arm);
            if (leak != 0 || arm != 2) rc = 1;
        }
        /* arc bottom tangents: no stray px hanging under the U-turn flats
           (g_arc must not draw the exact sin=-1 sample; device does not) */
        {
            int hang = 0, flat = 0;
            const int pts[4][2] = {{26,45},{52,45},{26,50},{52,50}};
            int k;
            for (k = 0; k < 4; k++)
                if (lcd_fb[pts[k][1] * 96 + pts[k][0]] < 100) hang++;
            flat = (lcd_fb[49 * 96 + 26] < 100) + (lcd_fb[44 * 96 + 26] < 100)
                 + (lcd_fb[49 * 96 + 52] < 100) + (lcd_fb[44 * 96 + 52] < 100);
            printf("mapfix arc tangent hang px = %d, flat px = %d\n", hang, flat);
            if (hang != 0 || flat != 4) rc = 1;
        }
        remove("KKM.rms"); remove("OPT.rms"); remove("KLP.rms"); remove("MKK.rms");
        return rc;
    }
    if (argc > 1 && strcmp(argv[1], "pr") == 0) {
        /* repro: live path enter dungeon -> walk -> pause -> resume -> walk */
        remove("KKM.rms"); remove("OPT.rms"); remove("KLP.rms"); remove("MKK.rms");
        game_init();
        run(20);
        soft(0); soft(0);                          // splash -> menu -> hint
        wait_ab(1, 400);                           // level card
        tap_h('r'); run(30);                       // dungeon
        printf("pr enter ab=%d c_=%d cy=%d cx=%d cz=%d\n", game_dbg(0), g_dungeon.pos_x >> 16,
               g_dungeon.pos_z, g_dungeon.facing, g_dungeon.pos_h >> 16);
        dump("pr_enter.bmp");
        tap_h('2'); run(24);
        printf("pr walk1 c_=%d cy=%d cz=%d bt=%d\n", g_dungeon.pos_x >> 16, g_dungeon.pos_z, g_dungeon.pos_h >> 16, g_scene.frame_no);
        dump("pr_walk1.bmp");
        tap_h('2'); run(24);
        printf("pr walk2 c_=%d cy=%d cz=%d bt=%d\n", g_dungeon.pos_x >> 16, g_dungeon.pos_z, g_dungeon.pos_h >> 16, g_scene.frame_no);
        dump("pr_walk2.bmp");
        soft(0); run(30);                          // pause (nav(MENU_PAUSE) saves)
        printf("pr pause ab=%d aj=%d type=%d\n", game_dbg(0), game_dbg(2), game_dbg(3));
        int pr_rc = 0;
        int px_s = g_dungeon.pos_x, pz_s = g_dungeon.pos_z;
        int lv_s = g_dungeon.level_no, mv_s = g_dungeon.moves;
        if (!mk_save_exists()) {
            printf("pr FAIL: pause did not write KKM.rms\n");
            pr_rc = 1;
        }
        soft(0); run(30);                          // Resume (sel 0)
        printf("pr resume ab=%d ai=%d aj=%d c_=%d cy=%d\n",
               game_dbg(0), game_dbg(1), game_dbg(2), g_dungeon.pos_x >> 16, g_dungeon.pos_z);
        dump("pr_resumed.bmp");
        tap_h('2'); run(24);
        printf("pr walk c_=%d cy=%d br=%d\n", g_dungeon.pos_x >> 16, g_dungeon.pos_z, g_scene.shake_on);
        dump("pr_after.bmp");
        /* true resume-boot round-trip: app boot off the pause-save must land
         * back in the paused run (same level, moves, marker cell) */
        game_init();
        run(20);
        soft(0);                                   // splash -> main menu
        if (game_dbg(6) != 1) {
            printf("pr FAIL: no Continue on menu after pause-save\n");
            pr_rc = 1;
        }
        soft(0);                                   // Continue (sel 0)
        run(60);
        if (g_dungeon.level_no != lv_s || g_dungeon.moves != mv_s ||
            g_dungeon.pos_x != px_s || g_dungeon.pos_z != pz_s) {
            printf("pr FAIL: resume-boot mismatch lvl %d/%d moves %d/%d x %d/%d z %d/%d\n",
                   g_dungeon.level_no, lv_s, g_dungeon.moves, mv_s,
                   g_dungeon.pos_x, px_s, g_dungeon.pos_z, pz_s);
            pr_rc = 1;
        } else {
            printf("pr resume-boot round-trip OK (lvl %d moves %d)\n", lv_s, mv_s);
        }
        remove("KKM.rms"); remove("OPT.rms"); remove("KLP.rms"); remove("MKK.rms");
        return pr_rc;
    }
    if (argc > 1 && strcmp(argv[1], "kb") == 0) {
        /* Regression: start a new game using Enter (soft select) only.
         * Enter on the world map while the monkey marker is still sliding
         * (<19 ticks) must still show the fully drawn level card:
         * build_text once inherited the marker's 16x14 paint clip and drew
         * the card text outside it (blank card: ink ~96 = separator line
         * only; correct: ~633). */
        int q, ink = 0, rc = 0;
        remove("KKM.rms"); remove("OPT.rms"); remove("KLP.rms"); remove("MKK.rms");
        game_init();
        run(20);
        soft(0); run(15);                        // Enter: splash -> menu
        soft(0); run(15);                        // Enter: New Game -> hint
        soft(0); run(15);                        // Enter: hint -> world map
        soft(0); wait_ab(1, 30);                 // Enter mid-slide -> level card
        for (q = 0; q < 96 * 65; q++) if (lcd_fb[q] < 200) ink++;
        printf("kb card ab=%d ink=%d\n", game_dbg(0), ink);
        dump("kb_card.bmp");
        if (game_dbg(0) != 1) { puts("kb: level card not reached"); rc = 1; }
        else if (ink < 400) { puts("kb: blank level card (build_text clip leak)"); rc = 1; }
        soft(0); run(30);                        // Enter: card -> dungeon
        if (game_dbg(0) != 2) { puts("kb: dungeon not entered"); rc = 1; }
        remove("KKM.rms"); remove("OPT.rms"); remove("KLP.rms"); remove("MKK.rms");
        return rc;
    }
    if (argc > 1 && strcmp(argv[1], "menu1") == 0) {
        /* main menu frame: title + items, for font/metric checks */
        remove("KKM.rms"); remove("OPT.rms"); remove("KLP.rms"); remove("MKK.rms");
        game_init();
        run(20);
        soft(0);                                   // splash -> menu
        run(20);
        printf("m1 ai=%d type=%d sel=%d n=%d\n",
               game_dbg(1), game_dbg(3), game_dbg(4), game_dbg(13));
        dump("menu_title.bmp");
        remove("KKM.rms"); remove("OPT.rms"); remove("KLP.rms"); remove("MKK.rms");
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "menu4") == 0) {
        /* repro live: fresh menu, digit 4, soft select */
        remove("KKM.rms"); remove("OPT.rms"); remove("KLP.rms"); remove("MKK.rms");
        game_init();
        run(20);
        soft(0);                                   // splash -> menu
        printf("m4 menu af=%d ai=%d type=%d sel=%d n=%d\n",
               game_dbg(6), game_dbg(1), game_dbg(3), game_dbg(4), game_dbg(13));
        tap('4');
        printf("m4 after4 ai=%d type=%d sel=%d\n",
               game_dbg(1), game_dbg(3), game_dbg(4));
        soft(0);                                   // SELECT
        run(20);
        printf("m4 afterEnter ai=%d type=%d sel=%d n=%d ab=%d\n",
               game_dbg(1), game_dbg(3), game_dbg(4), game_dbg(13), game_dbg(0));
        dump("m4_after.bmp");
        remove("KKM.rms"); remove("OPT.rms"); remove("KLP.rms"); remove("MKK.rms");
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "resume") == 0) {
        /* live-paused KKM.rms sits in cwd: fresh boot must show Continue */
        game_init();
        run(20);
        soft(0);                                  // splash -> menu
        printf("menu af=%d type=%d sel=%d\n", game_dbg(6), game_dbg(3), game_dbg(4));
        soft(0);                                  // Continue -> pause menu
        run(30);
        printf("after continue ab=%d ai=%d aj=%d type=%d\n",
               game_dbg(0), game_dbg(1), game_dbg(2), game_dbg(3));
        dump("rs_menu.bmp");
        soft(0);                                  // Resume -> dungeon
        run(30);
        printf("resumed ab=%d ai=%d c_=%d cy=%d\n", game_dbg(0), game_dbg(1),
               g_dungeon.pos_x >> 16, g_dungeon.pos_z);
        dump("rs_resumed.bmp");
        tap_h('2'); run(24);
        dump("rs_step1.bmp");
        tap_h('2'); run(24);
        dump("rs_step2.bmp");
        printf("step2 c_=%d c7=%d ab=%d aj=%d\n", g_dungeon.pos_x >> 16, g_dungeon.notes_left, game_dbg(0), game_dbg(2));
        game_key('2', 1); run(36); game_key('2', 0); run(12);   /* ~900 ms hold */
        printf("hold c_=%d c7=%d ab=%d aj=%d bh=%d\n", g_dungeon.pos_x >> 16, g_dungeon.notes_left,
               game_dbg(0), game_dbg(2), game_dbg(11));
        dump("rs_hold1.bmp");
        game_key('2', 1); run(36); game_key('2', 0); run(12);
        printf("hold2 c_=%d c7=%d ab=%d aj=%d bh=%d\n", g_dungeon.pos_x >> 16, g_dungeon.notes_left,
               game_dbg(0), game_dbg(2), game_dbg(11));
        dump("rs_hold2.bmp");
        remove("KKM.rms"); remove("OPT.rms"); remove("KLP.rms"); remove("MKK.rms");
        return 0;
    }
    remove("KKM.rms"); remove("OPT.rms"); remove("KLP.rms"); remove("MKK.rms");
    game_init();
    run(20);
    soft(0);                                 // splash -> menu
    soft(0);                                 // New Game -> hint
    tap('2'); run(45);                       // -> progress map
    wait_ab(1, 200);                         // -> "Level 1" card
    tap_h('r'); run(30);                     // -> dungeon
    printf("ab=%d ai=%d c_=%d cy=%d c6=%d\n", game_dbg(0), game_dbg(1), g_dungeon.pos_x >> 16, g_dungeon.pos_z, g_dungeon.level_no);
    int rc = 0;                              // the dumps only mean something
    if (game_dbg(0) != 2 || g_dungeon.level_no != 1) {   // if we got there
        printf("FAIL: never reached dungeon spawn (ab=%d c6=%d)\n", game_dbg(0), g_dungeon.level_no);
        rc = 1;
    }
    int x0 = g_dungeon.pos_x;
    dump("sc_spawn.bmp");
    dump_raw("sc_spawn_raw.bmp");
    tap_h('2'); run(24);                     // one step forward
    if (g_dungeon.pos_x == x0 && game_dbg(0) == 2) {
        printf("FAIL: walking did not move the monkey\n");
        rc = 1;
    }
    dump("sc_step1.bmp");
    dump_raw("sc_step1_raw.bmp");
    printf("step1 bt=%d c2=%d c_=%d br=%d aj=%d bh=%d dbh=%d paints_d=%d\n", g_scene.frame_no, g_dungeon.status, g_dungeon.pos_x >> 16,
           game_dbg(9), game_dbg(10), game_dbg(11), game_dbg(12), game_paint_count(1));
    tap_h('2'); run(24);
    dump("sc_step2.bmp");
    fflush(stderr); fflush(stdout); fprintf(stderr, "MARK step2\n"); fflush(stderr);
    dump_raw("sc_step2_raw.bmp");
    tap_h('4'); run(24);                     // turn left
    dump("sc_turn.bmp");
    dump_raw("sc_turn_raw.bmp");
    printf("turn bt=%d paints_d=%d\n", g_scene.frame_no, game_paint_count(1));
    remove("KKM.rms"); remove("OPT.rms"); remove("KLP.rms"); remove("MKK.rms");
    return rc;
}
