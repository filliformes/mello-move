# CLAUDE.md — Mello

Dual-bank tape-instrument for Ableton Move (Schwung). Mellotron × Casio SK-1 × Kiviak WoFi.
Supersedes the original Mello. **Read `design-spec.md` first** — full architecture there.

## What this is
- `component_type`: **sound_generator** (`plugin_api_v2`, init `move_plugin_init_v2`), pure C.
- **In-house WAV sampler** — NOT sfizz, NOT xsynth, NOT DecentSampler. Loads raw .wav
  files from `instruments/<bank>/`, parses the MIDI note from the filename, plays
  per-voice with linear interpolation. Schwung-sfz used to use sfizz; the parent
  migrated to xsynth (Rust). Neither is needed for the simple
  "play this WAV when this key is pressed" semantics Mello actually uses.
- Four pages × 8 knobs (jog-wheel page switch): **Main · Envelope · Tape & Preamp · FX**.

## Repo structure
- `src/dsp/mello.c` — plugin: API, voice manager, 4-page UI, tape, preamp, tone, 8 FX,
  limiter, master. ~1100 lines, single file.
- `src/dsp/wav_bank.{c,h}` — 16-bit PCM WAV loader + folder scanner + filename→MIDI
  parser (handles note-name, bare-MIDI, **Leisureland sequential `*-N.wav`**).
- `src/module.json` — metadata + ui_hierarchy + chain_params (39 params). DSP serves a
  dynamic `chain_params` via `get_param("chain_params")` so `bank_a`/`bank_b` enums
  reflect the scanned `instruments/` folder list at runtime.
- `instruments/` — bank folders. Each subfolder = one bank. Files are .wav. Filename
  conventions accepted: `A2.wav`, `C#3.wav`, `60.wav`, `prefix-8.wav` (sequential).
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

## Pipeline state
- Stage 1 DESIGN ✅ · Stage 2 FETCH ✅ (Essaim/Modes/Structor/Spectra/Denis surveyed,
  schwung-sfz cloned for reference) · Stage 3 SCAFFOLD ✅ · Stage 4 CODE ✅
- Stage 5 DSP REVIEW ✅ (multiple audit passes; v0.1.24 hygiene + CPU optimisation
  pass landed all critical findings) · Stage 6 BUILD ✅ (Docker ARM64) · Stage 7
  USER TESTS ON MOVE ✅ (extensive iteration v0.1.1 → v0.1.24, all FX retuned to
  user feedback) · Stage 8 UI REVIEW ✅ (menu reordered, Crossfade % added,
  Auto Retune verified) · **Stage 9 RELEASE v0.1.0 ✅** (README + LICENSE drafted,
  module.json + release.json aligned, workflow validated)
- Next: Stage 10 — Schwung catalog PR (submit to Charles Vestal's manager repo
  with download URL = `https://github.com/filliformes/mello-move/releases/download/v0.1.0/mello-module.tar.gz`).

## Critical constraints (sound generator)
- `plugin_api_v2_t` has **8 fields** incl. `get_error` (NULL) — omitting it SIGSEGVs.
- **Sound generators MUST serve `ui_hierarchy` via `get_param`** — done.
- DSP file installs as **`dsp.so`**, not `mello.so`. `chmod +x` after scp.
- `get_param` returns **-1** for unknown keys (never 0). Enums return **string names**.
- Steppers (`bank_a`/`bank_b`/`env_preset`/`tape_style`/`preamp_model`) accept either
  numeric index or option string in `set_param` (done via registry).
- No malloc / printf / locks in `render_block`. The `bank_lock` mutex protects bank
  hot-swap during set_param — render briefly waits if a swap is mid-flight. Synchronous
  bank load is acceptable for v0.1; v0.2 should move to async (pthread worker + atomic
  pointer swap).
- C decl order: tables/consts before functions that use them.
- Install base path differs by framework install: `MOVE_BASE` defaults to
  `/data/UserData/schwung/modules/sound_generators`; the DSP probes both that and
  `/data/UserData/move-anything/modules/sound_generators` at init.
- Power-cycle the Move to pick up module.json changes (cached at startup).

## Memory budget
- Each bank: 37 WAVs × ~16 frames-per-mono × 16-bit = ~30 MB. Loading 2 banks = ~60 MB.
  Plenty of headroom on Move's 4 GB. Banks load synchronously on `bank_a`/`bank_b`
  change — typical ~0.5–1 s glitch.

## CPU budget (ARM Cortex-A72 @ 48 kHz × 128 frames/block ≈ 375 blocks/sec)
- 16 voices × 2 banks = 32 mono voices × ~13 ops/sample = ~400 ops/sample
- Tape + preamp + tone + 8 FX + limiter = ~250 ops/sample
- Total ≈ 650 ops/sample, ~6 % of one core. Plenty of room.

## DSP reference modules (extraction map)
- **schwung-sfz (parent)** — migrated to xsynth/Rust; we don't depend on either.
  Reference for build patterns + ui_hierarchy shape only.
- **Essaim** — tape warble LFO (smooth sine 0.05–0.3 Hz), tape-hiss with signal-gate,
  saturation curve (`drive = 1 + sat²·N`, RMS-comp makeup), BBD delay (2-pole cascade
  + jitter LFO), smooth LFO shapes (Soft Saw, Soft Square, Skewed Sine, Warm Pulse).
  Mello adds: flutter LFO (6.5 Hz + noise, not in Essaim), random dropouts (not in
  Essaim), grit/crackle.
- **Modes** — pop-free bipolar DJ filter (3-stage biquad cascade, LP/HP crossfade
  ±0.05 around 0.5, biquad memset on flip). Copied verbatim.
- **Structor / Spectra / Dissolver** — granular OLA reference. Mello rolls a leaner
  single-stream grain cloud (~100 lines) + SVF bandpass shimmer (~30 lines) + 1-tap
  echo + bit-crush, all on one ribbon to mimic the Kiviak WoFi Texturer.
- **Denis** — preset apply + snap smoothed values pattern.
- **Phasma** — line-in / mapped-memory (kept in design for v0.2 live sampling).

## Build / deploy / release
```
./scripts/build.sh         # Docker ARM64 cross-compile (docker create+cp pattern)
./scripts/install.sh       # scp dsp.so + module.json (fast)
./scripts/install_banks.sh # rsync ~1 GB of WAVs (slow, one-time)
```
Release via `/move-schwung-release 0.1.0` (tag v0.1.0 must equal module.json version).

## v0.1 design principle (locked)

**Simplexity.** One knob, one idea. Every position sounds musical (Reface CP /
Microcosm references). NO vendor ports of big plugins — DSP must stay
hand-tuned and small enough to reason about. Any character we want from CHOWTape /
Dragonfly / Verglas / Ambiotica / Hera is folded in *as ideas*, not as 800+ LOC
vendored engines. Earlier attempts to vendor CHOWTape (J-A hysteresis, ~960 LOC GPL-3)
and Hera (~800 LOC C++ chorus) were reverted on 2026-05-23 once it became clear
they violated the simplexity rule. The tape character and the chorus stay
in-house, tuned to be musical across the full range of the knob.

## v0.1 Texturizer + Reverb refinements (Ambiotica-inspired, in-house)

- **Texturizer (`fx_texture`)**:
  - **Power-chord pitch quantization** for grains — unison / ±octave / ±perfect-fifth.
    Scatter probability widens with the knob (low = mostly unison, high = all five
    intervals equiprobable). This is the secret to grain clouds sounding musical.
  - **Auto-freeze at amt ≥ 0.95** — new grains stop spawning, existing ones loop
    forever. No separate freeze toggle (Ambiotica UX win).
  - **Soft-sat in echo feedback** — `x / (1 + |0.8x|)` instead of raw clip. Echo
    tap fixed to 8000 samples (167 ms) so it fits inside the 8192-sample ring.
  - Knob zones (musical at every spot): 0–5 % bypass · 5–30 % subtle live grain
    cloud · 30–55 % + shimmer · 55–80 % + echo · 80–95 % + bit-crush · 95–100 %
    freeze.

- **Reverb (`fx_reverb`)**: **6 async LFOs** at incommensurate rates (0.33 / 0.47
  / 0.61 / 0.79 / 0.93 / 1.13 Hz) modulate the 6 comb-tap read offsets by ±3..±8
  samples. At low knob settings the reverb is nearly static; at high settings it
  breathes organically. No new state — phases derived from existing
  `flutter_phase` with per-tap rotational offsets.

## v0.1 tape stage upgrades (Super Boum + KrautDrums)

- **Per-style saturation curves** (each tape_style has its own harmonic identity):
  - Clean = linear
  - M400 = KrautDrums **cubic** soft-clip → 3rd-harmonic emphasis
  - MkII = KrautDrums **asymmetric tanh** → 2nd-harmonic tube warmth
  - M300 = classic tanh + RMS makeup (neutral)
  - Worn = asym + cubic blend, hot
- **Peaking biquad head-bump** at 120 Hz, Q=1.5, per-style base dB (Clean 0 / M400 2 / MkII 3 / M300 1.5 / Worn 4) + `tape_sat * 3 dB` modulation. Authentic tape-head low-mid resonance.
- **Age ducking**: degrade reduces output by up to 25 % (Super Boum 'age' technique). Now degrade both ADDS noise AND removes level — closer to real tape wear.

Helpers consolidated above `apply_tape_stage`: `biquad_proc`, `biquad_set_{lpf,hpf,peaking}`, `tape_cubic`, `tape_asym`.

## v0.1 extras (added from Mellotron VST references — Arturia / M-Tron Pro IV / IK SampleTron 2 / AIR / Microtron)

| Feature | Param key | Where | Notes |
| --- | --- | --- | --- |
| Tape Reverse | `reverse` (enum) | Main menu | Voice's `base_rate` is negated; read_pos decrements from end |
| Aftertouch → flutter | (no param) | `on_midi`, 0xA0/0xD0 | `m->at_flutter` adds to flutter depth; decays 3 %/block when pressure released |
| Sample Start | `sample_start` (float 0..1) | Main menu | Skips fraction of WAV on note-on; combined with reverse, plays from `(1-start)*frames` |
| Chiff (key-off retract) | `chiff` (existing knob) | Tape menu | Was unused; now triggers a brief LP-filtered noise burst (~80 ms) on every note release. CHIFF_POOL=8 polyphony |
| Tape Bias | `bias` (float, 0.5 = neutral) | Tape menu | Bipolar offset to sat onset (low bias = harsher, more drive) + HF rolloff (high bias = darker) |
| Mechanical Noise | `mech` (float) | Tape menu | Sub-300 Hz transport rumble + random clicks; mixed in pre-tape |
| Instability | (under `flutter`) | code-only | At `flutter > 0.7`, adds slow random pitch wander (50 cent depth scaled by flutter overage). RNG updates ~5 Hz, smoothed |
| Bank C | `bank_c` (enum) + `c_level` (float) | Main menu | 3rd bank layered additively; `c_level=0` skips render. Adds 16 voices to the worst case (48 total) |

## Open items / v0.2
- Async bank loading (pthread worker + atomic pointer swap) so bank changes don't glitch.
- Live sampling (SK-1 "sample the world") via mapped_memory + audio_in_offset (Phasma).
- Velocity-layered/round-robin support if we add user banks beyond simple Mellotron sets.
- Per-bank `seq_base` override (currently global).
- Leisureland licensing — check terms before distributing factory content beyond personal use.
