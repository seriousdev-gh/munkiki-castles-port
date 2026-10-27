// m3d.c - software reimplementation of the Nokia M3D(O) fixed-function 3D
// engine subset that Munkiki's Castles (2002, Nokia 3410) drives through
// com.nokia.mid.m3d.M3D. Cross-checked against the open-source FreeJ2ME M3D
// implementation (github.com/hex007/freej2me, com/nokia/mid/m3d/M3D.java),
// which reproduces the device behaviour this game needs.
//
// Conventions (evidence in the port report):
//  - Matrices are row-major 4x4; points are row vectors p' = p * M, so a GL
//    post-multiply op T becomes M = T * M (exactly FreeJ2ME's matmul order).
//  - Vertex arrays are unsigned bytes read SIGNED ((int8_t)v); value counts
//    world units in float (device used 16.16 fixed; pixels don't care).
//  - Camera space: right-handed, looking down -Z. frustumxi stores l/r/b/t/n/f
//    in 16.16; projection mirrors FreeJ2ME's projection(): m0=2n/(r-l),
//    m5=2n/(t-b), m10=-(f+n)/(f-n), m11=-1, m14=-2fn/(f-n).
//  - Screen mapping = standard GL with y-flip: px = (xn*0.5+0.5)*W,
//    py = (1-(yn*0.5+0.5))*H. (FreeJ2ME reaches the identical result via
//    "no flip" + swapped top/bottom args; the ground horizon lands at row ~28
//    of 65, matching "straight horizon, ground wedge over the bottom third".)
//  - Near plane: triangles are Sutherland-Hodgeman clipped in camera space at
//    z <= -n with vertex ORDER PRESERVED, so c.z()'s ground triangle (apex at
//    the eye) clips to a polygon whose screen winding matches the block
//    models' front winding. Front face = signed area < 0 in top-left-origin
//    pixel coords (== CCW in GL math convention). Evidence: the au/at top fan
//    and the a0 shell project A<0 while facing the camera, and the
//    double-wound billboard index arrays aq/ax (each quad listed with both
//    windings) only make sense with a real winding cull.
//  - Texture coords (texCoordPointerub) are whole-number wrap units: byte 1
//    spans one full texture repeat (c.ar = {0,1} quads show the full 32x32
//    item sprites; c.ay = {0..4} tiles the deliberately seamless warp texture
//    4x4 across the portal quad). v = 0 is the FIRST row of the array.
//    Interpolation is affine in screen space (fixed-function era behaviour;
//    FreeJ2ME identical).
//  - Texel byte = gray4<<4 | alpha4; alpha4==0 discards; color = modulate:
//    out = vertexGray * gray4 / 15. Textured pixels write depth too.
//  - Depth: ndc.z quantized to int16, test LESS_OR_EQUAL (the coplanar
//    colored overlay faces must overwrite the black silhouette - c.j() relies
//    on it), write on pass.
#include "m3d.h"
#include "r_ext.h"
#include "mk_env.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>

static int g_tri_dbg = -1;   /* -1 = uninit; lazily resolved from MK_TRI env */
static int g_scr_cap = 40;   /* MK_SCR env: max 'scr' projection log lines */
static int tri_dbg(void)
{
    if (g_tri_dbg < 0) {
        g_tri_dbg = mk_env_on("MK_TRI") ? 1 : 0;
        { const char *e = getenv("MK_SCR");
          if (e) g_scr_cap = atoi(e); }
    }
    return g_tri_dbg;
}
static int g_tri_log2;
static int g_tri_log3;
static int g_tri_log4;
static int g_tri_log5;
static int g_tri_log6;
static int g_tri_logx;
static int g_ras_cnt;
static int g_tri_logy;
static int g_tri_log;

#define M3D_RAD (3.14159265358979323846 / 180.0)

// 16.16 fixed-point -> float: the M3D *xi entry points receive 16.16 values
// and these are the only sites where that scale factor is divided out.
#define M3D_FP   65536.0           // 16.16 fixed -> double (frustumxi)
#define M3D_FP_F 65536.0f          // 16.16 fixed -> float (translate/rotate/scale)
// Empty z-buffer sentinel: INT16_MIN as float.
#define ZBUF_MIN (-32768.0f)
// Framebuffer clear: the device draws on white.
#define FB_WHITE 255

static m3d g_m3d;

// Engine state with no slot in the frozen m3d struct (single instance anyway):
static int   g_vpx, g_vpy, g_vpw, g_vph;   // viewport
static float g_near = 3.0f;                // near plane, float units
static int   g_keep_pos_z = -1;            // 1 = keep z>=+n (forward=+z), 0 = keep z<=-n
static int   g_front_pos_A = -1;           // 1 = front face has A>0
static int   g_cullface = M3D_BACK;
static long  g_cull_skips;          /* faces skipped by the winding cull */

/* TEMP sign-sweep knobs: env-gated, read once at setupBuffers */
int g_dbg_mk_sy = 0, g_dbg_mk_sz = 0, g_dbg_mk_sp = 0, g_dbg_mv = 0;
/* NOTE: the chase-camera pitch/height knobs (MK_P/MK_H) were removed after
   the FreeJ2ME-master convention audit: the original d.java chain (+20 deg
   pitch, eye 0.5 cells above floor anchor) renders correctly with this
   driver's matrix accumulation; the earlier "-20/hfix=12" calibration had
   been fitted to a mis-cropped reference capture and is gone. */
void m3d_init_env(void)
{
    g_front_pos_A = mk_env_on("MK_ANEG") ? 0 : 1;
    g_keep_pos_z = mk_env_on("MK_ZPOS") ? 1 : 0;
    g_dbg_mk_sy = mk_env_on("MK_SY") ? 1 : 0;
    g_dbg_mk_sz = mk_env_on("MK_SZ") ? 1 : 0;
    g_dbg_mk_sp = mk_env_on("MK_SP") ? 1 : 0;
    g_dbg_mv = mk_env_on("MK_MV") ? 1 : 0;
}

m3d *m3d_get(void) { return &g_m3d; }

// ---------------------------------------------------------------- matrices
static void m_ident(float d[4][4])
{
    int i, j;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            d[i][j] = (i == j) ? 1.0f : 0.0f;
}

static void m_mul(float r[4][4], const float a[4][4], const float b[4][4])
{
    float t[4][4];
    int i, j, k;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) {
            float s = 0.0f;
            for (k = 0; k < 4; k++)
                s += a[i][k] * b[k][j];
            t[i][j] = s;
        }
    memcpy(r, t, sizeof t);
}

// GL "post-multiply" op application: with row vectors the op multiplies from
// the left (v' = v * OP * M), matching FreeJ2ME matmul(op, matrix).
static void m_applyop(m3d *m, const float op[4][4])
{
    float tmp[4][4];
    if (m->mode == M3D_PROJECTION) {
        m_mul(tmp, op, m->pj);
        memcpy(m->pj, tmp, sizeof m->pj);
    } else {
        m_mul(tmp, op, m->mv);
        memcpy(m->mv, tmp, sizeof m->mv);
    }
    if (tri_dbg() && g_tri_logy < 10) {
        int i2;
        g_tri_logy++;
        for (i2 = 0; i2 < 4; i2++)
            fprintf(stderr, "op%d mv%d: %.4g %.4g %.4g %.4g\n", (int)g_tri_logy, i2,
                    (double)m->mv[i2][0], (double)m->mv[i2][1], (double)m->mv[i2][2], (double)m->mv[i2][3]);
    }
}

static void xf(const float M[4][4], float x, float y, float z,
               float *ox, float *oy, float *oz, float *ow)
{
    *ox = x * M[0][0] + y * M[1][0] + z * M[2][0] + M[3][0];
    *oy = x * M[0][1] + y * M[1][1] + z * M[2][1] + M[3][1];
    *oz = x * M[0][2] + y * M[1][2] + z * M[2][2] + M[3][2];
    *ow = x * M[0][3] + y * M[1][3] + z * M[2][3] + M[3][3];
}

// ------------------------------------------------------------------ state
void m3d_setupBuffers(m3d *m, int flags, int w, int h)
{
    int i;
    (void)flags;                    // 1|32: color+z buffers, always both here
    m->width = w;
    m->height = h;
    m->clear_r = FB_WHITE;          // device default white, like FreeJ2ME
    memset(m->fb, FB_WHITE, sizeof m->fb);
    for (i = 0; i < M3D_W * M3D_H; i++)
        m->zbuf[i] = ZBUF_MIN;
    m_ident(m->mv);
    m_ident(m->pj);
    memcpy(m->mp, m->mv, sizeof m->mp);
    m->sp = 0;
    m->mode = M3D_MODELVIEW;
    m->depth_test = m->cull = m->texture_en = 0;
    m->client_vertex = m->client_tex = 0;
    m->color[0] = m->color[1] = m->color[2] = 0;
    m->color[3] = 255;
    m->vtx = 0; m->tex = 0;
    m->texbits = 0; m->texmask = 0; m->tex_w = m->tex_h = 0;
    g_vpx = 0; g_vpy = 0; g_vpw = w; g_vph = h;
    m3d_init_env();
}

void m3d_clearColor4ub(m3d *m, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    (void)g; (void)b; (void)a;
    m->clear_r = r;                 // gray buffer; game always passes r==g==b
}

void m3d_clear(m3d *m, int mask)
{
    int i;
    if (mask & M3D_COLOR_BUFFER_BIT)
        memset(m->fb, m->clear_r, sizeof m->fb);
    if (mask & M3D_DEPTH_BUFFER_BIT)
        /* FJ: float zbuf of negated z_view, larger (nearer) wins with
           test  stored_new >= old; empty sentinel = very negative */
        for (i = 0; i < M3D_W * M3D_H; i++)
            m->zbuf[i] = ZBUF_MIN;
}

void m3d_enable(m3d *m, int e)
{
    switch (e) {
    case M3D_DEPTH_TEST:   m->depth_test = 1; break;
    case M3D_CULL_FACE:    m->cull = 1; break;
    case M3D_TEXTURE_2D:   m->texture_en = 1; break;
    case M3D_VERTEX_ARRAY: m->client_vertex = 1; break;
    case M3D_TEXTURE_COORD_ARRAY: m->client_tex = 1; break;
    default: break;
    }
}

void m3d_disable(m3d *m, int e)
{
    switch (e) {
    case M3D_DEPTH_TEST:   m->depth_test = 0; break;
    case M3D_CULL_FACE:    m->cull = 0; break;
    case M3D_TEXTURE_2D:   m->texture_en = 0; break;
    case M3D_VERTEX_ARRAY: m->client_vertex = 0; break;
    case M3D_TEXTURE_COORD_ARRAY: m->client_tex = 0; break;
    default: break;
    }
}

void m3d_matrixMode(m3d *m, int mode) { m->mode = mode; }

void m3d_enableClientState(m3d *m, int a)
{
    if (a == M3D_VERTEX_ARRAY) m->client_vertex = 1;
    else if (a == M3D_TEXTURE_COORD_ARRAY) m->client_tex = 1;
}

void m3d_disableClientState(m3d *m, int a)
{
    if (a == M3D_VERTEX_ARRAY) m->client_vertex = 0;
    else if (a == M3D_TEXTURE_COORD_ARRAY) m->client_tex = 0;
}

void m3d_cullFace(m3d *m, int face) { (void)m; g_cullface = face; }
long m3d_cull_skips(void) { return g_cull_skips; }

void m3d_viewport(m3d *m, int x, int y, int w, int h)
{
    (void)m; g_vpx = x; g_vpy = y; g_vpw = w; g_vph = h;
}

void m3d_frustumxi(m3d *m, int l, int r, int t, int b, int n, int f)
{
    /* FreeJ2ME master verbatim: near=-n, far=-f; projm m[0]=2n/(r-l),
       m[5]=2n/(t-b) (game passes t=-H/2, b=+H/2 -> m[5]<0), m[11]=-1
       -> w=-z_view; FJ maps px=(x/w+1)*W/2, py=(y/w+1)*H/2 with w=-z.
       Visible half-space is z <= -near (front = -z, OpenGL-style).
         sx = ((2N/W)x/(-z) + 1)*W/2, sy = ((2N/(T-B))y/(-z) + 1)*H/2,
         depth = raw negative z_view (FJ float zbuf, >= wins). */
    double L = l / M3D_FP, R = r / M3D_FP, T = t / M3D_FP, B = b / M3D_FP;
    double N = n / M3D_FP, F = f / M3D_FP;
    (void)F;
    memset(m->pj, 0, sizeof m->pj);
    m->pfx = (float)(2.0 * N / (R - L));      /* >0 */
    m->pfy = (float)(2.0 * N / (T - B));      /* <0: T<B (game quirk) */
    g_near = (float)N;
    if (g_vpw <= 0) { g_vpx = 0; g_vpy = 0; g_vpw = m->width; g_vph = m->height; }
}

void m3d_loadIdentity(m3d *m)
{
    if (m->mode == M3D_PROJECTION) m_ident(m->pj);
    else m_ident(m->mv);
}

void m3d_pushMatrix(m3d *m)
{
    if (m->sp < 8) {
        memcpy(m->stk[m->sp], m->mv, sizeof m->mv);
        m->sp++;
    }
}

void m3d_popMatrix(m3d *m)
{
    if (m->sp > 0) {
        m->sp--;
        memcpy(m->mv, m->stk[m->sp], sizeof m->mv);
    }
}

void m3d_translatexi(m3d *m, int x, int y, int z)
{
    float T[4][4];
    m_ident(T);
    T[3][0] = (float)x / M3D_FP_F;
    T[3][1] = (float)y / M3D_FP_F;
    T[3][2] = (float)z / M3D_FP_F;
    m_applyop(m, T);
}

void m3d_rotatexi(m3d *m, int ang, int x, int y, int z)
{
    // angle: 65536 = 1 degree (d.m() rotatexi(1310720,..) == 20 deg; the yaw
    // wraps at 23592960 = 360 deg). Axis selector mirrors FreeJ2ME: X, then Y,
    // then Z; the game always passes exactly one 65536 selector.
    float a = ((float)ang / M3D_FP_F) * (float)M3D_RAD;
    float c = (float)cos(a), s = (float)sin(a);
    float R[4][4];
    m_ident(R);
    if (x != 0) {
        // M[input][output], RH +ang: y' = c*y - s*z, z' = s*y + c*z
        R[1][1] = c;  R[2][1] = -s;
        R[1][2] = s;  R[2][2] = c;
    }
    if (y != 0) {
        /* FreeJ2ME yaw verbatim (row-major rows [c,0,-s] / [s,0,c]):
           x' = c*x + s*z, z' = -s*x + c*z — opposite handedness to the
           textbook RH +ang about +y. The reference emulator that renders
           the original correctly uses exactly this; match it. */
        R[0][0] = c;  R[0][2] = -s;
        R[2][0] = s;  R[2][2] = c;
    }
    if (z != 0) {
        // RH +ang about +z: x' = c*x - s*y, y' = s*x + c*y
        R[0][0] = c;  R[1][0] = -s;
        R[0][1] = s;  R[1][1] = c;
    }
    m_applyop(m, R);
}

void m3d_scalexi(m3d *m, int x, int y, int z)
{
    float S[4][4];
    m_ident(S);
    S[0][0] = (float)x / M3D_FP_F;
    S[1][1] = (float)y / M3D_FP_F;
    S[2][2] = (float)z / M3D_FP_F;
    m_applyop(m, S);
}

void m3d_color4ub(m3d *m, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    m->color[0] = r; m->color[1] = g; m->color[2] = b; m->color[3] = a;
}

void m3d_vertexPointerub(m3d *m, int size, int stride, const uint8_t *v)
{
    m->vtx = v; m->vtx_sz = size; m->vtx_stride = stride;
}

void m3d_texCoordPointerub(m3d *m, int size, int stride, const uint8_t *v)
{
    m->tex = v; m->tex_sz = size; m->tex_stride = stride;
}

// --- textures: bindTexture only carries a pointer, so sizes come from a tiny
// registry fed by m3d_setTexture (the original bound Texture objects).
typedef struct { const uint8_t *nib; int w, h; } texent;
static texent g_texreg[8];
static int g_ntex;

void m3d_setTexture(m3d *m, const uint8_t *nib, int w, int h)
{
    int i;
    (void)m;
    for (i = 0; i < g_ntex; i++)
        if (g_texreg[i].nib == nib) { g_texreg[i].w = w; g_texreg[i].h = h; return; }
    if (g_ntex < 8) {
        g_texreg[g_ntex].nib = nib;
        g_texreg[g_ntex].w = w;
        g_texreg[g_ntex].h = h;
        g_ntex++;
    }
}

void m3d_bindTexture(m3d *m, int target, const uint8_t *tex)
{
    int i;
    (void)target;
    m->texbits = tex;
    m->tex_w = m->tex_h = 0;
    for (i = 0; i < g_ntex; i++)
        if (g_texreg[i].nib == tex) { m->tex_w = g_texreg[i].w; m->tex_h = g_texreg[i].h; }
}

// ---------------------------------------------------------------- triangles
typedef struct { float x, y, z, u, v; } v3t;   // camera space, pre-clip

static float lerp_f(float a, float b, float t) { return a + (b - a) * t; }
static int mk_bbox(float a, float b, float c, int ismin)
{
    float v = a;
    int i;
    if (ismin) {
        if (b < v) v = b;
        if (c < v) v = c;
        i = (int)v;                       /* floor for v >= 0 */
    } else {
        if (b > v) v = b;
        if (c > v) v = c;
        i = (int)v;                       /* trunc toward 0 */
        if ((float)i < v) i++;            /* ceil */
    }
    return i;
}


static void m3d_raster(m3d *m,
                       float x1, float y1, float z1, float u1, float v1,
                       float x2, float y2, float z2, float u2, float v2,
                       float x3, float y3, float z3, float u3, float v3)
{
    volatile float vx1 = x1, vy1 = y1, vx2 = x2, vy2 = y2, vx3 = x3, vy3 = y3;
    float A = (x2 - x1) * (y3 - y1) - (x3 - x1) * (y2 - y1);
    if (tri_dbg() && g_tri_log6 < 4000) {
        g_tri_log6++;
        fprintf(stderr, "ent %d %d %d %d %d %d A=%d\n",
                (int)vx1, (int)vy1, (int)vx2, (int)vy2, (int)vx3, (int)vy3, (int)A);
    }
    int minx, maxx, miny, maxy, px, py;
    int gray, textured, tw, th;
    if (A == 0.0f) return;
    /* FJ M3D never culls (cullFace() { } // guessing): order is resolved
       by the float depth test alone, so windings must both render. */
    textured = (m->texture_en && m->texbits && m->tex_w > 0 && m->tex_h > 0);
    tw = m->tex_w; th = m->tex_h;
    gray = m->color[0];
    minx = mk_bbox(x1, x2, x3, 1);
    maxx = mk_bbox(x1, x2, x3, 0);
    miny = mk_bbox(y1, y2, y3, 1);
    maxy = mk_bbox(y1, y2, y3, 0);
    if (tri_dbg() && g_tri_log5 < 4000) {
        g_tri_log5++;
        fprintf(stderr, "flt %d %d %d %d | vol %d %d %d %d %d %d\n",
                (int)fminf(fminf(vx1, vx2), vx3), (int)fmaxf(fmaxf(vx1, vx2), vx3),
                (int)fminf(fminf(vy1, vy2), vy3), (int)fmaxf(fmaxf(vy1, vy2), vy3),
                (int)vx1, (int)vy1, (int)vx2, (int)vy2, (int)vx3, (int)vy3);
    }
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx > m->width - 1) maxx = m->width - 1;
    if (maxy > m->height - 1) maxy = m->height - 1;

    int dbg_n = 0, dbg_pass = 0, dbg_z = 0;

    for (py = miny; py <= maxy; py++) {
        float fy = (float)py + 0.5f;
        for (px = minx; px <= maxx; px++) {
            float fx = (float)px + 0.5f;
            float e1 = (x3 - x2) * (fy - y2) - (y3 - y2) * (fx - x2);
            float e2 = (x1 - x3) * (fy - y3) - (y1 - y3) * (fx - x3);
            float e3 = A - e1 - e2;
            float l1, l2, l3, z, zq;
            int idx;
            if (A < 0.0f) { if (e1 > 0.0f || e2 > 0.0f || e3 > 0.0f) continue; }
            else          { if (e1 < 0.0f || e2 < 0.0f || e3 < 0.0f) continue; }
            dbg_pass++;
            l1 = e1 / A; l2 = e2 / A; l3 = e3 / A;
            z = l1 * z1 + l2 * z2 + l3 * z3;            // affine (era behavior)
            /* FJ: affine raw z_view (-3 near .. -1000 far), float zbuf,
               larger (nearer) wins: draw if depth >= stored */
            zq = z;
            if (zq < -32767.0f) zq = -32767.0f;
            if (zq > 32767.0f) zq = 32767.0f;
            idx = py * M3D_W + px;
            if (m->depth_test && zq < m->zbuf[idx]) { dbg_z++; continue; }
            if (textured) {
                float u = l1 * u1 + l2 * u2 + l3 * u3;
                float v = l1 * v1 + l2 * v2 + l3 * v3;
                int tu = (int)floorf(u * (float)tw) & (tw - 1); // pow2 wrap
                int tv = (int)floorf(v * (float)th);
                int tb;
                if (tv < 0) tv = -tv;
                tv &= (th - 1);
                tb = m->texbits[tv * tw + tu];
                if ((tb & 15) == 0) continue;                   // alpha discard
                /* Texture(int,int,Image) device/FJ2ME mapping: map() returns
                   colors[px&1] = {white,black} -- a 1bpp texel inverts (white
                   glyph -> black ink, black field -> white paper). */
                m->fb[idx] = (uint8_t)((gray * (15 - (tb >> 4))) / 15);
            } else {
                m->fb[idx] = (uint8_t)gray;
            }
            if (m->depth_test) m->zbuf[idx] = zq;
            dbg_n++;
        }
    }
    if (tri_dbg()) {
        g_ras_cnt++;
        if (g_ras_cnt <= 5000 || dbg_n > 800)   /* always see wall-sized faces */
            fprintf(stderr, "ras#%d n=%d pass=%d zkill=%d bbox(%d..%d,%d..%d) A=%.0f tex=%d cullskip=%d\n",
                    g_ras_cnt, dbg_n, dbg_pass, dbg_z, minx, maxx, miny, maxy, A, textured,
                    (m->cull && g_cullface == M3D_BACK && A > 0.0f));
    }
}

static void m3d_tri(m3d *m, const v3t *p)
{
    v3t poly[4];
    int np = 0, i, j;
    float znear = -g_near;                      /* FJ: keep z <= -near */
    for (i = 0; i < 3; i++) {
        const v3t *a = &p[i], *b = &p[(i + 1) % 3];
        int ain = a->z <= znear, bin = b->z <= znear;
        if (ain) poly[np++] = *a;
        if (ain != bin) {
            float t = (znear - a->z) / (b->z - a->z);
            v3t c;
            c.x = lerp_f(a->x, b->x, t);
            c.y = lerp_f(a->y, b->y, t);
            c.z = znear;
            c.u = lerp_f(a->u, b->u, t);
            c.v = lerp_f(a->v, b->v, t);
            poly[np++] = c;
        }
    }
    if (tri_dbg() && g_tri_log < 30000) {
        g_tri_log++;
        fprintf(stderr, "tri np=%d cull=%d tex=%d | cam %.1f,%.1f,%.1f %.1f,%.1f,%.1f %.1f,%.1f,%.1f | col %d\n",
                np, m->cull, m->texture_en,
                p[0].x, p[0].y, p[0].z, p[1].x, p[1].y, p[1].z,
                p[2].x, p[2].y, p[2].z, m->color[0]);
        if (g_tri_log <= 4) {
            fprintf(stderr, "mv %d: %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g\n",
                    (int)g_tri_log,
                    m->mv[0][0], m->mv[0][1], m->mv[0][2], m->mv[0][3],
                    m->mv[1][0], m->mv[1][1], m->mv[1][2], m->mv[1][3],
                    m->mv[2][0], m->mv[2][1], m->mv[2][2], m->mv[2][3],
                    m->mv[3][0], m->mv[3][1], m->mv[3][2], m->mv[3][3]);
        }
    }
    if (np < 3) {
        static int g_tri_logk;
        if (tri_dbg() && m->texture_en && g_tri_logk < 6) {
            g_tri_logk++;
            fprintf(stderr, "NPKILL np=%d znear=%.4g z=%.1f,%.1f,%.1f\n",
                    np, (double)znear, p[0].z, p[1].z, p[2].z);
        }
        return;
    }

    {
        if (tri_dbg() && g_tri_log4 < 12) {
            g_tri_log4++;
            fprintf(stderr, "raw np=%d:", np);
            for (j = 0; j < np; j++)
                fprintf(stderr, " [%.3f,%.3f,%.3f]", poly[j].x, poly[j].y, poly[j].z);
            fprintf(stderr, "\n");
        }
        float sx[4], sy[4], sz[4], su[4], sv[4];
        float hx = (float)(g_vpw / 2), hy = (float)(g_vph / 2);
        for (j = 0; j < np; j++) {
            float w = -poly[j].z;                      /* FJ w = -z_view */
            if (w < 1e-3f) w = 1e-3f;
            /* FreeJ2ME M3D master screen mapping verbatim:
               px = (m0*x/w)*(W/2 int) + W/2, py = (m5*y/w)*(H/2 int) + H/2,
               no y-flip. m5 < 0 because the game passes t < 0 to frustum. */
            sx[j] = (m->pfx * poly[j].x / w) * hx + hx + (float)g_vpx;
            sy[j] = (m->pfy * poly[j].y / w) * hy + hy + (float)g_vpy;
            sz[j] = poly[j].z;               /* raw negative z_view depth */
            su[j] = poly[j].u;
            sv[j] = poly[j].v;
        }
        if (tri_dbg() && g_tri_log2 < g_scr_cap) {
            g_tri_log2++;
            fprintf(stderr, "scr col=%d tex=%d |", m->color[0], m->texture_en);
            for (j = 0; j < np; j++)
                fprintf(stderr, " (%.1f,%.1f,%.2f)", sx[j], sy[j], sz[j]);
            fprintf(stderr, "\n");
        }
        /* c.java:536-537: enable(GL_CULL_FACE=2884) + cullFace(GL_BACK=1029).
           FreeJ2ME never implemented this (cullFace(){} "guessing"), but the
           real handset culled: from inside a block ALL its faces are backfaces
           -> the block looks transparent (user-verified original behavior).
           Cull on the post-projection screen-space winding (pfy<0 game quirk
           already folded into sy). */
        if (m->cull && g_cullface == M3D_BACK) {
            for (j = 1; j + 1 < np; j++) {
                float A = (sx[j] - sx[0]) * (sy[j + 1] - sy[0])
                        - (sx[j + 1] - sx[0]) * (sy[j] - sy[0]);
                if (A > 0.0f) {
                    g_cull_skips++;
                    if (tri_dbg())
                        fprintf(stderr, "CULL A=%.0f (%.1f,%.1f)(%.1f,%.1f)(%.1f,%.1f) z=%.1f,%.1f,%.1f tex=%d\n",
                                A, sx[0], sy[0], sx[j], sy[j], sx[j+1], sy[j+1],
                                sz[0], sz[j], sz[j+1], m->texture_en);
                    continue;
                }
                m3d_raster(m, sx[0], sy[0], sz[0], su[0], sv[0],
                           sx[j], sy[j], sz[j], su[j], sv[j],
                           sx[j + 1], sy[j + 1], sz[j + 1], su[j + 1], sv[j + 1]);
            }
        } else
        for (j = 1; j + 1 < np; j++)
            m3d_raster(m, sx[0], sy[0], sz[0], su[0], sv[0],
                       sx[j], sy[j], sz[j], su[j], sv[j],
                       sx[j + 1], sy[j + 1], sz[j + 1], su[j + 1], sv[j + 1]);
    }
}

static void get_vtx(m3d *m, int i, float xyz[3])
{
    int k = i * 3;
    xyz[0] = (float)(int8_t)m->vtx[k + 0];
    xyz[1] = (float)(int8_t)m->vtx[k + 1];
    xyz[2] = (float)(int8_t)m->vtx[k + 2];
}

static void get_uv(m3d *m, int i, float uv[2])
{
    if (m->tex && m->client_tex && m->texture_en) {
        int k = i * 2;
        uv[0] = (float)(int8_t)m->tex[k + 0];
        uv[1] = (float)(int8_t)m->tex[k + 1];
    } else {
        uv[0] = uv[1] = 0.0f;
    }
}

void m3d_drawElementsub(m3d *m, int mode, int count, const uint8_t *idx)
{
    int i, k;
    (void)mode;                     // M3D_TRIANGLES
    m_mul(m->mp, m->mv, m->pj);
    for (i = 0; i + 2 < count; i += 3) {
        v3t p[3];
        for (k = 0; k < 3; k++) {
            int vi = (int)(int8_t)idx[i + k];
            float xyz[3], uv[2], wcomp;
            get_vtx(m, vi, xyz);
            get_uv(m, vi, uv);
            xf(m->mv, xyz[0], xyz[1], xyz[2], &p[k].x, &p[k].y, &p[k].z, &wcomp);
            if (tri_dbg() && g_tri_logx < 12) {
                g_tri_logx++;
                fprintf(stderr, "xf in %d (%.3g,%.3g,%.3g) out (%.1f,%.1f,%.1f) wc=%.3g M11=%.4g\n",
                        (int)g_tri_logx, (double)xyz[0], (double)xyz[1], (double)xyz[2],
                        (double)p[k].x, (double)p[k].y, (double)p[k].z,
                        (double)wcomp, (double)m->mv[1][1]);
            }
            p[k].u = uv[0];
            p[k].v = uv[1];
        }
        m3d_tri(m, p);
    }
}

void m3d_drawArrays(m3d *m, int mode, int first, int count)
{
    /* FJ verbatim (Munkiki's "water/ground" trick, fj_m3d.drawArrays):
       the huge ground triangle is NOT rasterized. Its FIRST vertex (a far
       horizon point) is projected and everything BELOW that screen row is
       filled with the current color:
           y  = (verts[1] after mv) * projm[5] / (-verts[2] after mv)
           py = y*(H/2 int) + H/2 ; fillRect(0, (int)py, W, H-(int)py) */
    float x, y, z, wcomp, py;
    int px2, py2, y0i, gray = m->color[0];
    (void)mode; (void)first; (void)count;
    xf(m->mv, (float)(int8_t)m->vtx[0], (float)(int8_t)m->vtx[1],
       (float)(int8_t)m->vtx[2], &x, &y, &z, &wcomp);
    py = (m->pfy * y / (-z)) * (float)(g_vph / 2) + (float)(g_vph / 2)
         + (float)g_vpy;
    if (tri_dbg()) fprintf(stderr, "FILL v0=(%g,%g,%g) py=%g rect rows %d..%d x %d..%d\n",
                           x, y, z, py, (int)py < 0 ? 0 : (int)py, m->height - 1,
                           g_vpx, g_vpw - 1);
    y0i = (int)py;
    if (y0i < 0) y0i = 0;
    for (py2 = y0i; py2 < g_vph && py2 < m->height; py2++)
        for (px2 = g_vpx; px2 < g_vpw && px2 < m->width; px2++)
            m->fb[py2 * M3D_W + px2] = (uint8_t)gray;
}

void m3d_removeBuffers(m3d *m)
{
    // The original frees native buffers; ours are static, and the shell never
    // renders between u() and the next y()/setup, so nothing to do.
    (void)m;
}

void m3d_blit(m3d *m, uint8_t *dst, int x, int y, int w, int h)
{
    int r;
    for (r = 0; r < h; r++) {
        int yy = y + r;
        if (yy < 0 || yy >= M3D_H) continue;
        memcpy(dst + (size_t)yy * M3D_W + x, m->fb + (size_t)yy * M3D_W + x,
               (size_t)w);
    }
}

// ------------------------------------------------------------- DirectGraphics
void m3d_drawPixels1bpp_clip(const uint8_t *src, const uint8_t *srcmask, int srcoff,
                             int rowStridePx, int x, int y, int w, int h,
                             int transfer, int plane, uint8_t *dst, int dstw,
                             int cx0, int cy0, int cx1, int cy1)
{
    int i, j;
    const int mirror = (transfer & 8192) != 0;   // TRANSFORM_HORIZONTAL_MIRROR
    int dx0 = x, dy0 = y, dx1 = x + w, dy1 = y + h;
    (void)plane;    // -1 (background) and 1 (foreground) blend identically here
    if (dx0 < cx0) dx0 = cx0;
    if (dy0 < cy0) dy0 = cy0;
    if (dx1 > cx1) dx1 = cx1;
    if (dy1 > cy1) dy1 = cy1;
    if (plane == -1) {
        /* Nokia TYPE_BYTE_1_GRAY_VERTICAL (FreeJ2ME case -1): column-major
           1bpp, scanlength = w BYTES; bit (ods+row)&7, LSB-first within the
           byte. Mask: bit 1 = opaque, bit 0 = transparent; src bit 1 = black,
           src bit 0 = PAINTED WHITE: the monkey's white outline ring and its
           white backpack are opaque art, not holes (original drawPixels paints
           them; only mask=0 lets the scene through). */
        int ods = srcoff / rowStridePx;
        int oms = srcoff % rowStridePx;
        for (j = dy0; j < dy1; j++) {
            int col0 = (j - y);                   // data row index
            int byte_ = (ods + col0) >> 3;
            int b = (ods + col0) & 7;
            for (i = dx0; i < dx1; i++) {
                int col = mirror ? (w - 1 - (i - x)) : (i - x);
                const uint8_t *p = src + byte_ * rowStridePx + oms + col;
                const uint8_t *q = srcmask ? srcmask + byte_ * rowStridePx + oms + col : p;
                int s;
                if (srcmask && !((*q >> b) & 1)) continue;  // transparent
                s = (*p >> b) & 1;
                if (!s && (transfer & 3) == 1) continue;    // TRANS_WHITE
                if (s && (transfer & 3) == 2) continue;     // TRANS_BLACK
                dst[(size_t)j * dstw + i] = s ? 0 : 255;
            }
        }
        return;
    }
    for (j = dy0; j < dy1; j++) {
        int row = j - y;                       // source row (dest may be clipped)
        for (i = dx0; i < dx1; i++) {
            int col = mirror ? (w - 1 - (i - x)) : (i - x);
            int sidx = srcoff + row * rowStridePx + col;
            int s = (src[sidx >> 3] >> (7 - (sidx & 7))) & 1;
            if (srcmask &&
                !((srcmask[sidx >> 3] >> (7 - (sidx & 7))) & 1))
                continue;                                // transparent
            if (!s && (transfer & 3) == 1) continue;     // TRANS_WHITE
            if (s && (transfer & 3) == 2) continue;      // TRANS_BLACK
            dst[(size_t)j * dstw + i] = s ? 0 : 255;     // black / painted white
        }
    }
}

void m3d_drawPixels1bpp(const uint8_t *src, const uint8_t *srcmask, int srcoff,
                        int rowStridePx, int x, int y, int w, int h,
                        int transfer, int plane, uint8_t *dst)
{
    m3d_drawPixels1bpp_clip(src, srcmask, srcoff, rowStridePx, x, y, w, h,
                            transfer, plane, dst, M3D_W, 0, 0, M3D_W, M3D_H);
}

void m3d_drawPixelsNib(const uint8_t *src, int x, int y, int w, int h,
                       int transfer, uint8_t *dst)
{
    int i, j;
    const int mirror = (transfer & 8192) != 0;
    for (j = 0; j < h; j++) {
        int yy = y + j;
        if (yy < 0 || yy >= M3D_H) continue;
        for (i = 0; i < w; i++) {
            int xx = x + i;
            int col = mirror ? (w - 1 - i) : i;
            int b = src[j * w + col];
            if ((b & 15) == 0) continue;
            if (xx < 0 || xx >= M3D_W) continue;
            dst[(size_t)yy * M3D_W + xx] = (uint8_t)(b & 0xF0);
        }
    }
}
