/* SPDX-License-Identifier: MIT */
/* digisophie-dn engine: Sophie metallic percussion (FUSE/BOOM/PIPE/SHARD) as a
 * Digitone mk1 machine (id 11 "SOPH"), on machines-dn1
 * (digitone-mk1/machines.h, core-dn1 3.1).
 *
 * The DSP core (../sophie.c/h, fixed-point, 32-sample blocks) is reused
 * untouched; this file is param plumbing + voice lifecycle only:
 *  - pitch: dnm_phase_inc(v->pitch + OCT*12<<16) >> 16, clamped 8..24576
 *    (host-proven: A4 -> ~440 Hz, +12 -> 2x).
 *  - gate: v->fresh triggers (ds retrigger crossfade), !v->gate feeds the
 *    core's fade/sleep via a synthesized quiet level (companion owns the
 *    real AMP envelope; flags stay 0).
 *  - out: core 16.16 >> 2 -> Q1.31 ~ DNM_PEAK (host-proven peak window).
 *  - fuse: monophonic newest-wins (VAWT precedent), releases free
 *    immediately, sophie_reap() on the UI tick clears unobserved ends.
 *  - LFO: both system LFOs added per SYN slot like vawt_add_lfo.
 *
 * Knob map (all borrowed stock ranges; compressed knobs noted, mint later
 * only if hardware UX demands):
 *   SYN1 A OCT  (slot 17, stock 0-7): octave -2..+2 (raw-2, clamp).
 *   SYN1 B MODEL(slot 18, stock 0-18): FUSE/BOOM/PIPE/SHARD (raw/5).
 *   SYN1 C FOLD (slot 19, stock 0-35): 0-100% (36 detents; SYN2 exact TBD).
 *   SYN1 D spare (unused).
 *   SYN1 E SWEEP(slot 21, stock 37-89): bipolar, compressed (raw-64).
 *   SYN1 F METAL(slot 22, 0-127): exact.  G FBK (slot 23): exact.
 *   SYN1 H COLOR(slot 24, 0-127 unipolar): exact.
 *   SYN2 A TUNE (slot 27, 0-127): fine tune about +-100 cents (raw 64 = 0);
 *   SYN2 B-H spare for DECAY/LPG tuning if needed.
 * Stock-range assumptions for slots 17/18/19/21/24 (from machines.h + VAWT
 * tables) are marked ASSUME and need hardware turn-test confirmation.
 */
#include "digitone-mk1/machines.h"
#include "../sophie.h"

/* LFO: both system LFOs add per SYN slot (sound slots of the used SYN
 * knobs: 17, 18, 19, 21, 22, 23, 24), clamped to each slot's stock range
 * below, like vawt_add_lfo. FILTER/AMP dests are applied downstream and
 * ignored here. */
/* Stock clamp ranges in 8.8, slot order 17/18/19/21/22/23/24 (ASSUME
 * 17/18/19/21 per machines.h + VAWT; 22/23/24 are stock 0-127 — slot 24
 * verified unipolar in the emulator: COLOR reached 49% floor = raw 0): */
static const int32_t sophie_lfo_min[8] = {
    0, 0, 0, 9472, 0, 0, 0, 0,
};
static const int32_t sophie_lfo_max[8] = {
    1792, 4608, 8960, 22784, 32512, 32512, 32512, 32512,
};

static int32_t sophie_add_lfo(int32_t slot, int32_t val, int32_t mn, int32_t mx,
                              int32_t d0, int32_t m0, int32_t d1, int32_t m1)
{
    if (d0 == slot) {
        val += m0;
        if (val < mn)
            val = mn;
        else if (val > mx)
            val = mx;
    }
    if (d1 == slot) {
        val += m1;
        if (val < mn)
            val = mn;
        else if (val > mx)
            val = mx;
    }
    return val;
}

/* Voice cap, newest-wins per track (was global mono in 0.1a-0.1e: a second
 * SOPHIE track choked the first, which is wrong — tracks must share the
 * box like FM voices do). Each track keeps at most one rendering voice;
 * excess voices on the same track write silence until their slot frees.
 * Rationale for any cap at all (VAWT precedent: 4 simultaneous full
 * voices wedged the hardware by starving the UI task): Sophie full-patch
 * cost is real, and an unbounded 8-voice chord of FOLD-hot SHARD is the
 * exact shape that bricks boxes. Per-track mono bounds the worst case to
 * 4 voices (one per track) while leaving every track playable.
 * Bookkeeping: birth order from a render-side clock; releases free
 * immediately; sophie_reap() on the UI tick clears ends the render
 * stopped observing (its clock freezes with zero callers). */
#define SOPHIE_MAX_VOICES 1
#define SOPHIE_TICK_REAP 120u
uint8_t sophie_sounding[8];
uint32_t sophie_birth[8];
uint32_t sophie_seen[8];
static int32_t sophie_track[8];
static uint32_t sophie_clock;
uint32_t sophie_ticks;

void sophie_reap(void)
{
    int32_t w;
    for (w = 0; w < 8; w++) {
        if (sophie_sounding[w] &&
            (int32_t)(sophie_ticks - sophie_seen[w]) > (int32_t)SOPHIE_TICK_REAP) {
            sophie_sounding[w] = 0;
            sophie_birth[w] = 0;
        }
    }
}

static struct ds_voice sophie_voices[8];

static void put_int(char *buf, int32_t v)
{
    char t[8];
    int32_t n = 0;
    if (v < 0) {
        *buf++ = '-';
        v = -v;
    }
    do {
        t[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && n < 7);
    while (n)
        *buf++ = t[--n];
    *buf = 0;
}

static void put_pct(char *buf, int32_t p)
{
    int32_t n = 0;
    char t[8];
    if (p < 0)
        p = 0;
    if (p > 100)
        p = 100;
    do {
        t[n++] = (char)('0' + p % 10);
        p /= 10;
    } while (p && n < 7);
    while (n)
        *buf++ = t[--n];
    *buf++ = '%';
    *buf = 0;
}

void sophie_oct_text(int32_t value, char *buf)
{
    int32_t o = (value >> 8) - 2;
    if (o < -2)
        o = -2;
    if (o > 2)
        o = 2;
    if (o > 0)
        *buf++ = '+';
    put_int(buf, o);
}

void sophie_model_text(int32_t value, char *buf)
{
    static const char names[4][6] = { "FUSE", "BOOM", "PIPE", "SHARD" };
    int32_t m = (value >> 8) / 5;
    const char *s;
    int32_t i;
    if (m < 0)
        m = 0;
    if (m > 3)
        m = 3;
    s = names[m];
    for (i = 0; s[i]; i++)
        buf[i] = s[i];
    buf[i] = 0;
}

void sophie_fold_text(int32_t value, char *buf)
{
    int32_t raw = value >> 8;
    if (raw < 0)
        raw = 0;
    if (raw > 35)
        raw = 35;
    put_pct(buf, raw * 100 / 35);
}

void sophie_sweep_text(int32_t value, char *buf)
{
    int32_t d = (value >> 8) - 64;
    if (d < -64)
        d = -64;
    if (d > 63)
        d = 63;
    if (d > 0)
        *buf++ = '+';
    put_int(buf, d);
}

void sophie_pct127_text(int32_t value, char *buf)
{
    int32_t r = value >> 8;
    if (r < 0)
        r = 0;
    if (r > 127)
        r = 127;
    put_pct(buf, r * 100 / 127);
}

void sophie_color_text(int32_t value, char *buf)
{
    int32_t r = value >> 8;
    if (r < 0)
        r = 0;
    if (r > 127)
        r = 127;
    put_pct(buf, r * 100 / 127);
}

void sophie_tune_text(int32_t value, char *buf)
{
    int32_t raw = value >> 8;
    int32_t cents;
    if (raw < 0)
        raw = 0;
    if (raw > 127)
        raw = 127;
    cents = ((raw - 64) * 25) >> 4;
    if (cents > 0)
        *buf++ = '+';
    put_int(buf, cents);
}

const struct dnm_knob sophie_knobs[DNM_KNOBS] = {
    [0] = { "OCT", sophie_oct_text, 0 },
    [1] = { "MODEL", sophie_model_text, 0 },
    [2] = { "FOLD", sophie_fold_text, 109 },
    [4] = { "SWEEP", sophie_sweep_text, 111 },
    [5] = { "METAL", sophie_pct127_text, 112 },
    [6] = { "FBK", sophie_pct127_text, 113 },
    [7] = { "COLOR", sophie_color_text, 114 },
    [8] = { "TUNE", sophie_tune_text, 119 },
};

static void sophie_render(int32_t voice, int32_t *block, const struct dnm_voice *v)
{
    int32_t lfo0_dest, lfo0_mod, lfo1_dest, lfo1_mod;
    int32_t octr, modelr, foldr, sweepr, metalr, fbkr, colorr, tuner;
    int32_t i, w, rank;
    int32_t oct, model, fold, sweep, metal, feedback, color, cents;
    struct ds_params p;
    struct ds_voice *ds;
    int32_t vel;
    uint32_t inc;
    voice &= 7;

    sophie_seen[voice] = sophie_ticks;
    if (v->fresh) {
        sophie_sounding[voice] = 1;
        sophie_birth[voice] = ++sophie_clock;
        sophie_track[voice] = v->track;
    } else if (!v->gate) {
        sophie_sounding[voice] = 0;
    } else if (!sophie_sounding[voice]) {
        sophie_sounding[voice] = 1;
        sophie_birth[voice] = ++sophie_clock;
        sophie_track[voice] = v->track;
    }
    /* Per-track newest-wins: only same-track voices outrank. Stale
     * entries (awaiting reap) always hold older births, so they can
     * never silence a newer voice. */
    rank = 0;
    for (w = 0; w < 8; w++) {
        if (sophie_sounding[w] && sophie_track[w] == v->track &&
            (int32_t)(sophie_birth[w] - sophie_birth[voice]) > 0)
            rank++;
    }
    if (rank >= SOPHIE_MAX_VOICES) {
        for (i = 0; i < 32; i++)
            block[i] = 0;
        return;
    }

    ds = &sophie_voices[voice];
    /* Companion owns AMP: synthesize the core's gate signal from DN gate.
     * Held -> loud/active phase (never sleeps); released -> quiet phase
     * (32 blocks -> 128-sample fade -> sleep). Untouched core logic. */
    if (v->gate)
        ds_voice_gate(ds, 1 << 23, 3);
    else
        ds_voice_gate(ds, 0, 0);
    if (!v->fresh && (!ds->active || ds->sleeping)) {
        for (i = 0; i < 32; i++)
            block[i] = 0;
        return;
    }

    /* Knob bases + LFO in 8.8, clamped to each slot's stock range. */
    lfo0_dest = (int32_t)FW_LFO(voice, 0)[FW_LFO_DEST];
    lfo0_mod = (int32_t)FW_LFO(voice, 0)[FW_LFO_MOD];
    lfo1_dest = (int32_t)FW_LFO(voice, 1)[FW_LFO_DEST];
    lfo1_mod = (int32_t)FW_LFO(voice, 1)[FW_LFO_MOD];
    octr = sophie_add_lfo(17, (int32_t)FW_VOICE_PARAM(voice, 17),
                          sophie_lfo_min[0], sophie_lfo_max[0],
                          lfo0_dest, lfo0_mod, lfo1_dest, lfo1_mod) >> 8;
    modelr = sophie_add_lfo(18, (int32_t)FW_VOICE_PARAM(voice, 18),
                            sophie_lfo_min[1], sophie_lfo_max[1],
                            lfo0_dest, lfo0_mod, lfo1_dest, lfo1_mod) >> 8;
    foldr = sophie_add_lfo(19, (int32_t)FW_VOICE_PARAM(voice, 19),
                           sophie_lfo_min[2], sophie_lfo_max[2],
                           lfo0_dest, lfo0_mod, lfo1_dest, lfo1_mod) >> 8;
    sweepr = sophie_add_lfo(21, (int32_t)FW_VOICE_PARAM(voice, 21),
                            sophie_lfo_min[3], sophie_lfo_max[3],
                            lfo0_dest, lfo0_mod, lfo1_dest, lfo1_mod) >> 8;
    metalr = sophie_add_lfo(22, (int32_t)FW_VOICE_PARAM(voice, 22),
                            sophie_lfo_min[4], sophie_lfo_max[4],
                            lfo0_dest, lfo0_mod, lfo1_dest, lfo1_mod) >> 8;
    fbkr = sophie_add_lfo(23, (int32_t)FW_VOICE_PARAM(voice, 23),
                          sophie_lfo_min[5], sophie_lfo_max[5],
                          lfo0_dest, lfo0_mod, lfo1_dest, lfo1_mod) >> 8;
    colorr = sophie_add_lfo(24, (int32_t)FW_VOICE_PARAM(voice, 24),
                            sophie_lfo_min[6], sophie_lfo_max[6],
                            lfo0_dest, lfo0_mod, lfo1_dest, lfo1_mod) >> 8;
    tuner = sophie_add_lfo(27, (int32_t)FW_VOICE_PARAM(voice, 27),
                           sophie_lfo_min[7], sophie_lfo_max[7],
                           lfo0_dest, lfo0_mod, lfo1_dest, lfo1_mod) >> 8;

    oct = octr - 2;
    if (oct < -2)
        oct = -2;
    if (oct > 2)
        oct = 2;
    model = modelr / 5;
    if (model < 0)
        model = 0;
    if (model > 3)
        model = 3;
    /* FOLD 0-35 -> 0-127 (no divide: *29>>3 = *3.625, 35 -> 126). */
    fold = (foldr * 29) >> 3;
    if (fold < 0)
        fold = 0;
    if (fold > 127)
        fold = 127;
    sweep = sweepr - 64;
    if (sweep < -64)
        sweep = -64;
    if (sweep > 63)
        sweep = 63;
    if (metalr < 0)
        metalr = 0;
    if (metalr > 127)
        metalr = 127;
    metal = metalr;
    if (fbkr < 0)
        fbkr = 0;
    if (fbkr > 127)
        fbkr = 127;
    feedback = fbkr;
    if (colorr < 0)
        colorr = 0;
    if (colorr > 127)
        colorr = 127;
    color = colorr;
    if (tuner < 0)
        tuner = 0;
    if (tuner > 127)
        tuner = 127;
    /* Fine tune in cents, about +-100 over full travel. */
    cents = ((tuner - 64) * 25) >> 4;

    inc = dnm_phase_inc(v->pitch + (oct * 12 << 16) + cents * 655) >> 16;
    if (inc < 8)
        inc = 8;
    if (inc > 24576)
        inc = 24576;
    p.phase_inc = (uint16_t)inc;
    p.model = (uint8_t)model;
    p.color = (uint8_t)color;
    p.metal = (uint8_t)metal;
    p.sweep = (int8_t)sweep;
    p.feedback = (uint8_t)feedback;
    vel = v->velocity;
    if (vel < 0)
        vel = 0;
    if (vel > 127)
        vel = 127;
    p.velocity = (uint8_t)vel;
    p.fold = (uint8_t)fold;

    ds_voice_render(ds, &p, v->fresh, block, 32);
    ds_fold_block(block, 32, p.fold);
    /* Core 16.16 -> Q1.31 ~ DNM_PEAK (host-proven window). */
    for (i = 0; i < 32; i++)
        block[i] >>= 2;
}

/* Tile icon: 16 rows of 16 pixels, bit 15 left. Each row mirrors its
 * left half (high byte, bit-reversed) into the right half (low byte),
 * so the spark reads as one symmetric diamond. */
static const uint16_t sophie_icon[16] = {
    0x0000, 0x0810, 0x0810, 0x1c38, 0x1c38, 0x3e7c, 0x3e7c, 0x7ffe,
    0x7ffe, 0x3e7c, 0x3e7c, 0x1c38, 0x1c38, 0x0810, 0x0810, 0x0000,
};

/* Defaults in each knob's stock 8.8 domain (-1 leaves the knob alone):
 * OCT 0 (raw 2), MODEL FUSE, FOLD 0, SWEEP 0 (raw 64), METAL 64, FBK 32,
 * COLOR 64, TUNE 0 cents (raw 64); D + SYN2 B-H untouched. */
static const int16_t sophie_defaults[DNM_KNOBS] = {
    512, 0, 0, -1, 16384, 16384, 8192, 16384,
    16384, -1, -1, -1, -1, -1, -1, -1,
};

const struct dnm_machine sophie_machine = {
    11, "SOPH", sophie_icon, sophie_knobs, sophie_render, 0, sophie_defaults,
};
