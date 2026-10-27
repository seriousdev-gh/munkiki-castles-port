#include <math.h>
#include <stdlib.h>
// render.c - faithful port of class c of Munkiki's Castles (decompiled/c.java):
// the M3D scene assembler (blocks, billboards, ground-triangle background
// trick) plus the 2D overlays (smash/death particles, monkey sprite, curtain
// transition). Field/method names mirror c.java wherever the frozen
// src/render.h does not rename them; see comments for the original names.
//
// Frame order (c.j / c.z), preserved exactly:
//   blocks+billboards draw onto the M3D buffer (which holds white + the ground
//   wedge left by the PREVIOUS z()), then blit to the screen buffer, then 2D
//   sprites onto the screen buffer, then z() clears the M3D buffer and re-draws
//   the ground for the next frame.
//
// Geometry: parsed from MK_D+554 in the exact read order of c.ab().
#include "render.h"
#include "m3d.h"
#include "r_ext.h"
#include "engine.h"      // g_dungeon (camera state for the monkey matrix chain)
#include "mk_env.h"
#include "assets.h"
#include <string.h>
#include <stdio.h>

mk_scene g_scene;                       // the one renderer instance (class c *b*)

// ------------------------------------------------------------- static data
// byte[] arrays from MK_D+554 (c.a3..c.at); vertex bytes are read SIGNED by
// the M3D engine, so they are stored as raw unsigned bytes.
static uint8_t block_gray[6];              // gray per block type 1..6
static uint8_t cube_colored_verts[24], cube_colored_idx[36];     // colored faces of the single-cube model
static uint8_t cube_silhouette_verts[60], cube_silhouette_idx[72];     // black silhouette of the single-cube model
static uint8_t ground_tri_verts[9];              // ground triangle (view space, c.z)
static uint8_t warp_quad_verts[12], warp_quad_tex_coords[8], warp_quad_idx[12];   // warp portal quad (double-sided)
static uint8_t box_top_verts[12], box_top_idx[6];      // merged-box colored top
static uint8_t box_silhouette_verts[18], box_silhouette_idx[12];     // merged-box silhouette
// c.as / c.ar / c.aq (class-literal item billboard quad, double-sided):
static uint8_t billboard_quad_verts[12] = {0, 18, 0, 8, 10, 0, 0, 2, 0, 248, 10, 0};  // -8=248
static uint8_t billboard_quad_tex_coords[8]  = {0, 0, 1, 0, 1, 1, 0, 1};
static uint8_t billboard_quad_idx[12] = {0, 1, 2, 2, 3, 0, 0, 2, 1, 2, 0, 3};

// c.bi: 23 1bpp sprites {u8 w, pad, u8 h, pad, (w>>3)*h bytes}
static const uint8_t *sprite_bits[23];
static int sprite_w[23], sprite_h[23];
// c.bl: odometer digit strip (1bpp, black glyphs), for the HUD port
static const uint8_t *odometer_bits;
static int odometer_w, odometer_h;
// c.a9: note/key/warp/life textures (nibbles gray4<<4|alpha4)
static const uint8_t *billboard_tex[4];
// monkey x offset (c.bg). d.java never writes b.bg; kept private.
static int g_monkey_xofs;
// y(): bd/ba capacities are the level's block/billboard counts
static int g_cap_blocks, g_cap_bills;
// screen buffer (Graphics g of paint); r_frame() retargets it each call
static uint8_t g_fb_buf[M3D_W * M3D_H];
static uint8_t *g_dst = g_fb_buf;
// Graphics clip rectangle (Java Graphics state within one paint)
static int g_cx0, g_cy0, g_cx1, g_cy1;


// ------------------------------------------------------------------ helpers
static void clip_full(void)
{
    g_cx0 = 0; g_cy0 = 0; g_cx1 = M3D_W; g_cy1 = M3D_H;
}

// var1.setClip(x, y, w, h) - replaces the clip (normalized, screen-clipped)
static void set_clip(int x, int y, int w, int h)
{
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > M3D_W ? M3D_W : x + w;
    int y1 = y + h > M3D_H ? M3D_H : y + h;
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    g_cx0 = x0; g_cy0 = y0; g_cx1 = x1; g_cy1 = y1;
}

// var1.setColor(-1); var1.fillRect(x, y, w, h) - white fill, clipped
static void fill_white(int x, int y, int w, int h)
{
    int xi, yi;
    for (yi = y; yi < y + h; yi++) {
        if (yi < g_cy0 || yi >= g_cy1) continue;
        for (xi = x; xi < x + w; xi++) {
            if (xi < g_cx0 || xi >= g_cx1) continue;
            g_dst[yi * M3D_W + xi] = 255;
        }
    }
}

// DirectGraphics.drawPixels(bi-array, mask-array, ...): 1bpp, black pixels
static void draw_px(const uint8_t *src, const uint8_t *srcmask, int srcoff,
                    int stride, int x, int y, int w, int h, int transfer, int plane)
{
    m3d_drawPixels1bpp_clip(src, srcmask, srcoff, stride, x, y, w, h,
                            transfer, plane, g_dst, M3D_W,
                            g_cx0, g_cy0, g_cx1, g_cy1);
}

// ------------------------------------------------------------------- c.t()
// fixed sine approximation, 411774 = 2*pi; result ~65536*sin
static int sin_fixed(int angle)
{
    int sign = 1;
    if (angle > 308830) {
        angle = 411774 - angle;
        sign = -1;
    } else if (angle > 205887) {
        angle -= 205887;
        sign = -1;
    } else if (angle > 102943) {
        angle = 205887 - angle;
    }
    {
        int sq = (angle >> 8) * (angle >> 8);
        return sign * (((((1 * (sq >> 8)) - 10882) >> 8) * (sq >> 8) + 65536) >> 8) * (angle >> 8);
    }
}

// ------------------------------------------------------------------ c.v()
// monkey sprite; also resets the clip like the original
static void draw_monkey(void)
{
    int x = ((M3D_W - 40) >> 1) + g_monkey_xofs;
    int y = M3D_H - 3 - 40 + g_scene.shake_ofs;
    int bot_cut = g_scene.shake_ofs > 0 ? 2 + g_scene.shake_ofs : 0;
    if (40 - bot_cut >= 0) {
        const uint8_t *bn = MK_IMG;              // c.bn = imageV3 part 1
        const uint8_t *bm = MK_IMG + 8200;       // c.bm = part 2 (mask)
        if ((g_scene.monkey_sprite & 64) == 0) {
            draw_px(bn, bm, (g_scene.monkey_sprite & 63) * 40 * 40, 40, x, y, 40, 40 - bot_cut,
                    (g_scene.monkey_sprite & -128) != 0 ? 8192 : 0, -1);
        } else {
            draw_px(bn, bm, 43200 + (g_scene.monkey_sprite & 63) * 20 * 40, 20, x, y,
                    20, 40 - bot_cut, 0, -1);
            draw_px(bn, bm, 43200 + (g_scene.monkey_sprite & 63) * 20 * 40, 20, x + 20, y,
                    20, 40 - bot_cut, 8192, -1);
        }
        clip_full();                              // var1.setClip(0,0,bp,bo)
    }
}

// ---------------------------------------------------------------- lifecycle
static void rd_bytes(uint8_t *d, int n, const unsigned char **p)
{
    memcpy(d, *p, (size_t)n);
    *p += n;
}

void r_init(void)
{
    m3d *m = m3d_get();
    const unsigned char *p = MK_D + MK_D_GEOM;   // 'd' stream offset 554
    int i;

    // c.ab(): exact read order
    rd_bytes(block_gray, 6, &p);
    rd_bytes(cube_colored_verts, 24, &p);
    rd_bytes(cube_colored_idx, 36, &p);
    rd_bytes(cube_silhouette_verts, 60, &p);
    rd_bytes(cube_silhouette_idx, 72, &p);
    rd_bytes(ground_tri_verts, 9, &p);
    p += 12;                                     // skipped legacy arrays
    p += 8;
    rd_bytes(warp_quad_verts, 12, &p);
    rd_bytes(warp_quad_tex_coords, 8, &p);
    /* The asset stores warp texcoords in quarter-repeat units (0..4 on a
     * 16-unit quad). A real device shows exactly one spiral-ribbon glyph
     * across the portal (video frames: MK_TEX_WARP mapped once; billboards
     * {0,1} likewise map one texture over their 8-unit quad). Normalize to
     * m3d's 1 unit = 1 texture repeat, matching the device. */
    for (i = 0; i < 8; i++) warp_quad_tex_coords[i] /= 4;
    rd_bytes(warp_quad_idx, 12, &p);
    p += 18;
    p += 12;
    p += 12;
    p += 24;
    rd_bytes(box_top_verts, 12, &p);
    rd_bytes(box_top_idx, 6, &p);
    rd_bytes(box_silhouette_verts, 18, &p);
    rd_bytes(box_silhouette_idx, 12, &p);

    // c.bl = aa(InputStream): u16LE w, u16LE h, (w+7)/8 bytes/row MSB-first
    odometer_w = p[0] | (p[1] << 8);
    odometer_h = p[2] | (p[3] << 8);
    p += 4;
    odometer_bits = p;
    p += ((odometer_w + 7) / 8) * odometer_h;

    // c.bi[]: 23 sprites
    for (i = 0; i < 23; i++) {
        int w = (int)(int8_t)p[0];
        int h = (int)(int8_t)p[2];
        p += 4;
        sprite_bits[i] = p;
        sprite_w[i] = w;
        sprite_h[i] = h;
        p += (w >> 3) * h;
    }

    // c ctor: M3D setup
    m3d_setupBuffers(m, 1 | 32, M3D_W, M3D_H);
    m3d_clearColor4ub(m, 255, 255, 255, 255);
    m3d_enable(m, M3D_DEPTH_TEST);
    if (!mk_env_on("MK_NOCULL")) m3d_enable(m, M3D_CULL_FACE);
    m3d_cullFace(m, M3D_BACK);
    m3d_viewport(m, 0, 0, M3D_W, M3D_H);
    m3d_matrixMode(m, M3D_PROJECTION);
    /* c.java:540 verbatim: frustumxi(-bp<<11, bp<<11, -bo<<11, bo<<11,
       196608, 65536000) — note top is NEGATIVE (t=-2.03, b=+2.03), so
       m5 = 2n/(t-b) < 0, exactly as FreeJ2ME M3D records it. */
    m3d_frustumxi(m, -(M3D_W << 11), M3D_W << 11, -(M3D_H << 11), M3D_H << 11,
                  196608, 65536000);
    m3d_enableClientState(m, M3D_TEXTURE_COORD_ARRAY);
    m3d_matrixMode(m, M3D_MODELVIEW);
    m3d_enableClientState(m, M3D_VERTEX_ARRAY);

    // a8/a9: note, key, warp, life textures from assets.
    // Port deviation (fixes an original wart): the note billboard is an
    // opaque square, so its spinning silhouette reads as a diamond. Punch
    // a circular alpha mask into a private copy; m3d_raster discards
    // texels with alpha4==0, leaving a round icon.
    static uint8_t note_round[sizeof MK_TEX_NOTE];
    for (int k = 0; k < (int)sizeof MK_TEX_NOTE; k++) {
        int dx = 2 * (k % MK_TEX_NOTE_W) + 1 - MK_TEX_NOTE_W;
        int dy = 2 * (k / MK_TEX_NOTE_W) + 1 - MK_TEX_NOTE_H;
        note_round[k] = dx*dx + dy*dy <= MK_TEX_NOTE_W*MK_TEX_NOTE_W
                        ? MK_TEX_NOTE[k] : (uint8_t)(MK_TEX_NOTE[k] & 0xF0);
    }
    billboard_tex[0] = note_round;
    billboard_tex[1] = MK_TEX_KEY;
    billboard_tex[2] = MK_TEX_WARP;
    billboard_tex[3] = MK_TEX_LIFE;
    m3d_setTexture(m, billboard_tex[0], MK_TEX_NOTE_W, MK_TEX_NOTE_H);
    m3d_setTexture(m, billboard_tex[1], MK_TEX_KEY_W, MK_TEX_KEY_H);
    m3d_setTexture(m, billboard_tex[2], MK_TEX_WARP_W, MK_TEX_WARP_H);
    m3d_setTexture(m, billboard_tex[3], MK_TEX_LIFE_W, MK_TEX_LIFE_H);

    g_monkey_xofs = 0;
    g_cap_blocks = 0;
    g_cap_bills = 0;
    clip_full();

    // "Get ready" background: the original ctor is followed by c.z() before
    // the first paint (spec: call the shake once at startup)
    r_shake();
}

// c.y(nblocks, nbills)
void r_reset(int nblocks, int nbills)
{
    g_cap_blocks = nblocks > MK_MAX_BLOCKS ? MK_MAX_BLOCKS : nblocks;
    g_cap_bills = nbills > MK_MAX_BILLS ? MK_MAX_BILLS : nbills;
    memset(g_scene.block_rec, 0, sizeof g_scene.block_rec);
    memset(g_scene.bill_rec, 0, sizeof g_scene.bill_rec);
    g_scene.block_n = 0;
    g_scene.bill_n = 0;
    g_scene.sunk = 0;
    g_scene.pitch_ofs = 0;
    g_monkey_xofs = 0;
    g_scene.shake_ofs = 0;
    g_scene.curtain_state = 0;
    g_scene.curtain_prog = 0;
    g_scene.smash_parts = -1;
    g_scene.death_parts = -1;
    g_scene.shake_on = 1;
}

// c.u()
void r_free(void)
{
    g_scene.block_n = 0;
    g_scene.bill_n = 0;
    g_cap_blocks = 0;
    g_cap_bills = 0;
    m3d_removeBuffers(m3d_get());
}

// --------------------------------------------------------- scene building
// c.w(x, y, z, item): billboard. Returns the 1-based render id (callers in
// d.java add 1 to the original 0-based index; see report).
int r_add_billboard(int x, int y, int z, int item)
{
    if (g_scene.bill_n >= g_cap_bills)
        return 0;
    g_scene.bill_rec[g_scene.bill_n * MK_BILL_STRIDE] = x;
    g_scene.bill_rec[(g_scene.bill_n * MK_BILL_STRIDE) + 1] = y;
    g_scene.bill_rec[(g_scene.bill_n * MK_BILL_STRIDE) + 2] = z;
    g_scene.bill_rec[(g_scene.bill_n * MK_BILL_STRIDE) + 3] = item;
    g_scene.bill_n++;
    return g_scene.bill_n;
}

// c.x(x, y, z, type): block; when the list fills up, greedily merges type-5
// blocks into boxes (sx,sy,sz) and blanks the covered entries. 1-based id.
int r_add_block(int x, int y, int z, int type)
{
    if (g_scene.block_n >= g_cap_blocks)
        return 0;
    {
        int ent_off = g_scene.block_n * MK_BLOCK_STRIDE;
        g_scene.block_rec[ent_off] = x;
        g_scene.block_rec[++ent_off] = y;
        g_scene.block_rec[++ent_off] = z;
        g_scene.block_rec[++ent_off] = type;
        g_scene.block_rec[++ent_off] = 1;
        g_scene.block_rec[++ent_off] = 1;
        g_scene.block_rec[++ent_off] = 1;
        g_scene.block_n++;
    }
    if (g_scene.block_n == g_cap_blocks) {
        int rec_off = 0;
        int rec_no;
        for (rec_no = 0; rec_no < g_scene.block_n; rec_no++) {
            if (g_scene.block_rec[rec_off + 3] == 5) {
                int x0 = g_scene.block_rec[rec_off];
                int y0 = g_scene.block_rec[rec_off + 1];
                int z0 = g_scene.block_rec[rec_off + 2];
                int sx = g_scene.block_rec[rec_off + 4];
                int sy = g_scene.block_rec[rec_off + 5];
                int sz = g_scene.block_rec[rec_off + 6];
                int axis = 0;
                while (axis != 3) {
                    int count, scan_no, scan_off;
                    if (axis == 0) {
                        sx++;
                    } else if (axis == 1) {
                        sy++;
                    } else {
                        sz++;
                    }
                    count = 0;
                    scan_no = rec_no + 1;
                    scan_off = scan_no * MK_BLOCK_STRIDE;
                    for (; scan_no < g_scene.block_n; scan_no++) {
                        if (g_scene.block_rec[scan_off + 3] == 5
                           && g_scene.block_rec[scan_off] < x0 + (sx << 16)
                           && g_scene.block_rec[scan_off] >= x0
                           && g_scene.block_rec[scan_off + 1] < y0 + (sy << 16)
                           && g_scene.block_rec[scan_off + 1] >= y0
                           && g_scene.block_rec[scan_off + 2] < z0 + (sz << 16)
                           && g_scene.block_rec[scan_off + 2] >= z0)
                            count++;
                        scan_off += MK_BLOCK_STRIDE;
                    }
                    if (count != sx * sy * sz - 1) {
                        if (axis == 0) {
                            sx--;
                        } else if (axis == 1) {
                            sy--;
                        } else {
                            sz--;
                        }
                        axis++;
                    }
                }
                if (sx != 1 || sy != 1 || sz != 1) {
                    int blank_no = rec_no + 1;
                    int blank_off = blank_no * MK_BLOCK_STRIDE;
                    for (; blank_no < g_scene.block_n; blank_no++) {
                        if (g_scene.block_rec[blank_off + 3] == 5
                           && g_scene.block_rec[blank_off] < x0 + (sx << 16)
                           && g_scene.block_rec[blank_off] >= x0
                           && g_scene.block_rec[blank_off + 1] < y0 + (sy << 16)
                           && g_scene.block_rec[blank_off + 1] >= y0
                           && g_scene.block_rec[blank_off + 2] < z0 + (sz << 16)
                           && g_scene.block_rec[blank_off + 2] >= z0)
                            g_scene.block_rec[blank_off + 3] = 0;
                        blank_off += MK_BLOCK_STRIDE;
                    }
                    g_scene.block_rec[rec_off + 4] = sx;
                    g_scene.block_rec[rec_off + 5] = sy;
                    g_scene.block_rec[rec_off + 6] = sz;
                }
                g_scene.block_rec[rec_off] = g_scene.block_rec[rec_off] + ((sx - 1) << 15);
                g_scene.block_rec[rec_off + 2] = g_scene.block_rec[rec_off + 2] + ((sz - 1) << 15);
            }
            rec_off += MK_BLOCK_STRIDE;
        }
    }
    return g_scene.block_n;                                  // 1-based (java: 1 + be-1)
}

// bd[rid-1][3] = 0
void r_clear_block(int rid)
{
    if (rid > 0 && rid <= MK_MAX_BLOCKS)
        g_scene.block_rec[((rid - 1) * MK_BLOCK_STRIDE) + 3] = 0;
}

// ba[bid-1][3] = 0
void r_clear_billboard(int bid)
{
    if (bid > 0 && bid <= MK_MAX_BILLS)
        g_scene.bill_rec[((bid - 1) * MK_BILL_STRIDE) + 3] = 0;
}

// ---- entity motion (engine ac() block-physics tick) ---------------------
// ids are the 1-based values returned by r_add_block/r_add_billboard.
// World axis mapping (d.java m()/af()/ac()): bd/ba entry [+1] = height (y),
// [+2] = z.
void r_move_block(int id, int dx, int dy, int dz)
{
    if (id > 0 && id <= MK_MAX_BLOCKS) {
        g_scene.block_rec[((id - 1) * MK_BLOCK_STRIDE) + 0] += dx;
        g_scene.block_rec[((id - 1) * MK_BLOCK_STRIDE) + 1] += dy;   // height
        g_scene.block_rec[((id - 1) * MK_BLOCK_STRIDE) + 2] += dz;
    }
}

void r_move_bill(int id, int dx, int dy, int dz)
{
    if (id > 0 && id <= MK_MAX_BILLS) {
        g_scene.bill_rec[((id - 1) * MK_BILL_STRIDE) + 0] += dx;
        g_scene.bill_rec[((id - 1) * MK_BILL_STRIDE) + 1] += dy;   // height
        g_scene.bill_rec[((id - 1) * MK_BILL_STRIDE) + 2] += dz;
    }
}

int r_block_x(int id)
{
    return id > 0 && id <= MK_MAX_BLOCKS ? g_scene.block_rec[((id - 1) * MK_BLOCK_STRIDE) + 0] : 0;
}

int r_block_y(int id)
{
    return id > 0 && id <= MK_MAX_BLOCKS ? g_scene.block_rec[((id - 1) * MK_BLOCK_STRIDE) + 1] : 0;   // height
}

int r_block_z(int id)
{
    return id > 0 && id <= MK_MAX_BLOCKS ? g_scene.block_rec[((id - 1) * MK_BLOCK_STRIDE) + 2] : 0;
}

// block shape/type byte of a bd entry (c.bd[(id-1)*8+3]); -1 = no such entry
int r_block_type(int id)
{
    return id > 0 && id <= MK_MAX_BLOCKS ? (int)(int8_t)g_scene.block_rec[((id - 1) * MK_BLOCK_STRIDE) + 3]
                                : -1;
}

int r_bill_y(int id)
{
    return id > 0 && id <= MK_MAX_BILLS ? g_scene.bill_rec[((id - 1) * MK_BILL_STRIDE) + 1] : 0;   // height
}

// --------------------------------------------------------------- camera
// d.m() lines 1157-1171: the monkey matrix chain, applied to the current
// modelview (which z() left as identity). Includes the cz clamp of lines
// 1166-1168 because it sits between the two rotation calls.
extern int g_dbg_mv, g_dbg_tri;
void r_set_view_chain(void)
{
    m3d *m = m3d_get();
    // Java invariant: d.m() always runs on the modelview z() left behind,
    // i.e. identity (c.j() ends with c.z()'s loadIdentity). The C driver may
    // invoke m() several times between paints, so re-establish that state
    // here instead of accumulating the chain on top of itself.
    m3d_loadIdentity(m);
    /* Chase-camera view chain, verbatim from d.java:1158-1171. NOTE the
       call order: the driver accumulates M = op*M (FreeJ2ME master) and the
       raster transforms row vectors p*M, so the first call applies LAST.
       Effective per-vertex order: T(-monkey) -> yaw -> pitch(+20, down) ->
       camera offset (0,-0.5,-1cell) = master's chase chain exactly. */
    m3d_translatexi(m, 0, -MK_HALF, -(g_dungeon.freelook != 1 ? 1310720 : 131072));
    if (g_dungeon.freelook != 1) {
        m3d_rotatexi(m, MK_DEG(20), MK_ONE, 0, 0);
    } else {
        m3d_rotatexi(m, g_dungeon.pitch, MK_ONE, 0, 0);
        m3d_rotatexi(m, g_dungeon.yaw, 0, MK_ONE, 0);
        m3d_translatexi(m, 0, 0, g_dungeon.pitch >> 3);
    }
    if (g_dungeon.pos_h < MK_ONE)
        g_dungeon.pos_h = MK_ONE;
    m3d_rotatexi(m, g_dungeon.facing * 90, 0, MK_ONE, 0);
    m3d_translatexi(m, -g_dungeon.pos_x * MK_SCENE_UNIT, (-g_dungeon.pos_h - MK_HALF) * MK_SCENE_UNIT, -g_dungeon.pos_z * MK_SCENE_UNIT);
    if (g_dbg_mv)
        fprintf(stderr, "CHAIN aj=%d cam_y=%g\n", g_dungeon.freelook, (double)m->mv[3][1]);
}

// ------------------------------------------------------------------- c.z()
/* Portal spin: the original increments 11.25 deg (45<<14) Y and 22.5 deg
 * (45<<15) Z-roll per PAINTED frame, which at the device's real paint rate
 * (~6-10 fps) is a slow turn; our 60fps paint makes the portal a twister.
 * Slow BOTH spins by MK_WARP_SPIN (default 4; =1 reproduces the original
 * per-frame increments). */
static int warp_spin_angle(long long inc)
{
    static long long div = -1;
    if (div < 0) {
        const char *e = getenv("MK_WARP_SPIN");
        div = e ? atoi(e) : 4;
        if (div < 1) div = 1;
    }
    return (int)((g_scene.frame_no * inc / div) % (45LL << 19));
}

void r_shake(void)
{
    m3d *m = m3d_get();
    m3d_loadIdentity(m);
    m3d_clear(m, M3D_DEPTH_BUFFER_BIT | M3D_COLOR_BUFFER_BIT);
    if (g_scene.curtain_prog < 16) {
        m3d_color4ub(m, 224, 224, 224, 255);          // (byte)-32
        if (g_scene.pitch_ofs > 0) {
            m3d_scalexi(m, 655360, 655360 + g_scene.pitch_ofs, 655360);
        } else {
            m3d_scalexi(m, 655360, 655360, 655360);
            m3d_translatexi(m, 0, g_scene.pitch_ofs << 1, 0);
        }
        m3d_vertexPointerub(m, 3, 0, ground_tri_verts);
        m3d_drawArrays(m, M3D_TRIANGLES, 0, 3);
        m3d_loadIdentity(m);
    }
}

// ------------------------------------------------------------------- c.j()
void r_frame(uint8_t *fb)
{
    m3d *m = m3d_get();
    int bob, want_monkey, blk_no;
    g_dst = fb ? fb : g_fb_buf;
    clip_full();                                       // fresh paint clip
    if (g_dbg_mv)
        fprintf(stderr, "MVF g_scene.shake_on=%d mv %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g\n",
                g_scene.shake_on,
                m->mv[0][0], m->mv[0][1], m->mv[0][2], m->mv[0][3],
                m->mv[1][0], m->mv[1][1], m->mv[1][2], m->mv[1][3],
                m->mv[2][0], m->mv[2][1], m->mv[2][2], m->mv[2][3],
                m->mv[3][0], m->mv[3][1], m->mv[3][2], m->mv[3][3]);

    bob = sin_fixed((g_scene.frame_no & 15) * 205887 >> 3) >> 3;
    want_monkey = g_scene.shake_on;
    g_scene.frame_no++;

    if (g_scene.curtain_prog < 16) {
        /* Java paint order: z() (clear + horizon ground fill on identity+
           scale10 modelview) runs before j()'s geometry every dungeon tick
           (r_shake(), called right here; java d-tick -> ak.b.z() -> paint).
           The fill line is a fixed horizon; real ground geometry comes from
           block meshes composited over it. */
        r_shake();
        r_set_view_chain(); /* z() leaves identity; d.m() rebuilds it */
        if (g_scene.sunk)
            m3d_translatexi(m, 0, -bob * MK_SCENE_UNIT, 0);

        // ---- blocks
        for (blk_no = 0; blk_no < g_scene.block_n; blk_no++) {
            int btype = (int)(int8_t)g_scene.block_rec[(blk_no * MK_BLOCK_STRIDE) + 3];
            if (btype != 0) {
                int grounded, box_model;
                m3d_pushMatrix(m);
                m3d_translatexi(m, g_scene.block_rec[blk_no * MK_BLOCK_STRIDE] * MK_SCENE_UNIT,
                                g_scene.block_rec[(blk_no * MK_BLOCK_STRIDE) + 1] * MK_SCENE_UNIT,
                                g_scene.block_rec[(blk_no * MK_BLOCK_STRIDE) + 2] * MK_SCENE_UNIT);
                grounded = g_scene.block_rec[(blk_no * MK_BLOCK_STRIDE) + 1] == 0;
                box_model = grounded;
                if (grounded) {
                    if (btype == 2 && bob > 0) {
                        box_model = 0;
                    } else if (btype == 5 && g_scene.block_rec[(blk_no * MK_BLOCK_STRIDE) + 5] > 1) {
                        box_model = 0;
                    }
                    if (!box_model)
                        m3d_translatexi(m, 0, 1310720, 0);
                }
                if (btype == 2 && !box_model) {
                    m3d_scalexi(m, 65536, (grounded ? 0 : 65536) + bob, 65536);
                } else if (btype == 5) {
                    m3d_scalexi(m, g_scene.block_rec[(blk_no * MK_BLOCK_STRIDE) + 4] << 16,
                                box_model ? 65536 : (g_scene.block_rec[(blk_no * MK_BLOCK_STRIDE) + 5] << 16) - (grounded ? 65536 : 0),
                                g_scene.block_rec[(blk_no * MK_BLOCK_STRIDE) + 6] << 16);
                } else if (g_scene.block_rec[(blk_no * MK_BLOCK_STRIDE) + 1] < MK_ONE && !box_model) {
                    m3d_translatexi(m, 0, 1310720 - g_scene.block_rec[(blk_no * MK_BLOCK_STRIDE) + 1] * MK_SCENE_UNIT, 0);
                    m3d_scalexi(m, 65536, g_scene.block_rec[(blk_no * MK_BLOCK_STRIDE) + 1], 65536);
                }

                m3d_color4ub(m, 0, 0, 0, 255);
                // shape is a 3-bit field (1..7) but the palette has 6 entries
                // (a3[] c.java:136); Java threw AIOOBE here, clamp to type 1.
                if ((unsigned)(btype - 1) >= 6u) btype = 1;
                btype = block_gray[btype - 1];
                if (box_model) {
                    m3d_vertexPointerub(m, 3, 0, box_silhouette_verts);
                    m3d_drawElementsub(m, M3D_TRIANGLES, 12, box_silhouette_idx);
                    m3d_color4ub(m, (uint8_t)btype, (uint8_t)btype, (uint8_t)btype, 255);
                    m3d_vertexPointerub(m, 3, 0, box_top_verts);
                    m3d_drawElementsub(m, M3D_TRIANGLES, 6, box_top_idx);
                } else {
                    m3d_vertexPointerub(m, 3, 0, cube_silhouette_verts);
                    m3d_drawElementsub(m, M3D_TRIANGLES, 72, cube_silhouette_idx);
                    m3d_color4ub(m, (uint8_t)btype, (uint8_t)btype, (uint8_t)btype, 255);
                    m3d_vertexPointerub(m, 3, 0, cube_colored_verts);
                    m3d_drawElementsub(m, M3D_TRIANGLES, 36, cube_colored_idx);
                }
                m3d_popMatrix(m);
            }
        }

        // ---- billboards
        m3d_color4ub(m, 255, 255, 255, 255);
        m3d_enable(m, M3D_TEXTURE_2D);
        m3d_enableClientState(m, M3D_TEXTURE_COORD_ARRAY);
        {
            int bill_no;
            for (bill_no = 0; bill_no < g_scene.bill_n; bill_no++) {
                int item = g_scene.bill_rec[(bill_no * MK_BILL_STRIDE) + 3];
                int ti;
                const uint8_t *tx;
                if (item == 0)
                    continue;
                ti = (item >> 3) - 1;
                tx = (ti >= 0 && ti < 4) ? billboard_tex[ti] : 0;
                if (!tx)
                    continue;          // java: AIOOBE aborts the paint (e.g.
                                       // spawn marker item 40); skip instead
                m3d_pushMatrix(m);
                m3d_translatexi(m, g_scene.bill_rec[bill_no * MK_BILL_STRIDE] * MK_SCENE_UNIT,
                                g_scene.bill_rec[(bill_no * MK_BILL_STRIDE) + 1] * MK_SCENE_UNIT,
                                g_scene.bill_rec[(bill_no * MK_BILL_STRIDE) + 2] * MK_SCENE_UNIT);
                // Port deviation: collectibles (note/key/life) spin at half
                // the original rate; the portal's Y-spin AND in-plane Z-roll
                // are slowed by MK_WARP_SPIN (default 4) to compensate our
                // 60fps paint vs the device's lagging real paint rate.
                m3d_rotatexi(m, item == 24 ? warp_spin_angle(45LL << 14)
                                           : (g_scene.frame_no & 63) * 45 << 13,
                             0, MK_ONE, 0);
                m3d_bindTexture(m, M3D_TEXTURE_2D, tx);
                if (item == 24) {
                    m3d_translatexi(m, 0, 655360, 0);
                    m3d_rotatexi(m, warp_spin_angle(45LL << 15), 0, 0, MK_ONE);
                    m3d_vertexPointerub(m, 3, 0, warp_quad_verts);
                    m3d_texCoordPointerub(m, 2, 0, warp_quad_tex_coords);
                    m3d_drawElementsub(m, M3D_TRIANGLES, 12, warp_quad_idx);
                } else {
                    m3d_vertexPointerub(m, 3, 0, billboard_quad_verts);
                    m3d_texCoordPointerub(m, 2, 0, billboard_quad_tex_coords);
                    m3d_drawElementsub(m, M3D_TRIANGLES, 12, billboard_quad_idx);
                }
                m3d_popMatrix(m);
            }
        }
        m3d_disableClientState(m, M3D_TEXTURE_COORD_ARRAY);
        m3d_disable(m, M3D_TEXTURE_2D);

        m3d_blit(m, g_dst, 0, 0, M3D_W, M3D_H);
        if (g_dbg_mv) {
            int q, n = 0;
            for (q = 0; q < M3D_W * M3D_H; q++) if (g_dst[q] < 250) n++;
            fprintf(stderr, "BLIT dst=%p n_black=%d fb[0..3]=%d %d %d %d\n",
                    (void *)g_dst, n, m3d_get()->fb[0], m3d_get()->fb[1],
                    m3d_get()->fb[40 * 96 + 40], m3d_get()->fb[41 * 96 + 40]);
        }

        // ---- smash particles (c.a7)
        if (g_scene.smash_parts != -1) {
            int smash_no;
            for (smash_no = 0; smash_no < 9; smash_no++) {
                int col = smash_no % 3 - 1;
                int row = smash_no / 3 - 1;
                int px = 40 + (col << 1) * (6 + (g_scene.smash_parts * (row == 0 ? 3 : 2) >> 1));
                int py = 20 + row * (12 + (g_scene.smash_parts * (col == 0 ? 15 : 10) >> 3)) + (g_scene.smash_parts * g_scene.smash_parts >> 2);
                if (py < 40)
                    draw_px(sprite_bits[5], sprite_bits[16], 0, 16, px, py, 16, 14, 0, 1);
            }
            if (++g_scene.smash_parts == 20)
                g_scene.smash_parts = -1;
        }

        // ---- death spiral particles (c.a6)
        if (g_scene.death_parts != -1) {
            int death_no;
            for (death_no = 0; death_no < 8; death_no++) {
                int small_sprite = death_no % 3 != 0;
                int spiral_h = ((g_scene.death_parts + death_no) % 5 << 1) + (small_sprite ? 8 - (death_no * 5 & 7) : 0);
                if (spiral_h < g_scene.death_parts << 1 && spiral_h > (g_scene.death_parts << 2) - 28)
                    draw_px(sprite_bits[small_sprite ? 1 : 2], sprite_bits[small_sprite ? 12 : 13], 0,
                            small_sprite ? sprite_w[1] : sprite_w[2],
                            26 + death_no * 5, 48 - spiral_h,
                            small_sprite ? sprite_w[1] : sprite_w[2],
                            small_sprite ? sprite_h[1] : sprite_h[2], 0, 1);
                if (death_no == 3) {
                    draw_monkey();
                    want_monkey = 0;
                }
            }
        }
    } else {
        fill_white(0, 0, M3D_W, M3D_H);
    }

    // ---- curtain transition (c.a4 / c.a5)
    if (g_scene.curtain_state != 0) {
        int prog = g_scene.curtain_prog > 16 ? 16 : g_scene.curtain_prog;
        int strip_no;
        fill_white(0, 65 - g_scene.curtain_prog, 96, 65);

        for (strip_no = 0; strip_no < 3; strip_no++) {
            int x;
            if (strip_no == 2) {
                x = prog < 8 ? prog << 1 : 16;
            } else if (strip_no == 1) {
                x = prog < 12 ? prog * 8 / 3 : 32;
            } else {
                x = prog * 3;
            }
            draw_px(sprite_bits[0], 0, 0, 16, x - 16, strip_no == 0 ? -1 : 0, 16, 53, 0, 1);
            draw_px(sprite_bits[0], 0, 0, 16, 96 - x, strip_no == 0 ? -1 : 0, 16, 53, 8192, 1);
        }

        if (g_scene.curtain_state == 1) {
            int spark_no;
            for (spark_no = 0; spark_no < 8; spark_no++) {
                int x = spark_no * 17 % 80 + 4;
                int y = (spark_no * 29 + g_scene.curtain_prog * 3) % (76 + (spark_no << 1)) - 20;
                int spark_frame = (g_scene.curtain_prog + spark_no) % 12;
                if (spark_frame > 6)
                    spark_frame = 12 - spark_frame;
                if (y < 45 && y < g_scene.curtain_prog * 3)
                    draw_px(sprite_bits[5 + spark_frame], sprite_bits[16 + spark_frame], 0, 16, x, y, 16, 14, 0, 1);
            }
        } else {
            int wing_no;
            for (wing_no = 0; wing_no < 8; wing_no++) {
                int mirror_no = wing_no;
                int x, y;
                if (wing_no == 2 || wing_no == 4)
                    mirror_no = 6 - wing_no;
                x = mirror_no * 17 % 80 + 4;
                y = (mirror_no * 29 + g_scene.curtain_prog * 2) % (76 + (mirror_no << 1)) - 20;
                if (wing_no < 3) {
                    if (g_scene.curtain_prog > 20 && y < g_scene.curtain_prog * 2 - 40)
                        y = (g_scene.curtain_prog + mirror_no) * (g_scene.curtain_prog + mirror_no) * (g_scene.curtain_prog + mirror_no) % 3 + 63;
                    draw_px(sprite_bits[3], sprite_bits[14], 0, 24, x, 65 - y, 24, 26, 0, 1);
                } else if (y < 53 && y < g_scene.curtain_prog * 3) {
                    if (x < 38)
                        x = ((x - 4) >> 1) + 4;
                    else
                        x = 84 - ((84 - x) >> 1);
                    set_clip(x, y, 25, 15);
                    draw_px(sprite_bits[4], sprite_bits[15], 0, 24, x, y / 14 * 14, 24, 14, 0, 1);
                    draw_px(sprite_bits[4], sprite_bits[15], 0, 24, x, (y / 14 + 1) * 14, 24, 14, 0, 1);
                }
                if (wing_no == 2) {
                    draw_monkey();
                    want_monkey = 0;
                }
            }
        }
    }

    if (want_monkey)
        draw_monkey();

    r_shake();
}

uint8_t *r_fb(void)
{
    return g_dst;
}


// odometer strip for the HUD (d.av) port; bits MSB-first, glyph where bit==1
const uint8_t *r_bl_strip(int *w, int *h)
{
    if (w) *w = odometer_w;
    if (h) *h = odometer_h;
    return odometer_bits;
}
