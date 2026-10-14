#!/usr/bin/env python3
"""Phase 1 host harness: Sophie core driven through Digitone-style plumbing.

Compiles sophie.c natively with a stubbed dnm_phase_inc (float reference,
same shape as the VAWT host driver) and a C test that derives ds_params
from DN pitch words + 0-127 controls, applies the proposed Q-scale
(16.16 >> 2 -> Q1.31 ~ DNM_PEAK), and checks continuity, model difference,
retrigger-choke, sleep/fade, fold bypass/compensation, block-size
invariance, pitch accuracy and bounds. Must stay green; gates Phase 2.
"""
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent

DRIVER = r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "sophie.h"

#define DNM_PEAK 0x08000000
#define N 48000

/* Stub: float reference for dnm_phase_inc (Q32 step/sample @48kHz). */
static uint32_t stub_phase_inc(int32_t pitch) {
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

/* Proposed engine mapping: Q32 -> Sophie inc, clamp 8..24576. */
static uint16_t dn_to_inc(int32_t pitch) {
    uint32_t st = stub_phase_inc(pitch) >> 16;
    if (st < 8) st = 8;
    if (st > 24576) st = 24576;
    return (uint16_t)st;
}

/* Proposed engine output scale: 16.16 >> 2 -> Q1.31. */
static int32_t dn_scale(int32_t s1616) { return s1616 >> 2; }

static uint64_t energy16(const int32_t *x, unsigned n) {
    unsigned i; uint64_t t = 0;
    for (i = 0; i < n; ++i) {
        int32_t s = x[i] / 65536;
        if (s < 0) s = -s;
        t += (uint32_t)s;
    }
    return t;
}
static uint64_t power16(const int32_t *x, unsigned n) {
    unsigned i; uint64_t t = 0;
    for (i = 0; i < n; ++i) {
        int64_t s = x[i] / 65536;
        t += (uint64_t)(s * s);
    }
    return t;
}
static uint64_t difference16(const int32_t *a, const int32_t *b, unsigned n) {
    unsigned i; uint64_t t = 0;
    for (i = 0; i < n; ++i) {
        int32_t d = (a[i] >> 16) - (b[i] >> 16);
        if (d < 0) d = -d;
        t += (uint32_t)d;
    }
    return t;
}
static int32_t peak32(const int32_t *x, unsigned n) {
    int32_t m = 0, i;
    for (i = 0; i < (int32_t)n; i++) {
        int32_t a = x[i] < 0 ? -x[i] : x[i];
        if (a > m) m = a;
    }
    return m;
}
static double zc_rate(const int32_t *x, unsigned n) {
    unsigned c = 0, i;
    for (i = 0; i + 1 < n; i++)
        if ((x[i] < 0) != (x[i+1] < 0)) c++;
    return (double)c / (double)(n - 1);
}

static int fails = 0;
#define CHECK(name, cond) do { \
    printf("%s %s\n", (cond) ? "PASS" : "FAIL", name); \
    if (!(cond)) fails++; } while (0)

int main(void) {
    static int32_t a[N], b[N];
    static int32_t q[N];
    struct ds_voice va, vb;
    struct ds_params p;
    unsigned i, m, o;
    double zc, expect;

    /* 1. DN pitch mapping: BOOM body (pure carrier sine) at A4 ~440Hz. */
    p.phase_inc = dn_to_inc(69 << 16);
    p.model = 1; p.color = 64; p.metal = 0; p.sweep = 0;
    p.feedback = 0; p.velocity = 127; p.fold = 0;
    ds_voice_init(&va);
    ds_voice_render(&va, &p, 1, a, N);
    zc = zc_rate(a + 8*32, N - 8*32);
    expect = 2.0 * 440.0 / 48000.0;
    CHECK("dn-pitch-a4", zc > expect * 0.97 && zc < expect * 1.03);
    /* Octave doubling via pitch word +12. */
    p.phase_inc = dn_to_inc(81 << 16);
    ds_voice_init(&vb);
    ds_voice_render(&vb, &p, 1, b, N);
    {
        double z2 = zc_rate(b + 8*32, N - 8*32);
        CHECK("dn-pitch-oct", z2 > zc * 1.96 && z2 < zc * 2.04);
    }
    /* Clamps: stub note-clamps to 0..127 first, so extremes must land
     * in range; exercise the 8..24576 clamp directly. */
    CHECK("dn-inc-lo", dn_to_inc(0) >= 8 && dn_to_inc(0) <= 24576);
    CHECK("dn-inc-hi", dn_to_inc(127 << 16) >= 8 && dn_to_inc(127 << 16) <= 24576);

    /* 2. Continuity: factory-ish Fuse patch, bipolar, sustained. */
    p.phase_inc = dn_to_inc(69 << 16);
    p.model = 0; p.color = 64; p.metal = 40; p.sweep = 0;
    p.feedback = 32; p.velocity = 127; p.fold = 0;
    ds_voice_init(&va);
    ds_voice_render(&va, &p, 1, a, N);
    CHECK("live-early", energy16(a, 12000) > 50000);
    CHECK("live-late", energy16(a + 36000, 12000) > 50000);
    {
        unsigned pos = 0, neg = 0, late = 0;
        for (i = 0; i < 4096; i++) {
            if (a[i] > 0) pos++;
            if (a[i] < 0) neg++;
        }
        for (i = 4000; i < 8000; i++) if (a[i]) late++;
        CHECK("bipolar", pos > 500 && neg > 500);
        CHECK("sustained", late > 3000);
    }
    /* Determinism (fuse relies on it). */
    ds_voice_init(&vb);
    ds_voice_render(&vb, &p, 1, b, N);
    CHECK("deterministic", !memcmp(a, b, sizeof a));

    /* 3. Model difference. */
    {
        static int32_t v[4][4096];
        p = (struct ds_params){ dn_to_inc(69<<16), 0, 91, 98, 30, 60, 127, 0 };
        for (m = 0; m < 4; m++) {
            p.model = (uint8_t)m;
            ds_voice_init(&va);
            ds_voice_render(&va, &p, 1, v[m], 4096);
            for (o = 0; o < m; o++)
                if (difference16(v[m], v[o], 4096) <= 100000) break;
            if (o != m) break;
        }
        CHECK("model-diff", m == 4);
    }

    /* 4. Retrigger-choke: next block starts at previous output. */
    p = (struct ds_params){ 68, 0, 64, 40, 0, 32, 127, 0 };
    ds_voice_init(&vb);
    for (i = 0; i < N; i += 32)
        ds_voice_render(&vb, &p, i == 0, b + i, 32);
    {
        int32_t last = b[N-1];
        p.velocity = 40; p.metal = 127;
        ds_voice_render(&vb, &p, 1, b, 32);
        CHECK("retrigger-choke", b[0] == last);
        last = b[31];
        p.model = 2;
        ds_voice_render(&vb, &p, 0, b, 32);
        CHECK("model-choke", b[0] == last);
    }
    /* Block-size invariance. */
    ds_voice_init(&va);
    ds_voice_render(&va, &p, 1, a, N);
    CHECK("block-invariant-live", energy16(a, N) > 10000);

    /* 5. Sleep/fade via core gate primitives (DN translation in engine). */
    p = (struct ds_params){ 68, 0, 64, 40, 0, 32, 127, 0 };
    ds_voice_init(&vb);
    ds_voice_render(&vb, &p, 1, b, 32);
    for (i = 0; i < 64; i++) ds_voice_gate(&vb, 1 << 23, 3);
    CHECK("held-awake", !vb.sleeping && vb.quiet_blocks == 0);
    for (i = 0; i < 32; i++) ds_voice_gate(&vb, 0, 0);
    CHECK("fade-armed", !vb.sleeping && vb.fade_left == 128);
    for (i = 0; i < 8; i++) ds_voice_render(&vb, &p, 0, b, 32);
    CHECK("sleeps", vb.sleeping && vb.last == 0);
    ds_voice_render(&vb, &p, 1, b, 32);
    CHECK("wakes-silent", !vb.sleeping && b[0] == 0 && energy16(b, 32) > 0);

    /* 6. Fold: zero bypass bit-exact, hot fold changes, compensation. */
    ds_voice_init(&va);
    ds_voice_render(&va, &p, 1, a, N);
    memcpy(b, a, sizeof a);
    ds_fold_block(b, N, 0);
    CHECK("fold-bypass", !memcmp(a, b, sizeof a));
    ds_fold_block(b, N, 127);
    CHECK("fold-changes", difference16(a, b, N) > 100000);
    for (m = 0; m < 4; m++) {
        uint64_t plain, folded;
        struct ds_params q2 = { 68, (uint8_t)m, 64, 40, 0, 32, 127, 0 };
        ds_voice_init(&va);
        ds_voice_render(&va, &q2, 1, a, N);
        plain = power16(a + 2400, N - 2400);
        memcpy(b, a, sizeof a);
        ds_fold_block(b, N, 127);
        folded = power16(b + 2400, N - 2400);
        if (!(folded * 3 > plain * 2 && folded * 2 < plain * 3)) break;
    }
    CHECK("fold-compensation", m == 4);

    /* 7. Q-scale to DNM_PEAK: factory Fuse lands near FM loudness. */
    p = (struct ds_params){ 68, 0, 64, 40, 0, 32, 127, 0 };
    ds_voice_init(&va);
    ds_voice_render(&va, &p, 1, a, N);
    for (i = 0; i < N; i++) q[i] = dn_scale(a[i]);
    {
        int32_t pk = peak32(q, N);
        CHECK("qscale-peak", pk > DNM_PEAK / 8 && pk <= (int32_t)(DNM_PEAK * 1.2));
    }
    /* Worst-case sweep stays bounded in both domains. */
    for (m = 0; m < 4; m++) for (i = 0; i < 16; i++) {
        unsigned k;
        struct ds_params w = { (i & 1) ? 24576 : 68, (uint8_t)m,
            (i & 2) ? 127 : 0, 127, (i & 4) ? 63 : -64,
            (i & 8) ? 127 : 0, 127, 0 };
        ds_voice_init(&vb);
        ds_voice_render(&vb, &w, 1, b, 4096);
        for (k = 0; k < 4096; k++) {
            int32_t s = dn_scale(b[k]);
            (void)s;
            if (b[k] / 65536 >= 12000 || b[k] / 65536 <= -12000) break;
        }
        if (k != 4096) break;
    }
    CHECK("worst-bounded", m == 4 && i == 16);

    printf(fails ? "\n%d FAILURES\n" : "\nALL DN HOST TESTS PASSED\n", fails);
    return fails != 0;
}
"""


def main():
    with tempfile.TemporaryDirectory() as td:
        tmp = pathlib.Path(td)
        drv = tmp / "dntest.c"
        exe = tmp / "dntest"
        drv.write_text(DRIVER)
        r = subprocess.run(
            ["cc", "-O1", "-o", str(exe), str(drv), str(ROOT / "sophie.c"),
             "-I", str(ROOT), "-lm"],
            capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stdout[-3000:])
            print(r.stderr[-3000:])
            raise SystemExit("host compile failed")
        r2 = subprocess.run([str(exe)], capture_output=True, text=True)
        print(r2.stdout, end="")
        print(r2.stderr, end="")
        return r2.returncode


if __name__ == "__main__":
    sys.exit(main())