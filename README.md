# Mello

**Dual-bank tape instrument for [Ableton Move](https://www.ableton.com/move/).**
My dream super Mellotron.

Built for the [Schwung](https://github.com/charlesvestal/schwung) framework. Part of the open-source
lutherie banner **Filliformes**.

> Physical-model Rotary speaker. Per-style tape saturation. Crybaby-style
> Auto-Wah. WoFi-inspired granular Texturizer with vintage analog warmth.
> **7-style distortion + 3-mode Frippertronics tape delay** (Echoplex EP-3 /
> Binson Echorec / Roland RE-201, all self-osc-capable).
> **10 selectable FX-chain orderings.** Sustain loop with Blackbox-style
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
  MT/                          ← MellowTrawn — complete Leisureland archive (129 banks)
  SB/                          ← Sonic Bloom Mellotron pack (23 banks)
  90/                          ← 90s Mellotron Archive CD (28 banks, curated 35-note slice)
  LL/                          ← (legacy) Leisureland subset — recognised but optional
  YourCustomSet/               ← drop your own here
```

Three default sample libraries, ~180 banks, ~6 600 WAVs — the same Mellotron
material recorded by three independent transfers, so you get genuinely
different sonic personalities for each instrument. Switching the
**Samples Folder** menu param quick-jumps Bank A and B to the first
bank of that library. The plugin auto-discovers any library folder you
create.

**MT vs LL**: Up through v0.1.1 the canonical Leisureland library was
`LL/`, a 34-bank curated subset. Starting v0.1.2 the default Leisureland
library is `MT/` (MellowTrawn), the full 129-bank archive. The plugin
still recognises a folder named `LL/` for anyone whose existing setup
uses that name — both work, no breakage.

> **Sample WAVs are not bundled in this repository** — they're
> community-archived Mellotron recordings and not the author's to
> redistribute. All three default sets — MellowTrawn (Leisureland),
> Sonic Bloom, and the 90s Mellotron Archive CD — are **free downloads**
> circulated in the Mellotron community. See *Loading your own banks*
> below for the formats Mello accepts.

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

### FX 1 — 8-effect rack (knob order)
On the **FX 1** page, the eight knobs are:

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

### FX 2 — Distortion + Frippertronics tape delay (v0.1.5 new)
On the **FX 2** page, the eight knobs are split 4 + 4 between a multi-style
distortion and a vintage-voiced regenerative tape delay:

| # | Knob | What it is |
|---|---|---|
| 1 | **Dist Drive** | Amount of clipping / folding / crush |
| 2 | **Dist Style** | 7 sonically-distinct shapers (see below) |
| 3 | **Dist Tone** | Dark ↔ bright tilt (post-shaper LP/HP) |
| 4 | **Dist Blend** | Dry ↔ wet, equal-power crossfade |
| 5 | **Fripp Time** | 10 ms – 8 sec — quarter-root curve, 1 sec by 25 % knob |
| 6 | **Fripp Feedback** | 0 → self-oscillation (every voicing can hold a loop) |
| 7 | **Fripp Age** | Compounds HF rolloff + sat drive + wow depth on one knob |
| 8 | **Fripp Mix** | Dry ↔ wet, equal-power crossfade |

**7 distortion styles** — each sonically distinct, auto-level-matched
via running-RMS so Drive never raises perceived loudness:

| Style | Algorithm |
|---|---|
| **Tube** | Asymmetric tanh + 0.25 DC bias (strong 2nd-harmonic) |
| **Fuzz** | High-gain asymmetric hard clip with knees |
| **Crimson** | Klon-style antiparallel Si/Ge diode pair |
| **Wavefold** | Buchla-style iterated triangle reflection (4 folds) |
| **Bitcrush** | Sample-rate-hold + bit-depth quantize |
| **Spiral** | Airwindows `sin(x·\|x\|)/\|x\|` w/ 1-sample delayed presence (MIT) |
| **RAT** | Airwindows iterated polynomial cascade — ProCo RAT / SansAmp DI (MIT) |

**3 Frippertronics voicings** (menu: *Delay Type*) — ported from KrautDrums
with feedback caps raised so all three can self-oscillate:

| Voicing | Source | Character |
|---|---|---|
| **Tape** (default) | Maestro Echoplex EP-3 | Single head, FET preamp +4 dB @ 9.5 kHz, bright repeats |
| **Magnetic** | Binson Echorec | 4-tap drum, tube preamp +1.5 dB @ 4 kHz, dual-sine wobble |
| **Space** | Roland RE-201 Space Echo | 3-tap, free-floating tape, audible compression |

**Dist Position** menu param (Pre / In-Loop / Post) controls where the
distortion sits relative to Frippertronics:
- **Pre** — `dist → fripp`, echoes of the distorted signal
- **In-Loop** — same as Pre **plus** extra in-loop saturation in fripp's
  feedback path, so each repeat picks up more dirt (runaway distortion grows)
- **Post** — `fripp → dist`, clean delays then distort the stack of repeats

### FX-chain ordering
**Ten** selectable processing orderings (menu-only on the FX 1 page) —
the six v0.1 chains updated to slot in the FX 2 effects musically, plus
four new chains built around them:

- **Classic** — `texture → mod stack → dist+fripp → delay → reverb` (default)
- **Pedalboard** — `wah → mod stack → texture → dj_filter → dist+fripp → delay → reverb`
- **Wet Swirl** — time-based first, then dist+fripp, then mod sweeps the wet tail
- **Ambient Wash** — delay + reverb up front, then dist+fripp drives the wash through the mod stack
- **Dub** — dub-console: filter dry → time → **dist+fripp as a second time-domain layer** → mod the tail
- **Reverse** — reverb FIRST, delay echoes it, dist+fripp adds another time+dirt layer, mod paints, texture chops
- **Frippertronics** *(new)* — dist+fripp DOMINATES; light pre-mod, texture/reverb after
- **Wall of Orchestra** *(new)* — Brian Eno cathedral: rotary → reverb → dist warmth → fripp lush → texture shimmer
- **Loop Chaos** *(new)* — leslie → fripp on heavy fb → wavefold-style dist → autowah → texture chops → reverb catches
- **Dirty Cabinet** *(new)* — guitar-amp-into-spring sim: dist FIRST → DJ filter (cab tone) → fripp (spring sim) → autowah presence → reverb

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

### FX 1
**Knobs:** `autowah · vibrato · tremolo · rotary · dj_filter · texture · delay · reverb`

**Menu param:** FX Chain (Classic / Pedalboard / Wet Swirl / Ambient Wash /
Dub / Reverse / Frippertronics / Wall of Orchestra / Loop Chaos / Dirty Cabinet)

### FX 2
**Knobs:** `dist_drive · dist_style · dist_tone · dist_blend · fripp_time · fripp_fb · fripp_age · fripp_mix`

**Menu params:** Dist Position (Pre / In-Loop / Post) · Delay Type (Tape / Magnetic / Space)

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
  speaker physics), **KrautDrums (3 Frippertronics voicings — Echoplex /
  Echorec / RE-201)**, **Airwindows Spiral2 + Drive** (Spiral and RAT
  distortion shapers, MIT)

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
are NOT included in the GitHub repository. The three default sample
sets — Leisureland, Sonic Bloom, and the 90s Mellotron Archive CD — are
all freely available downloads circulated in the Mellotron community.
If you intend to redistribute the WAVs or use them commercially, check
each source's licensing terms first.

---

## Status

**v0.1.5** — adds the FX 2 page (7-style multi-distortion + 3-mode
Frippertronics tape delay) and expands the FX-chain selector from 6 to
10 modes with 4 new chains built around the new effects. Distortion
shapers include Airwindows Spiral2 and Drive ports (MIT) plus 5 in-house
designs (Tube / Fuzz / Crimson / Wavefold / Bitcrush). Frippertronics
voicings are KrautDrums ports (Echoplex EP-3 / Binson Echorec / Roland
RE-201) with feedback caps raised so every voicing can self-oscillate.
8-second stereo loop buffer. Age knob compounds HF rolloff + saturation
drive + wow depth on a single control.

**Earlier**:
- **v0.1.2** — MellowTrawn library renamed `LL` → `MT`, full 129-bank
  archive default; `LL` still recognised for legacy setups
- **v0.1.1** — `raw_midi` manifest hotfix (pitch bend / CC1 / AT silently
  ignored on v0.1.0 because the flag was inside `capabilities` instead
  of root)
- **v0.1.0** — first public release

Feedback and issues welcome at the GitHub tracker. PRs that respect the
**simplexity** design principle (one knob, one idea, musical at every
position — no big vendor-port engines) are very welcome.
