// engine.c - faithful C port of class d of Munkiki's Castles (2002).
// Clean-room reimplementation of the observed algorithm; symbols are renamed
// for readability and keep the decompiled/d.java name in the adjacent tag.
// Java byte arithmetic is reproduced by masking to 8 bits where the original
// truncates.
#include <string.h>
#include <stdio.h>
#include "engine.h"
#include "render.h"
#include "assets.h"

// --------------------------------------------------------------- world state
int8_t  mk_cl[2048];
int16_t mk_ck[2048];
mk_dungeon g_dungeon;
unsigned char mk_an[MK_NKEYS];
int     mk_start_level = 1;

// File-local helpers for this file only:
// floor a 16.16 position to a whole cell (was & ~65535)
#define MK_FIX_FLOOR (~((int)MK_ONE - 1))

// cl/ck cell index (d.java x + (z<<4) + (h<<8)): x step 1, z step
// MK_CELL_STEP_Z, height step MK_CELL_STEP_H in the MK_GRID_W x MK_GRID_H x
// MK_GRID_D grid.
static int cell_ix(int x, int h, int z)
{
    return x + z * MK_CELL_STEP_Z + h * MK_CELL_STEP_H;
}

// bounds-safe neighbor probe for stream-corrupted states: out-of-grid reads
// yield 0 (never "-128 free", never solid). Shipped data only ever reads
// in-grid cells, so behavior there is bit-identical; the original device
// would die with an ArrayIndexOutOfBounds on such states instead.
static int cl_at(int cell)
{
    return (unsigned)cell < sizeof(mk_cl) ? mk_cl[cell] : 0;
}

// Animation ids: raw values assigned to cn by d.m() (d.java 954-1087);
// index into the 17 cm[] frame tables. 11/unused by d (win dance is 2D).
enum { CN_NONE = 0, CN_WALK = 1, CN_TURNL = 2, CN_TURNR = 3, CN_UP = 4,
       CN_PUSH = 5, CN_SMASH = 6, CN_FALL = 7, CN_FALLF = 8, CN_FALLS = 9,
       CN_IDLE = 10, CN_EDGE = 12, CN_BACK = 13, CN_SLIDE = 14,
       CN_SPAWN = 15, CN_DEATH = 16 };


// statics from d.java
int8_t  mk_b1[13];
uint8_t mk_b0[64];
uint8_t mk_bw[1285];
int     mk_type_cell_count[13];              // short[] b_
uint8_t mk_cm[17][24];
int     mk_cm_len[17];
uint8_t mk_level_ids[MK_LEVELS];
static int block_count;                  // static int ca (block+item render count)

// KLP cleared-level storage (game.c implements file-backed)
int  mk_klp_cleared(int levelno);
void mk_klp_mark(int levelno);

// level bit-reader (class d fields b2/b3 + current stream)
static const uint8_t *stream_base;   // stream base
static int  stream_pos;             // byte position
static int  stream_len;

static int read_stream_byte(void) { return stream_pos < stream_len ? stream_base[stream_pos++] : 0; }

// as(): pre-reads b2; LSB-first bits
static int read_bits(int bits)
{
    int out = 0;
    for (int i = 0; i < bits; i++) {
        if (g_dungeon.bit_pos == 8) { g_dungeon.bit_byte = read_stream_byte(); g_dungeon.bit_pos = 0; }
        if (((uint8_t)g_dungeon.bit_byte) & (1 << g_dungeon.bit_pos)) out |= 1 << i;
        g_dungeon.bit_pos++;
    }
    return out & 0xFF;          // Java byte return (8-bit truncation)
}

// ar(): LSB-first bit writer (ap/save)
static uint8_t wb_byte; static int wb_bit, wb_len, wb_cap;
static void wb_reset(void) { wb_byte = 0; wb_bit = 0; wb_len = 0; wb_cap = 0x7FFFFFFF; }
static void write_bits(uint8_t *out, long val, int bits)
{
    for (int i = 0; i < bits; i++) {
        if (wb_bit == 8) {
            if (wb_len >= wb_cap) return;   // capacity reached (crafted world): truncate, never overrun
            out[wb_len++] = wb_byte; wb_byte = 0; wb_bit = 0;
        }
        wb_byte |= (uint8_t)((val & 1) << wb_bit);
        val >>= 1; wb_bit++;
    }
}
static void wb_flush(uint8_t *out) { if (wb_len < wb_cap) out[wb_len++] = wb_byte; wb_byte = 0; wb_bit = 0; }

// --------------------------------------------------------------- static init
void mk_load_tables(void)
{
    // cm tables: d[1945]: n(=17) then len+bytes per anim
    const uint8_t *p = MK_D + MK_D_CM;
    int n = *p++;
    if (n > 17) n = 17;
    for (int i = 0; i < n; i++) {
        int len = *p++;
        if (len > 24) len = 24;
        for (int j = 0; j < len; j++) mk_cm[i][j] = p[j];
        mk_cm_len[i] = len;
        p += len;
    }

    // MBF: b1[13], count, b0[], bw[]
    memcpy(mk_b1, MK_MBF, 13);
    int cnt = MK_MBF[13];
    if (cnt > 64) cnt = 64;
    // the id->index table is MK_LEVELS wide: a grown/bigger-count blob must
    // not overflow it (level counts beyond 36 stay visible via mk_b0 math)
    int nlev = cnt > (int)sizeof(mk_level_ids) ? (int)sizeof(mk_level_ids) : cnt;
    for (int i = 0; i < cnt; i++) mk_b0[i] = MK_MBF[14 + i];
    int off = 14 + cnt;
    // The embedded blob is shorter than mk_bw's capacity: copy what exists,
    // zero the tail (the unused capacity must not hold adjacent .rodata).
    int rem = (int)sizeof(MK_MBF) - off;
    if (rem > (int)sizeof(mk_bw)) rem = (int)sizeof(mk_bw);
    memcpy(mk_bw, MK_MBF + off, (size_t)rem);
    memset(mk_bw + rem, 0, sizeof(mk_bw) - (size_t)rem);

    // level id order (file order): first byte of each block
    int p2 = 0;
    for (int i = 0; i < nlev; i++) {
        mk_level_ids[i] = mk_bw[p2];
        p2 += mk_b0[i];
    }
}

int mk_level_index(int levelno)
{
    for (int i = 0; i < (int)(sizeof(mk_level_ids)); i++)
        if (mk_level_ids[i] == levelno) return i;
    return -1;
}

// ---------------------------------------------------------------- level load
// aq(level): find level block in bw by id, then an() decodes it
static int decode_level_by_id(int level)
{
    int block_off = 0;
    int i = 0;
    while (i < (int)(sizeof(mk_b0)) && mk_b0[i]) {
        if (mk_bw[block_off] == level) break;
        block_off += mk_b0[i];
        i++;
    }
    int nlev = MK_MBF[13];
    if (i >= nlev) return 0;
    stream_base = mk_bw + block_off + 1; stream_len = mk_b0[i] - 1; stream_pos = 0;

    // an()
    memset(mk_cl, 0, sizeof(mk_cl));
    memset(mk_ck, 0, sizeof(mk_ck));
    g_dungeon.moving_n = 0;
    block_count = 0;
    g_dungeon.level_keys = 0;
    g_dungeon.level_notes = 0;
    for (int j = 0; j < 13; j++) mk_type_cell_count[j] = 0;

    g_dungeon.bit_pos = 0;
    g_dungeon.bit_byte = read_stream_byte();               // pre-read first byte
    g_dungeon.facing_loaded = read_bits(2);
    g_dungeon.par_moves = read_bits(16);

    for (int t = 0; t < 13; t++) {
        if (read_bits(1)) {
            int cell_bits = mk_b1[t];  // byte
            if (480 & (1 << t)) {
                mk_type_cell_count[t] = 1;
                int cell_x = read_bits(4), cell_h = read_bits(3), cell_z = read_bits(4);
                if (!g_dungeon.extra_life || cell_bits != 32) {
                    int cell = cell_ix(cell_x, cell_h, cell_z);
                    mk_cl[cell] = (int8_t)(mk_cl[cell] | cell_bits);
                    if (cell_bits == MK_CELL_SPAWN_MARK) {
                        g_dungeon.spawn_cur_x = g_dungeon.spawn_dat_x = cell_x;
                        g_dungeon.spawn_cur_h = g_dungeon.spawn_dat_h = cell_h;
                        g_dungeon.spawn_cur_z = g_dungeon.spawn_dat_z = cell_z;
                    } else {
                        if (cell_bits & MK_CELL_SHAPE_MASK) block_count++;
                        if (cell_bits & MK_CELL_ITEM_MASK) g_dungeon.level_keys++;
                    }
                }
            } else {
                for (int y = 0; y < MK_GRID_H; y++) {
                    if (read_bits(1)) {
                        int x0 = read_bits(4);
                        int x1 = x0 + read_bits(4);
                        int z0 = read_bits(4);
                        int z1 = z0 + read_bits(4);
                        int y_base = y * MK_CELL_STEP_H;
                        int solid_rect = (x1 - x0 + (z1 - z0)) < 2;
                        /* harden against crafted streams: rect coords are
                           4-bit + 4-bit and may exceed the 16x16 grid */
                        if (x0 < 0) x0 = 0;
                        if (z0 < 0) z0 = 0;
                        if (x1 > MK_GRID_W - 1) x1 = MK_GRID_W - 1;
                        if (z1 > MK_GRID_D - 1) z1 = MK_GRID_D - 1;
                        for (int z = z0; z <= z1; z++) {
                            int row_base = y_base + z * MK_CELL_STEP_Z;
                            for (int x = x0; x <= x1; x++) {
                                if (solid_rect || read_bits(1) == 1) {
                                    mk_cl[row_base + x] = (int8_t)(mk_cl[row_base + x] | cell_bits);
                                    if (cell_bits & MK_CELL_SHAPE_MASK) { mk_type_cell_count[t]++; block_count++; }
                                    if (cell_bits & MK_CELL_ITEM_MASK) {
                                        mk_type_cell_count[t]++;
                                        g_dungeon.level_keys++;
                                        if (cell_bits == 8) g_dungeon.level_notes++;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    r_reset(block_count, g_dungeon.level_keys);
    int id = 0;
    for (int y = 0; y < MK_GRID_H; y++)
        for (int z = 0; z < MK_GRID_D; z++)
            for (int x = 0; x < MK_GRID_W; x++) {
                int cell_val = mk_cl[id];
                if ((cell_val & MK_CELL_ITEM_MASK) != MK_CELL_SPAWN_MARK) {
                    if (cell_val & MK_CELL_SHAPE_MASK)
                        mk_ck[id] = (int16_t)(1 + r_add_block(x << MK_FRACT_BITS, y << MK_FRACT_BITS, z << MK_FRACT_BITS, cell_val & MK_CELL_SHAPE_MASK));
                    else if (cell_val & MK_CELL_ITEM_MASK)
                        mk_ck[id] = (int16_t)(1 + r_add_billboard(x << MK_FRACT_BITS, y << MK_FRACT_BITS, z << MK_FRACT_BITS, cell_val & MK_CELL_ITEM_MASK) | MK_CK_BILLBOARD);

                    if ((cell_val & MK_CELL_SUPPORT_MASK) == MK_CELL_SUPPORT_MASK) {   // (cell_val & -64) == -64
                        if (g_dungeon.moving_n >= 64) continue;  // bz holds 64 records
                        int rec_off = g_dungeon.moving_n << 3;
                        g_dungeon.moving_rec[rec_off] = mk_ck[id];
                        g_dungeon.moving_rec[rec_off + 4] = 1;
                        g_dungeon.moving_rec[rec_off + 5] = x;
                        g_dungeon.moving_rec[rec_off + 6] = y;
                        g_dungeon.moving_rec[rec_off + 7] = z;
                        g_dungeon.moving_n++;
                    }
                }
                id++;
            }
    return 1;
}

// ------------------------------------------------------------------- ap() save
static int encode_run_state(uint8_t *out, int cap)
{
    // mirror of ap(): spawn marker uses ce/cd/cc position at save time
    g_dungeon.bit_pos = 0; g_dungeon.bit_byte = 0; wb_reset();
    // Java ap() streams into a growable ByteArrayOutputStream; the port's
    // fixed buffer must stop at its capacity (a crafted world can make the
    // encoder emit far more than any shipped level would).
    wb_cap = cap;
    write_bits(out, g_dungeon.facing_loaded, 2);
    write_bits(out, g_dungeon.par_moves, 16);

    for (int t = 0; t < 13; t++) {
        if (mk_type_cell_count[t] == 0) {
            write_bits(out, 0, 1);
        } else {
            write_bits(out, 1, 1);
            int cell_bits = mk_b1[t];
            if (480 & (1 << t)) {
                // single placement types: original ap() scans cl cells for the
                // marker; for spawn (40) the live position is ce/cd/cc, which is
                // exactly what was stored (see ax() setting cf before ap()).
                for (int i = 0; i < 2048; i++) {
                    if ((mk_cl[i] & cell_bits) == cell_bits) {
                        int x = i & 15, z = (i >> 4) & 15, y = (i >> 8) & 7;
                        write_bits(out, x, 4); write_bits(out, y, 3); write_bits(out, z, 4);
                    }
                }
            } else {
                for (int y = 0; y < MK_GRID_H; y++) {
                    int has = 0, minz = 10000, maxz = 0, minx = 10000, maxx = 0;
                    for (int z = 0; z < MK_GRID_D; z++)
                        for (int x = 0; x < MK_GRID_W; x++)
                            if ((mk_cl[cell_ix(x, y, z)] & cell_bits) == cell_bits) {
                                has = 1;
                                if (z < minz) minz = z; if (z > maxz) maxz = z;
                                if (x < minx) minx = x; if (x > maxx) maxx = x;
                            }
                    write_bits(out, has, 1);
                    if (has) {
                        write_bits(out, minx, 4); write_bits(out, maxx - minx, 4);
                        write_bits(out, minz, 4); write_bits(out, maxz - minz, 4);
                        if (maxx - minx + (maxz - minz) >= 2)
                            for (int yy = minz; yy <= maxz; yy++)
                                for (int xx = minx; xx <= maxx; xx++)
                                    write_bits(out,
                                        (mk_cl[cell_ix(xx, y, yy)] & cell_bits) == cell_bits, 1);
                    }
                }
            }
        }
    }
    wb_flush(out);
    return wb_len;
}

// ------------------------------------------------------------- KKM save/load
#define MK_KKM_FILE "KKM.rms"
// KKM.aj app-state mirror, maintained by the shell (set_screen). The aw()
// checkpoint in mk_load_level fires only while the game screen owns the run
// (aj==2); -1 = no shell (headless harness), 5 = instructions demo: neither
// autosaves.
int mk_shell_state = -1;

// KKM.java:490: game over runs deleteRecordStore("KKM") — a finished run
// must not stay resumable through "Continue".
void mk_delete_run(void) { remove(MK_KKM_FILE); }

void mk_save_run(void)
{
    uint8_t rec[2048];
    int n = 0;
    rec[n++] = g_dungeon.moves & 0xFF; rec[n++] = (g_dungeon.moves >> 8) & 0xFF;
    rec[n++] = g_dungeon.keys_total & 0xFF;
    rec[n++] = g_dungeon.notes_left & 0xFF;
    rec[n++] = g_dungeon.level_no & 0xFF;
    rec[n++] = g_dungeon.lives & 0xFF;
    rec[n++] = g_dungeon.score & 0xFF; rec[n++] = (g_dungeon.score >> 8) & 0xFF;
    rec[n++] = g_dungeon.keys_held & 0xFF;
    // cf must reflect current facing: original sets this.cf = cx>>16 before ap
    g_dungeon.facing_loaded = (g_dungeon.facing >> MK_FRACT_BITS);
    n += encode_run_state(rec + n, (int)sizeof(rec) - n);
    FILE *f = fopen(MK_KKM_FILE, "wb");
    if (f) { fwrite(rec, 1, n, f); fclose(f); }
}

int mk_save_exists(void)
{
    FILE *f = fopen(MK_KKM_FILE, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}


// shared decode body (used by aq and ay): p = stream after id byte
int mk_decode_level(const uint8_t *p, int len)
{
    stream_base = p; stream_len = len; stream_pos = 0;
    // same body as aq() decode section
    memset(mk_cl, 0, sizeof(mk_cl));
    memset(mk_ck, 0, sizeof(mk_ck));
    g_dungeon.moving_n = 0; block_count = 0; g_dungeon.level_keys = 0; g_dungeon.level_notes = 0;
    for (int j = 0; j < 13; j++) mk_type_cell_count[j] = 0;
    g_dungeon.bit_pos = 0; g_dungeon.bit_byte = read_stream_byte();
    g_dungeon.facing_loaded = read_bits(2);
    g_dungeon.par_moves = read_bits(16);
    for (int t = 0; t < 13; t++) {
        if (read_bits(1)) {
            int cell_bits = mk_b1[t];
            if (480 & (1 << t)) {
                mk_type_cell_count[t] = 1;
                int cell_x = read_bits(4), cell_h = read_bits(3), cell_z = read_bits(4);
                if (!g_dungeon.extra_life || cell_bits != 32) {
                    int cell = cell_ix(cell_x, cell_h, cell_z);
                    mk_cl[cell] = (int8_t)(mk_cl[cell] | cell_bits);
                    if (cell_bits == MK_CELL_SPAWN_MARK) {
                        g_dungeon.spawn_cur_x = g_dungeon.spawn_dat_x = cell_x; g_dungeon.spawn_cur_h = g_dungeon.spawn_dat_h = cell_h; g_dungeon.spawn_cur_z = g_dungeon.spawn_dat_z = cell_z;
                    } else {
                        if (cell_bits & MK_CELL_SHAPE_MASK) block_count++;
                        if (cell_bits & MK_CELL_ITEM_MASK) g_dungeon.level_keys++;
                    }
                }
            } else {
                for (int y = 0; y < MK_GRID_H; y++) {
                    if (read_bits(1)) {
                        int x0 = read_bits(4);
                        int x1 = x0 + read_bits(4);
                        int z0 = read_bits(4);
                        int z1 = z0 + read_bits(4);
                        int y_base = y * MK_CELL_STEP_H;
                        int solid_rect = (x1 - x0 + (z1 - z0)) < 2;
                        /* harden against crafted streams: rect coords are
                           4-bit + 4-bit and may exceed the 16x16 grid */
                        if (x0 < 0) x0 = 0;
                        if (z0 < 0) z0 = 0;
                        if (x1 > MK_GRID_W - 1) x1 = MK_GRID_W - 1;
                        if (z1 > MK_GRID_D - 1) z1 = MK_GRID_D - 1;
                        for (int z = z0; z <= z1; z++) {
                            int row_base = y_base + z * MK_CELL_STEP_Z;
                            for (int x = x0; x <= x1; x++) {
                                if (solid_rect || read_bits(1) == 1) {
                                    mk_cl[row_base + x] = (int8_t)(mk_cl[row_base + x] | cell_bits);
                                    if (cell_bits & MK_CELL_SHAPE_MASK) { mk_type_cell_count[t]++; block_count++; }
                                    if (cell_bits & MK_CELL_ITEM_MASK) { mk_type_cell_count[t]++; g_dungeon.level_keys++; if (cell_bits == 8) g_dungeon.level_notes++; }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    r_reset(block_count, g_dungeon.level_keys);
    int id = 0;
    for (int y = 0; y < MK_GRID_H; y++)
        for (int z = 0; z < MK_GRID_D; z++)
            for (int x = 0; x < MK_GRID_W; x++) {
                int cell_val = mk_cl[id];
                if ((cell_val & MK_CELL_ITEM_MASK) != MK_CELL_SPAWN_MARK) {
                    if (cell_val & MK_CELL_SHAPE_MASK)
                        mk_ck[id] = (int16_t)(1 + r_add_block(x << MK_FRACT_BITS, y << MK_FRACT_BITS, z << MK_FRACT_BITS, cell_val & MK_CELL_SHAPE_MASK));
                    else if (cell_val & MK_CELL_ITEM_MASK)
                        mk_ck[id] = (int16_t)(1 + r_add_billboard(x << MK_FRACT_BITS, y << MK_FRACT_BITS, z << MK_FRACT_BITS, cell_val & MK_CELL_ITEM_MASK) | MK_CK_BILLBOARD);
                    if ((cell_val & MK_CELL_SUPPORT_MASK) == MK_CELL_SUPPORT_MASK) {
                        if (g_dungeon.moving_n >= 64) continue;  // bz holds 64 records
                        int rec_off = g_dungeon.moving_n << 3;
                        g_dungeon.moving_rec[rec_off] = mk_ck[id];
                        g_dungeon.moving_rec[rec_off + 4] = 1;
                        g_dungeon.moving_rec[rec_off + 5] = x; g_dungeon.moving_rec[rec_off + 6] = y; g_dungeon.moving_rec[rec_off + 7] = z;
                        g_dungeon.moving_n++;
                    }
                }
                id++;
            }
    return 1;
}

int mk_resume_run(void)
{
    FILE *f = fopen(MK_KKM_FILE, "rb");
    if (!f) { mk_load_level(g_dungeon.level_no ? g_dungeon.level_no : mk_start_level); return 1; }
    uint8_t rec[2048];
    size_t n = fread(rec, 1, sizeof(rec), f);
    fclose(f);
    if (n < 9) { mk_load_level(mk_start_level); return 1; }
    g_dungeon.moves = rec[0] | (rec[1] << 8);
    g_dungeon.keys_total = rec[2];
    g_dungeon.notes_left = rec[3];
    g_dungeon.level_no = rec[4];
    g_dungeon.lives = rec[5];
    g_dungeon.score = rec[6] | (rec[7] << 8);
    g_dungeon.keys_held = rec[8];
    g_dungeon.extra_life = mk_klp_cleared(g_dungeon.level_no) != 0;
    mk_decode_level(rec + 9, (int)n - 9);
    g_dungeon.pos_x = g_dungeon.spawn_dat_x << MK_FRACT_BITS;
    g_dungeon.pos_h = g_dungeon.spawn_dat_h << MK_FRACT_BITS;
    g_dungeon.pos_z = g_dungeon.spawn_dat_z << MK_FRACT_BITS;
    g_dungeon.facing = g_dungeon.facing_loaded << MK_FRACT_BITS;
    return 1;
}

// ------------------------------------------------------------------ level init
// aw(level)
void mk_load_level(int level)
{
    g_dungeon.level_no = level;
    g_dungeon.extra_life = mk_klp_cleared(level) != 0;      // at()
    decode_level_by_id(level);                              // aq()
    g_dungeon.vel_z = g_dungeon.vel_x = g_dungeon.step_fwd = g_dungeon.vel_h = g_dungeon.turn_vel = 0;
    g_dungeon.action_ticks = 0;
    g_dungeon.pos_x = g_dungeon.spawn_dat_x << MK_FRACT_BITS;
    g_dungeon.pos_h = g_dungeon.spawn_dat_h << MK_FRACT_BITS;
    g_dungeon.pos_z = g_dungeon.spawn_dat_z << MK_FRACT_BITS;
    g_dungeon.facing = g_dungeon.facing_loaded << MK_FRACT_BITS;
    g_dungeon.notes_left = g_dungeon.level_notes;
    g_dungeon.moves = 0;
    g_dungeon.status = 0;
    // aw() end: if (a.aj != 5) ax();  — Java checkpoints the run after EVERY
    // level load (respawn, next level, new game, resume fallback), not only
    // on pause. mk_shell_state gates it exactly like KKM.aj did.
    if (mk_shell_state == 2) mk_save_run();
    g_dungeon.freelook = -1;
    g_scene.shake_ofs = -64;
    g_dungeon.anim_frame = 0;
    g_dungeon.action_code = 17;                              // spawn dance
    g_dungeon.anim_id = CN_SPAWN;
    g_dungeon.action_ticks = mk_cm_len[CN_SPAWN];
}

// d(KKM, canvas, fresh)
void mk_init_run(int startLevel, int fresh)
{
    memset(&g_dungeon, 0, sizeof(g_dungeon));
    g_dungeon.captured_key = -1;
    g_dungeon.action_code = 0; g_dungeon.anim_frame = 0; g_dungeon.anim_id = 0;
    g_dungeon.riding = 0; g_dungeon.extra_life = 0;
    if (fresh) {
        g_dungeon.lives = 3;
        g_dungeon.score = g_dungeon.keys_held = 0;
        mk_load_level(startLevel);
    } else {
        mk_resume_run();
        g_dungeon.freelook = -1; g_scene.shake_ofs = -64; g_dungeon.anim_frame = 0;
        g_dungeon.action_code = 17; g_dungeon.anim_id = CN_SPAWN; g_dungeon.action_ticks = mk_cm_len[CN_SPAWN];
    }
}

// ------------------------------------------------------------------ cell ops
static int cell_solid(int cell) { return (int8_t)(mk_cl[cell] & MK_CELL_SHAPE_MASK) != 0; }

// al(x,y,z): block below? (bouncy support check for renderer sinking)
static int cell_bouncy(int x, int y, int z)
{
    return (x >= 0 && x < MK_GRID_W && y >= 0 && y < MK_GRID_H && z >= 0 && z < MK_GRID_D)
        ? ((mk_cl[cell_ix(x, y, z)] & MK_CELL_SHAPE_MASK) == 2) : 0;
}

// am(): item pickup, returns item bits (8/16/24/32) or 0
static int pick_up_item(int x, int y, int z)
{
    /* harden against crafted saves: a monkey cell outside the 16x8x16 grid
       (Java threw AIOOBE in am()); same guard shape as cell_bouncy() */
    if (x < 0 || x >= MK_GRID_W || y < 0 || y >= MK_GRID_H || z < 0 || z >= MK_GRID_D)
        return 0;
    int cell = cell_ix(x, y, z);
    int item_bits = mk_cl[cell] & MK_CELL_ITEM_MASK;
    if (item_bits == MK_CELL_SPAWN_MARK) item_bits = 0;
    if (item_bits != 0) {
        if (mk_ck[cell] & MK_CK_BILLBOARD) {
            int bill_idx = (mk_ck[cell] & MK_CK_ID_MASK) - 1;
            r_clear_billboard(bill_idx);
        }
        mk_cl[cell] &= (int8_t)~MK_CELL_ITEM_MASK;
        mk_ck[cell] = 0;
    }
    return item_bits;
}

// ao(): move spawn marker to monkey cell
// Used before definition; file-scope prototypes (C4210 bans block-scope ones).
void mk_drop_unsupported_block(int cell, int x, int y, int z);
void mk_make_block_fall(int cell, int x, int y, int z, int flags);
static void move_spawn_marker(int x, int y, int z)
{
    if (g_dungeon.spawn_cur_x != x || g_dungeon.spawn_cur_z != z || g_dungeon.spawn_cur_h != y) {
        int old_cell = cell_ix(g_dungeon.spawn_cur_x, g_dungeon.spawn_cur_h, g_dungeon.spawn_cur_z);
        int new_cell = cell_ix(x, y, z);
        /* harden against crafted saves: an out-of-grid spawn monkey makes
           the old_cell + STEP_H probe (and the marker write) leave the
           2048-cell cl[] (the original would AIOOBE on the phone) */
        if (old_cell < 0 || old_cell >= (int)sizeof(mk_cl) ||
            new_cell < 0 || new_cell >= (int)sizeof(mk_cl)) return;
        mk_cl[old_cell] = (int8_t)(mk_cl[old_cell] & ~MK_CELL_ITEM_MASK);
        int above = old_cell + MK_CELL_STEP_H;
        if (above < (int)sizeof(mk_cl) && cell_solid(above)) {
            // block above old spawn falls/gets pushed: ai(cell, ...)
            mk_drop_unsupported_block(old_cell + MK_CELL_STEP_H, x, y, z);
        }
        g_dungeon.spawn_cur_x = x; g_dungeon.spawn_cur_h = y; g_dungeon.spawn_cur_z = z;
        mk_cl[cell_ix(g_dungeon.spawn_cur_x, g_dungeon.spawn_cur_h, g_dungeon.spawn_cur_z)] |= MK_CELL_SPAWN_MARK;
    }
}

// ai(cell,x,y,z): support removal propagation
void mk_drop_unsupported_block(int cell, int x, int y, int z)
{
    int block_bits = mk_cl[cell] & MK_CELL_SHAPE_MASK;
    if (block_bits != 5) {
        if (block_bits != 6) {
            if ((mk_cl[cell] & MK_CELL_SUPPORT_MASK) != MK_CELL_SUPPORT_MASK) {
                mk_make_block_fall(cell, x, y, z, 0);
            }
        }
    }
}

// ah(): make block fall (cell + its (x,y,z) origin)
void mk_make_block_fall(int cell, int x, int y, int z, int flags)
{
    if (g_dungeon.moving_n >= 64) return;      // bz holds 64 records
    int rec_off = g_dungeon.moving_n << 3;
    g_dungeon.moving_n++;
    g_dungeon.moving_rec[rec_off++] = mk_ck[cell] & MK_CK_ID_MASK | flags;
    g_dungeon.moving_rec[rec_off++] = 0;
    g_dungeon.moving_rec[rec_off++] = -(MK_ONE >> 2);              // vy = -0.25 (falling)
    g_dungeon.moving_rec[rec_off++] = 0;
    g_dungeon.moving_rec[rec_off++] = 4;                   // ticks
    g_dungeon.moving_rec[rec_off++] = x;
    g_dungeon.moving_rec[rec_off++] = y;
    g_dungeon.moving_rec[rec_off]   = z;
    mk_cl[cell - MK_CELL_STEP_H] = (int8_t)(mk_cl[cell - MK_CELL_STEP_H] & ~MK_CELL_LOW_MASK);
    mk_cl[cell - MK_CELL_STEP_H] = (int8_t)(mk_cl[cell - MK_CELL_STEP_H] | (mk_cl[cell] & MK_CELL_LOW_MASK));
    mk_ck[cell - MK_CELL_STEP_H] = (int16_t)(mk_ck[cell] | MK_CK_ANIM_MASK | flags);
    mk_ck[cell - MK_CELL_STEP_H] = (int16_t)(mk_ck[cell] | MK_CK_ANIM_MASK);
    mk_cl[cell] = (int8_t)(mk_cl[cell] & ~MK_CELL_LOW_MASK);
    mk_ck[cell] = 0;
}

// ag(): smash breakable in front; target cell (x,y,z)
static int smash_block(int x, int y, int z)
{
    int cell = cell_ix(x, y, z);
    if ((mk_cl[cell] & MK_CELL_SHAPE_MASK) != 3) return 0;
    if ((mk_ck[cell] & ~MK_CK_ID_MASK) == MK_CK_ANIM_MASK) return 0;   // animating
    int block_idx = (mk_ck[cell] & MK_CK_ID_MASK) - 1;
    mk_ck[cell] = 0;
    mk_cl[cell] = (int8_t)(mk_cl[cell] & ~MK_CELL_SHAPE_MASK);
    int item_bits = mk_cl[cell] & MK_CELL_ITEM_MASK;
    if (item_bits != 0) {
        mk_ck[cell] = (int16_t)(1 + r_add_billboard(x << MK_FRACT_BITS, y << MK_FRACT_BITS, z << MK_FRACT_BITS, item_bits) | MK_CK_BILLBOARD);
    } else {
        for (int above = y + 1; above < MK_GRID_H; above++) {
            cell += MK_CELL_STEP_H;
            if (cell_solid(cell)) {
                mk_drop_unsupported_block(cell, x, above - 1, z);
            } else if (mk_cl[cell] & MK_CELL_ITEM_MASK) {
                mk_make_block_fall(cell, x, above - 1, z, MK_CK_BILLBOARD);
            }
        }
    }
    r_clear_block(block_idx);
    g_scene.smash_parts = 0;                          // start smash particles (c.a7 = 0)
    return 1;
}

// aj(): push block at (x,y,z) toward (dx,dz)
static int push_block(int x, int y, int z, int dx, int dz)
{
    int cell = cell_ix(x, y, z);
    if (!cell_solid(cell)) return 0;
    if ((mk_cl[cell] & MK_CELL_SHAPE_MASK) == 6) return 0;
    if ((mk_cl[cell] & MK_CELL_SHAPE_MASK) == 5) return 0;
    if (mk_ck[cell] & MK_CK_ANIM_MASK) return 0;
    if (g_dungeon.moving_n >= 64) return 0;   // bz holds 64 records
    int dir_step = dx + MK_CELL_STEP_Z * dz;
    if (cell_solid(cell + dir_step)) return 0;
    if (mk_cl[cell + dir_step] & MK_CELL_ITEM_MASK) return 0;
    if (mk_cl[cell + MK_CELL_STEP_H] & MK_CELL_ITEM_MASK) return 0;
    if (cell_solid(cell + MK_CELL_STEP_H) && (mk_cl[cell + MK_CELL_STEP_H] & MK_CELL_SHAPE_MASK) != 5 && (mk_cl[cell + MK_CELL_STEP_H] & MK_CELL_SUPPORT_MASK) != MK_CELL_SUPPORT_MASK) return 0;

    mk_cl[cell + dir_step] = (int8_t)(mk_cl[cell + dir_step] | (mk_cl[cell] & MK_CELL_LOW_MASK));
    mk_ck[cell + dir_step] = (int16_t)(mk_ck[cell] | MK_CK_ANIM_MASK);
    int push_result = 0;
    int rec_off = g_dungeon.moving_n << 3;
    g_dungeon.moving_rec[rec_off++] = mk_ck[cell + dir_step] & MK_CK_ID_MASK;
    if ((mk_cl[cell] & MK_CELL_SHAPE_MASK) == 4) {
        g_dungeon.moving_rec[rec_off++] = dx << 13;
        g_dungeon.moving_rec[rec_off++] = 0;
        g_dungeon.moving_rec[rec_off++] = dz << 13;
        g_dungeon.moving_rec[rec_off++] = 8;
        push_result = 2;
    } else {
        g_dungeon.moving_rec[rec_off++] = dx << 12;
        g_dungeon.moving_rec[rec_off++] = 0;
        g_dungeon.moving_rec[rec_off++] = dz << 12;
        g_dungeon.moving_rec[rec_off++] = 16;
        push_result = 1;
    }
    g_dungeon.moving_rec[rec_off++] = x + dx;
    g_dungeon.moving_rec[rec_off++] = y;
    g_dungeon.moving_rec[rec_off]   = z + dz;
    g_dungeon.moving_n++;
    return push_result;
}

// ---------------------------------------------------------------- ac()
// Block/rubble physics tick. Each bz[] entry is a moving render object:
// [id|flags, vx, vy, vz, ticks, ox, oy, oz]. Faithful port of d.ac():
// no entry insertion ever happens - branches rewrite the current slot.
static void tick_moving_blocks(void)
{
    for (int i = 0; i < g_dungeon.moving_n; i++) {
        int rec_off = i << 3;
        int vx = g_dungeon.moving_rec[rec_off + 1];
        int vy = g_dungeon.moving_rec[rec_off + 2];
        int vz = g_dungeon.moving_rec[rec_off + 3];
        int render_idx = (g_dungeon.moving_rec[rec_off] & MK_CK_ID_MASK) - 1;
        if ((g_dungeon.moving_rec[rec_off] & MK_CK_BILLBOARD) == 0) {
            r_move_block(render_idx, vx, vy, vz);   // bd[id][0..2] += v
        } else {
            r_move_bill(render_idx, vx, vy, vz);    // ba[id][0..2] += v
        }
        g_dungeon.moving_rec[rec_off + 4]--;
        if (g_dungeon.moving_rec[rec_off + 4] != 0) continue;

        int ox = g_dungeon.moving_rec[rec_off + 5];
        int oy = g_dungeon.moving_rec[rec_off + 6];
        int oz = g_dungeon.moving_rec[rec_off + 7];
        if ((unsigned)ox >= MK_GRID_W || (unsigned)oy >= MK_GRID_H || (unsigned)oz >= MK_GRID_D) {
            /* crafted save state put a mover outside the grid: drop the
               record (the device would throw AIOOBE on the next cell read) */
            g_dungeon.moving_n--;
            memmove(g_dungeon.moving_rec + rec_off, g_dungeon.moving_rec + rec_off + 8,
                    (size_t)(g_dungeon.moving_n - i) * 8 * sizeof(int));
            i--;
            continue;
        }
        int cell = cell_ix(ox, oy, oz);
        mk_ck[cell] = (int16_t)(mk_ck[cell] & ~MK_CK_ANIM_MASK);  /* java: & -2049 == ~2048, not ~2049 */
        int clear_prev = 1;
        if (vx != 0) vx = vx > 0 ? 1 : -1;
        if (vy != 0) {
            if (vy > 0) vy = 1;
            else { if (vy == -(MK_ONE >> 2)) clear_prev = 0; vy = -1; }
        }
        if (vz != 0) vz = vz > 0 ? 1 : -1;
        int step_cell = vx + vz * MK_CELL_STEP_Z + vy * MK_CELL_STEP_H;
        if (clear_prev && step_cell != 0 && (unsigned)(cell - step_cell) < sizeof(mk_cl)) {
            mk_ck[cell - step_cell] = 0;
            mk_cl[cell - step_cell] = (int8_t)(mk_cl[cell - step_cell] & ~MK_CELL_LOW_MASK);
        }

        if (((uint8_t)mk_cl[cell] & MK_CELL_SUPPORT_MASK) == MK_CELL_SUPPORT_MASK) {
            // debris pile: choose the neighbour cell to flow into next
            int free_mask = 0;
            int from_mask = 0, axis_mask = 0, pick_mask = 0;
            if (cl_at(cell + 1) == -128) free_mask |= 1;
            if (cl_at(cell - 1) == -128) free_mask |= 2;
            if (cl_at(cell + MK_CELL_STEP_Z) == -128) free_mask |= 16;
            if (cl_at(cell - MK_CELL_STEP_Z) == -128) free_mask |= 32;
            if (cl_at(cell + MK_CELL_STEP_H) == -128 || cl_at(cell + MK_CELL_STEP_H) == -88) free_mask |= 4;
            if (oy > 0 && cl_at(cell - MK_CELL_STEP_H) == -128) free_mask |= 8;
            if (free_mask == 0) {
                g_dungeon.moving_rec[rec_off + 1] = 0; g_dungeon.moving_rec[rec_off + 2] = 0; g_dungeon.moving_rec[rec_off + 3] = 0;
                g_dungeon.moving_rec[rec_off + 4] = 1;
            } else {
                // current motion direction (the cell it came from), excluded
                if (vx == -1) from_mask = 1;
                if (vx == 1) from_mask = 2;
                if (vy == -1) from_mask = 4;
                if (vy == 1) from_mask = 8;
                if (vz == -1) from_mask = 16;
                if (vz == 1) from_mask = 32;
                axis_mask = 3;               // prefer current axis: z, then y, else x
                if (vy != 0) axis_mask = 12;
                if (vz != 0) axis_mask = 48;
                int dx = 0, dy = 0, dz = 0;
                free_mask &= ~from_mask;
                if (free_mask == 0) {
                    pick_mask = from_mask;         // only the origin cell is free: bounce
                } else {
                    pick_mask = free_mask & axis_mask;
                    if (pick_mask == 0) pick_mask = free_mask & ~axis_mask;
                }
                if (pick_mask & 1) dx = 1;
                else if (pick_mask & 2) dx = -1;
                else if (pick_mask & 16) dz = 1;
                else if (pick_mask & 32) dz = -1;
                else if (pick_mask & 4) dy = 1;
                else if (pick_mask & 8) dy = -1;
                step_cell = dx + dz * MK_CELL_STEP_Z + dy * MK_CELL_STEP_H;
                if (step_cell != 0 && (unsigned)(cell + step_cell) < sizeof(mk_cl)) {
                    mk_cl[cell + step_cell] = (int8_t)(mk_cl[cell + step_cell] | (mk_cl[cell] & MK_CELL_LOW_MASK));
                    mk_ck[cell + step_cell] = (int16_t)(mk_ck[cell] | MK_CK_ANIM_MASK);
                    mk_ck[cell] = (int16_t)(mk_ck[cell] | MK_CK_ANIM_MASK);
                }
                g_dungeon.moving_rec[rec_off + 1] = dx << 13;
                g_dungeon.moving_rec[rec_off + 2] = dy << 13;
                g_dungeon.moving_rec[rec_off + 3] = dz << 13;
                g_dungeon.moving_rec[rec_off + 4] = 8;
                g_dungeon.moving_rec[rec_off + 5] = ox + dx;
                g_dungeon.moving_rec[rec_off + 6] = oy + dy;
                g_dungeon.moving_rec[rec_off + 7] = oz + dz;
            }
        } else if (oy > 0 && (mk_cl[cell_ix(ox, oy - 1, oz)] & 63) == 0) {
            // support gone: drop one cell, reusing the same bz entry
            mk_cl[cell - MK_CELL_STEP_H] = (int8_t)(mk_cl[cell - MK_CELL_STEP_H] | (mk_cl[cell] & MK_CELL_LOW_MASK));
            mk_cl[cell] = (int8_t)(mk_cl[cell] & ~MK_CELL_LOW_MASK);
            mk_ck[cell - MK_CELL_STEP_H] = (int16_t)(g_dungeon.moving_rec[rec_off] | MK_CK_ANIM_MASK);
            mk_ck[cell] = 0;
            g_dungeon.moving_rec[rec_off + 1] = 0;
            g_dungeon.moving_rec[rec_off + 2] = -(MK_ONE >> 2);
            g_dungeon.moving_rec[rec_off + 3] = 0;
            g_dungeon.moving_rec[rec_off + 4] = 4;
            g_dungeon.moving_rec[rec_off + 6]--;            // origin height -= 1
        } else if ((mk_cl[cell] & MK_CELL_SHAPE_MASK) == 4 && (vz != 0 || vx != 0)
                   && (unsigned)(cell + step_cell) < sizeof(mk_cl) && !cell_solid(cell + step_cell)) {
            // sliding block keeps gliding
            mk_cl[cell + step_cell] = (int8_t)(mk_cl[cell + step_cell] | (mk_cl[cell] & MK_CELL_LOW_MASK));
            mk_ck[cell + step_cell] = (int16_t)(g_dungeon.moving_rec[rec_off] | MK_CK_ANIM_MASK);
            g_dungeon.moving_rec[rec_off + 4] = 8;
            g_dungeon.moving_rec[rec_off + 5] += vx;
            g_dungeon.moving_rec[rec_off + 7] += vz;
        } else {
            g_dungeon.moving_n--;
            memmove(g_dungeon.moving_rec + (i << 3), g_dungeon.moving_rec + ((i + 1) << 3),
                    (size_t)(g_dungeon.moving_n - i) * 8 * sizeof(int));
            i--;
        }
    }
}

// ---------------------------------------------------------------- support fn
// af(x,y,z): returns 0 supported, or negative fall code
static int check_support(int x, int y, int z)
{
    g_dungeon.riding = 0;
    if (y <= 0) return -111;
    int below_cell = cell_ix(x, y - 1, z);
    if (!cell_solid(below_cell)) {
        if (y <= 1) return -111;
        if (y == 2 && !cell_solid(below_cell - MK_CELL_STEP_H)) return -117;
        return (y > 2 && !cell_solid(below_cell - MK_CELL_STEP_H)) ? -115 : -116;
    }
    if ((mk_cl[below_cell] & MK_CELL_SUPPORT_MASK) == MK_CELL_SUPPORT_MASK) {
        g_dungeon.ride_id = mk_ck[below_cell] & MK_CK_ID_MASK;
        g_dungeon.riding = 1;
    }
    return 0;
}

// ae(): movement legality; returns action code + sets b4
static int compute_move_action(int x, int y, int z, int dx, int dz)
{
    int cell = cell_ix(x, y, z);
    int dir_step = dx + dz * MK_CELL_STEP_Z;
    int next_cell = cell + dir_step;
    int far_cell = cell + dir_step + dir_step;
    if (y > 0 && (mk_cl[cell - MK_CELL_STEP_H] & MK_CELL_SHAPE_MASK) == 2) {
        // standing on a bouncy block
        if (cell_solid(next_cell) && cell_solid(next_cell + MK_CELL_STEP_H) && !cell_solid(next_cell + 2 * MK_CELL_STEP_H)) { g_dungeon.support_idx = next_cell + MK_CELL_STEP_H; return 4; }
        if (cell_solid(far_cell) && !cell_solid(next_cell - MK_CELL_STEP_H) && !cell_solid(far_cell + MK_CELL_STEP_H)) { g_dungeon.support_idx = far_cell; return 5; }
    }
    if (cell_solid(next_cell)) {
        if (!cell_solid(next_cell + MK_CELL_STEP_H) && !cell_solid(cell + MK_CELL_STEP_H)) { g_dungeon.support_idx = next_cell; return 2; }
        g_dungeon.support_idx = cell - MK_CELL_STEP_H; return 0;
    } else if (y <= 0) {
        g_dungeon.support_idx = next_cell - MK_CELL_STEP_H; return 1;
    } else if (cell_solid(far_cell)) {
        g_dungeon.support_idx = next_cell - MK_CELL_STEP_H; return 1;
    } else {
        while (far_cell > 0 && !cell_solid(far_cell)) far_cell -= MK_CELL_STEP_H;
        while (next_cell > 0 && !cell_solid(next_cell)) next_cell -= MK_CELL_STEP_H;
        if (far_cell < 0 && next_cell < 0) { g_dungeon.support_idx = -1; return 1; }
        int far_h = far_cell >> 8;
        int near_h = next_cell >> 8;
        if (near_h >= far_h) {
            if (g_dungeon.riding && (mk_ck[next_cell] & MK_CK_ID_MASK) == (mk_ck[cell - MK_CELL_STEP_H] & MK_CK_ID_MASK)) {
                g_dungeon.support_idx = far_cell; return 3;
            }
            g_dungeon.support_idx = next_cell; return 1;
        } else if (y > 1 && far_h == y - 1 && near_h == y - 2) {
            g_dungeon.support_idx = next_cell; return 1;
        } else {
            g_dungeon.support_idx = far_cell; return 3;
        }
    }
}

// ad(): forward move; ad() wraps ae() with moving-block ride overrides
static int resolve_move_action(int x, int y, int z, int dx, int dz)
{
    int action = compute_move_action(x, y, z, dx, dz);
    int was_riding = g_dungeon.riding;
    g_dungeon.riding = 0;
    if (g_dungeon.support_idx > 0 && (mk_cl[g_dungeon.support_idx] & MK_CELL_SUPPORT_MASK) == MK_CELL_SUPPORT_MASK) {
        g_dungeon.riding = 1;
        g_dungeon.ride_id = mk_ck[g_dungeon.support_idx] & MK_CK_ID_MASK;
        action = 14;
    }
    if (was_riding && action != 14) {
        if (g_dungeon.support_idx > 0) g_dungeon.support_idx = mk_ck[g_dungeon.support_idx] & MK_CK_ID_MASK;
        action = 15;
    }
    return action;
}

// ------------------------------------------------------------------- m(): tick
void mk_tick100(void)
{
    if (g_dungeon.freelook == -1) {
        g_scene.shake_ofs += 8;
        if (g_scene.shake_ofs >= 0) { g_scene.shake_ofs = 0; g_dungeon.freelook = 0; }
    }

    tick_moving_blocks();

    if (g_dungeon.freelook == 1) {
        if (mk_an[MK_KEY_POUND]) {
            g_dungeon.freelook = 0;
            g_scene.shake_on = 1;
            g_scene.pitch_ofs = 0;
            r_shake();
            mk_an[MK_KEY_POUND] = 0;
        } else {
            int view_turned = 1;
            if (mk_an[MK_KEY_6] && g_dungeon.yaw < MK_LOOK_LIMIT) g_dungeon.yaw += MK_LOOK_STEP;
            else if (mk_an[MK_KEY_4] && g_dungeon.yaw > -MK_LOOK_LIMIT) g_dungeon.yaw -= MK_LOOK_STEP;
            if (mk_an[MK_KEY_8] && g_dungeon.pitch < MK_LOOK_LIMIT) g_dungeon.pitch += MK_LOOK_STEP;
            else if (mk_an[MK_KEY_2] && g_dungeon.pitch > -MK_LOOK_LIMIT) g_dungeon.pitch -= MK_LOOK_STEP;
            else view_turned = 0;
            g_scene.pitch_ofs = g_dungeon.pitch;
            if (view_turned) r_shake();
        }
        mk_an[MK_KEY_5] = mk_an[MK_KEY_0] = 0;
    } else {
        g_scene.pitch_ofs = 0;
    }

    if (g_dungeon.keys_hud_on && g_dungeon.captured_key == -1) {
        for (int i = 0; i < MK_NKEYS; i++)
            if (mk_an[i]) { g_dungeon.captured_key = i; }
    }

    int facing_dir = g_dungeon.facing >> MK_FRACT_BITS;
    int dx = (facing_dir & 1) == 1 ? 2 - facing_dir : 0;      // dx
    int dz = (facing_dir & 1) == 0 ? facing_dir - 1 : 0;      // dz
    int cell_x = (g_dungeon.pos_x + MK_HALF) >> MK_FRACT_BITS;   // x
    int cell_h = (g_dungeon.pos_h + MK_HALF) >> MK_FRACT_BITS;   // z
    int cell_z = (g_dungeon.pos_z + MK_HALF) >> MK_FRACT_BITS;   // y

    int any_key = 0;
    for (int i = 0; i < MK_NKEYS; i++) any_key |= mk_an[i];
    if (any_key && (g_dungeon.action_code & 64)) g_dungeon.action_ticks = 0;

    if (g_dungeon.action_ticks == 0 && g_dungeon.freelook == 0 && g_dungeon.status == 0) {
        int prev_action = g_dungeon.action_code;   // cp carried over from the previous tick
        g_dungeon.action_code = 0;
        g_dungeon.vel_z = g_dungeon.vel_x = g_dungeon.step_fwd = g_dungeon.vel_h = g_dungeon.turn_vel = 0;
        int item_bits = pick_up_item(cell_x, cell_h, cell_z);
        if (item_bits == 8) {
            g_dungeon.notes_left--;
            if (g_dungeon.notes_left == 0) { game_sound(8); g_dungeon.status = 2; }
            else game_sound(0);
        } else if (item_bits == 32) {
            g_dungeon.extra_life = 1; game_sound(9); g_dungeon.lives++;
        } else if (item_bits == 16) {
            g_dungeon.keys_held++; game_sound(1); g_dungeon.status = item_bits;
        } else if (item_bits == 24) {
            game_sound(2); g_dungeon.status = item_bits;
        }

        move_spawn_marker(cell_x, cell_h, cell_z);
        g_dungeon.action_code = check_support(cell_x, cell_h, cell_z);
        if (g_dungeon.action_code >= 0 && g_dungeon.status == 0) {
            if (mk_an[MK_KEY_POUND]) {
                g_dungeon.freelook = 1;
                g_scene.shake_on = 0;
                r_shake();
                g_dungeon.yaw = g_dungeon.pitch = 0;
                mk_an[MK_KEY_POUND] = 0;
            } else if (mk_an[MK_KEY_2]) {
                g_dungeon.action_code = resolve_move_action(cell_x, cell_h, cell_z, dx, dz);
            } else if (mk_an[MK_KEY_6]) {
                g_dungeon.action_code = 6;
            } else if (mk_an[MK_KEY_4]) {
                g_dungeon.action_code = 7;
            } else if (mk_an[MK_KEY_5]) {
                int push_result = push_block(cell_x + dx, cell_h, cell_z + dz, dx, dz);
                if (push_result == 1) g_dungeon.action_code = 8;
                if (push_result == 2) g_dungeon.action_code = 16;
            } else if (mk_an[MK_KEY_0]) {
                g_dungeon.action_code = 9;
            } else if (mk_an[MK_KEY_8] && !cell_solid(cell_ix(cell_x - dx, cell_h, cell_z - dz))) {
                g_dungeon.action_code = 10;
            }
        }

        if (g_dungeon.action_code == 0) {
            g_dungeon.anim_id = 0;
            g_dungeon.facing = (g_dungeon.facing + MK_HALF) & MK_FACING_WRAP;
            if (!g_dungeon.riding) {
                g_dungeon.pos_x = (g_dungeon.pos_x + MK_HALF) & MK_FIX_FLOOR;
                g_dungeon.pos_h = (g_dungeon.pos_h + MK_HALF) & MK_FIX_FLOOR;
                g_dungeon.pos_z = (g_dungeon.pos_z + MK_HALF) & MK_FIX_FLOOR;
            }
        } else if (g_dungeon.action_code == 1) {
            g_dungeon.moves++; g_dungeon.anim_id = CN_WALK; g_dungeon.step_fwd = MK_ONE;
        } else if (g_dungeon.action_code == 2) {
            game_sound(5); g_dungeon.moves++; g_dungeon.anim_id = CN_UP; g_dungeon.step_fwd = MK_ONE; g_dungeon.vel_h = MK_ONE;
        } else if (g_dungeon.action_code == 3) {
            game_sound(5); g_dungeon.step_fwd = 2 * MK_ONE; g_dungeon.moves++; g_dungeon.anim_id = CN_UP;
        } else if (g_dungeon.action_code == 14) {
            g_dungeon.moves++; g_dungeon.anim_id = CN_UP;
        } else if (g_dungeon.action_code == 15) {
            game_sound(5);
            if (g_dungeon.support_idx > 0) {
                g_dungeon.vel_x = r_block_x(g_dungeon.support_idx - 1) - g_dungeon.pos_x;
                g_dungeon.vel_z = r_block_z(g_dungeon.support_idx - 1) - g_dungeon.pos_z;
                g_dungeon.vel_h = MK_ONE + r_block_y(g_dungeon.support_idx - 1) - g_dungeon.pos_h;
            } else {
                g_dungeon.step_fwd = 2 * MK_ONE; g_dungeon.vel_h = -1;
            }
            if (g_dungeon.vel_h < 0) {
                // d.java:1003-1007 reads the BLOCK array (bd) for the step-off
                // probe; ride_id (b5) always names a block render id.
                if (MK_ONE + r_block_y(g_dungeon.ride_id - 1) - g_dungeon.pos_h >= 0) g_dungeon.vel_h = MK_ONE;
                else g_dungeon.vel_h = 0;
            }
            g_dungeon.moves++; g_dungeon.anim_id = CN_UP;
        } else if (g_dungeon.action_code == 4) {
            game_sound(5); g_dungeon.moves++; g_dungeon.anim_id = CN_UP; g_dungeon.step_fwd = MK_ONE; g_dungeon.vel_h = 2 * MK_ONE;
        } else if (g_dungeon.action_code == 5) {
            game_sound(5); g_dungeon.moves++; g_dungeon.anim_id = CN_UP; g_dungeon.step_fwd = 2 * MK_ONE; g_dungeon.vel_h = MK_ONE;
        } else if (g_dungeon.action_code == 6) {
            g_dungeon.anim_id = CN_TURNR; g_dungeon.turn_vel = MK_ONE;
        } else if (g_dungeon.action_code == 7) {
            g_dungeon.anim_id = CN_TURNL; g_dungeon.turn_vel = -MK_ONE;
        } else if (g_dungeon.action_code == 8) {
            g_dungeon.moves++; g_dungeon.anim_id = CN_PUSH; g_dungeon.step_fwd = MK_ONE; game_sound(4);
        } else if (g_dungeon.action_code == 16) {
            g_dungeon.moves++; g_dungeon.anim_id = CN_SLIDE; g_dungeon.step_fwd = MK_ONE; game_sound(4);
        } else if (g_dungeon.action_code == 9) {
            g_dungeon.moves++; g_dungeon.anim_id = CN_SMASH;
        } else if (g_dungeon.action_code == 10) {
            g_dungeon.moves++; g_dungeon.anim_id = CN_BACK; g_dungeon.step_fwd = -MK_ONE;
        } else if (g_dungeon.action_code == -117) {
            g_dungeon.anim_id = CN_EDGE; g_dungeon.vel_h = -98304;
        } else if (g_dungeon.action_code == -111) {
            if (prev_action == -117) { g_dungeon.vel_h = -MK_ONE; g_dungeon.anim_id = CN_DEATH; }
            else { g_dungeon.vel_h = -98304; g_dungeon.anim_id = CN_DEATH; }
            game_sound(11);
        } else if (g_dungeon.action_code == -115) {
            if (prev_action != -115) game_sound(7);
            g_dungeon.anim_id = CN_FALLF; g_dungeon.vel_h = -MK_ONE;
        } else if (g_dungeon.action_code == -116) {
            g_dungeon.anim_id = (prev_action == -115) ? CN_FALLS : CN_FALL; g_dungeon.vel_h = -MK_ONE;
        }

        if (g_dungeon.action_code != 0) {
            g_dungeon.idle_ticks = 0;
            g_dungeon.action_ticks = mk_cm_len[g_dungeon.anim_id];
            if (g_dungeon.step_fwd != 0) { g_dungeon.vel_x = dx * g_dungeon.step_fwd; g_dungeon.vel_z = dz * g_dungeon.step_fwd; }
            g_dungeon.vel_h /= g_dungeon.action_ticks;
            g_dungeon.turn_vel /= g_dungeon.action_ticks;
            g_dungeon.vel_x /= g_dungeon.action_ticks;
            g_dungeon.vel_z /= g_dungeon.action_ticks;
        } else if (g_dungeon.idle_ticks++ >= 40) {
            g_dungeon.idle_ticks = 0;
            g_dungeon.anim_id = CN_IDLE;
            g_dungeon.action_ticks = mk_cm_len[CN_IDLE];
            g_dungeon.action_code = 82;
        }
        if (prev_action != g_dungeon.action_code) g_dungeon.anim_frame = 0;   // new action: restart animation
    }

    if (g_dungeon.riding && (g_dungeon.action_code == 6 || g_dungeon.action_code == 7 || g_dungeon.action_code == 0 || g_dungeon.action_code == 82 || g_dungeon.action_code == 9)) {
        g_dungeon.pos_x = r_block_x(g_dungeon.ride_id - 1);
        g_dungeon.pos_h = MK_ONE + r_block_y(g_dungeon.ride_id - 1);
        g_dungeon.pos_z = r_block_z(g_dungeon.ride_id - 1);
    }

    if (g_dungeon.action_ticks > 0) {
        if (g_dungeon.action_code == 14) {
            g_dungeon.vel_x = r_block_x(g_dungeon.ride_id - 1) - g_dungeon.pos_x;
            g_dungeon.vel_h = MK_ONE + r_block_y(g_dungeon.ride_id - 1) - g_dungeon.pos_h;
            g_dungeon.vel_z = r_block_z(g_dungeon.ride_id - 1) - g_dungeon.pos_z;
            g_dungeon.vel_x /= g_dungeon.action_ticks; g_dungeon.vel_h /= g_dungeon.action_ticks; g_dungeon.vel_z /= g_dungeon.action_ticks;
        }

        if (g_dungeon.action_code == 9 && g_dungeon.action_ticks == 2 && smash_block(cell_x + dx, cell_h, cell_z + dz))
            game_sound(3);

        if (g_dungeon.action_code == -111) g_scene.death_parts++;   // b.a6++ : death particles start

        g_dungeon.pos_x += g_dungeon.vel_x;
        g_dungeon.pos_h += g_dungeon.vel_h;
        g_dungeon.pos_z += g_dungeon.vel_z;
        if (g_dungeon.pos_h < MK_ONE) g_scene.shake_ofs += (-g_dungeon.vel_h) >> 11;
        g_dungeon.facing = (g_dungeon.facing + g_dungeon.turn_vel + 262144) & 262143;
        g_dungeon.action_ticks--;
        if (g_dungeon.action_ticks == 0) {
            g_dungeon.facing = (g_dungeon.facing + MK_HALF) & MK_FACING_WRAP;
            if (g_dungeon.action_code == -111) { g_dungeon.status = 1; g_dungeon.anim_id = 0; }
        }
    }

    if (g_dungeon.anim_frame >= mk_cm_len[g_dungeon.anim_id]) g_dungeon.anim_frame = 0;
    g_scene.monkey_sprite = (uint8_t)mk_cm[g_dungeon.anim_id][g_dungeon.anim_frame++];
    g_scene.sunk = cell_bouncy(cell_x, cell_h - 1, cell_z);

    // camera transform chain (m() tail; performed on next frame's view chain)
    r_set_view_chain();
}

// ------------------------------------------------------------- inspection
int mk_cell(int x, int y, int z) { return mk_cl[cell_ix(x, y, z)]; }
int mk_count_notes(void) { return g_dungeon.level_notes; }
