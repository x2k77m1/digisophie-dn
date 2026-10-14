# digisophie-dn — Sophie metallic percussion for Digitone mk1

SOPHIE (machine 11, `SOPH`) for the Digitone mk1 / Keys, OS 1.43 + 1.44:
FUSE/BOOM/PIPE/SHARD two-operator metallic percussion through the
Digitone's own multimode filter, mixer, effects and AMP envelope, like FM.

- DSP core (reusable, fixed-point): `../sophie.c`, `../sophie.h`,
  `../sophie_sin_table.inc` (MIT, ported from the Digitakt `digisophie` mod).
- Digitone spec: `../docs/digitone-mod-template.md`. Host tests:
  `../tools/sophie_dn_host_test.py` (core via DN mapping),
  `../tools/sophie_engine_host_test.py` (render through stubbed companion).
- This dir holds the Digitone adapter: `mod.json` (device digitone-mk1,
  `dnmachine:11`, zero sites — all knobs borrow stock ranges),
  `engine.c` (machine descriptor + render + fuse), `overlay.c` (SYN pages),
  `pages.s` (empty anchor; no page-constructor patches needed).

Controls (SYN1 + SYN2 A): A OCT -2..+2, B MODEL FUSE/BOOM/PIPE/SHARD,
C FOLD 0-100% (36 detents on the borrowed 0-35 range), E SWEEP bipolar
(compressed on the borrowed range), F METAL, G FBK, H COLOR, SYN2 A TUNE
+-100 cents. FOLD 0 is a bit-exact bypass. Polyphony: newest-wins per
track (one voice each, up to 4 total — tracks never choke each other);
releases free immediately; UI-tick reap clears unobserved ends. System
LFOs reach all SYN slots; FILTER/AMP dests apply
downstream and are ignored in the render, as on FM voices.

Performance (hardware-measured, accepted): one voice per track
(newest-wins within a track; tracks never choke each other). About two
concurrent SOPHIE voices saturate the main CPU (~96% with 2 SOPHIE + 2 FM
sequenced, SYSTEM INFO), so three dense tracks with effects can glitch —
thin tutti arrangements accordingly, and more than two simultaneous SOPHIE
tracks is possible but not claimed. UI can feel sluggish under heavy
sequencer + lock load while audio stays clean (under investigation).

Signal path per voice/block: knobs+LFO → `ds_params` (pitch from
`dnm_phase_inc` >> 16) → `ds_voice_render` (16.16) → `ds_fold_block` →
>> 2 to Q1.31 ~`DNM_PEAK` → companion AMP envelope + velocity. Flags 0:
the companion owns AMP and velocity. Zero voices cost ~zero (companion
skips silent voices; sleeping Sophie voices write zeros).

Status: host harnesses green (24/24 core, 24/24 engine), cross-builds
clean, `digisophie-dn-0.1g` builds/lints/patches for 1.43 + 1.44. fwcheck
margins + emulator screenshots recorded, hardware tested through 0.1e.

Build (local only, stock `.syx` supplied by absolute path, never
committed): `sh scripts/build-digisophie-dn.sh STOCK43 [STOCK44 [MODVER]]`
with `ELEKLOADER_CROSS=m68k-elf-`; test images land in `out/` only.
Never flash from the repo directory; the unit always recovers via stock.
