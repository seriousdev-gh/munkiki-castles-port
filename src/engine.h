// engine.h - public contract of the simulation core: faithful C port of class
// d (dungeon/monkey) of the original jar. Symbols are renamed for
// readability; every field keeps its decompiled (decompiled/d.java) name as
// a "Java: xx" tag so the port can still be audited against the original.
#ifndef MK_ENGINE_H
#define MK_ENGINE_H

#include <stdint.h>

#define MK_LEVELS 36            // ids 90..95 (debug) then 1..30

// ---- 16.16 fixed point (d.java positions/velocities/angles are all 16.16) ----
#define MK_FRACT_BITS 16
#define MK_ONE  (1 << MK_FRACT_BITS)   // 65536: one cell, or one M3D degree
#define MK_HALF (1 << 15)              // half a cell: round-before-truncate, eye offset
#define MK_DEG(d) ((d) * MK_ONE)       // M3D rotatexi angle unit is 1 degree

// freelook (d.java aj==1): yaw/pitch clamp at 45 deg, 5 deg per 100ms tick
#define MK_LOOK_LIMIT MK_DEG(45)       // was 2949120
#define MK_LOOK_STEP  MK_DEG(5)        // was 327680

// facing (cx): quarter-turns counted in MK_ONE units, wrapped by the Java
// quantizer cx=(cx+32768)&196608; the renderer multiplies by 90 -> M3D degrees
#define MK_FACING_WRAP 196608          // 3*MK_ONE: mask of the 4 directions

// ---- world grid: mk_cl/mk_ck index = x + (z<<4) + (h<<8), extents 16x8x16 ----
#define MK_GRID_W 16                   // x extent (index step 1)
#define MK_GRID_D 16                   // z extent (index step MK_CELL_STEP_Z)
#define MK_GRID_H 8                    // height extent (index step MK_CELL_STEP_H)
#define MK_CELL_STEP_Z (1 << 4)
#define MK_CELL_STEP_H (1 << 8)

// cl[cell] bits
#define MK_CELL_SHAPE_MASK   7         // shape id (0 empty, 2 wall, 3 smashable, ...)
#define MK_CELL_ITEM_MASK    56        // pickups (am(): 8/16/24/32)
#define MK_CELL_SPAWN_MARK   40        // spawn cell stamp; also reads as note|key
#define MK_CELL_LOW_MASK     127       // shape|items, everything a falling block carries
#define MK_CELL_SUPPORT_MASK 0xC0      // support pair (both bits set = supported)
// ck[cell] bits (render link, 16 bit)
#define MK_CK_ID_MASK    2047          // 1-based r_add_* id (0 = none)
#define MK_CK_ANIM_MASK  2048          // block animating (fall/push)
#define MK_CK_BILLBOARD  4096          // id refers to a billboard, not a block

// ---- phone key event arrays (KKM.an[], b.d[]) ----
#define MK_NKEYS 12                    // '0'..'9', '#', '*'
enum {
    MK_KEY_0 = 0, MK_KEY_1, MK_KEY_2, MK_KEY_3, MK_KEY_4, MK_KEY_5,
    MK_KEY_6, MK_KEY_7, MK_KEY_8, MK_KEY_9,
    MK_KEY_POUND = 10, MK_KEY_STAR = 11,
};

// ---- static tables loaded from assets (mk_load_tables / MBF init) ----
extern int8_t   mk_b1[13];      // MBF header: cell value per type index
extern uint8_t  mk_b0[];        // per-level byte sizes (incl id byte)
extern uint8_t  mk_bw[1285];    // MBF tail blob (level bitstreams)
extern uint8_t  mk_cm[17][24];  // cm[cn][frame] = monkey sprite id (flags 0x40 half, 0x80 mirror)
extern int      mk_cm_len[17];
extern uint8_t  mk_level_ids[MK_LEVELS];

typedef struct mk_shell mk_shell;

// Simulation state. Fields renamed for readability; the original d.java
// obfuscated name is kept as a trailing "Java: xx" tag on every field.
// Coordinates fixed 16.16: pos_x = x, pos_h = height (d.java "cz"; see
// d.java:1100-1106: cz = bd+1 + 65536, cy = bd+2), pos_z = z, facing =
// facing angle (0..3 * 65536, wrapped 196608 for 4 dirs; m() turns:
// cx=(cx+32768)&196608).
typedef struct mk_dungeon {
    mk_shell *shell;            // Java: game, KKM* (shell)
    // level/world statics live in engine.c (mk_cl/mk_ck global arrays)
    int  captured_key;          // Java: da, first captured key id (-1 none)
    int  keys_hud_on;           // Java: db, demo/keys-HUD visible
    int  moves, keys_total;     // Java: c9 (moves, u16), c8 (keys total)
    int  notes_left;            // Java: c7, notes remaining
    int  level_no;              // Java: c6, level number
    int  lives;                 // Java: c5, lives
    int  score;                 // Java: c4, score (u16)
    int  keys_held;             // Java: c3, keys held
    int  status;                // Java: c2, 0 play | 1 dead | 2 notes cleared
                                //          | 16 key | 24 key+note
    int  freelook;              // Java: aj, freelook: 0 off, 1 on, -1 exiting
    int  yaw, pitch;            // Java: c1, c0, freelook yaw, pitch
    int  pos_x, pos_h, pos_z;   // Java: c_, cz, cy (see header note)
    int  facing;                // Java: cx, facing angle
    int  turn_vel;              // Java: cw, yaw rate
    int  vel_h;                 // Java: cv, height-axis velocity
    int  step_fwd;              // Java: cu, forward step scalar (x/z sign set
                                //          by vel_x/vel_z per tick)
    int  vel_x, vel_z;          // Java: ct, cr, x/z axis velocities
    int  action_ticks;          // Java: cq, ticks remaining in current action
    int  action_code;           // Java: cp, action code (see d.java m())
    long idle_ticks;            // Java: co, idle ticks counter -> idle anim
    int  anim_frame;            // Java: bh, animation frame counter
    int  anim_id;               // Java: cn, current animation id
    int  facing_loaded;         // Java: cf, loaded facing
    int  par_moves;             // Java: b7, par moves
    int  level_notes, level_keys; // Java: b8, b9, decoded note/key counts
    int  riding;                // Java: b6, riding a moving block
    int  ride_id;               // Java: b5, render id of riding block
    int  support_idx;           // Java: b4, support/target cell index+1
                                //          (0/-1 = none)
    int  bit_pos, bit_byte;     // Java: b3, b2, level bit-reader state
    int  spawn_cur_x, spawn_cur_h, spawn_cur_z;   // Java: ce, cd, cc
    int  spawn_dat_x, spawn_dat_h, spawn_dat_z;   // Java: cj, ci, ch
    int  extra_life;            // Java: bx, extra life picked this level
    int  moving_n;              // Java: by, moving blocks count
    int  moving_rec[64 * 8];    // Java: bz, moving block records:
                                //   id|flag,vx,vy,vz,ticks,ox,oy,oz
} mk_dungeon;

extern mk_dungeon g_dungeon;

// Input (KKM.an[] booleans; indices: 0='0' 1='1' 2='2' ... 9='9' 10='#' 11='*')
extern unsigned char mk_an[MK_NKEYS];

// Sound hook implemented by the shell (KKM.i(int))
void game_sound(int id);
// Screen size helper (KKM.w: first level number for new game, default 1)
extern int mk_start_level;

// ---- lifecycle ----
void mk_load_tables(void);                          // load cm + MBF tables (once)
void mk_init_run(int startLevel, int fresh);   // d(KKM,b,bool): new game or resume
void mk_load_level(int level);                     // aw(): load level, reset monkey
void mk_tick100(void);                           // m(): advance one 100 ms tick
void mk_save_run(void);                          // save run state ("KKM" record)
void mk_delete_run(void);                        // game over: deleteRecordStore("KKM")
int  mk_resume_run(void);                          // resume run state; 1 = ok
int  mk_save_exists(void);
int  mk_level_index(int levelno);            // index into mk_level_ids, -1 = no such level

// KKM.aj mirror maintained by the shell (set_screen): the aw() checkpoint in
// mk_load_level autosaves only while the game screen (2) owns the run.
extern int mk_shell_state;

// world inspection (tests / debugging)
extern int8_t  mk_cl[2048];
extern int16_t mk_ck[2048];
int  mk_cell(int x, int y, int z);        // cl value
int  mk_count_notes(void);                // decode-time note count of current level

#endif // MK_ENGINE_H
