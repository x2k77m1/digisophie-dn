# Digisophie-DN

A port of the Digitakt 1 mod [Digisophie](https://github.com/soejrd/digisophie)
to the Digitone 1.

SOPHIE metallic percussion as a Digitone mk1 / Keys machine (id 11, `SOPH`),
OS 1.43 and 1.44. Four models — FUSE, BOOM, PIPE, SHARD — through the
Digitone's own multimode filter, mixer, effects and AMP envelope, like FM.
The fixed-point DSP core (`sophie.c`/`sophie.h`) is shared with the Digitakt
mod; the machine adapter in `digisophie-dn/` is Digitone-only.

<strong>USE AT YOUR OWN RISK.</strong> This modifies your instrument's
firmware; back up your projects and sounds, and keep a stock OS file for
recovery.

## Controls

SYN1 + SYN2 A. All knobs borrow stock SYN ranges (no custom params).

| Knob | Control | What it does |
| --- | --- | --- |
| SYN1 A | OCT | Octave -2..+2 |
| SYN1 B | MODEL | FUSE / BOOM / PIPE / SHARD |
| SYN1 C | FOLD | Wavefolder 0-100%; 0 bypasses it bit-exact |
| SYN1 E | SWEEP | Bipolar pitch sweep toward the played note |
| SYN1 F | METAL | FM/ring intensity |
| SYN1 G | FBK | Oscillator feedback |
| SYN1 H | COLOR | Inharmonic character |
| SYN2 A | TUNE | Fine tune, about +-100 cents |

Polyphony: newest-wins per track (one voice each, up to 4 total — tracks
never choke each other); releases free immediately; UI-tick reap clears
unobserved ends. System LFOs reach all SYN slots; FILTER/AMP dests apply
downstream and are ignored in the render, as on FM voices.

Performance (hardware-tested, stated conservatively): one SOPHIE track
runs clean; two tracks are practical. Three or four SOPHIE tracks will
load and sound, but rapid sequencer notes on every track, heavy
modulation, p-locks and heavy FX can saturate the DSP, heard as audio
glitches and a sluggish UI while audio otherwise stays in time. More than
two simultaneous SOPHIE tracks is therefore possible but **not claimed**,
mirroring the Digitakt mod's tested one-or-two guidance. If you need
tutti, keep patches lean (low FOLD/FBK, fewer locked voices) and stop the
sequencer before editing pages.

## Install: no compiler required

You need only the prebuilt mod for your OS (`release/digisophie-dn-0.1g.elemod`
for 1.43, `release/digisophie-dn-0.1g-os1.44.elemod` for 1.44),
[elekloader](https://github.com/irpina/elekloader/releases/latest),
the `core-dn1` + `machines-dn1` mods it requires, and your own stock
Digitone mk1 OS 1.43 or 1.44 `.syx`. The `.elemod` contains this project's
code, **not** Elektron's firmware.

1. Open elekloader and select your stock OS using **Change stock firmware**.
2. Install `core-dn1`, `machines-dn1`, then `digisophie-dn` from file and enable
   all three. Elekloader refuses a mod built for the other OS version.
3. Wait for elekloader's **Ready to build** check, set the four-character
   OS version, then choose **Build Firmware**. Save the generated `.syx`.
4. Send that `.syx` to the Digitone with Elektron Transfer.

Elekloader builds and verifies the OS; **Elektron Transfer does the actual
upload**. Do not power off during the update. Neither the stock nor the
modified OS file belongs in this repository.

## Build from source (developers only)

You need Python 3.9+, a source checkout of
[elekloader](https://github.com/irpina/elekloader) at `tools/elekloader`
(gitignored, reference only), a ColdFire cross-toolchain (`m68k-elf-`
assembler, GCC and linker), and your own stock OS files. From this repo:

```sh
ELEKLOADER_CROSS=m68k-elf- sh scripts/build-digisophie-dn.sh \
  /path/to/Digitone_OS1.43.syx /path/to/Digitone_OS1.44.syx 0.1g
```

This builds core-dn1, machines-dn1 and digisophie-dn from source for each
OS, lints each combination, and writes verified test images to `out/`
(`out/digisophie-dn-1.43-0.1g.syx`, `out/digisophie-dn-1.44-0.1g.syx`). Stock `.syx`
files are build inputs only and are never committed.

Host DSP checks without firmware:

```sh
make test
python3 tools/sophie_dn_host_test.py
python3 tools/sophie_engine_host_test.py  # needs tools/elekloader headers
```

## Use and recovery

Back up before flashing. If anything misbehaves, re-send your stock OS
`.syx` with Elektron Transfer; the bootloader is never touched so the unit
always recovers via stock. Never flash from the repo directory; test images
live in `out/` only.

## Source layout

* `sophie.c`, `sophie.h`, `sophie_sin_table.inc` — shared fixed-point DSP
  core, also used by the Digitakt mod. Upstream fixes here merge cleanly;
  Digitone-only behavior lives in the adapter, not the core.
* `digisophie-dn/engine.c` — machine descriptor, knob map, render,
  per-track fuse, LFO plumbing, Q-scale to `DNM_PEAK`.
* `digisophie-dn/overlay.c` — SYN1/SYN2 full-redraw overlay (tick/draw/key/enc).
* `digisophie-dn/pages.s` — empty anchor; no page-constructor patches needed
  because all knobs borrow stock ranges.
* `digisophie-dn/mod.json` — device `digitone-mk1`, machine 11, zero sites.
* `scripts/build-digisophie-dn.sh` — reproducible build/lint/patch for both OSes.
* `scripts/keys-sophie*.txt` — emulator feature scripts.
* `tools/sophie_*_host_test.py` — host harnesses (no firmware).
* `docs/digitone-mod-template.md` — the Digitone machine-mod spec this port follows.

## Source and licenses

MIT licensed, see [LICENSE](LICENSE). The Sophie algorithm origin and
attribution are documented in [THIRD_PARTY.md](THIRD_PARTY.md). This project
is independent of, and not endorsed by, Elektron or the estate of SOPHIE. It
contains no Elektron firmware or samples.
