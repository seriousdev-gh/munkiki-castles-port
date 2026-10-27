// render.h - contract of the scene renderer: faithful port of class c of the
// original jar (the M3D scene assembler + HUD sprite/particle overlays).
// Fields renamed for readability; decompiled/c.java names kept as tags.
#ifndef MK_RENDER_H
#define MK_RENDER_H

#include <stdint.h>
#include "m3d.h"

#define MK_MAX_BLOCKS 384              // bd capacity (c.bd = 384 * 8 ints)
#define MK_MAX_BILLS  128              // ba capacity (c.ba = 128 * 4 ints)
#define MK_BLOCK_STRIDE 8              // ints per bd entry
#define MK_BILL_STRIDE  4              // ints per ba entry
#define MK_SCENE_UNIT  20              // one world cell = 20 camera units (c.m() *20)

typedef struct mk_scene {
    int  shake_ofs;           // Java: bf, shake offset accumulator
    int  pitch_ofs;           // Java: bq, camera pitch offset (free-look)
    int  shake_on;            // Java: br, shake enabled
    int  sunk;                // Java: bs, monkey unsupported (sink view)
    int  frame_no;            // Java: bt, global frame counter (texture anim)
    int  monkey_sprite;       // Java: bh, monkey sprite id (from cm[])
    int  bill_n;              // Java: bc, billboard count
    int  bill_rec[MK_MAX_BILLS * MK_BILL_STRIDE];   // Java: ba, billboards: x,y,z,item(8|16|24|32)
    int  block_n;             // Java: be, block render entries count
    int  block_rec[MK_MAX_BLOCKS * MK_BLOCK_STRIDE];// Java: bd, blocks: x,y,z fixed; type; sx,sy,sz
                              //        (merge)
    int  curtain_state;       // Java: a4, curtain transition state (0 none)
    int  curtain_prog;        // Java: a5, curtain progress
    int  death_parts;         // Java: a6, death particle counter (-1 idle)
    int  smash_parts;         // Java: a7, smash particle counter (-1 idle)
    int  unused;              // Java: a8 (never used)
} mk_scene;

extern mk_scene g_scene;

// ---- lifecycle ----
void r_init(void);                        // ctor: m3d setup + textures + geometry from MK_D@554
void r_reset(int nblocks, int nbills);    // y(): reset render lists
void r_free(void);                        // u(): remove buffers

// ---- scene building (called by engine port) ----
int  r_add_block(int x, int y, int z, int type);   // x(): returns render id (1-based)
int  r_add_billboard(int x, int y, int z, int item); // w()
void r_clear_block(int rid);              // block_rec[(rid-1)*8+3] = 0
void r_clear_billboard(int bid);          // bill_rec[(bid-1)*4+3] = 0

// ---- entity motion (called from the engine's ac() block-physics tick) ----
// block_rec entry per block i: block_rec[i*8 + 0..2] = x,y,z (16.16 fixed,
// world y = height), [+3] = type/flags.  bill_rec entry per billboard i:
// bill_rec[i*4 + 0..2] = x,y,z, [+3] = item.  ids are the 1-based values
// returned by r_add_*.
void r_move_block(int id, int dx, int dy, int dz);      // block_rec += d
void r_move_bill (int id, int dx, int dy, int dz);      // bill_rec  += d
int  r_block_x(int id);   // block_rec[(id-1)*8 + 0]
int  r_block_y(int id);   // block_rec[(id-1)*8 + 1]   (height axis)
int  r_block_z(int id);   // block_rec[(id-1)*8 + 2]
int  r_bill_y (int id);   // bill_rec[(id-1)*4 + 1]   (height axis)
int  r_block_type(int id);  // bd entry's shape/type byte; -1 = no such entry
// ---- per frame ----
void r_set_view_chain(void);              // monkey camera transform applied after m()
void r_frame(uint8_t *fb);                // j(Graphics): 3D + particles + sprite + curtain
void r_shake(void);                       // z(): reset view chain + redraw ground only

// sprite geometry access for the engine's shake handling (b.bf math)
uint8_t *r_fb(void);                      // 96x65 framebuffer

#endif // MK_RENDER_H
