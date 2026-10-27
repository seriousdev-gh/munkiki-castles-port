// game.c - game shell of "Munkiki's Castles" (2002, Nokia 3410): faithful C
// port of decompiled/KKM.java (MIDlet state machine, HUD overlays, records)
// + decompiled/b.java (FullCanvas input/paint/text helpers) + b.e()/d()/paint.
//
// Field-name mapping (Java -> C, kept recognizable):
//   KKM.aj -> g_screen_id (screen: 0 splash, 2 game, 3 hiscores, 4 form, 5 demo)
//   KKM.ab -> g_sub_state (in-game sub-state 0..7)  KKM.ai -> g_ui_id (menu id 24..28)
//   KKM.ad -> g_screen_t (frame counter)           KKM.ag -> g_intro_page (intro page)
//   KKM.ae -> g_screen_dirty (-> white clear)      KKM.af -> g_save_exists (save exists)
//   KKM.ac -> g_demo_scripted (demo scripted)      KKM.v/aa -> g_destroyed/g_paused
//   KKM.o/p/q/r (bonuses) -> g_bonus_key,g_bonus_last_lvl,g_bonus_keynote,g_bonus_par
//   KKM.m/n (hiscores) -> g_hiscore_second,g_hiscore_best   KKM.w -> mk_start_level (engine.h)
//   KKM.k/j (sound/lights on) -> g_sound_on,g_lights_on     KKM.an -> mk_an (engine.h)
//   KKM.ao -> g_strings (strings)    KKM.ah -> g_cards (images)   KKM.ak (d) -> engine g_dungeon + g_ak_active
//   KKM.l[] sounds -> platform_sound() via game_sound/snd()
// Methods: KKM.m -> tick_body, p -> set_screen, q -> set_substate, r -> nav, s -> ui_clear,
//   i -> snd, j -> paint_canvas_overlay, k -> odometer, l -> score_step, n -> die, o -> anykey,
//   b.a -> str_subst, b.b -> build_text, b.c -> key_route, b.d -> mark_visible_region,
//   b.e -> rel_clear, b.paint -> game_paint, d.j -> dungeon_hud, d.av -> paint_note_odometer.
#include "game.h"
#include "engine.h"
#include "render.h"
#include "assets.h"
#include "mk_env.h"                // debug hooks: value semantics (MK_X=0 is off)


#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

// KLP cleared-level store (d.at()/d.au()/ctor delete; file-backed here, engine
// extern-declares mk_klp_cleared/mk_klp_mark; see report):
int  mk_klp_cleared(int levelno);
void mk_klp_mark(int levelno);
void mk_klp_wipe(void);

#define SCR_W 96
#define SCR_H 65

// ---------------------------------------------------------------------------
// State-machine ids (the bytes the decompiled KKM passes as p/q/r arguments;
// the values stay verbatim, the names decode switch/case arms and call sites).
// ---------------------------------------------------------------------------
// KKM.p(byte) -> g_screen_id: who owns the canvas
#define SCREEN_NONE        (-1)   // a menu Displayable is shown instead
#define SCREEN_SPLASH      0      // title splash card
#define SCREEN_GAME        2      // dungeon play (carries g_sub_state below)
#define SCREEN_HISCORE     3      // high-score board (Form)
#define SCREEN_INTRO_FORM  4      // instructions page (Form)
#define SCREEN_DEMO        5      // scripted intro demo play
// KKM.q(byte) -> g_sub_state: in-game screens (while screen == SCREEN_GAME)
#define SUB_HINT           0      // "(" + hint string[15] + ")" card
#define SUB_LEVEL_CARD     1      // "Level N / Collect X notes" card
#define SUB_PLAY           2      // dungeon gameplay
#define SUB_LEVEL_CLEAR    3      // level-complete curtain/flash (6: unused)
#define SUB_BONUS          4      // bonus score odometer board
#define SUB_WORLD_MAP      5      // world map card, monkey marker slides
#define SUB_GAME_OVER      7      // GAME OVER plate + percent
// KKM.r(byte) -> g_ui_id / g_menu.type: canvas menu Displayables
#define MENU_PAUSE         24     // in-game pause list
#define MENU_MAIN          25     // main menu (Continue/New Game/...)
#define MENU_INTRO         26     // instructions Form (paged)
#define MENU_OPTIONS       27     // options (sound/lights)
#define MENU_LEVEL_PICK    28     // New Game level-restart dialog
// b.b(String,int): font slot picked by build_text
#define FONT_SMALL         0      // body text
#define FONT_TITLE         1      // card title ("Level N")
#define FONT_LARGE         2      // big (GAME OVER percent)
// Regular levels end at 25; each held key appends one castle level after it.
#define LVL_CHAPTER_END    25
#define LVL_MAX            30

// KKM.m() 100 ms timer thresholds on g_screen_t (KKM.ad); the comment on each
// line names the Java switch arm (p()/q() argument) the test sits in.
#define SPLASH_AUTOEXIT   25      // p(0): splash card -> main menu after 25 ticks
#define HISCORE_AUTOEXIT  64      // p(3): hiscore board -> main menu on tick 64
#define DEMO_PAGE_EXIT    50      // p(5): scripted demo page ends after 50 ticks
#define WORLD_MAP_EXIT    50      // q(5): world-map card -> next level after 50 ticks
#define GAME_OVER_EXIT    50      // q(7): GAME OVER board -> hiscores after 50 ticks
#define DEMO_KEY_TICK     15      // p(5): scripted demo key injected on tick 15
#define CARD_SCROLL_STEP   8      // q(0)/q(1): '2'/'8' scroll the text card by 8 px

// menu_paint() row geometry (port-chosen chrome: the Java List/Form was drawn
// by the device, so these have no d.java counterpart)
#define MENU_ROW_Y0        12     // baseline y of the first menu row
#define MENU_ROW_H          9     // pitch of list rows / wrapped intro lines
#define MENU_OPT_ROW_H     10     // options rows sit one pixel further apart

// ---------------------------------------------------------------------------
// Embedded bitmap font: fixed 5x7 (advance 6, height 8), row-major, bit7=left
// (drawn as 0x80>>col), cols 7..3.
// The original JAR ships NO font: b.java:180-182 picks handset system fonts
// via Font.getFont(PROPORTIONAL, PLAIN, 8/0/16). So this table is our stand-in:
// ASCII 0x20..0x7E and the 14 Latin-1 marks used by MK_STR come from the
// public-domain 5x7 font in Adafruit GFX (glcdfont.c), transposed from
// column-major; only g breve / dotless i / s cedilla (tr strings) are ours.
// All original strings fit 96 px with the fixed 6 px advance.
// ---------------------------------------------------------------------------
static const unsigned char g_font[112][7] = {
{0x00,0x00,0x00,0x00,0x00,0x00,0x00},
{0x20,0x20,0x20,0x20,0x20,0x00,0x20},
{0x50,0x50,0x50,0x00,0x00,0x00,0x00},
{0x50,0x50,0xF8,0x50,0xF8,0x50,0x50},
{0x20,0x78,0xA0,0x70,0x28,0xF0,0x20},
{0xC0,0xC8,0x10,0x20,0x40,0x98,0x18},
{0x40,0xA0,0xA0,0x40,0xA8,0x90,0x68},
{0x30,0x30,0x20,0x40,0x00,0x00,0x00},
{0x10,0x20,0x40,0x40,0x40,0x20,0x10},
{0x40,0x20,0x10,0x10,0x10,0x20,0x40},
{0x20,0xA8,0x70,0xF8,0x70,0xA8,0x20},
{0x00,0x20,0x20,0xF8,0x20,0x20,0x00},
{0x00,0x00,0x00,0x00,0x30,0x30,0x20},
{0x00,0x00,0x00,0xF8,0x00,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x30,0x30},
{0x00,0x08,0x10,0x20,0x40,0x80,0x00},
{0x70,0x88,0x98,0xA8,0xC8,0x88,0x70},
{0x20,0x60,0x20,0x20,0x20,0x20,0x70},
{0x70,0x88,0x08,0x70,0x80,0x80,0xF8},
{0xF8,0x08,0x10,0x30,0x08,0x88,0x70},
{0x10,0x30,0x50,0x90,0xF8,0x10,0x10},
{0xF8,0x80,0xF0,0x08,0x08,0x88,0x70},
{0x38,0x40,0x80,0xF0,0x88,0x88,0x70},
{0xF8,0x08,0x08,0x10,0x20,0x40,0x80},
{0x70,0x88,0x88,0x70,0x88,0x88,0x70},
{0x70,0x88,0x88,0x78,0x08,0x10,0xE0},
{0x00,0x00,0x20,0x00,0x20,0x00,0x00},
{0x00,0x00,0x20,0x00,0x20,0x20,0x40},
{0x08,0x10,0x20,0x40,0x20,0x10,0x08},
{0x00,0x00,0xF8,0x00,0xF8,0x00,0x00},
{0x40,0x20,0x10,0x08,0x10,0x20,0x40},
{0x70,0x88,0x08,0x30,0x20,0x00,0x20},
{0x70,0x88,0xA8,0xB8,0xB0,0x80,0x78},
{0x20,0x50,0x88,0x88,0xF8,0x88,0x88},
{0xF0,0x88,0x88,0xF0,0x88,0x88,0xF0},
{0x70,0x88,0x80,0x80,0x80,0x88,0x70},
{0xF0,0x88,0x88,0x88,0x88,0x88,0xF0},
{0xF8,0x80,0x80,0xF0,0x80,0x80,0xF8},
{0xF8,0x80,0x80,0xF0,0x80,0x80,0x80},
{0x78,0x88,0x80,0x80,0x98,0x88,0x78},
{0x88,0x88,0x88,0xF8,0x88,0x88,0x88},
{0x70,0x20,0x20,0x20,0x20,0x20,0x70},
{0x38,0x10,0x10,0x10,0x10,0x90,0x60},
{0x88,0x90,0xA0,0xC0,0xA0,0x90,0x88},
{0x80,0x80,0x80,0x80,0x80,0x80,0xF8},
{0x88,0xD8,0xA8,0xA8,0xA8,0x88,0x88},
{0x88,0x88,0xC8,0xA8,0x98,0x88,0x88},
{0x70,0x88,0x88,0x88,0x88,0x88,0x70},
{0xF0,0x88,0x88,0xF0,0x80,0x80,0x80},
{0x70,0x88,0x88,0x88,0xA8,0x90,0x68},
{0xF0,0x88,0x88,0xF0,0xA0,0x90,0x88},
{0x70,0x88,0x80,0x70,0x08,0x88,0x70},
{0xF8,0xA8,0x20,0x20,0x20,0x20,0x20},
{0x88,0x88,0x88,0x88,0x88,0x88,0x70},
{0x88,0x88,0x88,0x88,0x88,0x50,0x20},
{0x88,0x88,0x88,0xA8,0xA8,0xA8,0x50},
{0x88,0x88,0x50,0x20,0x50,0x88,0x88},
{0x88,0x88,0x50,0x20,0x20,0x20,0x20},
{0xF8,0x08,0x10,0x70,0x40,0x80,0xF8},
{0x78,0x40,0x40,0x40,0x40,0x40,0x78},
{0x00,0x80,0x40,0x20,0x10,0x08,0x00},
{0x78,0x08,0x08,0x08,0x08,0x08,0x78},
{0x20,0x50,0x88,0x00,0x00,0x00,0x00},
{0x00,0x00,0x00,0x00,0x00,0x00,0xF8},
{0x60,0x60,0x20,0x10,0x00,0x00,0x00},
{0x00,0x00,0x60,0x10,0x70,0x90,0x78},
{0x80,0x80,0xB0,0xC8,0x88,0xC8,0xB0},
{0x00,0x00,0x70,0x88,0x80,0x88,0x70},
{0x08,0x08,0x68,0x98,0x88,0x98,0x68},
{0x00,0x00,0x70,0x88,0xF8,0x80,0x70},
{0x10,0x28,0x20,0x70,0x20,0x20,0x20},
{0x00,0x00,0x70,0x98,0x98,0x68,0x08},
{0x80,0x80,0xB0,0xC8,0x88,0x88,0x88},
{0x20,0x00,0x60,0x20,0x20,0x20,0x70},
{0x10,0x00,0x10,0x10,0x10,0x90,0x60},
{0x80,0x80,0x90,0xA0,0xC0,0xA0,0x90},
{0x60,0x20,0x20,0x20,0x20,0x20,0x70},
{0x00,0x00,0xD0,0xA8,0xA8,0xA8,0xA8},
{0x00,0x00,0xB0,0xC8,0x88,0x88,0x88},
{0x00,0x00,0x70,0x88,0x88,0x88,0x70},
{0x00,0x00,0xB0,0xC8,0xC8,0xB0,0x80},
{0x00,0x00,0x68,0x98,0x98,0x68,0x08},
{0x00,0x00,0xB0,0xC8,0x80,0x80,0x80},
{0x00,0x00,0x78,0x80,0x70,0x08,0xF0},
{0x20,0x20,0xF8,0x20,0x20,0x28,0x10},
{0x00,0x00,0x88,0x88,0x88,0x98,0x68},
{0x00,0x00,0x88,0x88,0x88,0x50,0x20},
{0x00,0x00,0x88,0x88,0xA8,0xA8,0x50},
{0x00,0x00,0x88,0x50,0x20,0x50,0x88},
{0x00,0x00,0x88,0x88,0x78,0x08,0xF0},
{0x00,0x00,0xF8,0x10,0x20,0x40,0xF8},
{0x10,0x20,0x20,0x40,0x20,0x20,0x10},
{0x20,0x20,0x20,0x00,0x20,0x20,0x20},
{0x40,0x20,0x20,0x10,0x20,0x20,0x40},
{0x40,0xA8,0x10,0x00,0x00,0x00,0x00},
{0x00,0x00,0x00,0x00,0xF8,0xF8,0xF8},
{0xF8,0xF8,0xF8,0xF8,0x00,0x00,0x00},
{0x00,0x00,0x68,0x90,0x90,0x90,0x68},
{0x00,0xF8,0x98,0x80,0x80,0x80,0x80},
{0xF8,0x88,0x40,0x20,0x40,0x88,0xF8},
{0x00,0xF8,0xA0,0x20,0x20,0x20,0x20},
{0xF8,0x20,0x70,0x88,0x88,0x70,0x20},
{0x20,0x50,0x88,0xF8,0x88,0x50,0x20},
{0x20,0x50,0x88,0x88,0x50,0x50,0xD8},
{0x00,0x00,0x00,0x70,0xA8,0xA8,0x70},
{0x40,0x20,0x10,0x20,0x40,0x00,0xF8},
{0x30,0x30,0x00,0xF8,0x00,0x30,0x30},
{0x00,0x00,0x00,0x30,0x30,0x00,0x00},
{0x70,0x48,0x48,0x48,0x48,0x00,0x00},
{0x50,0x00,0x70,0x80,0xF0,0x88,0x78},
{0x00,0x00,0x60,0x20,0x20,0x20,0x70},
{0x00,0x00,0x78,0x80,0x70,0x08,0xF0}
};
static const unsigned short g_font_ext[17] = {
    0x00DC,0x00DF,0x00E0,0x00E2,0x00E4,0x00E7,0x00E8,0x00E9,
    0x00EA,0x00EC,0x00F2,0x00F6,0x00F9,0x00FC,0x011F,0x0131,0x015F
};

// Proportional advance (original device font is FACE_PROPORTIONAL):
// rightmost ink column + 2 px (>=1 px gap before next glyph), clamped 4..6; space = 3.
static const unsigned char g_adv[112] = {
3,4,5,6,6,6,6,5,
5,5,6,6,5,6,5,6,
6,5,6,6,6,6,6,6,
6,6,4,4,6,6,6,6,
6,6,6,6,6,6,6,6,
6,5,6,6,6,6,6,6,
6,6,6,6,6,6,6,6,
6,6,6,6,6,6,6,6,
5,6,6,6,6,6,6,6,
6,5,5,5,5,6,6,6,
6,6,6,6,6,6,6,6,
6,6,6,5,4,5,6,6,
6,6,6,6,6,6,6,6,
6,6,6,5,6,6,5,6
};
#define FONT_ADV 6
#define FONT_H   8

static int glyph_index(int cp)
{
    int i;
    if (cp >= 0x20 && cp <= 0x7E) return cp - 0x20;
    for (i = 0; i < 17; i++)
        if (g_font_ext[i] == cp) return 95 + i;
    return '?' - 0x20;                       // unknown codepoint -> '?'
}

static const unsigned char *glyph_for(int cp)
{
    return g_font[glyph_index(cp)];
}

static const char *utf8_next(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    int cp;
    if (p[0] < 0x80)       { cp = p[0]; *s += 1; }
    else if ((p[0] & 0xE0) == 0xC0) { cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F); *s += 2; }
    else if ((p[0] & 0xF0) == 0xE0) { cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); *s += 3; }
    else                   { cp = '?'; *s += 1; }
    return (const char *)(size_t)cp;
}

// b.c[]: charWidth / stringWidth (fixed 6 px advance; 'W' used by b() sizing math)
static int str_width(const char *s)
{
    int w = 0;
    while (*s)
        w += g_adv[glyph_index((int)(size_t)utf8_next(&s))];
    return w;
}

// ---------------------------------------------------------------------------
// Graphics: a Graphics target = pixel buffer (gray 0..255) + optional alpha
// (0 transparent, 255 opaque) + replacement clip + current color.
// ---------------------------------------------------------------------------
#define IMG_MAXH 224

typedef struct {
    int w, h;
    int has_alpha;
    unsigned char *px;                    // gray plane, row stride SCR_W
    unsigned char *al;                    // alpha plane (NULL = opaque)
    unsigned char buf[SCR_W * IMG_MAXH * 2];
} surface;

static void surf_own(surface *s)          // use embedded storage
{
    s->px = s->buf;
    s->al = s->buf + SCR_W * IMG_MAXH;
}

static surface g_fb_surf;                 // wraps the platform fb (opaque)
static surface *tgt;                      // current draw target
static int cl_x0, cl_y0, cl_x1, cl_y1;    // inclusive clip bounds
static int gcol;                          // current gray color

static void tgt_set(surface *s)
{
    tgt = s;
    cl_x0 = 0; cl_y0 = 0; cl_x1 = s->w - 1; cl_y1 = s->h - 1;
}

// Graphics.setClip: replaces the clip (lcdui semantics), intersected w/ surface.
static void g_setclip(int x, int y, int w, int h)
{
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w - 1, y1 = y + h - 1;
    if (x1 > tgt->w - 1) x1 = tgt->w - 1;
    if (y1 > tgt->h - 1) y1 = tgt->h - 1;
    cl_x0 = x0; cl_y0 = y0; cl_x1 = x1; cl_y1 = y1;
}

static void g_px(int x, int y)
{
    if (x < cl_x0 || x > cl_x1 || y < cl_y0 || y > cl_y1) return;
    if (x < 0 || y < 0 || x >= tgt->w || y >= tgt->h) return;
    tgt->px[x + y * SCR_W] = (unsigned char)gcol;
    if (tgt->has_alpha) tgt->al[x + y * SCR_W] = 255;
}

static void g_fillrect(int x, int y, int w, int h)
{
    int i, j;
    for (j = y; j < y + h; j++)
        for (i = x; i < x + w; i++) g_px(i, j);
}

static void g_line(int x0, int y0, int x1, int y1)
{
    int dx = x1 - x0, dy = y1 - y0;
    int sx = dx >= 0 ? 1 : -1, sy = dy >= 0 ? 1 : -1;
    int ax = dx >= 0 ? dx : -dx, ay = dy >= 0 ? dy : -dy;
    int err = ax - ay, e2, n = (ax > ay ? ax : ay), i;
    for (i = 0; i <= n; i++) {
        g_px(x0, y0);
        e2 = 2 * err;
        if (e2 > -ay) { err -= ay; x0 += sx; }
        if (e2 < ax)  { err += ax; y0 += sy; }
    }
    g_px(x1, y1);                          // inclusive endpoints
}

static void g_rect(int x, int y, int w, int h)
{
    g_line(x, y, x + w, y);
    g_line(x + w, y, x + w, y + h);
    g_line(x, y + h, x + w, y + h);
    g_line(x, y, x, y + h);
}

// Graphics.drawArc: ellipse outline, angles deg CCW, 0 = 3 o'clock, y flipped.
static void g_arc(int x, int y, int w, int h, int start, int arc)
{
    double cx = x + w / 2.0, cy = y + h / 2.0;
    double rx = w / 2.0, ry = h / 2.0;
    int a0 = start, a1 = start + arc, i, steps = 240, n;
    double t0, t1;
    int px0 = 0, py0 = 0, have = 0;
    if (a1 < a0) { int t = a0; a0 = a1; a1 = t; }
    n = (arc < 0 ? -arc : arc) * steps / 360;
    if (n < 16) n = 16;
    t0 = a0 * 3.14159265358979 / 180.0;
    t1 = a1 * 3.14159265358979 / 180.0;
    for (i = 0; i <= n; i++) {
        double t = t0 + (t1 - t0) * i / n, s = sin(t);
        int px = (int)(cx + rx * cos(t)), py = (int)(cy - ry * s);
        /* the exact bottom-tangent sample (sin == -1, py lands on a whole row
           at cy+ry) is NOT drawn by the device arc raster: the original card
           has no stray pixel hanging under the U-turn flats (user-verified,
           2x2 px). Neighbour chord samples already cover that column. */
        if (i > 0 && i < n && s <= -0.9999999) {
            continue;
        }
        if (have) g_line(px0, py0, px, py); else { px0 = px; py0 = py; }
        px0 = px; py0 = py; have = 1;
    }
}

// drawImage(img, x, y, TOP|LEFT): opaque copy where src alpha != 0.
static void g_image(const surface *s, int x, int y)
{
    int i, j;
    for (j = 0; j < s->h; j++) {
        int yy = y + j;
        if (yy < cl_y0 || yy > cl_y1 || yy < 0 || yy >= tgt->h) continue;
        for (i = 0; i < s->w; i++) {
            int xx = x + i;
            unsigned char a;
            if (xx < cl_x0 || xx > cl_x1 || xx < 0 || xx >= tgt->w) continue;
            a = s->has_alpha ? s->al[i + j * SCR_W] : 255;
            if (a == 0) continue;
            tgt->px[xx + yy * SCR_W] = s->px[i + j * SCR_W];
            if (tgt->has_alpha) tgt->al[xx + yy * SCR_W] = 255;
        }
    }
}

static void g_string(const char *s, int x, int y)   // anchor TOP|LEFT
{
    while (*s) {
        const unsigned char *g;
        int cp = (int)(size_t)utf8_next(&s);
        int col, row;
        g = glyph_for(cp);
        for (row = 0; row < 7; row++) {
            for (col = 0; col < 5; col++)
                if (g[row] & (0x80 >> col)) g_px(x + col, y + row);
        }
        x += g_adv[glyph_index(cp)];
    }
}

// ---------------------------------------------------------------------------
// Images: KKM.ah[4] + scratch; static strips decoded once from MK_D.
// 1bpp strips: bit=1 -> black, bit=0 -> white (verified: splash logo art,
// game-over plate white-carved letters, map tiles = black-outlined boxes).
// ---------------------------------------------------------------------------
static surface g_cards[4];                   // KKM.ah
static surface g_scratch;                 // b() scratch (j case 4 rebuilds/f)
static surface s_splash;                  // c.aa(3284) 96x65 title
static surface s_atlas;                   // c.aa(2116) 64x64 HUD/map montage
static surface s_over;                    // c.aa(2632) 96x54 GAME OVER plate
static surface s_bl;                      // c.aa(@927) 64x13 notes odometer strip

static void load_strip(int off, surface *dst)
{
    int w = MK_D[off] | (MK_D[off + 1] << 8);
    int h = MK_D[off + 2] | (MK_D[off + 3] << 8);
    const unsigned char *d = MK_D + off + 4;
    int x, y;
    dst->w = w; dst->h = h; dst->has_alpha = 0;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            int bit = (d[(x >> 3) + y * (w >> 3)] >> (7 - (x & 7))) & 1;
            dst->px[x + y * SCR_W] = bit ? 0 : 255;
            dst->al[x + y * SCR_W] = 255;
        }
}

static void copy_strip(const surface *s, surface *d)
{
    d->w = s->w; d->h = s->h; d->has_alpha = 0;
    memcpy(d->px, s->px, (size_t)s->h * SCR_W);
    memset(d->al, 255, (size_t)d->h * SCR_W);
}

// b.b(String,int): text image, width always 96, wrap at 94 px content width.
static char g_wrap[32][200];

static int text_wrap(const char *s1)
{
    int nchars = 0;
    const char *p;
    int line_no, line_w, word_start, word_w, i, cpw;
    for (p = s1; *p; p++) if ((*p & 0xC0) != 0x80) nchars++;
    if (str_width(s1) <= 94) {
        strcpy(g_wrap[0], s1);
        return 1;
    }
    // String[] var7 = new String[len*charWidth('W')/94 + 1];  (capacity, unused in C)
    line_no = 0; line_w = 0; word_start = 0; word_w = 0;
    g_wrap[0][0] = 0;
    for (i = 0, p = s1; i < nchars; i++) {
        const char *w = p;
        const char *nxt;
        int issp;
        nxt = p;
        cpw = (int)(size_t)utf8_next(&nxt);
        issp = (*p == ' ');
        if (!issp) word_w += g_adv[glyph_index(cpw)];
        if (i == nchars - 1 || issp) {
            int sublen;
            if (line_w == 0)
                g_wrap[line_no][0] = 0;
            else if (line_w + word_w + g_adv[0] <= 94) {
                strcat(g_wrap[line_no], " ");
                line_w += g_adv[0];
            } else {
                line_no++;
                line_w = 0;
                g_wrap[line_no][0] = 0;
            }
            // substring(var10, i==last ? i+1 : i): copy word (codepoints)
            sublen = (i == nchars - 1) ? i + 1 : i;
            {
                int k; const char *q = s1; char *o = g_wrap[line_no] + strlen(g_wrap[line_no]);
                for (k = 0; k < word_start; k++) { utf8_next(&q); }
                for (k = word_start; k < sublen; k++) { const char *e = q; utf8_next(&e); memcpy(o, q, (size_t)(e - q)); o += e - q; q = e; }
                *o = 0;
            }
            line_w += word_w;
            word_start = i + 1;
            word_w = 0;
            (void)w;
        }
        p = nxt;
    }
    return line_no + 1;
}

// build text image into `dst` (like b.b returns Image): font sizes collapsed.
static void build_text(surface *dst, int fontsize, const char *s1)
{
    int nlines = text_wrap(s1);
    int i;
    (void)fontsize;
    dst->w = SCR_W; dst->h = (FONT_H + 1) * nlines + 1;
    memset(dst->px, 0, (size_t)dst->w * dst->h);
    memset(dst->al, 0, (size_t)dst->w * dst->h);
    dst->has_alpha = 1;
    {
        int savedx0 = cl_x0, savedy0 = cl_y0, savedx1 = cl_x1, savedy1 = cl_y1;
        int savedcol = gcol;
        surface *saved = tgt;
        tgt = dst;
        gcol = 0;                           // default graphics color = black
        // Java b.b() draws into a fresh Image whose Graphics starts with the
        // full image clip; the port shares one global clip, so reset it here
        // (otherwise build_text from a tick inherits the last paint's clip --
        // Enter on the world map <19 ticks left card text outside a 16x14
        // marker clip -> blank "Level 1" card).
        g_setclip(0, 0, dst->w, dst->h);
        for (i = 0; i < nlines; i++)
            g_string(g_wrap[i], (SCR_W - str_width(g_wrap[i])) >> 1, i * (FONT_H + 1));
        tgt = saved; gcol = savedcol;
        cl_x0 = savedx0; cl_y0 = savedy0; cl_x1 = savedx1; cl_y1 = savedy1;
    }
}

// b.a(String,String): first "%N" (extracted assets store it as "%s") -> value
static void str_subst(const char *fmt, const char *val, char *out)
{
    const char *p = strstr(fmt, "%s");
    if (!p) { strcpy(out, fmt); return; }
    {
        size_t pre = (size_t)(p - fmt);
        memcpy(out, fmt, pre); out[pre] = 0;
        strcat(out, val);
        strcat(out, p + 2);
    }
}

static char *itoa_(int v, char *b) { sprintf(b, "%d", v); return b; }

// ---------------------------------------------------------------------------
// KKM state
// ---------------------------------------------------------------------------
static const char *const *g_strings;           // KKM.ao[] (MK_STR[lang])
static int g_screen_id = -1;                     // screen id
static int g_ui_id;                          // UI/menu id (0 = canvas visible)
static int g_sub_state;                          // in-game sub state
static int g_screen_t, g_intro_page;
static int g_screen_dirty, g_save_exists, g_demo_scripted, g_paused, g_destroyed;
static int g_sound_on = 1, g_lights_on = 0, g_dither_mode = 1;  // KKM.k sound, KKM.j lights;
                                                            // dither: port-only display option
static int g_bonus_key, g_bonus_last_lvl, g_bonus_keynote, g_bonus_par;            // bonus counters
static int g_hiscore_second, g_hiscore_best;                      // hiscores ("MKK")
static int g_ak_active;                   // KKM.ak != null
static int g_on_canvas = 1;               // Display.getCurrent() == canvas
static unsigned char g_keys_released[MK_NKEYS];   // b.d[]: key released since last e()
static int g_tickdiv;                     // Java Timer period 100 ms / 25 ms
static const signed char g_intro[16] = {  // KKM.e[]
    7, 8, 9, -90, 10, -91, 11, -92, 12, -93, 13, -94, 14, -95, 15, 16
};
static const signed char g_demo_keys[16] = { // byte[] at KKM.m() line 341
    -1, -1, -1, 2, -1, 5, -1, 0, -1, 2, -1, 5, -1, 3, -1, -1
};

// ---------------------------------------------------------------------------
// Record stores -> files next to the exe
// ---------------------------------------------------------------------------
static void opt_save(void)                // KKM.g(): "OPT", 3 bytes
{
    FILE *f = fopen("OPT.rms", "wb");
    if (f) {
        unsigned char b[4];
        b[0] = (unsigned char)(g_sound_on ? 1 : 0);
        b[1] = (unsigned char)(g_lights_on ? 1 : 0);
        b[2] = (unsigned char)mk_start_level;   // (byte)this.w
        b[3] = (unsigned char)g_dither_mode;    // port-only: display dithering
        fwrite(b, 1, 4, f);
        fclose(f);
    }
}

static void opt_load(void)                // KKM.h()
{
    FILE *f = fopen("OPT.rms", "rb");
    unsigned char b[4];
    int n;
    if (f && (n = (int)fread(b, 1, 4, f)) >= 3) {
        g_sound_on = (b[0] == 1);
        g_lights_on = (b[1] == 1);
        // harden OPT.rms like KKM.rms (e1e8ac8): the checkpoint level must be
        // a real level id, else -1/0/etc. drive decode_level_by_id off-table.
        {
            signed char lvl = (signed char)b[2];
            mk_start_level = (mk_level_index(lvl) >= 0) ? lvl : 1;
        }
        g_dither_mode = (n >= 4) ? (b[3] == 0 ? 0 : 1) : 1;   // legacy 1/2 modes map onto device halftone
    } else {
        g_sound_on = 1; g_lights_on = 0; mk_start_level = 1;  // exception defaults
    }
    if (f) fclose(f);
}

static void hiscore_load(void)            // KKM.init() lines 1048-1057: "MKK"
{
    FILE *f = fopen("MKK.rms", "rb");
    if (f) {
        unsigned char b[4];
        if (fread(b, 1, 4, f) == 4) {
            g_hiscore_best = (b[0] << 8) + b[1];
            g_hiscore_second = (b[2] << 8) + b[3];
        }
        fclose(f);
    }
}

static void hiscore_save(void)            // KKM.m() case ab==7: delete + add
{
    FILE *f = fopen("MKK.rms", "wb");
    if (f) {
        unsigned char b[4];
        b[0] = (unsigned char)(g_hiscore_best >> 8); b[1] = (unsigned char)(g_hiscore_best & 0xFF);
        b[2] = (unsigned char)(g_hiscore_second >> 8); b[3] = (unsigned char)(g_hiscore_second & 0xFF);
        fwrite(b, 1, 4, f);
        fclose(f);
    }
}

// "KLP": cleared-levels log (d.java 795-825). One raw byte per cleared level.
int mk_klp_cleared(int levelno)
{
    FILE *f = fopen("KLP.rms", "rb");
    unsigned char b;
    int found = 0;
    while (f && fread(&b, 1, 1, f) == 1)
        if ((signed char)b == (signed char)levelno) { found = 1; break; }
    if (f) fclose(f);
    return found;
}

void mk_klp_mark(int levelno)
{
    FILE *f = fopen("KLP.rms", "ab");
    if (f) {
        unsigned char b = (unsigned char)levelno;
        fwrite(&b, 1, 1, f);
        fclose(f);
    }
}

void mk_klp_wipe(void) { remove("KLP.rms"); }

// ---------------------------------------------------------------------------
// KKM.i(int): sound hook. game_sound() is the engine.h alias of the same.
// ---------------------------------------------------------------------------
void game_sound(int id)
{
    if (g_sound_on) platform_sound(id);
}

static void snd(int id) { if (g_sound_on) platform_sound(id); }

// ---------------------------------------------------------------------------
// b.e(): any key already released clears its an[] bit
// ---------------------------------------------------------------------------
static void rel_clear(void)
{
    int i;
    for (i = 0; i < MK_NKEYS; i++)
        if (g_keys_released[i]) mk_an[i] = 0;
}

static int g_state_soft_latch;                    // soft-key press observed while an
                                         // "any key" sub-screen (hint/card/map)
                                         // owned input: it counts as a digit
                                         // press, consumed by anykey(), and its
                                         // release is swallowed. Original J2ME
                                         // delivers keyReleased(-7) to the Game-
                                         // Canvas unconditionally: aj==2 -> r(24)
                                         // pause, so Enter on the hint opened
                                         // pause and Resume (q(2)) skipped the
                                         // map's c6++ -> "Level 0" + replay.

// KKM.o(): true if any key, clears all
static int anykey(void)
{
    int v = g_state_soft_latch, i;
    g_state_soft_latch = 0;
    for (i = 0; i < MK_NKEYS; i++) { v |= mk_an[i]; mk_an[i] = 0; }
    return v;
}

// ---------------------------------------------------------------------------
// Menu ("List"/"Form"/"ChoiceGroup" screens, KKM.r cases 24..28)
// ---------------------------------------------------------------------------
typedef struct {
    int type;                              // 24..28
    char title[256];
    int nitems;
    char items[5][256];
    int sel;
    int scroll;                            // form text scroll (first line)
    int cur_group;                         // options form
    const char *body;                      // instructions form string
} menu_state;
static menu_state g_menu;
static int g_menu_soft_latch;                 // soft-key press observed while a
                                         // menu owned input (J2ME routes the
                                         // whole press+release pair to the
                                         // Displayable focused at press time)
static int g_canvas_soft_latch;                // select-soft press observed while the
                                         // canvas owned input; its release must
                                         // only act if the pair stayed on the
                                         // canvas (J2ME delivers keyReleased to
                                         // the Displayable focused at press time)

static void menu_item(menu_state *m, const char *fmt, const char *val)
{
    if (m->nitems < 5) {
        if (val) str_subst(fmt, val, m->items[m->nitems]);
        else     strcpy(m->items[m->nitems], fmt);
        m->nitems++;
    }
}

// ---------------------------------------------------------------------------
// KKM.s(): drop UI screens
// ---------------------------------------------------------------------------
static void ui_clear(void)
{
    g_menu_soft_latch = 0;
    g_canvas_soft_latch = 0;
    g_state_soft_latch = 0;
    if (g_on_canvas) { g_screen_dirty = 1; rel_clear(); }
}

// forward
static void nav(int v);
static void set_screen(int v);
static void set_substate(int v);
static void die(void);

// b.d(boolean,boolean): render visibility (canvas.b.bv). bv == engine alive.
static void mark_visible_region(int visible, int free_lists)
{
    (void)free_lists;
    if (!visible) r_free();                   // u() / bd=ba=null both -> r_free
}

// ---------------------------------------------------------------------------
// KKM.p(byte): screen change
// ---------------------------------------------------------------------------
static void set_screen(int v)
{
    if (mk_env_on("MK_SCR"))
        fprintf(stderr, "scr_set(%d) from aj=%d ai=%d ab=%d\n", v, g_screen_id, g_ui_id, g_sub_state);
    if (g_screen_id == SCREEN_GAME || g_screen_id == SCREEN_DEMO) {
        g_ak_active = 0;
        mark_visible_region(0, 0);
    }
    if (g_ui_id != 0) { g_ui_id = 0; ui_clear(); g_on_canvas = 1; }
    g_screen_id = v;
    mk_shell_state = v;   // KKM.aj mirror: gates aw()'s end-of-load checkpoint
    g_screen_dirty = 1;
    switch (v) {
    case SCREEN_SPLASH:
        copy_strip(&s_splash, &g_cards[0]);  // ah[0] = c.aa(3284)
        snd(10);
        break;
    case SCREEN_GAME:
    case SCREEN_DEMO: {
        int fresh = (g_screen_t == 1);
        if (fresh) mk_klp_wipe();          // ctor deleteRecordStore("KLP") d.java:1324
        mk_init_run(mk_start_level, fresh);     // new d(this, al, ad == 1)
        if (g_screen_t == 1) g_dungeon.keys_total = 0;
        r_shake();                         // ak.b.z()
        g_ak_active = 1;
        break;
    }
    case SCREEN_HISCORE:
        g_screen_t = 0;
        copy_strip(&s_atlas, &g_cards[0]);   // ah[0] = c.aa(2116)
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// KKM.q(byte): in-game sub-state change
// ---------------------------------------------------------------------------
static void set_substate(int v)
{
    char num[16];
    char buf[256];
    if (g_ui_id != 0) { g_ui_id = 0; ui_clear(); g_on_canvas = 1; }
    g_sub_state = v;
    switch (v) {
    case SUB_HINT:
        g_screen_dirty = 1;
        strcpy(buf, "("); strcat(buf, g_strings[15]); strcat(buf, ")");
        build_text(&g_cards[0], FONT_SMALL, buf);
        g_screen_t = -((SCR_H - g_cards[0].h) >> 1);
        if (g_screen_t > 0) g_screen_t = 0;
        g_dungeon.level_no = mk_start_level - 1;
        break;
    case SUB_LEVEL_CARD:
        g_screen_dirty = 1;
        itoa_(g_dungeon.level_no, num);
        str_subst(g_strings[4], num, buf);
        build_text(&g_cards[1], FONT_TITLE, buf);
        str_subst(g_strings[(g_dungeon.level_no % 5 == 0 && g_dungeon.level_no < LVL_CHAPTER_END) ? 6 : 5], itoa_(g_dungeon.notes_left, num), buf);
        build_text(&g_cards[0], FONT_SMALL, buf);
        g_screen_t = -(((SCR_H - g_cards[0].h) - 14) >> 1);
        if (g_screen_t > 0) g_screen_t = 0;
        break;
    case SUB_BONUS:
        mk_start_level = g_dungeon.level_no + 1;
        if (g_dungeon.level_no > LVL_MAX) mk_start_level = LVL_MAX;
        opt_save();
        copy_strip(&s_atlas, &g_cards[0]);
        g_screen_dirty = 1;
        g_screen_t = 0;
        break;
    case SUB_WORLD_MAP: {
        // level-clear / world map card into ah[1] (96x65). Original:
        // Image.createImage(96,65) is OPAQUE WHITE (java KKM.java m() case 5
        // drawImage(ah[1],0,0) each frame erases the previous marker frame);
        // a transparent card here let the sliding monkey marker leave a
        // black smear in the lower-left corner.
        int goal_lvl = LVL_CHAPTER_END + g_dungeon.keys_held;
        int i;
        copy_strip(&s_atlas, &g_cards[0]);
        g_cards[1].w = SCR_W; g_cards[1].h = SCR_H; g_cards[1].has_alpha = 1;
        memset(g_cards[1].px, 255, SCR_W * SCR_H);
        memset(g_cards[1].al, 255, SCR_W * SCR_H);
        {
            surface *saved = tgt; int sc = gcol;
            tgt = &g_cards[1]; gcol = 0;
            cl_x0 = 0; cl_y0 = 0; cl_x1 = SCR_W - 1; cl_y1 = SCR_H - 1;
            g_line(3, 51, 6, 54);
            g_line(7, 54, 10, 51);
            { int arch_n = goal_lvl > LVL_CHAPTER_END ? 3 : 2;
              g_setclip(0, 0, (goal_lvl == 26) ? 65 : SCR_W, 9);
              for (i = 0; i < arch_n; i++) {
                  g_arc(3 + 26 * i, 1, 21, 21, 0, 180);
                  g_arc(10 + 26 * i, 6, 7, 7, 0, 180);
              }
            }
            g_setclip(0, 42, SCR_W, 51);       // y+h = 93 exceeds surface (clip keeps <=64)
            for (i = 0; i < 2; i++) {
                g_arc(16 + 26 * i, 29, 21, 21, 180, 180);
                g_arc(23 + 26 * i, 38, 7, 7, 180, 180);
            }
            g_setclip(0, 0, SCR_W, SCR_H);
            for (i = 0; i < 5; i++) {
                int tower_bottom = (i == 0) ? 51 : 41;
                g_line(3 + i * 13, 9, 3 + i * 13, tower_bottom);
                g_line(10 + i * 13, 9, 10 + i * 13, tower_bottom);
            }
            if (goal_lvl == 25) {
                g_line(55, 9, 55, 1); g_line(62, 9, 62, 1); g_line(55, 1, 62, 1);
            } else if (goal_lvl == 26) {
                g_line(64, 1, 75, 1); g_line(64, 6, 75, 6); g_line(75, 1, 75, 6);
            } else {
                int goal_top = goal_lvl * 10 - 251;
                g_line(68, 9, 68, goal_top); g_line(75, 9, 75, goal_top); g_line(68, goal_top, 75, goal_top);
            }
            for (i = g_dungeon.level_no - 1; i >= 0; i--) {
                int col = i % 5;
                int marker_x = i / 5 * 13 + 2;
                int marker_y = 42 - col * 10;
                if (i % 10 > 4) marker_y = 44 - marker_y;
                if (col == 4 && col != 29) marker_x++;
                else if (col == 0 && i != 0) marker_x--;
                g_setclip(marker_x, marker_y, 11, 9);
                g_image(&g_cards[0], marker_x, marker_y - 53);
            }
            for (i = 0; i < g_dungeon.keys_held; i++) {
                int slot_y = 1 + i * 10;
                g_setclip(80, slot_y, 14, 10);
                g_image(&g_cards[0], 80 - ((g_dungeon.level_no < 26 + i) ? 0 : 13), slot_y - 13);
            }
            tgt = saved; gcol = sc;
        }
        g_screen_dirty = 1;
        g_screen_t = 0;
        break;
    }
    case SUB_GAME_OVER: {
        int pct_val;
        char pct[32];
        mk_start_level = g_dungeon.level_no;
        opt_save();
        snd(6);
        g_screen_t = (g_dungeon.lives < 0) ? 0 : 20;
        pct_val = g_dungeon.level_no;
        if (g_dungeon.lives < 0) pct_val--;
        pct_val = pct_val * 90 / 30 + g_dungeon.score / 1000;
        copy_strip(&s_over, &g_cards[0]);
        sprintf(pct, "%d%%", pct_val);
        build_text(&g_cards[1], FONT_LARGE, pct);
        break;
    }
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// KKM.n(): death / restart level
// ---------------------------------------------------------------------------
static void die(void)
{
    if (g_dungeon.lives > 0) {
        g_dungeon.lives--;
        g_dungeon.keys_total++;
        if (g_dungeon.extra_life) mk_klp_mark(g_dungeon.level_no);       // ak.au()
        mk_load_level(g_dungeon.level_no);
        set_substate(SUB_LEVEL_CARD);
    } else {
        g_dungeon.lives--;
        set_substate(SUB_GAME_OVER);
    }
}

// ---------------------------------------------------------------------------
// KKM.k(Graphics,int,int,int,boolean): 5-digit right-aligned odometer
// ---------------------------------------------------------------------------
static void odometer(int val, int x, int y, int pad)
{
    int i;
    char d[2];
    if (val == 0 && !pad) return;
    d[1] = 0;
    for (i = 0; i < 5; i++) {
        d[0] = (char)('0' + val % 10);
        g_string(d, x - i * 10, y);        // 10 px digit pitch kept verbatim
        val /= 10;
        if (val == 0 && !pad) return;
    }
}

// KKM.l(int): score step by quantum; adds to engine score
static int score_step(int remain)
{
    int quantum = 1;
    if (remain >= 100) quantum = 100;
    else if (remain >= 10) quantum = 10;
    g_dungeon.score += quantum;
    return remain - quantum;
}

// ---------------------------------------------------------------------------
// KKM.j(Graphics): canvas overlay (drawn after dungeon when in game)
// ---------------------------------------------------------------------------
static void paint_canvas_overlay(void)
{
    g_setclip(0, 0, SCR_W, SCR_H);
    if (g_screen_dirty) {
        int savec = gcol; gcol = 255;
        g_fillrect(0, 0, SCR_W, SCR_H);
        gcol = savec;
        g_screen_dirty = 0;
    }
    gcol = 0;
    if (g_ui_id != 0) return;
    switch (g_screen_id) {
    case SCREEN_SPLASH:
        g_image(&g_cards[0], 0, 0);
        /* falls to case 1/default */
        break;
    case SCREEN_GAME:
        switch (g_sub_state) {
        case SUB_HINT:
        case SUB_LEVEL_CARD:
            g_image(&g_cards[0], 0, (g_sub_state == SUB_LEVEL_CARD ? 14 : 0) - g_screen_t);
            if (g_sub_state == SUB_LEVEL_CARD) {
                int sc = gcol; gcol = 255;
                g_fillrect(0, 0, SCR_W, 13);
                gcol = sc;
                g_image(&g_cards[1], 0, 0);
                gcol = 0;
                g_line(0, 12, 100, 12);
            }
            return;
        case SUB_PLAY:
            if (mk_an[MK_KEY_STAR]) {      // '*' -> pause menu
                mk_an[MK_KEY_STAR] = 0;
                nav(MENU_PAUSE);
            }
            return;
        case SUB_LEVEL_CLEAR:
        case 6:
        default:
            return;
        case SUB_BONUS: {
            int bonus_val;
            {
                char buf[256];
                char num[16];
                str_subst(g_strings[4], itoa_(g_dungeon.level_no, num), buf);
                build_text(&g_scratch, FONT_TITLE, buf);
            }
            g_image(&g_scratch, 0, (SCR_W - g_scratch.w) >> 1);
            g_line(0, 12, 100, 12);
            g_rect(20, 48, 53, 16);
            odometer(g_dungeon.score, 63, 50, 1);
            if (g_bonus_par > 0) {
                bonus_val = g_bonus_par;
                g_setclip(3, 20, 28, 22);
                g_image(&g_cards[0], -23, 2);
            } else if (g_bonus_key > 0) {
                bonus_val = g_bonus_key;
                g_setclip(16, 23, 15, 16);
                g_image(&g_cards[0], -34, -26);
            } else if (g_bonus_keynote > 0) {
                bonus_val = g_bonus_keynote;
                g_setclip(9, 20, 22, 22);
                g_image(&g_cards[0], -17, -20);
            } else {
                bonus_val = g_bonus_last_lvl;
                if (g_bonus_last_lvl != 0) {
                    g_setclip(5, 22, 26, 18);
                    g_image(&g_cards[0], -21, 22);
                }
            }
            g_setclip(0, 0, SCR_W, SCR_H);
            odometer(bonus_val, 63, 24, 0);
            return;
        }
        case SUB_WORLD_MAP: {
            int lvl = g_dungeon.level_no;
            int col = lvl % 5;
            int cur_x = lvl / 5 * 13;
            int cur_y = 39 - col * 10;
            if (lvl % 10 > 4) { cur_y = 39 - cur_y; cur_x--; }
            if (g_screen_t < 19) {
                int slide = g_screen_t > 8 ? g_screen_t - 8 : 0;
                int prev_x, prev_y, from_x, from_y, prev_col;
                g_image(&g_cards[1], 0, 0);
                lvl--;
                prev_col = lvl % 5;
                prev_x = lvl / 5 * 13;
                prev_y = 39 - prev_col * 10;
                if (lvl % 10 > 4) { prev_y = 39 - prev_y; prev_x--; }
                from_x = (cur_x - prev_x) * slide / 10 + prev_x;
                from_y = (cur_y - prev_y) * slide / 10 + prev_y;
                g_setclip(from_x + 1, from_y + 1, 13, 11);  /* monkey cell A bbox (atlas rows
                   1..11, cols 1..13 -- col 13 rows 4..5 are the monkey's own arm pixels).
                   Cols 14+ are a NEIGHBOURING sprite (blob head) in the 2116 strip: it drew
                   as a stray dash right of the monkey; the full 16x14 also grabbed cell B's
                   "###" bar and white-washed the slot ticks */
                g_image(&g_cards[0], from_x, from_y);
            } else if (g_screen_t < 39 && g_dungeon.level_no > 24) {
                if (g_screen_t < 35) {
                    g_setclip(cur_x + 1, cur_y + 1, 13, 11);
                    g_image(&g_cards[0], cur_x, cur_y);
                }
                if (g_screen_t < 26) {
                    g_setclip(g_screen_t + 51, cur_y + 4, 34 - g_screen_t, 7);
                    g_image(&g_cards[0], g_screen_t + 37, cur_y + 1);
                } else {
                    int key_frame = g_screen_t < 35 ? (g_screen_t - 1) % 3 : 0;
                    if (g_screen_t > 34) {
                        lvl = g_screen_t - 35;
                        g_setclip(80, cur_y + 1, 14, 5);
                        g_image(&g_cards[0], 80 - (lvl == 3 ? 13 : 0), cur_y - 12 - ((lvl + 1) & 3) * 10);
                    }
                    g_setclip(78, cur_y + 4, 7, 7);
                    g_image(&g_cards[0], 65, cur_y - 22 - key_frame * 10);
                }
                return;
            }
            return;
        }
        case SUB_GAME_OVER:
            if (g_screen_t < 20) {
                g_image(&g_cards[0], 0, 5 - (g_screen_t < 10 ? (10 - g_screen_t) * 6 : 0));
            } else {
                g_rect(20, 10, 53, 16);
                odometer(g_dungeon.score, 63, 12, 1);
                g_image(&g_cards[1], 0, 40);
            }
            return;
        }
        break;
    case SCREEN_HISCORE:
        g_rect(20, 0, 53, 16);
        odometer(g_hiscore_best, 63, 2, 1);
        g_screen_dirty = 1;
        if (g_hiscore_second != 0) {
            g_rect(20, 48, 53, 16);
            odometer(g_hiscore_second, 63, 50, 1);
        }
        {
            int marker_x = (g_screen_t << 1) - 24;
            g_setclip(marker_x + 1, 26, 13, 11);   /* walking monkey: cell A bbox (see map) */
            g_image(&g_cards[0], marker_x, 25);
        }
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// d.j(Graphics) + d.av(): dungeon HUD (notes odometer plate + key slots).
// Ported here because engine.h exposes no HUD entry point (see report).
// ---------------------------------------------------------------------------
static void paint_note_odometer(int notes, int x, int y)
{
    int rest = notes;
    int digit = 0;
    do {
        int digit_x = x - digit++ * 5;
        g_setclip(digit_x, y, 5, 7);
        g_image(&s_bl, digit_x - 24 - (rest % 10) * 4, y);
        rest /= 10;
    } while (rest > 0);
}

static void draw_tex44(const unsigned char *tex, int w, int h, int x, int y)
{
    int i, j;
    for (j = 0; j < h; j++) {
        int yy = y + j;
        for (i = 0; i < w; i++) {
            unsigned char v = tex[j * w + i];
            int xx = x + i;
            unsigned char a = v & 0x0F, g4 = v >> 4;
            if (a == 0) continue;
            if (xx < cl_x0 || xx > cl_x1 || yy < cl_y0 || yy > cl_y1) continue;
            if (xx < 0 || yy < 0 || xx >= SCR_W || yy >= SCR_H) continue;
            tgt->px[xx + yy * SCR_W] = (unsigned char)(g4 * 17);
            if (tgt->has_alpha) tgt->al[xx + yy * SCR_W] = 255;
        }
    }
}

static void dungeon_hud(void)
{
    if (g_dungeon.notes_left != 0 && g_dungeon.status == 0) {
        g_setclip(0, 0, 25, 14);
        g_image(&s_bl, 0, 0);
        paint_note_odometer(g_dungeon.notes_left, g_dungeon.notes_left < 10 ? 11 : 16, 3);
    }
    if (g_dungeon.keys_hud_on) {
        draw_tex44(MK_TEX_KB, MK_TEX_KB_W, MK_TEX_KB_H, 73, 2);
        if (g_dungeon.captured_key != -1) {
            int key_no = g_dungeon.captured_key;
            int slot_col, slot_row;
            if (key_no == 11) key_no = 12;
            if (key_no == 0) key_no = 11;
            key_no--;
            slot_col = key_no % 3;
            slot_row = key_no / 3;
            draw_tex44(MK_TEX_KY, MK_TEX_KY_W, MK_TEX_KY_H, 75 + 6 * slot_col, 4 + 5 * slot_row);
        }
    }
}

// ---------------------------------------------------------------------------
// KKM.m(): 100 ms tick body (Java Timer period 100 ms; run every 4th tick25)
// ---------------------------------------------------------------------------
static void tick_body(void)
{
    if (g_destroyed) return;
    if (g_paused) return;
    if (g_ui_id != 0) return;
    switch (g_screen_id) {
    case SCREEN_SPLASH:
        if (anykey() || ++g_screen_t > SPLASH_AUTOEXIT) nav(MENU_MAIN);
        /* fallthrough (Java: case 0 -> case 1 -> default: break) */
        break;
    case 1:
    case SCREEN_INTRO_FORM:
    default:
        break;
    case SCREEN_HISCORE:
        if (anykey() || ++g_screen_t == HISCORE_AUTOEXIT) nav(MENU_MAIN);
        break;
    case SCREEN_DEMO:
        if (g_demo_scripted) { g_screen_t = 1; g_demo_scripted = 0; }
        g_screen_t++;
        if (mk_an[MK_KEY_5] || g_screen_t > DEMO_PAGE_EXIT) {
            mk_an[MK_KEY_5] = 0;
            nav(MENU_INTRO);
            break;
        } else {
            int i;
            for (i = 0; i < MK_NKEYS; i++) mk_an[i] = 0;
            if (g_screen_t == DEMO_KEY_TICK) {
                int k = g_demo_keys[g_intro_page - 1];
                mk_an[k] = 1;              // demo pages always have k >= 0
            }
        }
        /* fallthrough to case 2 (Java) */
    case SCREEN_GAME:
        if (g_sub_state == SUB_HINT || g_sub_state == SUB_LEVEL_CARD) {
            int reserved = (g_sub_state == SUB_LEVEL_CARD) ? 14 : 0;
            if (g_cards[0].h > SCR_H - reserved) {
                if (mk_an[MK_KEY_2]) {
                    g_screen_dirty = 1;
                    g_screen_t -= CARD_SCROLL_STEP;
                    if (g_screen_t < 0) g_screen_t = 0;
                    mk_an[MK_KEY_2] = 0;
                }
                if (mk_an[MK_KEY_8]) {
                    int max_scroll = g_cards[0].h + reserved - SCR_H;
                    g_screen_dirty = 1;
                    g_screen_t += CARD_SCROLL_STEP;
                    if (g_screen_t > max_scroll) g_screen_t = max_scroll;
                    mk_an[MK_KEY_8] = 0;
                }
            }
        }
        switch (g_sub_state) {
        case SUB_HINT:
            if (anykey()) set_substate(SUB_WORLD_MAP);
            break;
        case SUB_LEVEL_CARD:
            if (anykey()) { set_substate(SUB_PLAY); mk_tick100(); }
            break;
        case SUB_PLAY:
            mk_tick100();
            if (g_screen_id == SCREEN_DEMO) mk_an[MK_KEY_5] = 0;
            if (g_dungeon.status == 1) {
                die();
            } else if (g_dungeon.status != 0 && g_dungeon.level_no < 90) {
                g_screen_t = 0;
                set_substate(SUB_LEVEL_CLEAR);
                g_bonus_key = (g_dungeon.status == 16) ? 500 : 0;
                g_bonus_keynote = (g_dungeon.status == 24) ? (5 - g_dungeon.level_no % 5) * 400 : 0;
                if (g_bonus_keynote + g_bonus_key == 0 && g_dungeon.par_moves < g_dungeon.moves)
                    g_bonus_par = 190 * g_dungeon.par_moves / (2 * g_dungeon.moves - g_dungeon.par_moves) + 10;
                else
                    g_bonus_par = 200;
                while (g_dungeon.keys_total > 0) { g_bonus_par = g_bonus_par * 3 >> 2; g_dungeon.keys_total--; }
                if (g_dungeon.level_no < 26) g_bonus_par >>= 1;
                g_bonus_last_lvl = (g_dungeon.level_no == 30) ? 400 : 0;
            }
            break;
        case SUB_LEVEL_CLEAR: {
            int duration = 38;
            if (LVL_CHAPTER_END + g_dungeon.keys_held == g_dungeon.level_no) {
                g_scene.curtain_prog = g_screen_t;
                g_scene.curtain_state = (g_dungeon.level_no == 30) ? 2 : 1;
                duration = 58;
            }
            g_dungeon.anim_id = 11;
            mk_tick100();
            if (g_lights_on) { /* DeviceControl.setLights(0, (ad & 4) * 5) */ }
            if (++g_screen_t > duration) set_substate(SUB_BONUS);
            break;
        }
        case SUB_BONUS:
            g_screen_dirty = 1;
            if (++g_screen_t > 8) {
                if (g_bonus_par > 0) {
                    if ((g_bonus_par = score_step(g_bonus_par)) == 0) g_screen_t = 0;
                } else if (g_bonus_key > 0) {
                    if ((g_bonus_key = score_step(g_bonus_key)) == 0) g_screen_t = 0;
                } else if (g_bonus_keynote > 0) {
                    if ((g_bonus_keynote = score_step(g_bonus_keynote)) == 0) { g_screen_t = 0; g_bonus_keynote = -100; }
                } else if (g_bonus_last_lvl > 0) {
                    if ((g_bonus_last_lvl = score_step(g_bonus_last_lvl)) == 0) g_screen_t = 0;
                } else if (g_screen_t > 20) {
                    if (g_dungeon.level_no < LVL_CHAPTER_END + g_dungeon.keys_held) set_substate(SUB_WORLD_MAP);
                    else set_substate(SUB_GAME_OVER);
                }
            }
            break;
        case SUB_WORLD_MAP:
            if (anykey() || ++g_screen_t > WORLD_MAP_EXIT) {
                g_dungeon.level_no++;
                if (g_bonus_keynote == -100 && g_dungeon.level_no % 5 != 0) {
                    g_bonus_keynote = 100;
                    set_substate(SUB_BONUS);
                } else {
                    mk_load_level(g_dungeon.level_no);
                    g_dungeon.keys_total = 0;
                    set_substate(SUB_LEVEL_CARD);
                }
            }
            break;
        case 6:
        default:
            break;
        case SUB_GAME_OVER:
            g_screen_dirty = 1;
            if (++g_screen_t > GAME_OVER_EXIT) {
                if (g_dungeon.score > g_hiscore_best) { g_hiscore_second = g_hiscore_best; g_hiscore_best = g_dungeon.score; }
                else g_hiscore_second = g_dungeon.score;
                hiscore_save();
                // KKM.java:490: deleteRecordStore("KKM") on the game-over
                // board commit — otherwise "Continue" resurrects the dead run.
                mk_delete_run();
                nav(MENU_MAIN);
            }
            break;
        }
        break;
    }
    if (g_ui_id == 0) rel_clear();            // al.e()
}

// ---------------------------------------------------------------------------
// KKM.commandAction / KKM.itemStateChanged (Java dialogs -> canvas menus)
// ---------------------------------------------------------------------------
#define CMD_U   0                          // Command u (CANCEL, "back/exit")
#define CMD_S   1                          // Command s (BACK, next intro page)
#define CMD_SEL 2                          // List.SELECT_COMMAND

static void command_action(int cmd, int sel)
{
    if (cmd == CMD_U) {
        if (g_ui_id == MENU_MAIN) {
            // v = true; al.d(false,false); destroyApp(true); notifyDestroyed()
            g_destroyed = 1;
            g_paused = 1;
            mark_visible_region(0, 0);
        } else {
            nav(MENU_MAIN);
        }
    }
    if (cmd == CMD_S) nav(MENU_INTRO);
    if (cmd == CMD_SEL) {
        int item_idx = sel;
        switch (g_ui_id) {
        case MENU_PAUSE:
            switch (item_idx) {
            case 0: set_substate(SUB_PLAY); return;
            case 1: die(); return;
            case 2: nav(MENU_MAIN); return;
            default: return;
            }
        case MENU_MAIN:
            if (g_save_exists) item_idx--;              // Continue item shifts the table
            switch (item_idx) {
            case -1:
                g_screen_t = 0; set_screen(SCREEN_GAME); set_substate(SUB_PLAY);
                break;
            case 0:
                if (mk_start_level > 1) {
                    nav(MENU_LEVEL_PICK);
                } else {
                    g_screen_t = 1; set_screen(SCREEN_GAME); set_substate(SUB_HINT);
                }
                break;
            case 1:
                set_screen(SCREEN_HISCORE);
                break;
            case 2:
                g_intro_page = 0; nav(MENU_INTRO);
                break;
            case 3:
                nav(MENU_OPTIONS);
                break;
            default:
                break;
            }
            break;
        case MENU_INTRO:
        case MENU_OPTIONS:
        default:
            break;
        case MENU_LEVEL_PICK:
            if (item_idx == 0) mk_start_level = 1;
            g_screen_t = 1;
            set_screen(SCREEN_GAME);
            set_substate(SUB_HINT);
            break;
        }
    }
}

static void item_state(int group)          // KKM.itemStateChanged
{
    if (group == 0) {                      // this.i: sounds
        g_sound_on = !g_sound_on;                        // var3 = isSelected(1)
        if (g_sound_on) snd(0);
    } else if (group == 1) {             // this.h: lights
        g_lights_on = !g_lights_on;
        /* DeviceControl.setLights(0, j ? 100 : 0) */
    } else {                             // port: dithering On(device)/Off
        g_dither_mode = g_dither_mode ? 0 : 1;
    }
}

// ---------------------------------------------------------------------------
// KKM.r(byte): navigation -> build canvas menu screens
// ---------------------------------------------------------------------------
static void nav(int v)
{
    char num[16];
    if (g_ui_id == MENU_OPTIONS) opt_save();
    g_ui_id = v;
    ui_clear();
    memset(&g_menu, 0, sizeof g_menu);
    g_menu.type = v;
    switch (v) {
    case MENU_PAUSE:
        mk_save_run();                            // ak.ax() save run state
        str_subst(g_strings[17], itoa_(g_dungeon.lives, num), g_menu.title);
        menu_item(&g_menu, g_strings[18], 0);
        menu_item(&g_menu, g_strings[g_dungeon.lives == 0 ? 21 : 19], 0);
        menu_item(&g_menu, g_strings[20], 0);
        break;
    case MENU_MAIN:
        g_screen_id = SCREEN_NONE;
        strcpy(g_menu.title, "Munkiki's castles");
        g_save_exists = mk_save_exists();            // RecordStore "KKM" exists?
        if (g_save_exists) menu_item(&g_menu, g_strings[0], 0);
        menu_item(&g_menu, g_strings[1], 0);
        menu_item(&g_menu, g_strings[2], 0);
        menu_item(&g_menu, g_strings[3], 0);
        menu_item(&g_menu, g_strings[22], 0);
        break;
    case MENU_INTRO:
        g_screen_t = 0;
        g_intro_page++;
        g_demo_scripted = 0;
        if (g_intro_page > (int)sizeof g_intro) {
            nav(MENU_MAIN);
            return;
        } else {
            signed char entry = g_intro[g_intro_page - 1];
            if (entry < 0) {
                g_screen_t = 1;
                set_screen(SCREEN_DEMO);
                set_substate(SUB_PLAY);
                mk_load_level(-entry);
                g_demo_scripted = 1;
                if (entry != -95) g_dungeon.keys_hud_on = 1;
                g_menu.type = 0;           // stays on canvas
                g_on_canvas = 1;
                return;
            } else {
                g_ui_id = MENU_INTRO;
                g_screen_id = SCREEN_INTRO_FORM;   // Form: instructions page
                strcpy(g_menu.title, g_strings[3]);
                g_menu.body = g_strings[entry];
            }
        }
        break;
    case MENU_OPTIONS:
        g_screen_id = SCREEN_NONE;
        strcpy(g_menu.title, g_strings[22]);    // "Options"
        break;
    case MENU_LEVEL_PICK: {
        char t[256];
        strcpy(g_menu.title, g_strings[1]);
        str_subst(g_strings[4], "1", t);
        menu_item(&g_menu, t, 0);
        str_subst(g_strings[4], itoa_(mk_start_level, num), t);
        menu_item(&g_menu, t, 0);
        break;
    }
    default:
        break;
    }
    g_on_canvas = 0;                       // am.setCurrent(this.g)
}

// ---------------------------------------------------------------------------
// Menu screen rendering (replaces the Java List/Form device chrome)
// ---------------------------------------------------------------------------
static void menu_paint(uint8_t *fb)
{
    int i;
    g_fb_surf.w = SCR_W; g_fb_surf.h = SCR_H; g_fb_surf.has_alpha = 0;
    g_fb_surf.px = fb;
    tgt = &g_fb_surf;
    cl_x0 = 0; cl_y0 = 0; cl_x1 = SCR_W - 1; cl_y1 = SCR_H - 1;
    gcol = 255;
    g_fillrect(0, 0, SCR_W, SCR_H);
    gcol = 0;
    g_string(g_menu.title, (SCR_W - str_width(g_menu.title)) >> 1, 0);
    g_line(0, 9, 95, 9);
    if (g_menu.type == MENU_PAUSE || g_menu.type == MENU_MAIN || g_menu.type == MENU_LEVEL_PICK) {
        for (i = 0; i < g_menu.nitems; i++) {
            int y = MENU_ROW_Y0 + i * MENU_ROW_H;
            const char *s = g_menu.items[i];
            if (i == g_menu.sel) {
                gcol = 0; g_fillrect(0, y - 1, SCR_W, 9); gcol = 255;
            }
            g_string(s, 2, y);
            gcol = 0;
        }
    } else if (g_menu.type == MENU_INTRO) {
        int n = text_wrap(g_menu.body);
        int y = 11;
        if (g_menu.scroll > n - 1) g_menu.scroll = n - 1;
        if (g_menu.scroll < 0) g_menu.scroll = 0;
        for (i = g_menu.scroll; i < n && y < 54; i++, y += MENU_ROW_H)
            g_string(g_wrap[i], 1, y);
    } else if (g_menu.type == MENU_OPTIONS) {
        char line[256];
        for (i = 0; i < 3; i++) {
            int y = MENU_ROW_Y0 + i * MENU_OPT_ROW_H;
            strcpy(line, i == 0 ? g_strings[26] : i == 1 ? g_strings[25] : "Dithering");
            strcat(line, ": ");
            strcat(line, (i == 0 ? g_sound_on : i == 1 ? g_lights_on : g_dither_mode) ? g_strings[29] : g_strings[28]);
            if (i == g_menu.cur_group) {
                gcol = 0; g_fillrect(0, y - 1, SCR_W, 9); gcol = 255;
            }
            g_string(line, 2, y);
            gcol = 0;
        }
    }
    // soft-key labels (device chrome in the original; labels chosen here)
    {
        const char *L, *R;
        switch (g_menu.type) {
        case MENU_MAIN: L = "Select"; R = "Exit"; break;
        case MENU_INTRO: L = "Next";   R = "Back"; break;
        default: L = "Select"; R = "Back"; break;
        }
        g_line(0, 54, 95, 54);
        g_string(L, 2, 57);
        g_string(R, 94 - str_width(R), 57);
    }
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
static void menu_key(int key, int down)
{
    // A soft-key press+release pair belongs to the Displayable focused at
    // press time: a release whose press happened on the canvas is swallowed.
    if (key == J2M_SOFT_SELECT || key == J2M_SOFT_BACK || key == J2M_FIRE) {
        if (down) { g_menu_soft_latch = 1; return; }
        if (!g_menu_soft_latch) return;
        g_menu_soft_latch = 0;
    }
    if (g_menu.type == MENU_PAUSE || g_menu.type == MENU_MAIN || g_menu.type == MENU_LEVEL_PICK) {
        if (key == J2M_SOFT_SELECT || key == J2M_FIRE) {
            command_action(CMD_SEL, g_menu.sel);              // select soft / FIRE
            return;
        }
        if (key == J2M_SOFT_BACK) {
            command_action(CMD_U, 0);                         // right soft: u
            return;
        }
        if (!down) return;
        if (key == J2M_UP) {                   // UP: highlight up (Nokia list wraps)
            if (g_menu.nitems)
                g_menu.sel = (g_menu.sel + g_menu.nitems - 1) % g_menu.nitems;
        } else if (key == J2M_DOWN) {          // DOWN: highlight down
            if (g_menu.nitems)
                g_menu.sel = (g_menu.sel + 1) % g_menu.nitems;
        } else if (key >= 49 && key <= 57) {   // phone digit = jump highlight
            int idx = key - 49;            // (S40 List: no auto-select here;
            if (idx < g_menu.nitems)       //  selection is the soft key)
                g_menu.sel = idx;
        }
    } else if (g_menu.type == MENU_INTRO) {
        if (key == J2M_SOFT_SELECT && !down) command_action(CMD_S, 0);   // left soft: next page
        else if (key == J2M_SOFT_BACK && !down) command_action(CMD_U, 0);
        else if (down && key == J2M_RIGHT) g_menu.scroll++;        // RIGHT: scroll down
        else if (down && key == J2M_LEFT) g_menu.scroll--;         // LEFT: scroll up
        (void)down;
    } else if (g_menu.type == MENU_OPTIONS) {
        if (key == J2M_SOFT_SELECT && !down) item_state(g_menu.cur_group);
        else if (key == J2M_SOFT_BACK && !down) command_action(CMD_U, 0);
        else if (down && key == J2M_FIRE) item_state(g_menu.cur_group);
        else if (down && key == J2M_DOWN) g_menu.cur_group = (g_menu.cur_group + 1) % 3;
        else if (down && key == J2M_UP) g_menu.cur_group = (g_menu.cur_group + 2) % 3;
        (void)down;
    }
}

// game_key: J2ME phone key -> b.c() routing (b.keyPressed/keyReleased).
void game_key(int key, int down)
{
    if (mk_env_on("MK_SCR")) { fprintf(stderr, "KEYIN %d d=%d ai=%d\n", key, down, g_ui_id); fflush(stderr); }
    if (g_ui_id != 0) { menu_key(key, down); return; }

    if (key == J2M_SOFT_SELECT) {
        // Java -7 (Nokia select soft key): press is remembered; on release
        // r(24/25) -- but only when the press+release pair stayed on the
        // canvas. A release landing here after CMD_SEL already switched the
        // Displayable must be swallowed (one Enter press = one action).
        // On the in-game "any key" sub-screens (hint 0 / card 1 / map 5) the
        // soft key acts like Space: the press feeds anykey(), the release is
        // swallowed. (Original quirk: keyReleased always ran r(24) since p(2)
        // set aj==2 before the hint was shown; pausing on the hint and picking
        // "Return to game" jumped to q(2) without the map's c6++, desyncing
        // the level counter -> stats "Level 0" and the level replayed.)
        if (down) {
            if (g_screen_id == SCREEN_GAME && (g_sub_state == SUB_HINT || g_sub_state == SUB_LEVEL_CARD || g_sub_state == SUB_WORLD_MAP)) g_state_soft_latch = 1;
            else g_canvas_soft_latch = 1;
            return;
        }
        if (g_state_soft_latch) return;   // press already queued as an any key press
        if (!g_canvas_soft_latch) return;
        g_canvas_soft_latch = 0;
        if (g_screen_id == SCREEN_GAME) nav(MENU_PAUSE);
        else nav(MENU_MAIN);
        return;
    }
    if (key == J2M_SOFT_BACK) {
        // Java -8: b.c() has no -8 branch -> ignored (structure kept)
        return;
    }
    {
        int idx = -1;
        if (key == 'r' || key == 'R') idx = MK_KEY_STAR;     // pause / restart (phone '*')
        else if (key == 'e' || key == 'E') idx = MK_KEY_0;   // smash blocks (phone '0')
        else if (key == 'f' || key == 'F') idx = MK_KEY_5;   // push blocks (phone '5')
        else if (key == 35) idx = MK_KEY_POUND;          // '#'
        else if (key == '*') idx = MK_KEY_STAR;          // phone '*' (b.java: 42 -> an[11]; main.c '/' binding)
        else if (key == '0') idx = MK_KEY_0;             // phone '0'  (b.java: 48 -> an[0]; raw digit row)
        else if (key >= 49 && key <= 57) idx = key - 48;   // '1'..'9'
        else if (key == J2M_FIRE) idx = MK_KEY_5;            // FIRE == '5' (b.c(): 53 || -6)
        else {
            // getGameAction mapping (b.c(): UP->2, LEFT->4, RIGHT->6,
            // DOWN->8, FIRE->5; game action codes -3/-4/-5/-7/-6)
            switch (key) {
            case J2M_UP: idx = MK_KEY_2; break;            // UP    -> '2'
            case J2M_LEFT: idx = MK_KEY_4; break;          // LEFT  -> '4'
            case J2M_RIGHT: idx = MK_KEY_6; break;         // RIGHT -> '6'
            case J2M_DOWN: idx = MK_KEY_8; break;          // DOWN  -> '8'
            default: break;
            }
        }
        if (mk_env_on("MK_SCR")) { fprintf(stderr, "KEY %d d=%d idx=%d\n", key, down, idx); fflush(stderr); }
        if (idx != -1) {
            if (down) { mk_an[idx] = 1; g_keys_released[idx] = 0; }
            else g_keys_released[idx] = 1;             // d[i] = !pressed
        }
    }
}

// ---------------------------------------------------------------------------
// Paint (b.paint) + tick (a.java TimerTask -> KKM.m() @100 ms)
// ---------------------------------------------------------------------------
static int g_paints_menu, g_paints_dungeon, g_paints_other;
int game_paint_count(int i) { return i == 0 ? g_paints_menu : i == 1 ? g_paints_dungeon : g_paints_other; }
void game_paint(uint8_t *fb)
{
    if (g_ui_id != 0) { menu_paint(fb); g_paints_menu++; return; }
    g_fb_surf.w = SCR_W; g_fb_surf.h = SCR_H; g_fb_surf.has_alpha = 0;
    g_fb_surf.px = fb;
    tgt_set(&g_fb_surf);
    gcol = 0;
    if (g_ak_active && (g_sub_state == SUB_PLAY || g_sub_state == SUB_LEVEL_CLEAR)) {
        r_frame(fb);
        dungeon_hud();
        g_paints_dungeon++;
    } else g_paints_other++;
    paint_canvas_overlay();
}

void game_tick25(void)
{
    // Java: Timer.schedule(task, 0, 100) + 25 ms sleep -> whole KKM.m() body
    // runs once per 100 ms; on a 25 ms platform tick that is every 4th call.
    if (++g_tickdiv >= 4) {
        g_tickdiv = 0;
        tick_body();
    }
}

// ---------------------------------------------------------------------------
// Lifecycle: KKM.startApp -> h() + init()
// ---------------------------------------------------------------------------
// MK_DIAG=1 showroom level (debug): every block shape and every collectible
// visible from one spawn, entered straight into gameplay (no menu).
// Grid 16x8x16, cell = x + z*16 + h*256. cell bits: shape 0..7, items
// 8 note / 16 key / 24 warp / 32 life (shipped levels store bare values;
// the 0xC0 support pair only marks blocks tracked in moving_rec).
static void mk_diag_level(void)
{
    memset(mk_cl, 0, sizeof mk_cl);
    memset(mk_ck, 0, sizeof mk_ck);
    for (int x = 0; x < MK_GRID_W; x++)            // solid floor, layer 0
        for (int z = 0; z < MK_GRID_D; z++)
            mk_cl[x + z * MK_CELL_STEP_Z] = 1;
    // shape showroom row at z=10: block(x,1,10) shape s, item rides on top
    static const int shape_of[7] = { 1, 2, 3, 4, 5, 6, 7 };
    static const int item_of[7]  = { 8, 16, 24, 32, 8, 16, 32 };
    for (int i = 0; i < 7; i++) {
        int x = 2 + i;
        mk_cl[x + 10 * MK_CELL_STEP_Z + MK_CELL_STEP_H] = (int8_t)shape_of[i];
        mk_cl[x + 10 * MK_CELL_STEP_Z + 2 * MK_CELL_STEP_H] = (int8_t)item_of[i];
    }
    mk_cl[12 + 10 * MK_CELL_STEP_Z + MK_CELL_STEP_H] = 1;
    mk_cl[12 + 10 * MK_CELL_STEP_Z + 2 * MK_CELL_STEP_H] = 1;
    mk_cl[12 + 10 * MK_CELL_STEP_Z + 3 * MK_CELL_STEP_H] = 32; // life on 2-stack
    mk_cl[13 + 10 * MK_CELL_STEP_Z + 2 * MK_CELL_STEP_H] = 8;  // note floating
    g_dungeon.pos_x = 8 * MK_ONE;
    g_dungeon.pos_h = 1 * MK_ONE;
    g_dungeon.pos_z = 5 * MK_ONE;
    g_dungeon.facing = 2 * MK_ONE;           // 2 quarter-turns: the row is at
                                             // z=10 and facing 0 looks toward -z
    g_dungeon.spawn_dat_x = g_dungeon.spawn_cur_x = 8;
    g_dungeon.spawn_dat_h = g_dungeon.spawn_cur_h = 1;
    g_dungeon.spawn_dat_z = g_dungeon.spawn_cur_z = 5;
    g_dungeon.level_notes = 0;
    g_dungeon.notes_left = 0;
    g_dungeon.status = 0;
    // The block/billboard records (mk_ck, r_add_*) are built once at level
    // decode (d.java:1300-1330); the showroom replaced cl afterwards, so
    // replay that record pass here.
    int nb = 0, ni = 0;
    for (int i = 0; i < (int)sizeof mk_cl; i++) {
        int v = mk_cl[i];
        if ((v & MK_CELL_ITEM_MASK) == MK_CELL_SPAWN_MARK) continue;
        if (v & MK_CELL_SHAPE_MASK) nb++;
        else if (v & MK_CELL_ITEM_MASK) ni++;
    }
    r_reset(nb, ni);
    g_dungeon.moving_n = 0;
    int id = 0;
    for (int y = 0; y < MK_GRID_H; y++)
        for (int z = 0; z < MK_GRID_D; z++)
            for (int x = 0; x < MK_GRID_W; x++, id++) {
                int v = mk_cl[id];
                if ((v & MK_CELL_ITEM_MASK) == MK_CELL_SPAWN_MARK) continue;
                if (v & MK_CELL_SHAPE_MASK)
                    mk_ck[id] = (int16_t)(1 + r_add_block(x << MK_FRACT_BITS,
                        y << MK_FRACT_BITS, z << MK_FRACT_BITS, v & MK_CELL_SHAPE_MASK));
                else if (v & MK_CELL_ITEM_MASK)
                    mk_ck[id] = (int16_t)(1 + r_add_billboard(x << MK_FRACT_BITS,
                        y << MK_FRACT_BITS, z << MK_FRACT_BITS,
                        v & MK_CELL_ITEM_MASK) | MK_CK_BILLBOARD);
                if ((v & MK_CELL_SUPPORT_MASK) == MK_CELL_SUPPORT_MASK &&
                    g_dungeon.moving_n < 64) {
                    int ro = g_dungeon.moving_n << 3;
                    g_dungeon.moving_rec[ro] = mk_ck[id];
                    g_dungeon.moving_rec[ro + 4] = 1;
                    g_dungeon.moving_rec[ro + 5] = x;
                    g_dungeon.moving_rec[ro + 6] = y;
                    g_dungeon.moving_rec[ro + 7] = z;
                    g_dungeon.moving_n++;
                }
            }
}

// ---------------------------------------------------------------------------
void game_init(void)
{
    int i;
    opt_load();                            // h()
    if (mk_env_on("MK_LIGHTS")) g_lights_on = 1;   // debug knob (prototype)
    g_strings = MK_STR[0];                      // init(): locale default English
    mk_load_tables();                                // d.ab(this)
    r_init();                              // c.ab(this)
    surf_own(&g_fb_surf);
    for (i = 0; i < 4; i++) surf_own(&g_cards[i]);
    surf_own(&g_scratch); surf_own(&s_splash); surf_own(&s_atlas);
    surf_own(&s_over); surf_own(&s_bl);
    load_strip(927, &s_bl);
    load_strip(2116, &s_atlas);
    load_strip(2632, &s_over);
    load_strip(3284, &s_splash);
    for (i = 0; i < 4; i++) { g_cards[i].w = 0; g_cards[i].h = 0; }
    for (i = 0; i < MK_NKEYS; i++) { mk_an[i] = 0; g_keys_released[i] = 0; }
    g_ui_id = 0;
    g_destroyed = 0;
    g_paused = 0;
    g_demo_scripted = 0;
    g_intro_page = 0;
    g_bonus_key = g_bonus_last_lvl = g_bonus_keynote = g_bonus_par = 0;
    g_tickdiv = 0;
    g_on_canvas = 1;
    set_screen(SCREEN_SPLASH);                // this.p((byte)0)
    hiscore_load();                        // "MKK" read after p(0), as Java
    if (mk_env_on("MK_DIAG")) {            // jump straight into the showroom
        g_screen_t = 0;
        set_screen(SCREEN_GAME);          // runs mk_init_run (loads L1)
        mk_diag_level();                  // overwrite with the showroom map
        set_substate(SUB_PLAY);
    }
}

// ---------------------------------------------------------------------------
// Shell state introspection for headless tests (tests/logic_test.c).
// ---------------------------------------------------------------------------
int game_dbg(int what)
{
    switch (what) {
    case 0: return g_sub_state;
    case 1: return g_ui_id;
    case 2: return g_screen_id;
    case 3: return g_menu.type;
    case 4: return g_menu.sel;
    case 5: return g_screen_t;
    case 6: return g_save_exists;
    case 7: return g_ak_active;
    case 8: return g_on_canvas;
    case 9: return g_scene.shake_on;
    case 10: return g_dungeon.freelook;
    case 11: return g_scene.monkey_sprite;
    case 12: return g_dungeon.anim_frame;
    case 13: return g_menu.nitems;
    case 14: return g_destroyed;
    case 15: return g_paused;
    case 16: return g_menu.cur_group;
    default: return 0;
    }
}

int game_lights(void)                     // shell: LCD backlight emulation tint
{
    return g_lights_on;
}

int game_dither(void)                    // display dither mode (0 off, 1 device halftone)
{
    return g_dither_mode;
}

void game_set_dither(int mode)           // shell debug key F8 toggles 0<->1
{
    g_dither_mode = (mode == 1) ? 1 : 0;
}

// 5x7 glyph rows (bit7=left); exposed for the font regression in logic_test.
const unsigned char *mk_font_glyph(int cp)
{
    return glyph_for(cp);
}

int mk_font_adv(int cp)
{
    return g_adv[glyph_index(cp)];
}

