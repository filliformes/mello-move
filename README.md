# Mello

**Dual-bank tape instrument for [Ableton Move](https://www.ableton.com/move/).**
My dream super Mellotron.

Built for the [Schwung](https://github.com/charlesvestal/schwung) framework
([Move Everything](https://moveeverything.com)). Part of the open-source
lutherie banner **Filliformes**.

> Physical-model Rotary speaker. Per-style tape saturation. Crybaby-style
> Auto-Wah. WoFi-inspired granular Texturizer with vintage analog warmth.
> 6 selectable FX-chain orderings. Sustain loop with Blackbox-style
> Crossfade %. Auto Retune with MIDI-table-verified pitch correction.
> Per-bank auto level-match so different sample sets play at consistent
> loudness. MIDI mod wheel / aftertouch / pitch bend / panic-CC support.

---

## What it is

Mello is a **sound generator** module for Ableton Move. It loads a folder
of WAV samples as one bank — one note per file, filename = pitch — and
plays them back with polyphonic varispeed, an authentic tape signal
chain, and a curated 8-effect FX rack. Two banks (A and B) layer and
crossfade equal-power so you can blend different Mellotron tapes against
each other on one keyboard.

This is an **in-house WAV sampler** — no sfizz, no xsynth, no SFZ
authoring required. Drop note-named WAVs in a folder, the plugin
recognises them.

## Sample libraries

Mello's `instruments/` folder is organised by **library**:

```
instruments/
  LL/                          ← Leisureland set (community archive)
  SB/                          ← Sonic Bloom Mellotron pack
  90/                          ← 90s Mellotron Archive CD
  YourCustomSet/               ← drop your own here
```

A free-circulation Leisureland Mellotron set, the Sonic Bloom Mellotron
pack, and a curated 35-note slice of the 90s Mellotron Archive CD work
out of the box (~85 banks, ~3000 WAVs total). The plugin auto-discovers
any library folder you create — switching the **Samples Folder** menu
param quick-jumps Bank A and B to the first bank of that library.

> **Sample WAVs are not bundled in this repository** — they're
> community-archived Mellotron recordings and not the author's to
> redistribute. See *Loading your own banks* below for the formats Mello
> accepts; the Leisureland and Mellotron-Archive sets are widely
> available in the community.

## Features

### Sampling
- **Dual-bank A/B layering** with equal-power crossfade
- **Async bank loading** — switching banks doesn't glitch the audio thread
- **3 filename conventions** auto-detected: note-name (`A2.wav`,
  `Bb4.wav`, C4 = 60), bare MIDI number (`60.wav`), Leisureland
  sequential (`prefix-N.wav`, configurable base)
- **Auto Retune** *(off by default)* — bakes a 12-TET MIDI→frequency
  table, autocorrelates each sample with parabolic-interp sub-sample
  precision, applies a pitch correction only if a re-detect pass
  confirms it. Conservative bounds in the low range so naturally-warbly
  Mellotron content isn't "fixed" into blandness.
- **Auto level-match** — every bank is peak-scanned on load and
  normalised to a target headroom so libraries play at consistent
  loudness and the distortion stages have room to be driven hard
- **Sustain loop** *(on by default)* with auto-detected loop points +
  **Crossfade %** parameter (0–100 %, ~5–130 ms, 1010 Blackbox-style)
- **Reverse**, **Sample Start**, **Half Speed** toggles
- **Map modes**: *Tape (exact)* — Mellotron-authentic chromatic fallback
  to nearest sample; *Full range (stretched)* — every key gets a sample
  even ±4 octaves away

### Tape stage
- **Per-style saturation curves**: Off (bypass), Clean (gentle tanh),
  M400 (cubic / 3rd-harmonic emphasis), MkII (asymmetric tanh / tube
  warmth), M300 (classic tanh), Worn (asym + cubic blend, hot)
- **Tape head bump** — 120 Hz peaking biquad, depth varies per tape
  style
- **Wow + flutter** LFOs with per-voice pitch modulation
- **Mechanical noise** — sub-300 Hz transport rumble + occasional click
- **Degrade** — signal-gated hiss + random dropouts + crackle + age
  ducking
- **Chiff** — brief tape-retract noise burst on every note-off
- **Bias** — bipolar tape-bias offset (low = harsh / more drive,
  high = darker)

### Preamps
- Off (bypass), Clean DI, MkII Tube, **M400 Solid-State (default)**,
  SK-1 Lo-fi

### 8-effect FX rack — knob order
On the FX page, the eight knobs are:

| # | FX | What it is |
|---|---|---|
| 1 | **Auto-Wah** | Crybaby-style envelope-controlled BPF (450 Hz – 4.6 kHz, Q 6.5 → 2) with two-tier gain comp |
| 2 | **Vibrato** | 3-voice chorus / vibrato with incremental quadrature LFOs |
| 3 | **Tremolo** | Sine ↔ trapezoid morph, three-zone curve (0.5 → 2 → 11 → 22 Hz) |
| 4 | **Rotary** | Physical-model rotary speaker — geometric doppler via variable delay length (LeslieSim + Surge XT derived), 800 Hz crossover, AM via inverse-distance. Two-zone rate curve: 0–25 % realistic Leslie sweep (0.5 → 11 Hz), 25–100 % accelerates into the heterodyne zone (11 → 44 Hz). Asymmetric ramp — 60 ms spool-up, 30 ms spool-down. |
| 5 | **DJ Filter** | Pop-free 3-stage biquad LP/HP cascade (Essaim port) with per-sample-smoothed cutoff. LP 200 Hz – 18 kHz, HP 80 Hz – 4 kHz |
| 6 | **Texturizer** | One-knob WoFi-inspired ribbon: granular cloud → SVF shimmer → 1-tap echo → auto-freeze at the very top of the knob. Vintage warmth LP rolls off above 4 kHz for analog-tape feel |
| 7 | **Delay** | BBD-style gain-staged tape echo with soft-clipped buffer write |
| 8 | **Reverb** | 6-tap diffuse plate with quadrature LFO smear, diffuser allpasses, 120 Hz HPF on the loop input, and off-state reset to silence the natural mode when the knob sits at zero |

Each effect is musical at every position of its knob (Reface CP /
Hologram Microcosm school — no dead zones).

### FX-chain ordering
Six selectable processing orderings (menu-only on the FX page):

- **Classic** — `texture → vibrato → rotary → dj_filter → tremolo → autowah → delay → reverb`
- **Pedalboard** — `autowah → vibrato → tremolo → rotary → texture → dj_filter → delay → reverb`
- **Wet Swirl** — time-based first, then modulation sweeps the wet tail
- **Ambient Wash** — delay + reverb upfront, every later effect colours the wash
- **Dub** — dub-console workflow: filter the dry, send to time, mod the late tail
- **Reverse** — reverb-first "preserved in amber" workflow ending in granular freeze

### Envelope
- Per-voice ADSR with **attack-curve** and **release-curve** shapers
- **22 envelope presets** — Tape Organ (default), Strings, Slow Pad,
  Choir Swell, Pluck, Stab, Bowed Swell, Gate, Reverse Swell, Staccato,
  Drone, Soft Keys, Cinematic Rise, Pluck Bass, Soft Lead, Pad Wash,
  Marcato, Hammer, Vox Ahh, Brass Hit, Slow Drone, Pizzicato — plus a
  **Custom** slot that the live ADSR edits write through to
- Mr-Sample-style release-rate scaling (release-time stays constant
  regardless of where in the envelope the note was released — no clicks
  on short notes)

### MIDI controller support
- Note on / note off ✓
- **Mod wheel (CC1)** → adds to Vibrato amount (classic Mellotron mapping)
- **Channel + poly aftertouch** (`0xA0` / `0xD0`) → adds to tape Flutter
- **Pitch bend** (`0xE0`) → ±2 semitones, smoothed per block
- **CC 120 / CC 123** (All Sound Off / All Notes Off) → panic, releases
  all voices
- Same-note retrigger guard releases the previous voice before starting
  a new one (catches dropped note-offs from USB MIDI)

---

## Installation

### From the Schwung Manager (recommended)
1. In Schwung Manager, refresh the catalog.
2. Find **Mello** under sound generators, click Install.
3. Power-cycle the Move so the host picks up the new module.json.
4. Add Mello to a chain — you're done.

### Manual install (devs / pre-release)
```bash
git clone https://github.com/filliformes/mello-move.git
cd mello-move
./scripts/build.sh          # Docker ARM64 cross-compile → dist/mello/dsp.so
./scripts/install.sh        # scp dsp.so + module.json to move.local
./scripts/install_banks.sh  # rsync your WAV library (one-time, you supply)
```

Then power-cycle the Move.

---

## Pages (jog wheel switches pages)

### Main
**Knobs:** `bank_a · bank_b · ab_mix · pitch (±24 st) · tone · env_preset · crunch · volume`

**Menu params:** Samples Folder · Reverse · Half Speed · Sample Start ·
Crossfade % · Loop Mode · Map Mode · Auto Retune · Seq Base

### FX
**Knobs:** `autowah · vibrato · tremolo · rotary · dj_filter · texture · delay · reverb`

**Menu param:** FX Chain (Classic / Pedalboard / Wet Swirl / Ambient
Wash / Dub / Reverse)

### Envelope
**Knobs:** `env_preset · env_a · env_d · env_s · env_r · env_ac · env_rc · env_vel`

### Tape & Preamp
**Knobs:** `tape_style · warble · flutter · degrade · tape_sat · preamp_model · input_level · limiter`

**Menu params:** Tape Len · Chiff · Bias · Mech Noise

---

## Loading your own banks

Mello's WAV sampler accepts three filename conventions:

```
instruments/<library>/<bank>/
  A2.wav                       # note name + octave (C4 = MIDI 60)
  C#3.wav                      # sharps and flats both work
  Bb4.wav
  60.wav                       # bare MIDI number (21..108)
  MyBank-8.wav                 # Leisureland sequential, seq_base + N
```

Drop a folder under `instruments/<library>/` (e.g.
`instruments/LL/MyBank/`), run `./scripts/install_banks.sh "LL/MyBank"`,
and the bank appears in the Bank A / Bank B enum the next time you
reopen Mello.

Library folders themselves (`LL`, `SB`, `90`, or any new one) become
entries in the **Samples Folder** menu — switching it quick-jumps Bank A
and Bank B to the first bank of that library.

The plugin auto-handles **16-bit PCM**, mono or stereo (downmixed to
mono on load), any sample rate (resampled at playback time via the
varispeed engine).

---

## Building from source

Requires Docker (or a local `aarch64-linux-gnu-gcc` toolchain).

```bash
./scripts/build.sh         # cross-compile dsp.so for ARM64
./scripts/install.sh       # scp dsp.so + module.json
./scripts/install_banks.sh # rsync WAV library (skip if not changed)
```

Source layout:

```
src/
  dsp/
    mello.c           — plugin entry, voice manager, FX rack, render loop
    wav_bank.{c,h}    — WAV loader, folder scanner, filename → MIDI parser,
                        autocorrelation pitch detector with parabolic interp
  module.json         — metadata + UI hierarchy + chain_params fallback
instruments/          — your sample libraries (gitignored, user-supplied)
scripts/              — build, install, install_banks, Dockerfile
.github/workflows/    — release.yml (tagged release → uploads tarball)
```

---

## Tech notes

- **Pure C**, single-file DSP core, no STL, no malloc inside
  `render_block`
- **48 kHz, up to 16 voices per bank × 2 banks** = 32 voices polyphony
- **CPU budget**: ~6 % of one ARM Cortex-A72 core typical, ~12 % worst
  case (full poly + all FX engaged). v0.1.24 introduced incremental
  quadrature LFOs (replacing ~11 sinf/cosf calls per sample with rotation-
  matrix advances), fast_pow for env_shape, and hoisted-pitch-multiplier
  optimisation — saves ~13 M CPU cycles/sec at typical load
- **Memory**: ~30 MB per bank loaded, two banks active = ~60 MB. Plenty
  of headroom on Move's 4 GB.
- **Sound design DNA** — references for the curated curves, not vendor
  ports: Essaim (tape warble + DJ filter), Modes (DJ filter cascade),
  Ambiotica (Texturizer chord quantisation + auto-freeze UX), Super Boum
  + KrautDrums (tape saturation curves), LeslieSim + Surge XT (rotary
  speaker physics)

---

## Credits

- **DSP design + plugin code**: Vincent Fillion ([Filliformes](https://vincentfillion.com))
- **Schwung framework**: [Charles Vestal](https://github.com/charlesvestal/schwung)
- **DSP references** — algorithms studied and re-implemented from
  scratch (no code vendored): Essaim, Modes, Structor, Spectra, Denis,
  Mr Sample (Schwung ecosystem); Surge XT's RotarySpeakerEffect (GPL-3
  — algorithm reference only); LeslieSim by Miccio (algorithm
  reference); CHOWTape research papers; Ambiotica reverb-grain UX

## License

MIT — see [LICENSE](LICENSE).

The repository contains plugin code only — bundled WAV sample libraries
are NOT included in the GitHub repository. If you supply your own WAVs
(e.g. the Leisureland or 90s-Archive Mellotron sets, both circulated
freely in the community for personal use), check the source's licensing
terms before redistributing.

---

## Status

**v0.1.0** — first public release.

Feedback and issues welcome at the GitHub tracker. PRs that respect the
**simplexity** design principle (one knob, one idea, musical at every
position — no big vendor-port engines) are very welcome.
