/* SPDX-License-Identifier: MIT */
/* digisophie-dn overlay: full SYN1/SYN2 redraws on a SOPHIE machine track
 * (digitone-mk1/machines.h, core-dn1 3.1).
 *
 * Stock SYN pages draw FM's pictures (algorithm diagram, envelope graphs)
 * and format values as FM, which core 3.1 cannot replace per track. This
 * paints the whole content area on ev_draw after the firmware drew the
 * frame: an 8-knob grid with SOPHIE labels, true value texts and mini
 * bars. Title bar, left rail and status bar stay stock.
 *
 * Gating uses firmware state, never pixels: paint only when the active
 * track's sound plays SOPHIE, the showing key is SYN1/SYN2, the brain shows
 * no screen over the pages, and the shown view is on its first page. A
 * held sequencer step does NOT fail the gate: in GRID/STEP record the
 * overlay stays up and shows that step's lock values (slot-indexed rows:
 * the step row's word for the knob's sound slot, -1 = none); otherwise
 * trigs play notes and the kit shows, as stock. Copy/paste/clear toasts
 * yield the frame ~90 ticks. ev_tick re-arms only on change. Runs on the
 * UI task.
 */
#include "digitone-mk1/machines.h"

#define SOPH_ID     11

/* UI-tick counter, shared with engine.c's fuse (last-seen stamps and
 * sophie_reap's horizon). Defined in engine.c; render only reads it. */
extern uint32_t sophie_ticks;
void sophie_reap(void);

extern const struct dnm_knob sophie_knobs[DNM_KNOBS];

#ifndef KEYVAR_AT
#error "KEYVAR_AT: the showing key's address (0x41367ce4 in 1.43)"
#endif
#ifndef PATPTR_AT
#error "PATPTR_AT: the active-pattern pointer (0x4138e214 in 1.43)"
#endif
#ifndef PROJPTR_AT
#error "PROJPTR_AT: the Project object pointer (0x419efde0 in 1.43)"
#endif

/* Brain/screen chain, as core_page_shown uses it: the key dispatcher's
 * brain keeps bottom/top screens at +0x54/+0x5c; bottom screen's page
 * manager at +8, shown view at +0xc0; a view's place at +144. */
#define BRAIN_BOTTOM    0x54
#define BRAIN_TOP       0x5c
#define SCREEN_MGR      8
#define MGR_VIEW        0xc0
#define VIEW_CUR        144

/* Content area and grid (panel coords, y = 0 top). Clear rect starts 3 px
 * left of the grid: stock SYN1 chrome animates under it while the rail
 * provably ends at x14 — clearing from 15 takes the bleed. */
#define CX0 18
#define CXC 15
#define CX1 127
#define CY0 12
#define CYT 11
#define CY1 63
#define COLS 4
#define ROWS 2

static uint32_t sophie_frames;
static uint32_t sophie_enc_at;
static int32_t sophie_enc = -1;
static int32_t sophie_enc_page = -1;
static void *sophie_brain;
static uint32_t sophie_trigs;
static uint32_t sophie_edit_rec;
static uint32_t sophie_live;
static uint32_t sophie_rec_down;
static uint32_t sophie_func_down;
static uint32_t sophie_ptn_down;
static uint32_t sophie_page_down;
static uint32_t sophie_msg_until;
#define SOPHIE_MSG_HOLD 90u

static int32_t msg_suppressed(void)
{
    return (int32_t)(sophie_ticks - sophie_msg_until) < 0;
}

#define YB(yt) (63 - (yt))

static void cell_bar(void *bmp, int32_t x, int32_t ytop, int32_t w,
                     int32_t val, int32_t max, int32_t center)
{
    int32_t yb = YB(ytop), fill, f0, f1;
    if (val < 0)
        val = 0;
    if (val > max)
        val = max;
    fw_framerect(bmp, x, yb - 4, x + w, yb, 1);
    if (!center) {
        fill = (w * val) / max;
        if (fill > 0)
            fw_fillrect(bmp, x + 1, yb - 3, x + fill, yb - 1, 1);
        return;
    }
    fill = (w * val) / max;
    f0 = x + 1 + w / 2;
    f1 = x + 1 + w / 2 + fill - w / 2;
    if (f1 < f0) {
        int32_t t = f0;
        f0 = f1;
        f1 = t;
    }
    if (f1 > f0)
        fw_fillrect(bmp, f0, yb - 3, f1, yb - 1, 1);
    fw_fillrect(bmp, x + w / 2, yb - 3, x + 1 + w / 2, yb - 1, 1);
}

static void draw_cell(void *bmp, int32_t k, int32_t x, int32_t ytop,
                      int32_t w, int32_t value, int32_t sel, int32_t locked)
{
    char buf[16];
    const char *name = sophie_knobs[k].name;
    if (!name || !name[0])
        return;
    fw_textf(bmp, fw_font5, x + 1, YB(ytop) - 7, -1, "%s", name);
    sophie_knobs[k].fmt(value, buf);
    fw_textf(bmp, fw_font5, x + 1, YB(ytop) - 14, -1, "%s", buf);
    if (locked)
        fw_fillrect(bmp, x, YB(ytop) - 19, x + w, YB(ytop) - 9, -1);
    if (k == 0)
        cell_bar(bmp, x + 1, ytop + 19, w - 2, value >> 8, 7, 0);
    else if (k == 1)
        cell_bar(bmp, x + 1, ytop + 19, w - 2, (value >> 8) / 5, 3, 0);
    else if (k == 2)
        cell_bar(bmp, x + 1, ytop + 19, w - 2, value >> 8, 35, 0);
    else if (k == 4)
        cell_bar(bmp, x + 1, ytop + 19, w - 2, (value >> 8) - 37, 52, 1);
    else if (k == 5 || k == 6)
        cell_bar(bmp, x + 1, ytop + 19, w - 2, value >> 8, 127, 0);
    else if (k == 7)
        cell_bar(bmp, x + 1, ytop + 19, w - 2, value >> 8, 127, 0);
    else if (k == 8)
        cell_bar(bmp, x + 1, ytop + 19, w - 2,
                 (((value >> 8) - 64) * 25 >> 4) + 100, 200, 1);
    if (sel)
        fw_framerect(bmp, x, YB(ytop + 17), x + w, YB(ytop), 1);
}

/* Knob index -> sound slot. SYN1 17-24 then SYN2 27-34 (slot 26 unused on
 * synth tracks; index 15 = slot 34). Unused knobs still map (locks for
 * their slot show if held) but draw blank. */
static int knob_slot(int32_t k)
{
    static const uint8_t slots[16] = {
        17, 18, 19, 20, 21, 22, 23, 24, 27, 28, 29, 30, 31, 32, 33, 34,
    };
    return k < 0 || k > 15 ? -1 : slots[k];
}

static int32_t held_step(void)
{
    uint32_t m = sophie_trigs, i;
    if (!m)
        return -1;
    for (i = 0; i < 16; i++)
        if (m & (1u << i))
            return (int32_t)i;
    return -1;
}

/* Lock-table layout (same firmware, same RAM as the proven reference):
 * step row at pattern + 0x1e80 + track*10319 + step*160, word index =
 * sound slot, -1 = none. */
#define LOCK_TAB_OFF    0x1e80u
#define LOCK_TRACK_STR  10319u
#define LOCK_STEP_STR   160u

static int32_t lock_read(void *pattern, int32_t track, int32_t step,
                         int32_t slot)
{
    uint8_t *row;
    int16_t v;
    if (!pattern || step < 0 || step > 63 || slot < 0 || slot > 78)
        return -1;
    row = (uint8_t *)pattern + LOCK_TAB_OFF + (uint32_t)track * LOCK_TRACK_STR +
        (uint32_t)step * LOCK_STEP_STR;
    v = *(volatile int16_t *)(row + (uint32_t)slot * 2u);
    return (int32_t)v;
}

static void shown_state(uint8_t *kit, int32_t track, int32_t page,
                        int32_t *vals, int32_t *lockedmask)
{
    int32_t step = held_step();
    int32_t locked = 0;
    void *pattern = 0;
    int32_t i;
    if (step >= 0 && sophie_edit_rec)
        pattern = *(void *volatile *)PATPTR_AT;
    for (i = 0; i < 8; i++) {
        int32_t k = (page - 1) * 8 + i, slot, v;
        slot = knob_slot(k);
        v = slot < 0 ? 0x7fffffff : FW_SOUND_SLOT(kit, track, slot);
        if (step >= 0 && pattern && slot >= 0) {
            int32_t lv = lock_read(pattern, track, step, slot);
            if (lv != -1) {
                v = lv;
                locked |= 1 << i;
            }
        }
        vals[i] = v;
    }
    *lockedmask = locked;
}

void sophie_draw(void *bmp, void *ctrl)
{
    int32_t track = fw_active_track, mid, page = 0, pos;
    uint8_t *kit;
    void *bottom, *top, *mgr, *view;
    int32_t colw, rowh, r, c;
    int32_t key = *(volatile int32_t *)KEYVAR_AT;
    (void)ctrl;
    sophie_frames++;
    if (!sophie_brain)
        return;
    bottom = *(void *volatile *)((uint8_t *)sophie_brain + BRAIN_BOTTOM);
    top = *(void *volatile *)((uint8_t *)sophie_brain + BRAIN_TOP);
    if (!bottom || bottom != top)
        return;
    if (track < 0 || track > 3)
        return;
    kit = fw_kit;
    if (!kit)
        return;
    mid = FW_SOUND_SLOT(kit, track, 0) >> 8;
    if (mid != SOPH_ID)
        return;
    if (key != 1 && key != 2)
        return;
    page = key;
    mgr = *(void *volatile *)((uint8_t *)bottom + SCREEN_MGR);
    if (!mgr)
        return;
    view = *(void *volatile *)((uint8_t *)mgr + MGR_VIEW);
    if (!view)
        return;
    pos = *(volatile int32_t *)((uint8_t *)view + VIEW_CUR);
    if (pos != 0)
        return;
    if (msg_suppressed())
        return;
    {
        int32_t vals[8], locked = 0;
        shown_state(kit, track, page, vals, &locked);
        /* Clear from CYT (above the grid): covers the stock RATIO group
         * heading whose "A" top otherwise bleeds through one line under
         * the title bar (seen in the emulator screenshot; hardware status
         * unconfirmed). CYT=11, not higher: the title/tempo backgrounds
         * reach row 10 on hardware, so 9 cut them. Grid cells stay
         * anchored at CY0; only the clear reaches higher. */
        fw_fillrect(bmp, CXC, YB(CY1), CX1, YB(CYT), 0);
        colw = (CX1 - CX0 + 1) / COLS;
        rowh = (CY1 - CY0 + 1) / ROWS;
        for (r = 0; r < ROWS; r++) {
            for (c = 0; c < COLS; c++) {
                int32_t k = (page - 1) * 8 + r * COLS + c;
                int32_t x, yt, sel, li;
                x = CX0 + c * colw;
                yt = CY0 + r * rowh;
                li = r * COLS + c;
                sel = k - (page - 1) * 8 == sophie_enc &&
                    sophie_enc_page == page && sophie_ticks - sophie_enc_at < 45;
                draw_cell(bmp, k, x, yt, colw - 2, vals[li], sel,
                          (locked >> li) & 1);
            }
        }
    }
}

int sophie_key_handler(void *brain, void *ev)
{
    int32_t id, flags;
    if (brain)
        sophie_brain = brain;
    if (!ev)
        return 0;
    id = *(volatile int32_t *)((const uint8_t *)ev + 12);
    flags = *(volatile int32_t *)((const uint8_t *)ev + 16);
    if (id == 1) {
        if (flags & 0x10)
            sophie_func_down = 0;
        else
            sophie_func_down = 1;
    } else if (id == 4) {
        if (flags & 0x10)
            sophie_ptn_down = 0;
        else
            sophie_ptn_down = 1;
    } else if (id == 19) {
        if (flags & 0x10)
            sophie_page_down = 0;
        else
            sophie_page_down = 1;
    }
    if (id == 10) {
        if (flags & 0x10) {
            sophie_rec_down = 0;
        } else {
            sophie_rec_down = 1;
            if (flags == 1 && !sophie_func_down && !sophie_ptn_down &&
                !sophie_page_down && !sophie_trigs) {
                if (sophie_live) {
                    sophie_live = 0;
                    sophie_edit_rec = 1;
                } else {
                    sophie_edit_rec ^= 1;
                }
            }
        }
    } else if (id == 11 && flags == 1) {
        if (sophie_rec_down) {
            sophie_live = 1;
            sophie_edit_rec = 0;
        } else if (sophie_live) {
            sophie_live = 0;
        }
    } else if (id == 12 && flags == 1) {
        if (sophie_rec_down) {
            sophie_live = 0;
            sophie_edit_rec = 1;
        } else if (sophie_live) {
            sophie_live = 0;
        }
    }
    if (id >= 26 && id <= 41) {
        if (flags & 0x10)
            sophie_trigs &= ~(1u << (uint32_t)(id - 26));
        else
            sophie_trigs |= 1u << (uint32_t)(id - 26);
    } else if (id >= 20 && id <= 25 && (flags & 1)) {
        sophie_trigs = 0;
    }
    if (id >= 42 && id <= 45 && (flags & 1)) {
        sophie_trigs = 0;
        sophie_func_down = 0;
        sophie_ptn_down = 0;
        sophie_page_down = 0;
        sophie_rec_down = 0;
    }
    if ((id == 10 || id == 11 || id == 12) && !(flags & 0x10)) {
        if (sophie_func_down || sophie_ptn_down || sophie_page_down || sophie_trigs ||
            ((id == 11 || id == 12) && sophie_rec_down))
            sophie_msg_until = sophie_ticks + SOPHIE_MSG_HOLD;
    }
    return 0;
}

int sophie_enc_handler(void *brain, void *ev)
{
    int32_t enc, delta, key;
    if (brain)
        sophie_brain = brain;
    if (!ev)
        return 0;
    enc = *(volatile int32_t *)((const uint8_t *)ev + 12);
    delta = *(volatile int32_t *)((const uint8_t *)ev + 16);
    (void)delta;
    if (enc < 1 || enc > 8)
        return 0;
    key = *(volatile int32_t *)KEYVAR_AT;
    if (key != 1 && key != 2)
        return 0;
    sophie_enc = enc - 1;
    sophie_enc_page = key;
    sophie_enc_at = sophie_ticks;
    return 0;
}

static int32_t tick_valid;
static int32_t tick_track = -1;
static int32_t tick_key = -1;
static int32_t tick_step = -1;
static int32_t tick_vals[8];
static int32_t tick_locked;
static int32_t tick_enc = -1;
static int32_t tick_enc_page = -1;
static int32_t tick_hl;

void sophie_tick(void *ctrl)
{
    int32_t track = fw_active_track, mid, key, page, step, i, hl, locked = 0;
    int32_t dirty = 0;
    int32_t vals[8];
    uint8_t *kit;
    sophie_ticks++;
    if (!ctrl)
        return;
    if (sophie_brain) {
        void *bottom = *(void *volatile *)((uint8_t *)sophie_brain + BRAIN_BOTTOM);
        void *top = *(void *volatile *)((uint8_t *)sophie_brain + BRAIN_TOP);
        if (!bottom || bottom != top) {
            sophie_trigs = 0;
            sophie_func_down = 0;
            sophie_ptn_down = 0;
            sophie_page_down = 0;
            sophie_rec_down = 0;
        }
    }
    sophie_reap();
    if (track < 0 || track > 3)
        return;
    kit = fw_kit;
    if (!kit)
        return;
    mid = FW_SOUND_SLOT(kit, track, 0) >> 8;
    if (mid != SOPH_ID) {
        tick_valid = 0;
        return;
    }
    key = *(volatile int32_t *)KEYVAR_AT;
    if (key != 1 && key != 2) {
        tick_valid = 0;
        return;
    }
    if (msg_suppressed()) {
        tick_valid = 0;
        return;
    }
    page = key;
    step = held_step();
    shown_state(kit, track, page, vals, &locked);
    hl = sophie_enc >= 0 && sophie_enc_page == page &&
        sophie_ticks - sophie_enc_at < 45;
    if (!tick_valid || track != tick_track || key != tick_key ||
        step != tick_step || locked != tick_locked ||
        sophie_enc != tick_enc || sophie_enc_page != tick_enc_page || hl != tick_hl)
        dirty = 1;
    else {
        for (i = 0; i < 8; i++) {
            if (vals[i] != tick_vals[i]) {
                dirty = 1;
                break;
            }
        }
    }
    if (!dirty)
        return;
    tick_valid = 1;
    tick_track = track;
    tick_key = key;
    tick_step = step;
    tick_locked = locked;
    for (i = 0; i < 8; i++)
        tick_vals[i] = vals[i];
    tick_enc = sophie_enc;
    tick_enc_page = sophie_enc_page;
    tick_hl = hl;
    *((volatile uint8_t *)ctrl + 0x20) = 1;
}
