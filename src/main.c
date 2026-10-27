#ifdef __EMSCRIPTEN__
#include <emscripten.h>      /* browser must own the loop (raylib web wiki 4.1) */
#include <unistd.h>          /* chdir into the IDBFS-backed save dir */
#endif
#include <stdio.h>
// main.c - raylib shell for the Munkiki's Castle remake.
// Owns nothing but the window, the 25 ms clock, keyboard -> phone-key codes
// and sound blips. All behavior lives in game.c (KKM.java port), engine.c
// (d.java port), render.c + m3d.c (c.java + M3D port).
// Original device: Nokia 3410, 96x65 monochrome LCD, 100 ms UI loop.
#include "raylib.h"
#include "game.h"
#include "sound.h"
#include "mk_env.h"
#include <stdlib.h>
#include <string.h>

#define LCDW 96
#define LCDH 65
#define SCALE 6
#define TICK_MS 25.0   // game_tick25 cadence (a.java TimerTask; KKM.m every 4th)
#define SND_RATE 16000   // OTA tone render rate (Hz): max partial well under Nyquist
#define MAX_CATCHUP_MS 500.0  // max clock debt replayed after the window was paused

static unsigned char fb[LCDW * LCDH];
static Image lcdImage;
static Texture2D lcdTex;

// ------------------------------------------------------------------ sounds
// platform_sound(id): the originals are Smart Messaging OTA/OTT tone
// programs, the 12 records at the head of MK_D (loaded by KKM from resource
// /d, played via com.nokia.mid.sound FORMAT_TONE). src/sound.c decodes that
// grammar and renders the square-wave tone streams here.
#define NSND 12
static Wave sndWaves[NSND];
static Sound snds[NSND];

static void make_sounds(void)
{
    for (int id = 0; id < NSND; id++) {
        short *samples = NULL;
        int n = mk_sound_render(id, SND_RATE, &samples);
        if (!n) { // never happens for the shipped records; keep silence
            samples = (short *)calloc(1, sizeof(short));
            n = 1;
        }
        sndWaves[id].frameCount = n;
        sndWaves[id].sampleRate = SND_RATE;
        sndWaves[id].sampleSize = 16;
        sndWaves[id].channels = 1;
        sndWaves[id].data = samples;
        snds[id] = LoadSoundFromWave(sndWaves[id]);
        free(samples);              // LoadSoundFromWave copies; sound.h: caller frees
        sndWaves[id].data = NULL;
    }
}

void platform_sound(int id)
{
    if (id >= 0 && id < NSND) PlaySound(snds[id]);
}

// -------------------------------------------------- virtual touch keypad
// Phones have no keyboard: when the window leaves a black band below the
// LCD (portrait), it becomes a NOKIA-style pad: the 1-9/*0# keypad is the
// big centerpiece (the game is played with its digits), with a one-line
// dpad (< ^ v >) and the two soft keys squeezed above it.
// Touch points map onto the buttons and drive the very same
// game_key() press/release edges as the keyboard, so multi-touch
// (walk + action) works. MK_TOUCH=1 forces the pad on any window.
typedef struct { Rectangle r; int code; char lbl[8]; } VKey;
static VKey g_vk[24];
static int g_nvk;
static Rectangle g_lcd;   // LCD rect on screen (drawing + touch mapping)

static void add_vkey(int *n, float x, float y, float w, float h, int code, const char *lbl)
{
    g_vk[*n].r = (Rectangle){ x, y, w, h };
    g_vk[*n].code = code;
    strncpy(g_vk[*n].lbl, lbl, sizeof g_vk[0].lbl - 1);
    g_vk[*n].lbl[sizeof g_vk[0].lbl - 1] = 0;
    (*n)++;
}

static int build_pad(void)    // buttons below g_lcd; 0 = no room
{
    float rx = (float)GetRenderWidth(), ry = (float)GetRenderHeight();
    float padtop = g_lcd.y + g_lcd.height + 4, padh = ry - padtop;
    // Keypad (4 rows of c) is the big centerpiece; above it a one-line dpad
    // (4 x c) with SEL/BACK squeezed to the sides.
    float c = (padh - 8) / 4.9f, maxc = (rx - 8) / 5.6f;
    if (c > maxc) c = maxc;
    if (c < 24) return 0;
    int n = 0;
    float th = c * 0.9f, ax = rx*0.5f - 2*c;        // dpad strip spans 4c centered
    float sw = ax - 8;                              // soft keys fill the sides
    add_vkey(&n, 4, padtop, sw, th, J2M_SOFT_SELECT, "SEL");
    add_vkey(&n, ax,       padtop, c, th, J2M_LEFT,  "<");
    add_vkey(&n, ax + c,   padtop, c, th, J2M_UP,    "^");
    add_vkey(&n, ax + 2*c, padtop, c, th, J2M_DOWN,  "v");
    add_vkey(&n, ax + 3*c, padtop, c, th, J2M_RIGHT, ">");
    add_vkey(&n, rx - 4 - sw, padtop, sw, th, J2M_SOFT_BACK, "BACK");
    float kx = rx*0.5f - 1.5f*c, ky = padtop + th + 4;
    for (int i = 0; i < 12; i++) {
        static const char keys[] = "123456789*0#";
        // game_key() binds smash/push/pause to the PC-remap codes, not the
        // raw phone digits (game.c:1605): send 'e','f','r' under those caps.
        int code = keys[i] == '0' ? 'e' : keys[i] == '5' ? 'f' :
                   keys[i] == '*' ? 'r' : keys[i];
        char lbl[2] = { keys[i], 0 };
        add_vkey(&n, kx + (i % 3)*c, ky + (i / 3)*c, c, c, code, lbl);
    }
    return n;
}

// Anchor the LCD (vertically centered, or top-anchored when a pad follows)
// and rebuild the pad; called once per frame before drawing.
static void layout(void)
{
    float rx = (float)GetRenderWidth(), ry = (float)GetRenderHeight();
    float s = rx / LCDW < ry / LCDH ? rx / LCDW : ry / LCDH;
    float offy = (ry - LCDH*s) * 0.5f;
    if (ry - LCDH*s > 120.0f || mk_env_on("MK_TOUCH"))
        offy = ry * 0.05f + 8.0f;   // keep the LCD clear of the cutout / rounded corners
    g_lcd = (Rectangle){ (rx - LCDW*s) * 0.5f, offy, LCDW*s, LCDH*s };
    g_nvk = build_pad();
}

static void poll_touch(void)
{
    static unsigned prev;
    if (!g_nvk) { prev = 0; return; }
    unsigned now = 0;
    int nt = GetTouchPointCount();
    for (int i = 0; i < nt; i++) {   // raylib caps touch points internally
        Vector2 p = GetTouchPosition(i);
        for (int k = 0; k < g_nvk; k++) {
            Rectangle r = g_vk[k].r;
            if (p.x >= r.x && p.x < r.x + r.width && p.y >= r.y && p.y < r.y + r.height)
                now |= 1u << k;
        }
    }
    for (int k = 0; k < g_nvk; k++) {
        unsigned bit = 1u << k;
        if ((now & bit) != (prev & bit)) game_key(g_vk[k].code, (now & bit) ? 1 : 0);
    }
    prev = now;
}

static void draw_pad(void)
{
    Color bg = { 36, 36, 36, 216 }, fg = { 224, 224, 224, 255 };
    for (int k = 0; k < g_nvk; k++) {
        Rectangle r = g_vk[k].r;
        DrawRectangleRec(r, bg);
        DrawRectangleLinesEx(r, 1, fg);
        int fs = (int)(r.height * 0.45f);
        DrawText(g_vk[k].lbl,
                 (int)(r.x + (r.width - MeasureText(g_vk[k].lbl, fs)) * 0.5f),
                 (int)(r.y + (r.height - fs) * 0.5f), fs, fg);
    }
}

// ------------------------------------------------------------------- input
// Phone key codes per game.h: '0'..'9', '*', '#' plus J2M_SOFT_SELECT/BACK.
static void dump_live_fb(void);
// MK_AUTONAV: scripted shell-driven key schedule (verification aid, no OS input).
static void autonav(double t)
{
    static double t0 = -1;
    static int step = 0;
    static const struct { double at; int key; } seq[] = {
        { 6.0, J2M_SOFT_SELECT },   // splash -> main menu
        { 8.0, J2M_SOFT_SELECT },   // "New Game" -> level hint
        { 13.0, 0 },                // dump the live LCD frame (map, slide done)
    };
    if (t0 < 0) t0 = t;
    while (step < (int)(sizeof seq / sizeof seq[0]) && t - t0 >= seq[step].at) {
        if (seq[step].key == 0) dump_live_fb();
        else { game_key(seq[step].key, 1); game_key(seq[step].key, 0); }
        fprintf(stderr, "AUTONAV step %d\n", step); fflush(stderr);
        step++;
    }
}
static void poll_input(void)
{
    // Arrows/Fire arrive as game-action special codes (device dpad);
    // WASD + digit row keep phone-digit semantics (b.c() maps them to
    // an[] slots directly; menus only see the special codes).
    static const int key_map_vk[] = { KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_SPACE,
                              KEY_W, KEY_S, KEY_A, KEY_D };
    static const int key_map_code[] = { J2M_UP, J2M_DOWN, J2M_LEFT, J2M_RIGHT, J2M_FIRE, '2', '8', '4', '6' };
    for (unsigned k = 0; k < sizeof(key_map_vk) / sizeof(key_map_vk[0]); k++) {
        if (IsKeyPressed(key_map_vk[k]))  game_key(key_map_code[k], 1);
        if (IsKeyReleased(key_map_vk[k])) game_key(key_map_code[k], 0);
    }
    for (char c = '0'; c <= '9'; c++) {
        if (IsKeyPressed(c))  game_key(c, 1);
        if (IsKeyReleased(c)) game_key(c, 0);
    }
    if (IsKeyPressed(KEY_SLASH))  game_key('*', 1);
    if (IsKeyReleased(KEY_SLASH)) game_key('*', 0);
    // PC remap of the phone keys (64edc1d): 'e' = smash (phone '0'),
    // 'r' = pause/restart level (phone '*'), 'f' = push blocks (phone '5').
    if (IsKeyPressed(KEY_E))  game_key('e', 1);
    if (IsKeyReleased(KEY_E)) game_key('e', 0);
    if (IsKeyPressed(KEY_R))  game_key('r', 1);
    if (IsKeyReleased(KEY_R)) game_key('r', 0);
    if (IsKeyPressed(KEY_F))  game_key('f', 1);
    if (IsKeyReleased(KEY_F)) game_key('f', 0);
    // Enter = left soft key only (sending '#' too made one press run two
    // actions). The phone '#' key lives on NumpadEnter.
    if (IsKeyPressed(KEY_ENTER))      game_key(J2M_SOFT_SELECT, 1);
    if (IsKeyReleased(KEY_ENTER))     game_key(J2M_SOFT_SELECT, 0);
    if (IsKeyPressed(KEY_MINUS))  game_key('#', 1);
    if (IsKeyReleased(KEY_MINUS)) game_key('#', 0);
    // backspace = right soft key (back)
    if (IsKeyPressed(KEY_BACKSPACE))  game_key(J2M_SOFT_BACK, 1);
    if (IsKeyReleased(KEY_BACKSPACE)) game_key(J2M_SOFT_BACK, 0);
    if (IsKeyPressed(KEY_F8)) {
        game_set_dither((game_dither() + 1) % 2);   // debug toggle, mirrors the menu option
        fprintf(stderr, "DITHER=%d\n", game_dither()); fflush(stderr);
    }
    poll_touch();
}

static void fput32(FILE *f, unsigned v) { fputc(v & 255, f); fputc(v >> 8 & 255, f); fputc(v >> 16 & 255, f); fputc(v >> 24 & 255, f); }
static void fput16(FILE *f, unsigned v) { fputc(v & 255, f); fputc(v >> 8 & 255, f); }
// 96x65 grayscale BMP of the live frame buffer (verification tool).
static void dump_live_fb(void)
{
    FILE *f = fopen("mk_live.bmp", "wb");
    if (!f) return;
    int row = ((LCDW + 3) & ~3), sz = 54 + 256 * 4 + row * LCDH;
    fwrite("BM", 1, 2, f);
    fput32(f, sz); fput32(f, 0); fput32(f, 54 + 256 * 4);
    fput32(f, 40); fput32(f, LCDW); fput32(f, LCDH);
    fput16(f, 1); fput16(f, 8); fput32(f, 0); fput32(f, 0);
    fput32(f, 0); fput32(f, 0);                    /* px/m, py/m */
    fput32(f, 0); fput32(f, 0);                    /* clrused, important */
    for (int i = 0; i < 256; i++) { /* gray palette */
        fputc(i, f); fputc(i, f); fputc(i, f); fputc(0, f);
    }
    static unsigned char pad[4];
    for (int y = LCDH - 1; y >= 0; y--) {
        fwrite(fb + y * LCDW, 1, LCDW, f);
        fwrite(pad, 1, row - LCDW, f);
    }
    fclose(f);
    fprintf(stderr, "DUMP mk_live.bmp\n"); fflush(stderr);
}

/* Free raylib resources when the desktop loop ends. On the web there is no
   way to close the "window" (the tab is the app), so this stays desktop-only:
   the web port's WindowShouldClose() even calls emscripten_sleep(). */
#ifndef __EMSCRIPTEN__
static void app_shutdown(void)
{
    UnloadTexture(lcdTex);
    for (int i = 0; i < NSND; i++) UnloadSound(snds[i]);
    CloseAudioDevice();
    CloseWindow();
}
#endif

/* One Update+Draw frame. On the web this runs as an emscripten main-loop
   callback (raylib web wiki 4.1: the browser must own the loop); the
   desktop build keeps the traditional while(!WindowShouldClose()) around it. */
static void frame(void)
{
    static double acc = 0.0, last_time = 0.0;
    double now = GetTime();
    if (last_time == 0.0) last_time = now;     /* first frame: no time jump */
    acc += (now - last_time) * 1000.0;
    last_time = now;
    if (acc > MAX_CATCHUP_MS) acc = MAX_CATCHUP_MS;  // don't spiral after pause
    poll_input();                              // once per frame: raylib
    if (mk_env_on("MK_AUTONAV")) autonav(now);
    if (mk_env_on("MK_LIVEDUMP")) {               // one-shot LCD dump (~6 s in)
        static int ldf = 0;
        if (++ldf == 150) dump_live_fb();
    }
    while (acc >= TICK_MS) {                   // edge events are per-frame
        game_tick25();
        if (mk_env_on("MK_SCR")) {
            static int prev_ab = -999, prev_ai = -999, prev_aj = -999, prev_mt = -999, prev_br = -999, prev_vel = -999, prev_daj = -999;
            int st_ab = game_dbg(0), st_ai = game_dbg(1), st_aj = game_dbg(2);
            int st_mt = game_dbg(3), st_br = game_dbg(9);
            int st_vel = game_dbg(14) * 2 + game_dbg(15);
            int st_daj = game_dbg(10) * 64 + game_dbg(12);
            if (st_ab != prev_ab || st_ai != prev_ai || st_aj != prev_aj || st_mt != prev_mt || st_br != prev_br || st_vel != prev_vel || st_daj != prev_daj) {
                fprintf(stderr, "ST ab=%d ai=%d aj=%d mt=%d br=%d v=%d Daj=%d\n", st_ab, st_ai, st_aj, st_mt, st_br, st_vel, game_dbg(10));
                fflush(stderr);
                prev_ab = st_ab; prev_ai = st_ai; prev_aj = st_aj; prev_mt = st_mt; prev_br = st_br; prev_vel = st_vel; prev_daj = st_daj;
            }
        }
        acc -= TICK_MS;
    }

    game_paint(fb);                            // full 96x65 gray frame

    // The original drew everything as 1bpp: 2D art carries baked dither
    // dots and the M3D flat grays were halftoned by the LCD itself. The
    // device pattern is an ordered dither with a nested 4x4 cluster rank
    // matrix (measured from real-device footage: autocorrelation of the
    // ground field gives lattice vectors (4,0)+(2,2) LCD cells = exactly
    // rank cells (0,0)+(2,2) lit for ground gray 224; 192 smashables then
    // fill the (even,even) sublattice "every other pixel, empty rows" and
    // faces 128/140 reach the (even,even)+(odd,odd) checkerboard -- all
    // three observed halftones are one threshold sweep of this matrix):
    static uint8_t fb_bw[LCDW * LCDH];
    int dither_mode = game_dither();   // 0 smooth gray, 1 device halftone
    if (dither_mode == 1) {
        static const uint8_t rank[16] = {   // fill order at (y&3)*4+(x&3)
             0,  8,  3, 12,
            10,  4, 13,  7,
             2, 14,  1,  9,
            15,  6, 11,  5,
        };
        for (int i = 0; i < LCDW * LCDH; i++) {
            int x = i % LCDW, y = i / LCDW;
            int t = ((255 - fb[i]) * 16 + 127) / 255;   // black cells per cluster
            fb_bw[i] = (rank[(y & 3) * 4 + (x & 3)] < t) ? 0 : 255;
        }
    }

    BeginDrawing();
    ClearBackground(BLACK);
    UpdateTexture(lcdTex, dither_mode ? fb_bw : fb);
    // HiDPI desktops: the GL framebuffer is larger than the requested
    // 288x195 window; scale the LCD to fill it exactly (centered; anchored
    // to the top when the virtual touch pad needs the space below).
    layout();
    /* PROTOTYPE: "Lights On" imitates a green LCD backlight (KKM.j
       DeviceControl.setLights) by tinting the white pixels green;
       black pixels are unaffected (texture * tint). */
    Color tint = game_lights() ? (Color){ 0x9b, 0xbc, 0x0f, 255 } : WHITE;
    DrawTexturePro(lcdTex, (Rectangle){ 0, 0, LCDW, LCDH }, g_lcd,
                   (Vector2){ 0, 0 }, 0.0f, tint);
    draw_pad();
    EndDrawing();
}

int main(void)
{
#ifdef __ANDROID__
    InitWindow(0, 0, "Munkiki's Castle");  // 0 = native surface size (rcore_android)
#else
    InitWindow(LCDW * SCALE, LCDH * SCALE, "Munkiki's Castle");
#endif
    InitAudioDevice();
    make_sounds();

    SetTargetFPS(60);
    lcdImage = (Image){ .data = fb, .width = LCDW, .height = LCDH,
                        .mipmaps = 1, .format = PIXELFORMAT_UNCOMPRESSED_GRAYSCALE };
    lcdTex = LoadTextureFromImage(lcdImage);

#ifdef __EMSCRIPTEN__
    /* web/shell.html mounts IndexedDB at /persist and waits for the load
       sync; save files (OPT.rms, KKM.rms, ...) are relative paths, so the
       cwd decides where they go. */
    if (chdir("/persist") != 0) fprintf(stderr, "chdir /persist failed\n");
#endif

    game_init();


#if defined(__EMSCRIPTEN__)
    emscripten_set_main_loop(frame, 0, 1);  /* rAF-paced; never returns */
#else
    while (!WindowShouldClose()) frame();
    app_shutdown();
#endif
    return 0;
}
