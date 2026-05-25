# Mello — Design Spec

> **Supersedes the original Mello** (multi-bank WAV loader). Same name, new architecture:
> dual-bank sfizz core + amp envelope + deep tape/preamp modelling + an FX rack.
> Mellotron × Casio SK-1 × Kiviak WoFi, for Ableton Move (Schwung / Filliformes).

**Four pages of eight knobs**, each mapping 1:1 to Move's 8 encoders. "Simplex":
one knob = one idea; every FX macro is musical from 0 % (bypass) to 100 %.

| Page | Purpose |
| --- | --- |
| **Main** | banks, blend, pitch, tone, envelope-preset, crunch, volume |
| **Envelope** | ADSR + attack/release curves + velocity, with the preset mirrored |
| **Tape & Preamp** | tape style/warble/flutter/degradation/saturation + preamp model/input/limiter |
| **FX** | 8 musical-at-any-position effect macros |

---

## 1. Module identity

| Field | Value |
| --- | --- |
| `id` | `mello` |
| `name` | `Mello` |
| `component_type` | `sound_generator` |
| `api_version` | `2` (plugin_api_v2) |
| init symbol | `move_plugin_init_v2(const host_api_v1_t *host)` |
| dsp | `mello.so` |
| SR / block | 48000 Hz / 128 frames, stereo float out |

A **sound generator** (plays samples on note/pad input). Envelope, tape/preamp, and the FX
rack all live *inside* the generator and process the post-mix signal.

---

## 2. Engine — in-house WAV sampler (no sfizz / xsynth / SFZ / DecentSampler)

**Pivot decision (2026-05-23).** `schwung-sfz` migrated to xsynth (Rust); sfizz is
vestigial in the parent. Both add complexity (submodules, Rust toolchain, SFZ
parser, .dspreset converter) we never benefit from — Mellotron semantics are
trivially simple: *one WAV per key, no loop, play from offset 0, release on key-up*.
No velocity layers, no round-robin, no keyswitching. So we **roll our own minimal
WAV sampler in C** (~400 lines) and skip the whole SFZ ecosystem.

- **In-house sampler core** (`src/dsp/mello.c` + `src/dsp/wav_bank.{c,h}`):
  - 16-bit PCM WAV loader (mono/stereo, any sample rate; stereo downmixed on load).
  - 128-slot per-bank array indexed by MIDI note. Two banks (A, B).
  - 16-voice polyphonic manager per bank, oldest-voice stealing.
  - Linear interpolation read with per-sample varispeed (44.1k → 48k baseline +
    pitch knob + wow + flutter all multiply the read rate).
  - Per-voice ADSR with attack/release curve shape exponent (Mellotron-authentic
    velocity-off by default; user can dial in via `env_vel`).
- **No `dlopen`, no Rust, no submodules.** Pure C, links only `-lm -lpthread`.

### Bank model
- **Bank A** / **Bank B** = two loaded instrument folders (two sfizz instances), same notes,
  outputs crossfaded equal-power by `ab_mix`.
- `bank_a` / `bank_b` = enum selectors from scanning `instruments/` at init (folder name).

### Bank format: a folder of WAVs is a bank (no authoring required)
Drop a folder of .wav files in `instruments/`. The plugin scans it at init and on
bank change, parses MIDI notes from filenames, and loads each WAV into the
corresponding slot. No SFZ, no DecentSampler, no temp files — just raw WAV I/O.

Filename → MIDI note priority (`wav_bank.c`):
1. Note name + octave: `A2`, `C#3`, `Db4`, `F-1`
2. **Leisureland sequential** (`*-N.wav`): MIDI = `N + seq_base`. `seq_base = 40`
   default → file `-1.wav` = F2 (MIDI 41), `-8.wav` = C3, `-20.wav` = C4, `-32.wav`
   = C5, `-37.wav` = F5 (matches the Leisureland M400/MkII 37-file convention).
3. Bare MIDI number (in 21..108): `60.wav`, `note_72.wav`.

Voice assignment for an incoming note uses **Tape exact with chromatic fallback**:
exact slot if mapped, else the nearest sample is played with varispeed pitch
shift to reach the requested note.

- **SK-1 lo-fi:** never bake bit-crush into samples — apply live via Crunch / Degradation /
  Texturizer so it stays tweakable.
- **Licensing:** Leisureland samples have no stated licence — personal use OK, clear terms
  before shipping any as Filliformes factory banks.

---

## 3. Signal flow

```
notes/pads → [ Bank A (sfizz) ]┐
                               ├─ equal-power A↔B mix
             [ Bank B (sfizz) ]┘
   │   amp ENVELOPE per voice (sfizz ampeg: A D S R + attack/release shape + vel)  ← Envelope page
   │   PITCH varispeed 2^(semi/12) at read ptr; wow/flutter pitch-mod injected here ← Tape page
   ▼
[ Input Level ]   front trim into tape           ← Tape & Preamp page
   ▼
[ TAPE  : warble · flutter · degrade(hiss/dropout/grit) · saturation ]  voiced by Tape Style
   ▼
[ PREAMP: model curve ] driven by Main-page Crunch   (MkII tube / M400 SS / SK-1 / Clean)
   ▼
[ Tone ]   tilt EQ ~800 Hz, 0.5 = flat            ← Main page
   ▼
┌─ FX CHAIN (fixed order) ───────────────────────────────────────────────────┐
│ vibrato/chorus → leslie → DJ filter → tremolo → phaser → texture → delay →  │
│ reverb                                                                       │
└──────────────────────────────────────────────────────────────────────────────┘
   ▼
[ Limiter ]  output limiter  ← Tape & Preamp page
   ▼
[ Volume ]   ← Main page → stereo out
```

Two gain stages are intentional and authentic: **Input Level** (how hot signal hits the tape
saturation, front) and **Crunch** (preamp drive into the selected model, later). The
**wow/flutter pitch** component is injected at the engine read pointer (real tape feel);
hiss/dropout/grit run in the tape block.

---

## 4. Page "Main" (8 knobs)

Root level mirrors this page.

| # | key | label | range | default | behaviour |
| --- | --- | --- | --- | --- | --- |
| 1 | `bank_a` | Bank A | enum (scanned) | first | instrument folder for layer A (stepper) |
| 2 | `bank_b` | Bank B | enum (scanned) | first | instrument folder for layer B (stepper) |
| 3 | `ab_mix` | A · Mix · B | 0–1 | 0.0 | equal-power crossfade; 0 = A, 1 = B |
| 4 | `pitch` | Pitch | −12…+12 st | 0 | **varispeed** (resample); −12 ≈ 2× tape length & darker, +12 brighter |
| 5 | `tone` | Tone | 0–1 | 0.5 | **tilt EQ** ~800 Hz; <0.5 darken, >0.5 brighten |
| 6 | `env_preset` | Envelope | enum × 12 | Tape Organ | **preset-envelope selector** (mirrored on Envelope page); applies A/D/S/R + curves |
| 7 | `crunch` | Crunch | 0–1 | 0.0 | preamp **drive** into the model on the Tape & Preamp page: `1 + crunch²·N`, soft-clip, ≥20 ms smoothing |
| 8 | `volume` | Volume | 0–2 | 0.8 | output gain |

`bank_a`/`bank_b`/`env_preset` are **steppers** needing **direct `set_param` handlers**
(not just `knob_N_adjust`). Selecting an env preset snaps all envelope smoothed values.

---

## 5. Page "Envelope" (8 knobs)

Amp envelope (implemented via sfizz `ampeg_*` opcodes, knob-controlled live through
`_oncc` modulation; curves via `ampeg_attack_shape` / `ampeg_release_shape`, vel via
`amp_veltrack`). Editing any knob below sets the preset display to **Custom**.

| # | key | label | range | behaviour |
| --- | --- | --- | --- | --- |
| 1 | `env_preset` | Preset | enum × 12 | **mirror** of Main knob 6 (same key) |
| 2 | `env_a` | Attack | 0–5 s | attack time |
| 3 | `env_d` | Decay | 0–3 s | decay time |
| 4 | `env_s` | Sustain | 0–1 | sustain level |
| 5 | `env_r` | Release | 0–5 s | release time (tape spring-retract feel) |
| 6 | `env_ac` | Atk Curve | 0–1 | 0 = log (fast-start), 0.5 = linear, 1 = exp (slow-start) → `ampeg_attack_shape` |
| 7 | `env_rc` | Rel Curve | 0–1 | same mapping → `ampeg_release_shape` |
| 8 | `env_vel` | Velocity | 0–1 | vel→amp depth; 0 = none (Mellotron-authentic), 1 = full |

### The 12 envelope presets

| # | name | A (s) | D (s) | S | R (s) | Atk crv | Rel crv | use |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | Tape Organ | 0.00 | 0.00 | 1.00 | 0.12 | lin | lin | classic instant 'tron |
| 2 | Strings | 0.12 | 0.10 | 1.00 | 0.45 | exp | exp | bowed strings |
| 3 | Slow Pad | 0.80 | 0.20 | 1.00 | 1.20 | exp | exp | ambient swell |
| 4 | Choir Swell | 0.40 | 0.00 | 1.00 | 0.80 | exp | exp | 8-choir |
| 5 | Pluck | 0.00 | 0.30 | 0.20 | 0.25 | lin | log | decay to low sustain |
| 6 | Stab | 0.00 | 0.18 | 0.00 | 0.12 | lin | log | percussive one-shot |
| 7 | Bowed Swell | 1.20 | 0.00 | 1.00 | 1.50 | exp | exp | slow attack lead |
| 8 | Gate | 0.00 | 0.00 | 1.00 | 0.01 | lin | lin | hard on/off |
| 9 | Reverse Swell | 2.00 | 0.00 | 1.00 | 0.40 | exp | log | swell-in fades |
| 10 | Staccato | 0.00 | 0.10 | 0.00 | 0.08 | lin | log | short |
| 11 | Drone | 0.05 | 0.00 | 1.00 | 3.00 | lin | exp | infinite ambient |
| 12 | Soft Keys | 0.01 | 0.40 | 0.55 | 0.30 | log | exp | EP-ish decay |

Store as `{name, a, d, s, r, ac, rc, vel}`; `apply_env_preset()` writes all and snaps
smoothed companions (preset-load pattern from Denis).

---

## 6. Page "Tape & Preamp" (8 knobs)

Deep tape/preamp character. `tape_style` and `preamp_model` are steppers; the rest are
0–1 ribbons, musical at any position.

| # | key | label | range | behaviour |
| --- | --- | --- | --- | --- |
| 1 | `tape_style` | Tape Style | enum | base tape voicing: Clean / M400 / MkII / M300 / Worn |
| 2 | `warble` | Warble | 0–1 | **wow** — slow pitch drift depth (→ engine read ptr) |
| 3 | `flutter` | Flutter | 0–1 | fast pitch instability / scrape depth (→ read ptr) |
| 4 | `degrade` | Degrade | 0–1 | hiss + dropout probability + grit/crinkle |
| 5 | `tape_sat` | Saturation | 0–1 | tape compression/saturation (pre-preamp) |
| 6 | `preamp_model` | Preamp | enum | Clean DI / MkII Tube / M400 Solid-State / SK-1 Lo-fi |
| 7 | `input_level` | Input | 0–1 | front trim into the tape/sat stage (distinct from Crunch) |
| 8 | `limiter` | Limiter | 0–1 | output limiter amount (also the safety net for stacked FX) |

References: Essaim (tape pitch warp, hiss/crackle, saturation curves) for warble/flutter/
degrade/sat; preamp models = voiced soft-clip curves (tube = even harmonics + sag, SS =
harder knee, SK-1 = 8-bit decimation + aliasing).

---

## 7. Page "FX" (8 knobs)

All 0–1, **0 = true bypass**, equal-power dry/wet, each a curated multi-param ribbon.
Tempo-sync time-based ones. (Tape warble moved to the Tape & Preamp page; **DJ filter**
takes knob 3.)

| # | key | label | macro maps (0 → 100 %) | reference |
| --- | --- | --- | --- | --- |
| 1 | `vibrato` | Vibrato/Chorus | subtle 1-voice vibrato → multi-voice chorus | xsynth chorus + pitch-LFO |
| 2 | `leslie` | Leslie | rotor slow→fast + horn/drum split + doppler + light drive; smooth accel | custom rotary |
| 3 | `dj_filter` | DJ Filter | bipolar: LP 200 Hz→18 kHz (0→0.5), HP 20→400 Hz (0.5→1.0); ±0.05 crossfade, memset biquad on flip | Modes (pop-free) |
| 4 | `tremolo` | Tremolo | amp-LFO depth ↑, sine→soft-square at top; synced | Essaim soft LFO |
| 5 | `phaser` | Phaser | depth + feedback + mix ↑, more resonant at top | xsynth phaser |
| 6 | `texture` | Texturizer | grain density + pitch-shift shimmer + feedback ↑; **gain-stage/voice-cap** | Bobine/Boris; Spectra/Dissolver |
| 7 | `delay` | Delay | mix + fb ↑, BBD darkening/sat in repeats; synced; fb ≤0.95 | Essaim + xsynth delay |
| 8 | `reverb` | Reverb | wet + decay + pre-delay ↑, plate/spring, darkens | xsynth reverb |

Note `dj_filter` default = **0.5** (open/bypass at center), unlike the other FX which bypass
at 0 — it's bipolar. Output protection lives on the Tape & Preamp `limiter`.

### CPU budget (ARM, ~345 blocks/s @ 128/48 k)
Expensive: two sfizz engines, `texture`, `reverb`. Cheap: modulation FX. If tight: cap
polyphony, voice-cap granular first, make Bank B optional (single engine when `ab_mix` = A).

---

## 8. Menu-only params (in `params`, not `knobs`)

`bank_a_variant` / `bank_b_variant` (pick `.sfz` variant) · `map_mode`
(Tape exact / Full range) · `loop_mode` (Tape no-loop / Sustain) · `tape_len`
(run-out ceiling s) · `chiff` (attack-transient amount) · `half_speed` (M4000D octave-down) ·
`glide` (varispeed glide time) · `octave_shift` (±12 for C3=60 vs C4=60 packs).

---

## 9. module.json skeleton (Shadow UI pattern — authoritative)

Four levels + root mirroring Main. `chain_params` **required** in `capabilities`;
`component_type` at root **and** in `capabilities`; no `ui_chain.js`; do not return
`ui_hierarchy` from `get_param`.

```jsonc
{
  "id": "mello", "name": "Mello", "dsp": "mello.so",
  "api_version": 2, "component_type": "sound_generator",
  "capabilities": {
    "component_type": "sound_generator",
    "ui_hierarchy": { "modes": null, "levels": {
      "root": { "name": "Mello",
        "knobs": ["bank_a","bank_b","ab_mix","pitch","tone","env_preset","crunch","volume"],
        "params": [ {"level":"Main","label":"Main"}, {"level":"Envelope","label":"Envelope"},
                    {"level":"Tape","label":"Tape & Preamp"}, {"level":"FX","label":"FX"} ] },
      "Main": { "label":"Main",
        "knobs": ["bank_a","bank_b","ab_mix","pitch","tone","env_preset","crunch","volume"],
        "params": [ /* per-knob defs + menu-only */ ] },
      "Envelope": { "label":"Envelope",
        "knobs": ["env_preset","env_a","env_d","env_s","env_r","env_ac","env_rc","env_vel"],
        "params": [ /* per-knob defs */ ] },
      "Tape": { "label":"Tape & Preamp",
        "knobs": ["tape_style","warble","flutter","degrade","tape_sat","preamp_model","input_level","limiter"],
        "params": [ /* per-knob defs */ ] },
      "FX": { "label":"FX",
        "knobs": ["vibrato","leslie","dj_filter","tremolo","phaser","texture","delay","reverb"],
        "params": [ /* per-knob defs */ ] }
    } },
    "chain_params": [ /* ALL params from ALL pages, with type/min/max/step/options */ ]
  }
}
```

DSP handles `set_param("_level", "Main"|"Envelope"|"Tape"|"FX"|"Mello")` for nav (map
"Mello"/"Main" → page 0) and dispatches `knob_N_adjust/name/value` per current page
(four KNOB_MAP arrays). `env_preset` shares its key across Main + Envelope (auto-mirrors).

---

## 10. Implementation reference map

| Need | Status / source |
| --- | --- |
| Multisample load (WAV I/O, voice manager, varispeed, ADSR) | **In-house** (`mello.c` + `wav_bank.c`) — no sfizz, no xsynth |
| Tape warble (slow sine 0.4 Hz) | In-house, ported pattern from Essaim |
| Tape flutter (6.5 Hz + noise) | In-house (Essaim has wow only, not flutter) |
| Tape hiss with signal-level gate | Essaim pattern, ported |
| Random dropouts + grit/crackle | In-house (Essaim has neither) |
| Saturation curve `1 + sat²·7` + RMS makeup | Essaim pattern, ported |
| Preamp models (Tube/SS/SK-1 8-bit) | In-house (tanh + asym + fold) |
| Tone tilt EQ | In-house (one-pole LP/HP split cross-fade) |
| Pop-free DJ LP/HP filter | **Modes** verbatim (3-stage biquad cascade, ±0.05 crossfade) |
| Vibrato/Chorus, Leslie, Tremolo, Phaser | In-house |
| Texturizer (WoFi-style: grain + shimmer + echo + crush) | In-house, single ribbon, ~250 lines |
| BBD delay (2-pole LP feedback + jitter LFO) | Essaim pattern, ported |
| Plate reverb (6-tap FDN + allpass diffusion) | In-house |
| Limiter (sub-ms attack, peak follower) | In-house |
| Preset apply + snap smoothed values | Denis pattern (env_preset) |
| Line-in/mic (v0.2 live sampling) | Phasma (mapped_memory + audio_in_offset) |

---

## 11. Decisions locked / Open

**Locked:**
1. **Engine** — in-house WAV sampler. Not sfizz, not xsynth, not SFZ-based.
2. **Map mode default** — Tape exact with chromatic fallback above/below the
   bank's sampled range (`mello_bank_pick_note` returns the nearest sample +
   semitone offset to be applied via varispeed).
3. **Custom preset display** — `env_preset = 12` (Custom) is set automatically
   when any `env_*` knob is edited; selecting any other preset overwrites the
   A/D/S/R/curves/vel values from the preset table.
4. **Factory banks** — all 33 Leisureland M400/MkII/Mki folders ship in
   `instruments/`. License unclear — personal use only, not for redistribution
   as Filliformes factory content without permission from Leisureland.

**Open / v0.2:**
1. **Async bank loading** — v0.1 loads synchronously on bank-change (~0.5–1 s
   glitch); v0.2 should swap to pthread worker + atomic pointer swap.
2. **Live sampling (SK-1 "sample the world")** — capture from line-in/mic via
   `g_host->mapped_memory + g_host->audio_in_offset` (Phasma pattern). Requires
   `"audio_in": true` in module.json.
3. **Bank B always-on vs. optional** — currently both engines always run; could
   skip Bank B render when `ab_mix = 0` to save CPU.
4. **Per-bank `seq_base` override** — currently a global menu param; per-bank
   would let mixed packs work without remapping.
