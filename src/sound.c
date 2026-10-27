// Nokia FORMAT_TONE playback: the 12 records shipped at the head of MK_D are
// Smart Messaging OTA/OTT tone programs (bit-level grammar). KKM loads them
// from resource /d into Sound[12] (KKM.java:1018-1026) and plays rec[id] per
// effect id (KKM.java:128). Grammar + tables reimplemented from the tone
// format references (NokiaOTTDecoder): note = semitone above 523 Hz (C..B)
// times a scale factor x0.5/1/2/4, tempo from the OTA BPM table (24 PPQ),
// gate by style (natural / x1.1 continuous / x0.7 staccato), volume from the
// OTA level table. The device tone channel was a square wave, so is ours,
// but post-filtered to imitate the small speaker's treble rolloff (softer,
// per phone video recordings). Knobs: MK_SND_GAIN (0.6), MK_SND_SOFT (Hz).
#include <math.h>
#include <stdlib.h>

#include "sound.h"
#include "assets.h"

#define PPQ   24
#define MAXEV 1024

enum { EV_NOTE, EV_TEMPO };

typedef struct {
    unsigned char kind;
    unsigned tick;          // position in PPQ ticks
    // EV_NOTE
    unsigned char note;     // 0 = pause, 1..12 = C..B
    unsigned dur;           // ticks
    unsigned char scale;    // 0..3 -> x0.5 x1 x2 x4
    unsigned char style;    // 0 natural, 1 continuous, 2 staccato
    unsigned char vol;      // OTA level in effect at note-on
    unsigned bpm;           // EV_TEMPO
    // render pass, milliseconds x4 fixed point
    unsigned on4, off4;
} Ev;

typedef struct { const unsigned char *b; int nbits, pos, eof; unsigned tick; } Bits;

static int rb(Bits *p, int k)
{
    int v = 0;
    while (k--) {
        if (p->pos >= p->nbits) { p->eof = 1; return 0; }
        v = (v << 1) | ((p->b[p->pos >> 3] >> (7 - (p->pos & 7))) & 1);
        p->pos++;
    }
    return v;
}

static const unsigned short BPM_TAB[32] = { 25, 28, 31, 35, 40, 45, 50, 56,
    63, 70, 80, 90, 100, 112, 125, 140, 160, 180, 200, 225, 250, 285, 320,
    355, 400, 450, 500, 565, 635, 715, 800, 900 };

static const unsigned char VOL_TAB[16] = { 0, 48, 56, 64, 72, 80, 88, 92,
    100, 104, 108, 112, 116, 120, 124, 127 };

// reference pitch table (rounded Hz), C..B
static const double FREQ[13] = { 0, 523, 554, 587, 622, 659, 698, 740,
    784, 831, 880, 932, 988 };

static unsigned dur_ticks(int nd, int ds)
{
    static const unsigned base[8] = { 96, 48, 24, 12, 6, 3, 24, 24 };
    unsigned t = base[nd & 7];
    if (ds == 1) t += t / 2;              // dotted
    else if (ds == 2) t += t / 2 + t / 4; // double dotted
    else if (ds == 3) t = t * 2 / 3;      // triplet feel
    return t;
}

static void instr(Bits *p, Ev *ev, int *nev, int *scale, int *style, int *vol)
{
    int it = rb(p, 3);
    if (p->eof) return;
    switch (it) {
    case 0:                                   // start of next pattern header
        p->pos -= 3;
        break;
    case 1: {                                 // note
        if (*nev >= MAXEV) break;
        int nv = rb(p, 4), nd = rb(p, 3), ds = rb(p, 2);
        if (nv > 12) nv = 0;                 // reserved values = pause
        Ev *e = &ev[(*nev)++];
        e->kind = EV_NOTE;
        e->tick = p->tick;
        e->note = (unsigned char)nv;
        e->dur = dur_ticks(nd, ds);
        e->scale = (unsigned char)*scale;
        e->style = (unsigned char)*style;
        e->vol = (unsigned char)*vol;
        p->tick += e->dur;
        break;
    }
    case 2: *scale = rb(p, 2); break;
    case 3: *style = rb(p, 2); break;
    case 4:
        if (*nev < MAXEV) {
            Ev *e = &ev[(*nev)++];
            e->kind = EV_TEMPO;
            e->tick = p->tick;
            e->bpm = BPM_TAB[rb(p, 5)];
        } else rb(p, 5);
        break;
    case 5: *vol = rb(p, 4); break;   // channel volume, note-on captures it
    default: break;                           // 6,7: consumed, ignored
    }
}

static void ottsound(Bits *p, Ev *ev, int *nev)
{
    int sg = rb(p, 3);
    if (sg == 1) {                            // basic song: skip title
        int tl = rb(p, 4);
        while (tl-- && !p->eof) rb(p, 8);
    } else if (sg != 2) {                     // 2 = temporary song
        return;                               // midi/digitized/poly: unused
    }
    int vol = 7;                            // CC7 channel state persists
    int npat = rb(p, 8);
    while (npat-- && !p->eof) {
        rb(p, 3); rb(p, 2);
        int loop = rb(p, 4);
        if (loop == 0xF) loop = 0;            // 0xF = endless marker -> once
        int mark = p->pos;
        for (;;) {
            p->pos = mark;
            int scale = 0, style = 0;         // reset per pattern run
            int n = rb(p, 8);
            while (n-- && !p->eof) instr(p, ev, nev, &scale, &style, &vol);
            if (loop <= 0) break;
            loop--;
        }
    }
}

int mk_sound_render(int id, int rate, short **out)
{
    if (id < 0 || id > 11) return 0;
    // locate record id: [len:LE16][body] chain at head of MK_D. The chain and
    // the length it yields are trusted nowhere else, so clamp them to the
    // array: a corrupted/re-extracted MK_D must not read past it.
    const unsigned char *r = MK_D;
    const unsigned char *dend = MK_D + MK_D_LEN;
    for (int i = 0; i < id; i++) {
        if (r + 2 > dend) return 0;
        int ln = r[0] | (r[1] << 8);
        if (ln == 0) return 0;
        r += 2 + ln;
    }
    if (r + 2 > dend) return 0;
    int ln = r[0] | (r[1] << 8);
    if (ln == 0 || ln > (int)(dend - (r + 2))) return 0;
    Bits p = { r + 2, ln * 8, 0, 0, 0 };

    Ev *ev = (Ev *)calloc(MAXEV, sizeof(Ev));
    if (!ev) return 0;
    int nev = 0;

    int cmdlen = rb(&p, 8);
    while (cmdlen-- && !p.eof) {
        int ct = rb(&p, 8);
        if (ct == 0x4a) {                     // ringing tone programming
            if (rb(&p, 7) == 0x1d) ottsound(&p, ev, &nev);
        } else if (ct == 0x3a) {              // sound
            ottsound(&p, ev, &nev);
        }
    }

    // ticks -> ms with the tempo map; a later note rekeys (overwrites) the
    // single tone channel, as on the device
    static const double SCALEF[4] = { 0.5, 1.0, 2.0, 4.0 };
    unsigned bpm = 120, cur_tk = 0;
    double cur_ms = 0.0, total_ms = 0.0;
    for (int i = 0; i < nev; i++) {
        Ev *e = &ev[i];
        if (e->tick > cur_tk) {
            cur_ms += (double)(e->tick - cur_tk) * 60000.0 / ((double)bpm * PPQ);
            cur_tk = e->tick;
        }
        if (e->kind == EV_TEMPO) { bpm = e->bpm; continue; }
        double on = cur_ms + (double)(e->tick - cur_tk) * 60000.0 / ((double)bpm * PPQ);
        double dur = (double)e->dur * 60000.0 / ((double)bpm * PPQ);
        if (e->style == 1) dur *= 1.1;
        else if (e->style == 2) dur *= 0.7;
        e->on4 = (unsigned)(on * 4.0 + 0.5);
        e->off4 = (unsigned)((on + dur) * 4.0 + 0.5);
        if (on + dur > total_ms) total_ms = on + dur;
    }

    // A malformed record can drive the tick totals to absurd lengths; cap the
    // render instead of overflowing the int casts below.
    if (!(total_ms >= 0.0) || total_ms > 600000.0) { free(ev); return 0; }
    int nsamp = (int)(total_ms * rate / 1000.0) + 2;
    short *s = (short *)calloc((size_t)nsamp, sizeof(short));
    if (!s) { free(ev); return 0; }
    double gain = 0.6;                        // master trim: device speaker is modest
    const char *gv = getenv("MK_SND_GAIN");
    if (gv) gain = atof(gv);
    for (int i = 0; i < nev; i++) {
        Ev *e = &ev[i];
        if (e->kind != EV_NOTE || e->note == 0) continue;
        int amp = (int)(9000.0 * gain * VOL_TAB[e->vol & 15] / 127);
        if (amp > 32000) amp = 32000;      // MK_SND_GAIN past unity must clip, not wrap
        else if (amp < -32000) amp = -32000;
        if (!amp) continue;
        double f = FREQ[e->note] * SCALEF[e->scale & 3];
        int a = (int)((double)e->on4 * rate / 4000.0);
        int b = (int)((double)e->off4 * rate / 4000.0);
        if (b > nsamp) b = nsamp;
        double ph = 0.0;
        for (int k = a; k < b; k++) {
            s[k] = (short)(ph < 0.5 ? amp : -amp);
            ph += f / (double)rate;
            if (ph >= 1.0) ph -= 1.0;
        }
    }
    // The device's tiny speaker rolls off the square wave's upper harmonics
    // (raw square sounds harsh on recordings). Two one-pole stages, fc
    // ~2.5 kHz: fundamentals survive, >=3rd harmonics and edge clicks drop.
    double fc = 2500.0;
    const char *fv = getenv("MK_SND_SOFT");
    if (fv) fc = atof(fv);
    if (fc > 0.0 && fc < (double)rate / 2.0) {
        double a = 1.0 - exp(-6.283185307179586 * fc / (double)rate);
        double y1 = 0.0, y2 = 0.0;
        for (int k = 0; k < nsamp; k++) {
            double x = (double)s[k];
            y1 += a * (x - y1);
            y2 += a * (y1 - y2);
            s[k] = (short)(y2 >= 0.0 ? y2 + 0.5 : y2 - 0.5);
        }
    }
    free(ev);
    *out = s;
    return nsamp;
}
