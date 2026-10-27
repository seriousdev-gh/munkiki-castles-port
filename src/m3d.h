// m3d.h - software reimplementation of the Nokia M3D(O) fixed-function 3D
// engine (com.nokia.mid.m3d.M3D), matching the subset Munkiki's Castles uses.
// Fixed 16.16 matrices, unsigned-byte vertex arrays (signed interpretation:
// 0..255 -> -128..127 via (int8_t) scaling x65536), perspective frustum,
// z-buffered flat-shaded triangles + 4-bit gray/alpha textured quads,
// rendered into a 96x65 8-bit gray buffer (255=white .. 0=black).
#ifndef MK_M3D_H
#define MK_M3D_H

#include <stdint.h>

#define M3D_W 96
#define M3D_H 65

// GL-ish constants used by the original
#define M3D_TRIANGLES        4
#define M3D_DEPTH_BUFFER_BIT 16384
#define M3D_COLOR_BUFFER_BIT 256
#define M3D_MODELVIEW        5888
#define M3D_PROJECTION       5889
#define M3D_DEPTH_TEST       2929
#define M3D_CULL_FACE        2884
#define M3D_TEXTURE_2D       3553
#define M3D_VERTEX_ARRAY     32884
#define M3D_TEXTURE_COORD_ARRAY 32888
#define M3D_BACK             1029
#define M3D_TEXTURE0         33984

typedef struct m3d {
    // framebuffers
    uint8_t fb[M3D_W * M3D_H];        // gray 0..255
    float zbuf[M3D_W * M3D_H];        /* FJ: raw z_view floats, larger wins */
    // state
    int width, height;
    int clear_r;                      // gray
    float mv[4][4], pj[4][4], mp[4][4]; // modelview, projection, modelview*proj
    float stk[8][4][4]; int sp;
    int mode;                         // MODELVIEW/PROJECTION
    int depth_test, cull, texture_en;
    int client_vertex, client_tex;
    uint8_t color[4];                 // rgba 0..255
    const uint8_t *vtx; int vtx_sz, vtx_stride;   // vertexPointerub
    const uint8_t *tex; int tex_sz, tex_stride;   // texCoordPointerub
    const uint8_t *texbits; int tex_w, tex_h;     // bindTexture data (nibbles)
    const uint8_t *texmask;                       // optional mask (1bpp)
    float eye[4];                     // derived: inverse projection eye pos
    float pfx, pfy;                   // frustum focal scales 2N/W, 2N/H
} m3d;


extern int g_dbg_mk_sy, g_dbg_mk_sz, g_dbg_mk_sp;   /* TEMP sign-sweep knobs (env) */
m3d *m3d_get(void);

// GL-like calls (xi = fixed 16.16 args)
void m3d_setupBuffers(m3d *m, int flags, int w, int h);
void m3d_clearColor4ub(m3d *m, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void m3d_enable(m3d *m, int e);
void m3d_disable(m3d *m, int e);
void m3d_cullFace(m3d *m, int m_);
long m3d_cull_skips(void);
void m3d_viewport(m3d *m, int x, int y, int w, int h);
void m3d_matrixMode(m3d *m, int mode);
// NOTE: parameter order is (l,r,t,b,...) exactly as m3d.c defines it: the
// game passes t<0,b>0 (inverted screen Y); naming these (b,t) invites a
// future caller to swap them and silently flip the projection.
void m3d_frustumxi(m3d *m, int l, int r, int t, int b, int n, int f);
void m3d_enableClientState(m3d *m, int a);
void m3d_disableClientState(m3d *m, int a);
void m3d_loadIdentity(m3d *m);
void m3d_pushMatrix(m3d *m);
void m3d_popMatrix(m3d *m);
void m3d_translatexi(m3d *m, int x, int y, int z);
void m3d_rotatexi(m3d *m, int ang, int x, int y, int z);
void m3d_scalexi(m3d *m, int x, int y, int z);
void m3d_color4ub(m3d *m, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void m3d_vertexPointerub(m3d *m, int size, int stride, const uint8_t *v);
void m3d_texCoordPointerub(m3d *m, int size, int stride, const uint8_t *v);
void m3d_bindTexture(m3d *m, int target, const uint8_t *tex);
void m3d_setTexture(m3d *m, const uint8_t *nib, int w, int h); // nibbles gray4/alpha4
void m3d_drawElementsub(m3d *m, int mode, int count, const uint8_t *idx);
void m3d_drawArrays(m3d *m, int mode, int first, int count);
void m3d_removeBuffers(m3d *m);

// 2D blit of the 3D buffer into a target gray buffer (DirectGraphics blit)
void m3d_blit(m3d *m, uint8_t *dst, int x, int y, int w, int h);

// ---- DirectGraphics 1bpp/gray sprite helpers (used by c.java / KKM) ----
// drawPixels: src 1bpp MSB-first rows, optional 1bpp mask, transfer:
//   0 = SRC (set), TRANSFORM_HORIZONTAL_MIRROR_MASK = 8192 mirror.
// sprite format from 'd' bi[]: gray+alpha nibbles for gray assets; here
// we keep the generic 1bpp form the game uses for masks + mono sprites.
void m3d_drawPixels1bpp(const uint8_t *src, const uint8_t *srcmask, int srcoff,
                        int rowStridePx, int x, int y, int w, int h,
                        int transfer, int plane, uint8_t *dst);
// gray/nibble sprite blit (2bpp-packed gray4|alpha4) with mirror flag 8192
void m3d_drawPixelsNib(const uint8_t *src, int x, int y, int w, int h,
                       int transfer, uint8_t *dst);

#endif // MK_M3D_H
