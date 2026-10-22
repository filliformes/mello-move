# CLAUDE.md — Mello

Dual-bank tape-instrument for Ableton Move (Schwung). Mellotron × Casio SK-1 × Kiviak WoFi.
Supersedes the original Mello. **Read `design-spec.md` first** — full architecture there.

## What this is
- `component_type`: **sound_generator** (`plugin_api_v2`, init `move_plugin_init_v2`), pure C.
- **In-house WAV sampler** — NOT sfizz, NOT xsynth, NOT DecentSampler. Loads raw .wav
  files from `instruments/<library>/<bank>/`, parses the MIDI note from the filename,
  plays per-voice with linear interpolation. The simple "play this WAV when this key is
  pressed" semantics Mello needs don't warrant a vendored sampler engine.
- **Five pages × 8 knobs** (jog-wheel page switch): **Main · FX 1 · FX 2 · Envelope ·
  Tape & Preamp**.

## Repo structure
- `src/dsp/mello.c` — plugin: API, voice manager, 5-page UI, tape, preamp, tone,
  FX 1 (8 macros), FX 2 (7-style distortion + 3-voicing Frippertronics tape delay),
  10 FX-chain routings, async bank loader, auto-retune, pitch-bend/mod-wheel, limiter,
  master. **~3,900 lines, single file.**
- `src/dsp/wav_bank.{c,h}` — 16-bit PCM WAV loader + folder scanner + filename→MIDI
  parser (note-name, bare-MIDI, **Leisureland sequential `*-N.wav`**) + autocorrelation
  pitch detection for Auto-Retune + per-bank peak-normalise (`bank_gain`).
- `src/module.json` — metadata + ui_hierarchy (5 levels) + chain_params (**55 params**).
  DSP serves a dynamic `chain_params` via `get_param("chain_params")` so the `sample_lib`,
  `bank_a` and `bank_b` enums reflect the scanned `instruments/<library>/<bank>/` tree at
  runtime.
- `instruments/` — **two-tier**: `instruments/<library>/<bank>/*.wav`. Top-level folders
  are libraries (MT, SB, 90, …); each holds bank subfolders. The Main-page "Samples
  Folder" param (`sample_lib`) switches library; `bank_a`/`bank_b` list every bank across
  all libraries as `Library/Bank`. Filename conventions: `A2.wav`, `C#3.wav`, `60.wav`,
  `prefix-8.wav` (sequential).
- `scripts/{build,install,install_banks}.sh`, `scripts/Dockerfile`,
  `.github/workflows/release.yml`.

## Filename → MIDI note (Leisureland sequential)
The Leisureland Mellotron library names files `M4008Choir-1.wav` through `-37.wav`,
no note names. The parser maps `file_index N → MIDI = N + seq_base`. Default
`seq_base = 40`, so:
- `-1.wav` → MIDI 41 (F2)
- `-8.wav` → MIDI 48 (C3)
- `-20.wav` → MIDI 60 (C4)
- `-32.wav` → MIDI 72 (C5)
- `-37.wav` → MIDI 77 (F5)
That's the M400 35-key keyboard plus 2 extras at the low end. `seq_base` is a
menu param so other sample packs with different bottom notes can be loaded.
Note-name matching requires a **word boundary** (start of string or preceded by a
non-alpha char) so bank names like `MkiiFlute-1.wav` don't false-match `e-1` inside the
name (v0.1.6 fix — the `trailing_seq` sequential parser then wins correctly).

## Pipeline state
- Stages 1–8 ✅ (design → fetch → scaffold → code → DSP review → build → on-Move user
  tests → UI review). Shipped iteratively v0.1.0 → **v0.1.8**; all FX hand-tuned to
  on-device user feedback.
- **Stage 9 RELEASE ✅ — latest v0.1.8** (2026-10-22). Pushing a `vX.Y.Z` tag triggers
  `.github/workflows/release.yml`, which verifies tag == `module.json` version, builds the
  ARM64 module, **auto-bumps `release.json`**, and publishes the GitHub release
  (`dsp.so` + `mello-module.tar.gz` + `module.json`).
- **v0.1.8 landed a full `dsp-review` audit pass:** sample rate corrected 48000 → **44100**
  (Move native — playback had been ~1.5 semitones flat); FX 2 page + `reverse`/`bias`/
  `mech`/`sample_start` now persist via `get_param("state")`; `chain_params` builder
  snprintf hardened against overflow; reverb all-pass denormal guards; per-instance
  voice-age counter (was a shared `static`).
- **Unreleased (post-v0.1.8), from a second `dsp-review` pass verified on an aarch64
  harness:** `fast_exp2` replaced (pitch was off by up to 70 cents); reverb gets an
  in-loop ~20 Hz DC blocker (the loop's DC gain is > 1 above ~16 % Reverb and latched a
  +0.58 offset, even from silence); the outgoing bank is freed by the worker instead of on
  the audio thread; Auto-Retune verifies on an independent window at fractional-period
  resolution. Still open from that review: bank_a/bank_b declare 180 options on the
  author's Move against the host's 128-option enum cap.
- Next: **Stage 10 — Schwung catalog PR** (submit to Charles Vestal's manager repo),
  download URL =
  `https://github.com/filliformes/mello-move/releases/download/v0.1.8/mello-module.tar.gz`.

## Critical constraints (sound generator)
- **Sample rate is `44100`** (`#define SR 44100.0f` — Move native; every sibling module
  uses 44100). Do NOT set 48000: `create_instance`'s `cfg` is ignored, so a mismatch
  detunes all playback ~1.5 semitones and shifts every filter/LFO/time constant ~8.8 %.
  Four hand-baked one-pole coeffs are pre-derived for 44100 and must be recomputed if SR
  ever changes: leslie crossover `0.1077`, texture warmth `0.4343`, reverb loop-HPF
  `0.9830`, tone-tilt pivot `0.1077` (all = the 44100 value of their labeled corner freq).
- `plugin_api_v2_t` has **8 fields** incl. `get_error` (NULL) — omitting it SIGSEGVs.
- **Sound generators MUST serve `ui_hierarchy` via `get_param`** — done.
- DSP file installs as **`dsp.so`**, not `mello.so`. `chmod +x` after scp.
- `get_param` returns **-1** for unknown keys (never 0). Enums return **string names**.
- Steppers (`bank_a`/`bank_b`/`env_preset`/`tape_style`/`preamp_model`/`dist_style`/
  `delay_type`/…) accept either numeric index or option string in `set_param` (registry +
  `enum_index` strcmp-then-strtol fallback).
- No malloc / **free** / printf / waiting on the worker in `render_block`. Bank loading is
  **async**: `set_param` posts a request, a per-slot worker (`bank_loader_thread`) loads
  off-thread, and `render_block` swaps it in via `check_and_swap_pending()` (a `trylock` —
  it never waits on the worker). The outgoing bank is parked in `retired_bank` and **freed
  by the worker**: freeing a bank is dozens of `munmap()`s and must never run on the
  callback. The initial load in `create_instance` is synchronous (one-time, acceptable).
  `destroy_instance` signals + joins both workers, then drains pending/retired banks.
- **`fast_exp2` drives every pitch path** (Pitch knob, bend, fallback notes,
  wow/flutter/aftertouch, Auto-Retune, envelope curves). Its mantissa polynomial must keep
  p(0) = 1 and p(1) = 2 exactly. The v0.1.8 cubic had p(1) = 1.961 and detuned up to
  70 cents (−34 c on any negative input, so even Pitch +12 played flat). Re-run the DSP
  harness if it is ever touched.
- `get_param("state")` / `set_param("state")` round-trip the full patch as a
  space-separated `key=value` string (NOT JSON — the host stores it opaquely). Every
  user-settable param must appear in the `state` getter or it silently resets on reload.
- C decl order: tables/consts before functions that use them.
- Install base path differs by framework install: defaults to
  `/data/UserData/schwung/modules/sound_generators`; `detect_instruments_dir` probes both
  that and `/data/UserData/move-anything/modules/sound_generators` at init. (On some
  devices move-anything is a symlink to the schwung tree — one copy, both paths.)
- Power-cycle the Move to pick up module.json changes (cached at startup).

## Memory budget
- Each bank: ~37 WAVs ≈ ~30 MB. Two banks (A + B) ≈ ~60 MB — plenty of headroom on Move's
  4 GB. Runtime `bank_a`/`bank_b` changes load on a worker thread (no audio-thread stall);
  active voices fast-release first so the swap is inaudible.

## CPU budget (ARM Cortex-A72 @ 44.1 kHz × 128 frames/block ≈ 345 blocks/sec)
- 16 voices × 2 banks = 32 mono voices × ~13 ops/sample ≈ ~400 ops/sample
- Tape + preamp + tone + FX 1 (8 macros) + FX 2 (distortion + Frippertronics) + limiter
  ≈ ~300 ops/sample
- Total ≈ ~700 ops/sample. Trig is hoisted to block rate (quadrature oscillators) across
  vibrato / leslie / reverb LFOs. Comfortable on one core.

## DSP reference modules (extraction map)
- **schwung-sfz (parent)** — migrated to xsynth/Rust; we don't depend on either.
  Reference for build patterns + ui_hierarchy shape only.
- **Essaim** — tape warble LFO (smooth sine 0.05–0.3 Hz), tape-hiss with signal-gate,
  saturation curve (`drive = 1 + sat²·N`, RMS-comp makeup), BBD delay (2-pole cascade
  + jitter LFO). Mello adds: flutter LFO (6.5 Hz + noise), random dropouts, grit/crackle.
- **Modes** — pop-free bipolar DJ filter (3-stage biquad cascade, LP/HP crossfade
  ±0.05 around 0.5, biquad memset on flip). Copied verbatim.
- **Structor / Spectra / Dissolver** — granular OLA reference. Mello rolls a leaner
  single-stream grain cloud + SVF bandpass shimmer + 1-tap echo to mimic the Kiviak WoFi
  Texturer.
- **KrautDrums** — the Frippertronics tape-delay voicings + several tape saturation curves
  are ported from here (and it, like every sibling, runs at 44100 — see SR constraint).
- **Super Boum** — drive-mix RMS level-matching pattern (used by FX 2 distortion), the
  `state` round-trip reference, 'age' output ducking.
- **Denis** — preset apply + snap smoothed values pattern.
- **Phasma** — line-in / mapped-memory (kept in design for v0.2 live sampling).

## Build / deploy / release
```
./scripts/build.sh         # Docker ARM64 cross-compile (docker create+cp pattern)
./scripts/install.sh       # scp dsp.so + module.json + help.json (fast), md5-verified
./scripts/install_banks.sh # rsync the WAV library (slow, one-time)
```
Release: bump `src/module.json` `version`, commit, then push a matching annotated tag
(`git tag -a vX.Y.Z -m "…" && git push origin vX.Y.Z`). The Release workflow verifies the
tag equals `module.json` (CI fails otherwise), builds, **auto-bumps `release.json`**
(don't hand-edit it), and publishes the GitHub release. Latest: **v0.1.8**.

## v0.1 design principle (locked)

**Simplexity.** One knob, one idea. Every position sounds musical (Reface CP /
Microcosm references). NO vendor ports of big plugins — DSP must stay
hand-tuned and small enough to reason about. Any character we want from CHOWTape /
Dragonfly / Verglas / Ambiotica / Hera is folded in *as ideas*, not as 800+ LOC
vendored engines. Earlier attempts to vendor CHOWTape (J-A hysteresis, ~960 LOC GPL-3)
and Hera (~800 LOC C++ chorus) were reverted on 2026-05-23 once it became clear
they violated the simplexity rule. The tape character and the chorus stay
in-house, tuned to be musical across the full range of the knob.

## FX 1 rack (8 macros, `fx_chain` reorders them)
Vibrato/Chorus · Rotary (physical-model Leslie) · DJ Filter (bipolar LP/HP) · Tremolo ·
Auto-Wah (envelope BPF) · Texturizer · Delay (BBD) · Reverb (6-tap modulated).
Every stage is **always-run** (state tracks the dry signal continuously) so engaging a
knob never surfaces stale audio — the knob only gates the wet mix. Exact LFO rates,
coefficients and knob zones live in the code comments (they've been retuned many times to
on-device feedback; treat the source as ground truth, not any single number here).

## FX 2 page + sample libraries (v0.1.5+)
- **Distortion** (`dist_style` / `dist_drive` / `dist_tone` / `dist_blend`): 7 styles —
  Tube, Fuzz, Crimson, Wavefold, Bitcrush, Spiral, RAT (last two from Airwindows, MIT).
  RMS level-matched so Drive never raises perceived loudness; tone-tilt + equal-power blend.
- **Frippertronics** (`delay_type` / `fripp_time` / `fripp_fb` / `fripp_age` / `fripp_mix`):
  3 tape-delay voicings — Echoplex EP-3 (Tape), Binson Echorec (Magnetic), Roland RE-201
  (Space). 8 s buffer; per-voicing `fb_cap` just above unity for controlled self-osc
  (tanh bounds the runaway); `Age` compounds HF rolloff + saturation + wow/flutter.
- **`dist_position`** (Pre / In-Loop / Post) sequences distortion against Frippertronics;
  In-Loop injects extra saturation into the feedback path (dirt grows over repeats).
- **10 FX-chain routings** (`fx_chain`): Classic, Pedalboard, Wet Swirl, Ambient Wash,
  Dub, Reverse, Frippertronics, Wall of Orchestra, Loop Chaos, Dirty Cabinet.
- **Two-tier sample libraries** + per-bank peak-normalise (`bank_gain`, target 0.35) so
  packs switch at consistent loudness with headroom to drive the preamp/tape stages.

## Tape stage (Super Boum + KrautDrums)
- **Per-style saturation curves** (each `tape_style` has its own harmonic identity):
  Off = bypass · Clean = gentle tanh · M400 = cubic soft-clip (3rd-harmonic) · MkII = asym
  tanh (2nd-harmonic tube warmth) · M300 = classic tanh + RMS makeup · Worn = asym+cubic
  blend, hot.
- **Peaking biquad head-bump** at 120 Hz, per-style base dB + `tape_sat` modulation.
- **Age ducking**: degrade both ADDS noise/dropouts/grit AND removes up to 25 % level.
- Helpers above `apply_tape_stage`: `biquad_proc`, `biquad_set_{lpf,hpf,peaking,bpf}`,
  `tape_cubic`, `tape_asym`.

## Texturizer (Ambiotica-inspired, in-house)
- **Power-chord pitch quantization** for grains — unison / ±octave / ±perfect-fifth;
  scatter probability widens with the knob (low = mostly unison). This is the secret to
  grain clouds sounding musical.
- **Auto-freeze near full knob** — new grains stop spawning, existing ones loop forever
  (no separate toggle). Shimmer SVF + soft-sat 1-tap echo + analog-warmth LP layer in as
  the knob climbs. Exact zone boundaries are in the code (remapped in v0.1.20).

## v0.1 extras (from Arturia / M-Tron Pro IV / IK SampleTron 2 / AIR / Microtron refs)

| Feature | Param key | Where | Notes |
| --- | --- | --- | --- |
| Tape Reverse | `reverse` (enum) | Main menu | Voice's `base_rate` is negated; read_pos decrements from end |
| Aftertouch → flutter | (no param) | `on_midi` 0xA0/0xD0 | `m->at_flutter` adds to flutter depth; decays ~3 %/block when released |
| Pitch bend | (no param) | `on_midi` 0xE0 | ±2 semitones, smoothed into the per-sample rate |
| Mod wheel | (no param) | `on_midi` CC1 | Adds to Vibrato depth (classic Mellotron wheel) |
| Panic | (no param) | `on_midi` CC120/123 | All-sound-off / all-notes-off release every voice |
| Sample Start | `sample_start` (float 0..1) | Main menu | Skips a fraction of the WAV on note-on; combines with reverse |
| Sustain-loop crossfade | `xfade_pct` (int 0..100) | Main menu | Equal-power loop crossfade, ~5..130 ms |
| Auto-Retune | `auto_retune` (enum) | Main menu | Autocorrelation + parabolic-interp detune correction (±8..35 cents), applied at note-on; default Off |
| Chiff (key-off retract) | `chiff` (knob) | Tape menu | Brief LP-filtered noise burst (~80 ms) on note release; CHIFF_POOL=8 |
| Tape Bias | `bias` (float, 0.5 = neutral) | Tape menu | Bipolar sat-onset offset + HF rolloff |
| Mechanical Noise | `mech` (float) | Tape menu | Sub-300 Hz transport rumble + random clicks; mixed in pre-tape |
| Instability | (under `flutter`) | code-only | At `flutter > 0.7`, slow random pitch wander scaled by the overage |

## Open items / v0.2
- Live sampling (SK-1 "sample the world") via mapped_memory + audio_in_offset (Phasma).
- Velocity-layered / round-robin support if we add user banks beyond simple Mellotron sets.
- Per-bank `seq_base` override (currently global).
- Leisureland licensing — check terms before distributing factory content beyond personal use.
- Cosmetic cleanups flagged by the v0.1.8 audit: the `"Crossfade %"` literal `%` in the
  `chain_params` format string (works on glibc but technically UB — escape as `%%`);
  `MIDI_FREQ[128]` has 4 excess initialiser entries past index 127 (harmless — the
  compiler trims them; size the array or drop the extras to silence the warning).
