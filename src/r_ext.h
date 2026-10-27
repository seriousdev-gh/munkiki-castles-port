// r_ext.h - private contracts between the two renderer files I own
// (src/m3d.c, src/render.c). NOT part of the fixed public headers; nothing
// outside the renderer needs it except the HUD port wanting the odometer
// sprite strip (bl in c.java).
#ifndef MK_R_EXT_H
#define MK_R_EXT_H

#include <stdint.h>
#include "m3d.h"

// M3D's clear(mask). The public m3d.h has the buffer bits but (by omission)
// no clear entry point; z() in c.java needs it. mask:
// M3D_COLOR_BUFFER_BIT fills the gray fb with the current clear color,
// M3D_DEPTH_BUFFER_BIT resets the z-buffer to farthest.
void m3d_clear(m3d *m, int mask);

// m3d_drawPixels1bpp with an explicit destination clip rect [cx0,cy0]..[cx1,cy1)
// (the c.java curtain/win-text code drives Graphics.setClip around drawPixels).
void m3d_drawPixels1bpp_clip(const uint8_t *src, const uint8_t *srcmask, int srcoff,
                             int rowStridePx, int x, int y, int w, int h,
                             int transfer, int plane, uint8_t *dst, int dstw,
                             int cx0, int cy0, int cx1, int cy1);

// The bl odometer digit strip parsed from MK_D (c.bl). 1bpp MSB-first rows,
// black digit glyphs where the bit is 1, white where 0. Caller: d.av() HUD port
// (digit d of the counter sits at x = 24 + 4*d in the strip).
const uint8_t *r_bl_strip(int *w, int *h);

#endif // MK_R_EXT_H
