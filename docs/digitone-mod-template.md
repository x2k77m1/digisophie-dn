# Template: a new sound-engine mod for the Digitone mk1

A start-to-finish specification for building a machine mod like VAWT:
a new sound engine picked per track from MACHINES, with its own SYN
pages, parameters, locks, and polyphonic render. Read top to bottom the
first time; afterwards use the checklists (§2, §9, §10) per milestone.

Conventions: `fw_*` names resolve against core-dn1 at link time (see
`elekloader/sdk/include/digitone-mk1/core3.h`); numeric addresses below
are 1.43 examples — always re-derive per OS version, never hard-code.
`mods/dn-vawt/` is the worked reference for every section.

## 1. What a machine mod owns (and what it doesn't)

- One sound per track: the machine id lives in sound slot 0 (`id << 8`,
  0 = FM). The voice loads it per note, so sound locks switch engines
  per trig with no extra work.
- Every audio block, the companion calls your `render` for each voice
  playing your sounds, replacing the FM block. The Digitone's filter,
  mixer, effects and AMP envelope apply afterwards, as for FM.
- Parameters stay the *stock* parameters': values live in sound slots
  with stock ranges, so p-locks, MIDI CC, LFOs and presets work as on
  FM. You relabel and reformat them, and may add your own parameter
  records (§5).
- SYN1/SYN2 first pages become yours via a full redraw overlay (§6);
  second pages, rail, title and status stay stock. Turn popups keep
  stock long names (core limit — document, don't fight it).
- You never touch the bootloader, flash layout, DSP image, or project
  save format. Never write flash; never commit firmware-derived files.

## 2. Starting tree and milestones

```
mods/<name>/
  mod.json      §3 (id, sites, events, contributions, resources)
  engine.c      §4 (descriptor, knobs, render, formatters, defaults)
  overlay.c     §6 (ev_tick/ev_draw/ev_key/ev_enc)
  pages.s       §5 (page-constructor patches, if you need own params)
  README.md     controls, signal path, status, build, files
tools/<name>_host_test.py   §10 (DSP tests, no firmware)
scripts/build.sh            §9 (build + lint + patch)
docs/evidence.md            §11 (running proof log)
```

Milestones: (a) host-tested render → (b) boots in fwcheck vs stock →
(c) pages/labels/turns in emulator → (d) hardware sound + photos →
(e) locks/polyphony/edge cases → (f) distribution artifacts. Do not
skip (a)–(c): hardware flashes are for confirmation, not discovery.

## 3. mod.json, field by field

```json
{
  "id": "dnxyz", "version": "0.1a", "ele3_version": "0.1a",
  "device": "digitone-mk1", "os": "1.43",
  "sources": ["engine.c", "overlay.c", "pages.s"],
  "cflags": ["-DFOO_AT=0x..."],
  "ports": {"1.44": {
    "cflags": ["-DFOO_AT=0x..."],
    "defsym": {"PAGE_X_REC": "0x..."},
    "sites": [{"addr": "0x...", "stock": "<exact 6+ hex bytes>",
               "op": "jsr", "target": "my_hook"}]}},
  "defsym": {"PAGE_X_REC": "0x..."},
  "sites": [ ... same for 1.43 ... ],
  "subscribe": [
    {"event": "ev_tick", "fn": "my_tick", "order": 60},
    {"event": "ev_draw", "fn": "my_draw", "order": 60},
    {"event": "ev_key", "fn": "my_key_handler", "order": 60},
    {"event": "ev_enc", "fn": "my_enc_handler", "order": 60}],
  "contribute": [
    {"to": "dn_machines", "order": 50, "data": "00000000",
     "relocs": [[0, "abs32", "sym:my_machine", 0]]},
    {"to": "core_params", "order": 50, "data": "00000000",
     "relocs": [[0, "abs32", "sym:my_param", 0]]}],
  "resources": {"names": ["dnmachine:<id>", "param:<id>", ...],
                "core": "3.1"},
  "requires": ["core", "machines"]
}
```

- `version`/`ele3_version`: exactly 4 characters (ELE3-exact); the unit
  shows `ele3_version`, not the OS number. Bump per shipped change.
- `ports`: every new OS version needs its addresses re-derived
  (§5: opcode scan, never assume ±offset — verify bytes).
- `sites`: a `jsr` needs an exact stock-instruction fit (6 bytes for
  `move.l dN,(abs).L`); the build refuses on stock-byte mismatch, which
  is your safety net. Prefer hook-bus events over sites wherever one
  exists.
- `resources`: claim `dnmachine:<id>` (1–127, unique) and any
  `param:<id>` you add (182–184 are the core_params slots). Two builds
  claiming the same id/exports are *refused* — exactly one of your mod
  per image, which also governs custom user builds.
- `subscribe` order 60 puts you after stock, before/after peers by
  number; never take (`return nonzero`) events you only observe.

## 4. The engine

```c
const struct dnm_machine my_machine = {
    ID, "NAME", icon_or_0, my_knobs, my_render, 0, my_defaults,
};
```

- Knobs: 16 entries (`DNM_KNOBS`), SYN1 A–H then SYN2 A–H. Each is
  `{label (≤5 chars, 0 = unused), fmt or 0, look}`. `look` borrows a
  stock knob graphic (0 = plain); `fmt(value 8.8, buf)` writes the value
  text. Values are the stock parameters' (see `knob_id`: SYN1 107–114,
  SYN2 119–126) — design your mapping *inside* stock ranges.
- `render(voice 0–7, block[32] Q1.31, voice*)`: runs at **interrupt
  level, 1500 blocks/s (~667 µs)**. Rules: no unbounded loops, no
  allocation, no file/blocking calls, bounded stack. Prefer shifts over
  divides; split lerp shifts so `(diff * frac)` never overflows 32 bits.
  gcc `-mcpu=54455 -O2` emits plain `muls/mulu` (no EMAC) — verify with
  `objdump`, don't assume. Fresh-note init on `v->fresh`; keep phases
  advancing through zero-skips so mid-note turns stay continuous.
- `struct dnm_voice`: pitch word, velocity 1–127, fresh, track, gate.
  Write ~`DNM_PEAK` peaks to match FM loudness at defaults.
- `defaults[16]`: 8.8 values within each knob's stock range, `-1` to
  leave a knob alone. Set on machine pick via `core_sound_set`.
- Flags: leave 0 (companion applies the AMP envelope + velocity). Only
  set `DNM_OWN_ENV` if you fully own envelopes.

## 5. Parameters: borrow stock, or mint your own

- **Borrow** when the stock range fits: pick the stock id whose slot and
  range suit the knob (SYN1 slots 17–24, SYN2 27–34; each knob's slot is
  fixed). Everything (locks, LFO, CC, presets) then works untouched.
- **Mint** (`param:182–184`) when it doesn't (e.g. 5 octaves on a 0–2
  knob): add a `struct core_param` (clone a same-shaped stock record;
  same sound slot, wider min/max) and reroute the page-constructor knob
  slot to it with a `jsr` site. Recipe that worked twice:
  1. opcode-scan the firmware for `move.l dN,(abs).L` (`23Cx…`) writing
     the knob's record address; confirm by the preceding `moveq #id,dN`;
  2. routine sets the new id (or the stock id for non-machine tracks —
     check the kit's slot 0 so FM behavior is untouched);
  3. only `move`-class instructions besides branches, or save/restore
     CCR (the displaced `move.l` left it untouched);
  4. `defsym` the record address + `sites` entry per OS; build refuses
     on mismatch.
- Consequences to document, not fix: LFO destination lists are
  stock-built (new params route by slot, verify audibly); turn popups
  keep stock long names; give new params no MIDI CC (`-1`) unless
  you've traced the CC table — a shared CC cross-talks.
- Re-pick/preset note: changing a knob's meaning or range strands old
  sounds — README must say to re-pick the machine (or re-set knobs).

## 6. Display: the overlay recipe

- Paint the content rect on `ev_draw` *after* firmware, gated on
  **firmware state, never pixels**: machine id on the active track,
  showing key (SYN1/SYN2 address), no screen over the pages (key
  dispatcher's brain bottom==top screens), shown view on place 0.
  Pixel probes rot with titles/values — deleted on sight.
- `ev_tick` re-arms the frame **only on change** (track, key, values,
  locks, encoder highlight + expiry). Re-arming every tick starves UI
  handling on hardware (turns feel buffered). Signature-compute, then
  set the dirty flag exactly once per change.
- `ev_key`/`ev_enc` remember the brain; derive the encoder cell from
  the *showing* key, not the last frame. Never take observed events.
- Key facts (Digitone mk1): page keys 20–25, trigs 26–41, tracks 42–45,
  FUNC 1 / PTN 4 / RECORD 10 / PLAY 11 / STOP 12 / PAGE 19; flags `1`
  press, `9` held, `0x10` release. The Mod Menu and bank/PTN popups eat
  releases — track edge states, sweep them on **every tick** (draws only
  run on dirty frames; a sweep in draw can't clear idle stuck bits),
  and drop edge states on track presses (menu opens eat without any
  brain-screen change).
- Toasts (copy/paste/clear) draw in the *same* view: yield the frame
  ~90 ticks on chorded RECORD/STOP/PLAY; bare presses never yield.
- Record modes for lock display (manual 10.2/10.9.1): track OFF /
  GRID / LIVE / STEP from key chords (RECORD toggles, REC+PLAY live,
  REC+STOP step, PLAY/STOP exit live); show locks only in GRID/STEP.
- Lock-table layout (hardware-proven, cross-confirmed): step row at
  `pattern + 0x1e80 + track*10319 + step*160`, **word index = sound
  slot**, `-1` = none, count at word 79; pool headers in the project
  mirror are slot+track, live-first. Up to 80 distinct params/pattern.
- Steps address the shown page 1:4 until someone maps the page state.

## 7. Polyphony and CPU

- Budget math: SINE costs ~3k instructions/voice/block (per core docs);
  size your worst case (all knobs hot × 8 voices) against 667 µs and
  confirm with fwcheck's render margin (needs 10%+).
- Diet first: skip zeroed stages as *bit-exact* identities (prove with
  host tests, which must stay green).
- Fuse second: cap simultaneously rendering voices (VAWT: 4 global);
  oldest keep slots, excess write silence, releases free immediately,
  unobserved ends age out on the UI tick (a render-side clock freezes
  with zero callers — the exact failure that once muted the
  sequencer). Dropping notes beats bricking the box, always.
- Band-limit though: naive SAW/SQUARE/PULSE alias badly high up
  (measured −14dB at 1760Hz); PolyBLEP the edges (division only adjacent
  to one) and pin it with host FFT alias assertions. Wavetable
  mip-maps don't fit flash — keytracked filtering instead.
- Zero-voice cost must be ~zero (companion already skips silent
  voices); never do per-block work without sounding voices.

## 8. Input handling rules (hard-won)

- Press edges: act on `flags == 1` exactly (hold repeats must not
  retoggle). Releases: `flags & 0x10`. Chords need all member states,
  which you track — with the sweep (§6) as backstop.
- Copy/paste/clear chords share RECORD/STOP/PLAY with transport and
  record-mode entry: disambiguate by modifiers (FUNC/PTN/PAGE/held
  trig/held RECORD), never by timing.
- Keyboard vs step holds are indistinguishable at the key level —
  record mode is the only signal (§6).

## 9. Build, lint, patch, budgets

```sh
export ELEKLOADER_CROSS=m68k-elf-
python3 -m elekloader.sdk.build <coremod> --stock $STOCK
python3 -m elekloader.sdk.build <machinesmod> --stock $STOCK
python3 -m elekloader.sdk.build mods/<mine> --stock $STOCK
python3 -m elekloader.lint <mine.elemod> --stock $STOCK \
  --with <core.elemod> --with <machines.elemod>
python3 -m elekloader.patch --stock $STOCK --mod ... --out test.syx \
  --version XXXX
```

- Budgets: mod RAM 128KB shared (`.run` is RAM-copied — there is **no**
  flash-resident section, so large const tables count against RAM, not
  flash); `.bss` likewise. The link refuses over-budget; keep ≥10KB
  headroom. Flash had ~1.6MB spare — irrelevant for `.run`.
- `.gitignore` must cover `*.syx *.bin *.elemod out/ sections/`
  (firmware-derived, Elektron copyright), stock OS files, vendor
  manuals, and any user-supplied sample/content inputs.
- Never flash from the repo directory; test images only. The unit
  always recovers via stock `.syx` (bootloader never touched).

## 10. Verification ladder

1. **Host tests** (no firmware): compile the engine natively with
   stubbed helpers; drive `render` through waveforms, knobs, levels,
   bounds, morphs, clamps. Gate every data change on them (custom
   tables included — the tests compile the generated header).
2. **fwcheck** (`--no-timing` for speed, full timing for release):
   build PASS with no new violations (record pre-existing stock ones
   once); first screen difference exactly where intended; audio differs
   only where intended; margins recorded.
3. **Emulator screenshots**: grids, turns, holds, toasts, second pages.
   Known limits: scripted turns don't move values (both builds);
   crafted RAM state may not move stock display.
4. **Hardware**: sound, pages, turns, holds, transport, menus. Photograph
   everything non-obvious; prefer raw-state debug screens over prose.

## 11. Debugging playbook

- Reproduce before theorizing; one variable per flash.
- Prefer **state screens** over descriptions: a debug build that prints
  raw table values + internal bits while held answers mapping questions
  in one photo (pool headers, row words, kit reference; recorder bits,
  last key id/flags, modifier nibble).
- Photo protocols: fixed sequence, fresh boot/power-cycle when stuck
  voice state is suspected (stuck gates survive soft reboots via the
  DSP side — power off/on to clear).
- Diff against stock at every step (fwcheck compare); any new violation
  is yours until proven otherwise.
- Keep an evidence log per version: what changed, proof commands and
  their output, what remains unverified. Mark assumptions explicitly
  (e.g. "slot 33 by induction, hardware turn-test pending") and close
  them.

## 12. Distribution

- Ship `.elemod` (+ manifest: sources, stock checksum, timestamp) and
  optionally built `.syx`; users combine in elekloader. Enforce
  exactly-one-of-your-mod per build (shared claims refuse).
- Custom-content builds (user data baked at build time) reuse the same
  gates: validation up front, generated header, sdk.build in a temp
  copy (tree stays clean), host tests against the custom header,
  lint+patch. Deterministic: same inputs → same bytes.
- Docs hygiene for release: no brand names for formats (state the exact
  spec), no local paths, factory vs custom content described
  neutrally, user inputs gitignored. User-supplied content requires
  rights clarity before it ships in factory artifacts.
- Licence: GPL-2.0-or-later, unofficial, not affiliated with Elektron.

## 13. Pitfall catalog (all hit building VAWT)

| symptom | cause | fix |
|---|---|---|
| overlay shows seconds then vanishes | pixel probes in title text | gate on firmware state only |
| overlay needs extra SYN press / blinks | no `ev_tick` re-arm | change-signature tick, re-arm once |
| sluggish turns, worse with voices | tick re-arms every frame | re-arm only on change |
| lock display shows kit / wrong cells | positional row reads | slot-indexed reads, `-1` = none |
| locks visible outside REC | holds tracked without mode | OFF/GRID/LIVE/STEP state machine |
| locks never show after working | stuck edge bit blocks toggle | sweep in tick + clear on track press |
| sequencer silent, keyboard sounds | reap clock froze with zero callers | free on release, reap on UI tick |
| 7–8 notes wedge the box | block overrun starves UI | polyphony fuse + diet |
| toast hidden under overlay | same-view message | 90-tick yield on chords |
| stray pixels at grid edge | animated stock chrome under clear rect | widen clear to measured rail-safe edge |
| highlight bar crosses value bar | shared routine geometry | frame over content, shortened above bars |
| build RAM refusal | const tables in `.run` | fit ~24 banks; rebuild sets |
| silent/broken custom build | bad wav / over-cap / missing toolchain | fail-fast validation + host-test gate |
