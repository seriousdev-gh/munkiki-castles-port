#ifndef MUNKI_SOUND_H
#define MUNKI_SOUND_H

// Renders effect sound id (0..11 = the 12 tone records at the head of MK_D)
// into a freshly allocated 16-bit mono PCM buffer at `rate` Hz; returns the
// sample count, 0 on failure. Caller frees.
int mk_sound_render(int id, int rate, short **out);

#endif
