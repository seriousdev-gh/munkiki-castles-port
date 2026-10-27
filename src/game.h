// game.h - contract of the game shell: faithful port of KKM.java + b.java
// (screens, menus, HUD overlays, scoring, records, languages, sound triggers).
// The shell owns the state machine; it drives the engine (mk_tick100/mk_load_level/...) and
// the renderer (r_frame), and produces the final 96x65 gray framebuffer.
#ifndef MK_GAME_H
#define MK_GAME_H

#include <stdint.h>

// Platform -> shell input. `key` is the J2ME phone key code:
// '0'..'9' (48..57), '*' (42), '#' (35); plus soft keys and game actions
// below. Canvas routing maps UP/LEFT/RIGHT/DOWN/FIRE to digits 2/4/6/8/5
// inside game_key (getGameAction fallback in b.c()); menu routing uses the
// arrows directly (J2ME List nav).
enum {
    J2M_SOFT_SELECT = -1,  // left soft key
    J2M_SOFT_BACK   = -2,  // right soft key
    J2M_UP          = -3,
    J2M_LEFT        = -4,
    J2M_RIGHT       = -5,
    J2M_FIRE        = -6,
    J2M_DOWN        = -7,
};
void game_key(int key, int down);

// Lifecycle
void game_init(void);                 // startApp + first paint assets
void game_tick25(void);               // a.java TimerTask -> KKM.m(), every 25 ms
void game_paint(uint8_t *fb);         // paint: 96x65 gray buffer (0=black 255=white)

// Shell -> platform sound trigger: play OTA/OTT tone effect id (0..11),
// the 12 records at the head of MK_D
void platform_sound(int id);

// Shell state introspection for headless tests (tests/logic_test.c):
// 0=sub-state 1=ui/menu id 2=screen id 3=menu type 4=menu sel 5=timer 6=save-exists
int game_dbg(int what);

// "Lights" option state (KKM.j) -> shell tints the LCD green (backlight proto).
int game_lights(void);

// Display dithering (port-only option in the Options menu; F8 debug toggles).
int game_dither(void);                  // 0 off, 1 device halftone (measured 4x4 matrix)
void game_set_dither(int mode);

// 5x7 glyph rows (bit7=left) + proportional advance, for logic_test.
const unsigned char *mk_font_glyph(int cp);
int mk_font_adv(int cp);

#endif // MK_GAME_H
