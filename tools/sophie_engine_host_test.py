#!/usr/bin/env python3
"""Phase 2 host harness: digisophie-dn engine render through stubbed companion.

Compiles digisophie-dn/engine.c + sophie.c natively with stubbed dnm_phase_inc
and fw_* arrays, drives sophie_machine.render through pitch, knob zones,
fold, fuse newest-wins, release/reap, LFO and bounds. Must stay green.
"""
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
INC = ROOT / "tools" / "elekloader" / "elekloader" / "sdk" / "include"

DRIVER = r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "digitone-mk1/machines.h"

#define DNM_PEAK 0x08000000

volatile int16_t fw_voice_params[1024];
volatile int32_t fw_lfo_state[256];
volatile uint32_t fw_voice_pitch[8];
volatile int32_t fw_voice_len[8];
volatile uint32_t fw_gate_off;
volatile uint32_t fw_timeline;
volatile int32_t fw_transpose[5];
volatile int32_t fw_voice_track[8];
volatile uint32_t fw_gate_on;
const char fw_str_empty[] = "";

uint32_t dnm_phase_inc(int32_t pitch) {
    int32_t n = pitch >> 16;
    uint32_t f = (uint32_t)pitch & 0xffffu;
    double fa, fb;
    uint32_t a, b;
    if (n < 0) n = 0;
    if (n > 127) n = 127;
    fa = 440.0 * pow(2.0, ((double)n - 69.0) / 12.0) / 48000.0 * 4294967296.0;
    fb = 440.0 * pow(2.0, ((double)(n + 1) - 69.0) / 12.0) / 48000.0 * 4294967296.0;
    a = (uint32_t)(fa + 0.5); b = (uint32_t)(fb + 0.5);
    return a + (uint32_t)(((b - a) >> 8) * (f >> 8));
}
int32_t dnm_sin(uint32_t phase) {
    return (int32_t)(sin(6.283185307179586 * (double)phase / 4294967296.0) * 32767.0);
}
int32_t dnm_knob(int32_t voice, int32_t knob) { (void)voice; (void)knob; return 0; }
void dnm_knob_range(int32_t knob, int32_t *mn, int32_t *mx) {
    (void)knob; *mn = 0; *mx = 32512;
}

extern const struct dnm_machine sophie_machine;
extern uint32_t sophie_ticks;
extern uint8_t sophie_sounding[8];
void sophie_reap(void);

static void set_slot(int v, int slot, int32_t val) {
    fw_voice_params[9 + 79 * v + slot] = (int16_t)val;
}
static void defaults(int v) {
    int i;
    for (i = 0; i < 79; i++) fw_voice_params[9 + 79 * v + i] = 0;
    set_slot(v, 17, 512);    /* OCT 0 */
    set_slot(v, 18, 0);      /* MODEL FUSE */
    set_slot(v, 19, 0);      /* FOLD 0 */
    set_slot(v, 21, 16384);  /* SWEEP 0 */
    set_slot(v, 22, 16384);  /* METAL 64 */
    set_slot(v, 23, 8192);   /* FBK 32 */
    set_slot(v, 24, 16384);  /* COLOR 64 */
    set_slot(v, 27, 16384);  /* TUNE 0 cents */
    fw_lfo_state[20 * v + 0 + 16] = 127;
    fw_lfo_state[20 * v + 0 + 17] = 0;
    fw_lfo_state[20 * v + 10 + 16] = 127;
    fw_lfo_state[20 * v + 10 + 17] = 0;
}
static void run(int v, struct dnm_voice *dv, int32_t *out, int nblocks) {
    int b;
    for (b = 0; b < nblocks; b++) {
        dv->fresh = (b == 0);
        sophie_machine.render(v, out + 32 * b, dv);
    }
}
static int32_t peak32(const int32_t *x, int n) {
    int32_t m = 0, i;
    for (i = 0; i < n; i++) {
        int32_t a = x[i] < 0 ? -x[i] : x[i];
        if (a > m) m = a;
    }
    return m;
}
static double zc_rate(const int32_t *x, int n) {
    int c = 0, i;
    for (i = 0; i + 1 < n; i++)
        if ((x[i] < 0) != (x[i+1] < 0)) c++;
    return (double)c / (double)(n - 1);
}
static uint64_t diff(const int32_t *a, const int32_t *b, int n) {
    uint64_t t = 0;
    int i;
    for (i = 0; i < n; i++) {
        int32_t d = (a[i] >> 14) - (b[i] >> 14);
        if (d < 0) d = -d;
        t += (uint32_t)d;
    }
    return t;
}

static int fails = 0;
#define CHECK(name, cond) do { \
    printf("%s %s\n", (cond) ? "PASS" : "FAIL", name); \
    if (!(cond)) fails++; } while (0)

int main(void) {
    static int32_t buf[32 * 64], buf2[32 * 64];
    struct dnm_voice dv, dv2;
    double zc, expect;

    memset(&dv, 0, sizeof dv);
    dv.velocity = 100; dv.track = 0; dv.gate = 1;
    defaults(0);

    /* 1. Pitch: BOOM body at A4 ~440Hz (METAL 0 keeps body clean). */
    set_slot(0, 18, 1 * 256);  /* MODEL BOOM: raw 5ish -> zone 1 */
    set_slot(0, 22, 0);
    dv.pitch = 69 << 16;
    run(0, &dv, buf, 48);
    zc = zc_rate(buf + 8 * 32, 40 * 32);
    expect = 2.0 * 440.0 / 48000.0;
    CHECK("pitch-a4", zc > expect * 0.95 && zc < expect * 1.05);
    /* OCT +1 doubles. */
    set_slot(0, 17, 3 * 256);
    run(0, &dv, buf2, 48);
    {
        double z2 = zc_rate(buf2 + 8 * 32, 40 * 32);
        CHECK("oct-doubles", z2 > zc * 1.9 && z2 < zc * 2.1);
    }
    set_slot(0, 17, 512);

    /* 1b. TUNE: SYN2 A fine tune about +-100 cents around the note. */
    set_slot(0, 27, 64 * 256);
    run(0, &dv, buf, 48);
    zc = zc_rate(buf + 8 * 32, 40 * 32);
    set_slot(0, 27, 0);
    run(0, &dv, buf2, 48);
    CHECK("tune-down", zc_rate(buf2 + 8 * 32, 40 * 32) < zc * 0.96);
    set_slot(0, 27, 127 * 256);
    run(0, &dv, buf2, 48);
    CHECK("tune-up", zc_rate(buf2 + 8 * 32, 40 * 32) > zc * 1.04);
    set_slot(0, 27, 64 * 256);

    /* 2. Model zones: B raw 0/5/10/15 give 4 different voices
     * (needs METAL up: at METAL 0 FUSE and BOOM are both a pure sine). */
    {
        static int32_t v[4][32 * 8];
        int m, o, ok = 1;
        struct dnm_voice t;
        memset(&t, 0, sizeof t);
        t.velocity = 100; t.track = 0; t.gate = 1; t.pitch = 69 << 16;
        set_slot(0, 22, 64 * 256);
        set_slot(0, 23, 60 * 256);
        for (m = 0; m < 4; m++) {
            set_slot(0, 18, m * 5 * 256);
            run(0, &t, v[m], 8);
            for (o = 0; o < m; o++)
                if (diff(v[m] + 128, v[o] + 128, 4 * 32) <= 20000) ok = 0;
        }
        CHECK("model-zones", ok);
        set_slot(0, 18, 0);
    }

    /* 3. Fold: C=0 bypass vs hot fold changes timbre. Hot fold drives
     * level by design (Digitakt-stock: downstream AMP/filter own it), so
     * the bound here is int32-safe, not FM-loudness. */
    set_slot(0, 19, 0);
    run(0, &dv, buf, 8);
    set_slot(0, 19, 35 * 256);
    run(0, &dv, buf2, 8);
    CHECK("fold-changes", diff(buf + 128, buf2 + 128, 4 * 32) > 20000);
    CHECK("fold-bounded", peak32(buf2, 8 * 32) <= (int32_t)(DNM_PEAK * 6));
    set_slot(0, 19, 0);

    /* 4. Loudness: factory-ish FUSE near DNM_PEAK window. */
    set_slot(0, 18, 0);
    set_slot(0, 22, 40 * 256);
    set_slot(0, 23, 32 * 256);
    dv.pitch = 69 << 16;
    run(0, &dv, buf, 8);
    {
        int32_t pk = peak32(buf + 32, 7 * 32);
        CHECK("loudness", pk > DNM_PEAK / 16 && pk <= (int32_t)(DNM_PEAK * 1.2));
    }

    /* 5. Sweep both directions matter (PIPE keeps COLOR/FBK live). */
    set_slot(0, 18, 2 * 5 * 256);
    set_slot(0, 21, 37 * 256);
    run(0, &dv, buf, 8);
    set_slot(0, 21, 89 * 256);
    run(0, &dv, buf2, 8);
    CHECK("sweep-matters", diff(buf + 128, buf2 + 128, 4 * 32) > 20000);
    set_slot(0, 21, 16384);
    set_slot(0, 18, 0);

    /* 6. Fuse: second fresh voice chokes the first (newest wins). */
    memset(&dv2, 0, sizeof dv2);
    dv2.velocity = 100; dv2.track = 0; dv2.gate = 1; dv2.pitch = 69 << 16;
    defaults(1);
    dv.pitch = 69 << 16; dv.gate = 1;
    dv.fresh = 1;
    sophie_machine.render(0, buf, &dv);
    dv.fresh = 0;
    dv2.fresh = 1;
    sophie_machine.render(1, buf2, &dv2);
    dv2.fresh = 0;
    sophie_machine.render(0, buf, &dv);
    sophie_machine.render(1, buf2, &dv2);
    CHECK("fuse-old-silent", peak32(buf, 32) == 0);
    CHECK("fuse-new-live", peak32(buf2, 32) > 1000);
    /* Release frees immediately: old voice resumes. */
    dv2.gate = 0;
    sophie_machine.render(1, buf2, &dv2);
    sophie_machine.render(0, buf, &dv);
    CHECK("release-frees", peak32(buf, 32) > 0);

    /* 6b. Cross-track coexistence: a voice on another track keeps its
     * own voice (the reported T1-vs-T3 choke). */
    defaults(2);
    memset(&dv2, 0, sizeof dv2);
    dv2.velocity = 100; dv2.track = 1; dv2.gate = 1; dv2.pitch = 69 << 16;
    dv2.fresh = 1;
    sophie_machine.render(2, buf2, &dv2);
    dv2.fresh = 0;
    dv.gate = 1; dv.fresh = 0;
    sophie_machine.render(0, buf, &dv);
    sophie_machine.render(2, buf2, &dv2);
    CHECK("xtrack-both-live", peak32(buf, 32) > 0 && peak32(buf2, 32) > 0);
    /* Same track still chokes: a second voice on track 1 silences its own. */
    {
        struct dnm_voice dv3;
        memset(&dv3, 0, sizeof dv3);
        dv3.velocity = 100; dv3.track = 1; dv3.gate = 1; dv3.pitch = 69 << 16;
        defaults(3);
        dv3.fresh = 1;
        sophie_machine.render(3, buf, &dv3);
        dv3.fresh = 0;
        sophie_machine.render(2, buf2, &dv2);
        sophie_machine.render(3, buf, &dv3);
        CHECK("sametrack-chokes", peak32(buf2, 32) == 0 && peak32(buf, 32) > 0);
    }

    /* 7. Reap: unobserved voice ages out on UI tick. */
    dv.fresh = 1;
    sophie_machine.render(2, buf, &dv);
    dv.fresh = 0;
    sophie_ticks += 200;
    sophie_reap();
    CHECK("reap-clears", sophie_sounding[2] == 0);

    /* 8. Release -> sleep: sustained zeros after fade, re-fresh wakes. */
    defaults(3);
    memset(&dv, 0, sizeof dv);
    dv.velocity = 100; dv.track = 0; dv.gate = 1; dv.pitch = 69 << 16;
    run(3, &dv, buf, 4);
    {
        int b, slept = 0;
        dv.gate = 0; dv.fresh = 0;
        for (b = 0; b < 64; b++) {
            sophie_machine.render(3, buf2, &dv);
            if (peak32(buf2, 32) == 0) { slept = 1; break; }
        }
        CHECK("release-sleeps", slept);
        dv.gate = 1; dv.fresh = 1;
        sophie_machine.render(3, buf2, &dv);
        CHECK("retrigger-wakes", peak32(buf2, 32) > 0);
    }

    /* 9. LFO: FOLD slot MOD changes timbre; FILTER dest ignored.
     * Compare steady blocks only: each run()'s fresh retrigger crossfades
     * 2 blocks from a different tail. */
    defaults(4);
    memset(&dv, 0, sizeof dv);
    dv.velocity = 100; dv.track = 0; dv.gate = 1; dv.pitch = 69 << 16;
    run(4, &dv, buf, 8);
    fw_lfo_state[20 * 4 + 0 + 16] = 19;
    fw_lfo_state[20 * 4 + 0 + 17] = 16384;
    run(4, &dv, buf2, 8);
    CHECK("lfo-fold", diff(buf + 128, buf2 + 128, 4 * 32) > 20000);
    CHECK("lfo-bounded", peak32(buf2, 8 * 32) <= (int32_t)(DNM_PEAK * 6));
    fw_lfo_state[20 * 4 + 0 + 16] = 66;
    fw_lfo_state[20 * 4 + 0 + 17] = 16384;
    run(4, &dv, buf2, 8);
    CHECK("lfo-filter-ignored", diff(buf + 128, buf2 + 128, 4 * 32) == 0);
    fw_lfo_state[20 * 4 + 0 + 16] = 127;
    fw_lfo_state[20 * 4 + 0 + 17] = 0;

    /* 10. Bounds: extremes on every used knob stay int32-safe + live
     * (hot fold excluded from the FM-loudness window by design). */
    {
        int ok = 1;
        set_slot(5, 17, 0); set_slot(5, 18, 0); set_slot(5, 19, 0);
        set_slot(5, 21, 9472); set_slot(5, 22, 0);
        set_slot(5, 23, 0); set_slot(5, 24, 0); set_slot(5, 27, 0);
        memset(&dv, 0, sizeof dv);
        dv.velocity = 127; dv.track = 0; dv.gate = 1; dv.pitch = 100 << 16;
        run(5, &dv, buf, 8);
        if (!(peak32(buf, 8 * 32) > 0 &&
              peak32(buf, 8 * 32) <= (int32_t)(DNM_PEAK * 1.2))) ok = 0;
        set_slot(5, 17, 1792); set_slot(5, 18, 4608); set_slot(5, 19, 8960);
        set_slot(5, 21, 22784); set_slot(5, 22, 32512);
        set_slot(5, 23, 32512); set_slot(5, 24, 32512);
        set_slot(5, 27, 32512);
        run(5, &dv, buf, 8);
        if (!(peak32(buf, 8 * 32) > 0 &&
              peak32(buf, 8 * 32) <= (int32_t)(DNM_PEAK * 6))) ok = 0;
        CHECK("extremes", ok);
    }

    /* 11. Descriptor sanity. */
    CHECK("desc-id", sophie_machine.id == 11);
    CHECK("desc-knobs", sophie_machine.knobs[0].name &&
          sophie_machine.knobs[1].name && sophie_machine.knobs[5].name);
    CHECK("desc-flags", sophie_machine.flags == 0);

    printf(fails ? "\n%d FAILURES\n" : "\nALL ENGINE TESTS PASSED\n", fails);
    return fails != 0;
}
"""


def main():
    with tempfile.TemporaryDirectory() as td:
        tmp = pathlib.Path(td)
        drv = tmp / "engtest.c"
        exe = tmp / "engtest"
        drv.write_text(DRIVER)
        r = subprocess.run(
            ["cc", "-O1", "-o", str(exe), str(drv),
             str(ROOT / "digisophie-dn" / "engine.c"), str(ROOT / "sophie.c"),
             "-I", str(INC), "-I", str(ROOT / "digisophie-dn"), "-I", str(ROOT),
             "-lm"],
            capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stdout[-3000:])
            print(r.stderr[-3000:])
            raise SystemExit("engine host compile failed")
        r2 = subprocess.run([str(exe)], capture_output=True, text=True)
        print(r2.stdout, end="")
        print(r2.stderr, end="")
        return r2.returncode


if __name__ == "__main__":
    sys.exit(main())
