/* ============================================================================
 * mello.c — Mello sound generator (Schwung / Ableton Move, plugin_api_v2)
 *
 * Dual-bank tape instrument: Mellotron × Casio SK-1 × Kiviak WoFi.
 * In-house WAV sampler (no sfizz/xsynth), folder-of-WAVs banks, deep tape +
 * preamp modelling, 8-macro FX rack. See design-spec.md for architecture.
 *
 * Build (ARM64 cross via Docker, see scripts/build.sh):
 *   aarch64-linux-gnu-gcc -O3 -shared -fPIC -ffast-math -march=armv8-a \
 *       -o dsp.so mello.c wav_bank.c -lm -lpthread
 * ============================================================================ */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <sys/stat.h>
#include "wav_bank.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define TWO_PI 6.28318530717958647692f

/* ============================================================================
 * Schwung host + plugin_api_v2 typedefs (embedded — no external headers)
 * ============================================================================ */
typedef struct host_api_v1 {
    uint32_t api_version;
    uint8_t *mapped_memory;
    uint32_t audio_in_offset;
} host_api_v1_t;

typedef struct {
    uint32_t api_version;
    void *(*create_instance)(const char *, const char *);
    void  (*destroy_instance)(void *);
    void  (*on_midi)(void *, const uint8_t *, int, int);
    void  (*set_param)(void *, const char *, const char *);
    int   (*get_param)(void *, const char *, char *, int);
    int   (*get_error)(void *, char *, int);
    void  (*render_block)(void *, int16_t *, int);
} plugin_api_v2_t;

static const host_api_v1_t *g_host = NULL;

/* ============================================================================
 * Constants
 * ============================================================================ */
#define SR              48000.0f
#define SR_INV          (1.0f / 48000.0f)
#define MAX_BLOCK       512
#define NUM_VOICES      16          /* per bank */
#define INSTRUMENTS_BASE_DEFAULT \
    "/data/UserData/schwung/modules/sound_generators/mello/instruments"

/* enum option tables */
static const char *ENV_PRESETS[] = {
    /* Original 12 */
    "Tape Organ","Strings","Slow Pad","Choir Swell","Pluck","Stab",
    "Bowed Swell","Gate","Reverse Swell","Staccato","Drone","Soft Keys",
    /* 10 new musical presets curated for the Mellotron bank diversity */
    "Cinematic Rise","Pluck Bass","Soft Lead","Pad Wash","Marcato",
    "Hammer","Vox Ahh","Brass Hit","Slow Drone","Pizzicato",
    "Custom"
};
static const int ENV_PRESET_COUNT = 23;
static const char *TAPE_STYLES[]   = { "Off","Clean","M400","MkII","M300","Worn" };
static const int   TAPE_STYLE_COUNT = 6;
static const char *PREAMP_MODELS[] = { "Off","Clean DI","MkII Tube","M400 Solid-State","SK-1 Lo-fi" };
static const int   PREAMP_COUNT    = 5;
static const char *MAP_MODES[]     = { "Tape (exact)","Full range (stretched)" };
static const char *LOOP_MODES[]    = { "Tape (no loop)","Sustain (loop)" };
static const char *ONOFF[]         = { "Off","On" };

/* FX chain orderings — each preset routes the 8 effect units in a
 * different sequence.  Order changes timbre dramatically because some FX
 * (reverb, delay, texture) generate audio the downstream FX then sweep.
 *   Classic       = current order (mod → filter → time)
 *   Pedalboard    = wah → mod stack → filter → time
 *   Wet Swirl     = time first, then mod sweeps the wet tail
 *   Ambient Wash  = delay+reverb upfront so every later effect colours the
 *                   wash; texture/wah/leslie act on the smeared field
 *   Dub           = dub-console workflow — filter the dry, send to time,
 *                   then modulate the late tail
 *   Reverse       = reverb FIRST so the dry note bakes immediately, end
 *                   with texture grain-chopping the fully-effected signal */
static const char *FX_CHAIN_MODES[] = {
    "Classic", "Pedalboard", "Wet Swirl", "Ambient Wash", "Dub", "Reverse"
};
#define FX_CHAIN_COUNT 6

/* envelope preset table {a, d, s, r, atk_curve, rel_curve, vel}.
 * Curve values: 0 = log (fast-start), 0.5 = linear, 1 = exp (slow-start). */
typedef struct { float a, d, s, r, ac, rc, vel; } env_def_t;
static const env_def_t ENV_DEFS[22] = {
    /* Original 12 */
    {0.05f, 0.00f, 1.00f, 0.50f, 0.5f, 0.5f, 0.0f},  /* Tape Organ */
    {0.12f, 0.10f, 1.00f, 0.45f, 1.0f, 1.0f, 0.0f},  /* Strings    */
    {0.80f, 0.20f, 1.00f, 1.20f, 1.0f, 1.0f, 0.0f},  /* Slow Pad   */
    {0.40f, 0.00f, 1.00f, 0.80f, 1.0f, 1.0f, 0.0f},  /* Choir Swell*/
    {0.00f, 0.30f, 0.20f, 0.25f, 0.5f, 0.0f, 0.0f},  /* Pluck      */
    {0.00f, 0.18f, 0.00f, 0.12f, 0.5f, 0.0f, 0.0f},  /* Stab       */
    {1.20f, 0.00f, 1.00f, 1.50f, 1.0f, 1.0f, 0.0f},  /* Bowed Swell*/
    {0.00f, 0.00f, 1.00f, 0.01f, 0.5f, 0.5f, 0.0f},  /* Gate       */
    {2.00f, 0.00f, 1.00f, 0.40f, 1.0f, 0.0f, 0.0f},  /* Reverse Swell */
    {0.00f, 0.10f, 0.00f, 0.08f, 0.5f, 0.0f, 0.0f},  /* Staccato   */
    {0.05f, 0.00f, 1.00f, 3.00f, 0.5f, 1.0f, 0.0f},  /* Drone      */
    {0.01f, 0.40f, 0.55f, 0.30f, 0.0f, 1.0f, 0.0f},  /* Soft Keys  */
    /* New presets (curated for Mellotron banks) */
    {3.00f, 0.00f, 1.00f, 2.00f, 1.0f, 1.0f, 0.0f},  /* Cinematic Rise — long swell pads/strings */
    {0.00f, 0.15f, 0.00f, 0.10f, 0.5f, 0.0f, 0.3f},  /* Pluck Bass — fast attack, fast decay, vel-sensitive */
    {0.05f, 0.00f, 1.00f, 0.40f, 1.0f, 1.0f, 0.0f},  /* Soft Lead — gentle articulated lead */
    {1.50f, 0.50f, 0.70f, 2.00f, 1.0f, 1.0f, 0.0f},  /* Pad Wash — washy ambient pad */
    {0.02f, 0.05f, 0.90f, 0.18f, 0.5f, 0.5f, 0.4f},  /* Marcato — articulated, vel-sensitive */
    {0.00f, 0.60f, 0.00f, 0.40f, 0.5f, 0.0f, 0.2f},  /* Hammer — mallet/bell attack with tail */
    {0.30f, 0.20f, 0.85f, 0.80f, 1.0f, 1.0f, 0.0f},  /* Vox Ahh — vocal swell for choir banks */
    {0.02f, 0.40f, 0.55f, 0.30f, 0.5f, 0.0f, 0.5f},  /* Brass Hit — punchy brass with vel response */
    {1.00f, 0.00f, 1.00f, 4.00f, 1.0f, 1.0f, 0.0f},  /* Slow Drone — very long release ambient */
    {0.00f, 0.18f, 0.00f, 0.06f, 0.5f, 0.0f, 0.5f},  /* Pizzicato — tight pluck, no tail */
};

/* ============================================================================
 * Voice (one per polyphony slot per bank)
 * ============================================================================ */
typedef enum { ENV_IDLE = 0, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE } env_stage_t;

typedef struct {
    int          active;
    int          midi_note;        /* note pressed (may differ from slot) */
    int          slot;             /* bank slot whose sample we're playing */
    int          pitch_offset;     /* semitone offset (note - slot) */
    float        velocity;         /* 0..1 */
    double       read_pos;         /* fractional frame pos in samples[] */
    float        base_rate;        /* sample-rate ratio + slot pitch (per-sample mul) */
    int          reverse;          /* 0 forward, 1 reverse playback */
    env_stage_t  stage;
    float        env;              /* current envelope amplitude */
    float        atk_a, atk_b;     /* linear rise per sample (will be curve-shaped) */
    float        dec_rate;
    float        sustain;
    float        rel_rate;
    uint32_t     age;              /* for voice stealing */
} voice_t;

/* ============================================================================
 * Bank slot (one of two — A and B)
 * ============================================================================ */
typedef struct {
    mello_bank_t bank;
    voice_t      voices[NUM_VOICES];
    int          loaded_idx;       /* index into instruments list, -1 if unloaded */
    int          requested_idx;    /* latest async-load target ( -1 = idle ) */
    /* ---- async bank loader (per slot) ----
     * set_param posts a request via requested_idx, the worker loads the bank
     * into pending_bank off the audio/event threads, and the audio thread
     * picks it up via check_and_swap_pending() at the start of render_block. */
    mello_bank_t      pending_bank;
    int               pending_idx;
    volatile int      pending_ready;
    pthread_mutex_t   loader_mtx;
    pthread_cond_t    loader_cv;
    pthread_t         loader_tid;
    volatile int      loader_run;
    void             *m_ref;        /* back-pointer (void*; cast to mello_t*) */
} bank_slot_t;

/* per-voice "tape retract" chiff burst, triggered on note-off */
typedef struct {
    int   active;
    float env;          /* fast attack, ~80 ms decay */
    float dec;
    float lp;           /* one-pole LP state */
} chiff_voice_t;
#define CHIFF_POOL 8

/* ============================================================================
 * FX state
 * ============================================================================ */
typedef struct { float b0, b1, b2, a1, a2; float x1, x2, y1, y2; } biquad_t;

#define DELAY_MAX_SAMPLES (SR * 2.0f)        /* 2 s max for delay */
#define TEX_GRAIN_BUF     (32768)            /* ~683 ms @ 48k — bigger so
                                                pitch>1 grains don't outrun
                                                the write head into stale
                                                audio (= clicks) */
#define TEX_MAX_GRAINS    6
#define REVERB_DLY_MAX    32768              /* ~680 ms — was 8192 (too small/metallic) */
#define LESLIE_DLY_MAX    1024               /* doppler */
#define VIB_DLY_MAX       2048               /* chorus */
/* (was PHASER_STAGES; phaser replaced by Auto-Wah in v0.1.12) */

typedef struct {
    float buf_l[(int)DELAY_MAX_SAMPLES];
    float buf_r[(int)DELAY_MAX_SAMPLES];
    int   wp;
    float lp1_l, lp2_l, lp1_r, lp2_r;        /* 2-pole feedback LP (BBD) */
    float jitter_phase;
    /* Smoothed per-sample param shadows. Without these, turning the delay
     * knob = abrupt tap-position jumps = clicks. With them the time changes
     * glide like a real tape capstan slow-down. */
    float dly_smooth;                        /* float samples, linear-interp read */
    float fb_smooth;
    float wet_smooth;
    float input_gain_smooth;
} delay_state_t;

typedef struct {
    float buf_l[VIB_DLY_MAX], buf_r[VIB_DLY_MAX];
    int   wp;
    float phase_a, phase_b, phase_c;          /* multi-voice chorus phases */
    /* Incremental sin/cos state for each LFO — avoids per-sample sinf().
     * Maintains (c,s) on the unit circle, rotates by (dc,ds) each sample.
     * `(c,s)` is the running quadrature value; `phase_*` is kept in sync so
     * a rate change can re-seed without a phase jump. */
    float c_a, s_a, c_b, s_b, c_c, s_c;
} vibrato_state_t;

typedef struct {
    /* MONO pre-rotation delay lines — one for the HF horn, one for the LF
     * drum.  We read TWO taps from each (left & right listener positions)
     * with per-tap delay length set by the geometric distance from the
     * rotating point source to that listener.  This is the physical model
     * from LeslieSim / Surge XT — variable-length delay = doppler shift,
     * no separate pitch-shift pass needed. */
    float horn_buf[LESLIE_DLY_MAX], drum_buf[LESLIE_DLY_MAX];
    int   wp;
    float horn_phase, drum_phase;
    float horn_rate, drum_rate;               /* smoothed speeds */
    float xover_lp;                           /* 800 Hz one-pole crossover state */
    /* Incremental quadrature oscillators — replace per-sample sinf/cosf
     * with one float-mul-add per sample (the rotation matrix).  Rate-change
     * paths re-seed both (c,s) and the delta from horn_phase / horn_rate. */
    float horn_c, horn_s, drum_c, drum_s;
} leslie_state_t;

typedef struct {
    float env;                         /* envelope follower output */
    biquad_t bq_l, bq_r;                /* resonant bandpass per channel */
} autowah_state_t;

typedef struct {
    float grain_buf[TEX_GRAIN_BUF];           /* mono ring of incoming audio */
    int   wp;
    /* per-grain: read pos in buf (fractional), age (samples since start), len, pitch */
    float g_read[TEX_MAX_GRAINS];
    int   g_age[TEX_MAX_GRAINS];
    int   g_len[TEX_MAX_GRAINS];
    float g_pitch[TEX_MAX_GRAINS];
    float g_pan[TEX_MAX_GRAINS];
    int   spawn_counter;
    /* shimmer SVF (bandpass tuned ~5-7kHz with long decay feedback) */
    float shim_lp_l, shim_bp_l, shim_lp_r, shim_bp_r;
    float shim_fb;
    /* echo tap */
    float echo_buf[8192];
    int   echo_wp;
    /* Vintage warmth — one-pole LP on the grain output, rolls off above
     * ~4 kHz to give the cloud an analog tape-style "warm" character that
     * masks the up-pitch grain harshness.  Per-channel state for stereo. */
    float warmth_l, warmth_r;
} texture_state_t;

typedef struct {
    float buf_l[REVERB_DLY_MAX], buf_r[REVERB_DLY_MAX];
    int   wp;                                 /* single write head, 6 read taps */
    float ap_l[4], ap_r[4];                   /* small all-passes */
    float lp_l, lp_r;
    float tap_lfo_phase[6];                   /* persistent async LFO phases */
    /* Incremental quadrature for the 6 tap-position LFOs — avoids 6 sinf
     * calls per sample (the heaviest single LFO cost in the rack). */
    float tap_lfo_c[6], tap_lfo_s[6];
    /* 120 Hz one-pole HPF on the input — kills the sub-bass buildup that
     * happens when sustained low notes accumulate in the diffuser loop
     * (user-reported "really big build up in the low sub frequencies"). */
    float hpf_x1, hpf_y1;
    /* "Off-state" detector — when amt has been below threshold for a
     * full quiet window, we zero out the loop state so the comb network's
     * natural resonance can't keep ringing audibly on residual content
     * from earlier playing.  Stops the "reverb still emits a tone when I
     * turn the knob even without any sound coming in" symptom. */
    int   off_counter;
} reverb_state_t;

typedef struct {
    biquad_t dj_bq_l[3], dj_bq_r[3];
    biquad_t head_bump_l, head_bump_r;     /* tape head resonance @ ~120 Hz */
    int      prev_dj_mode;
    float    dj_djf_smooth;                /* per-sample-smoothed cutoff target */
    float    tone_lo_l, tone_lo_r;            /* tilt EQ one-pole accumulators */
    float    tone_hi_l, tone_hi_r;
    /* tape stage HF rolloff state — was function-static which caused
     * cross-instance audio bleed when two Mellos run on different tracks. */
    float    tape_hf_l, tape_hf_r;
    /* limiter */
    float    lim_env;
    /* hiss / crackle rng */
    uint32_t rng;
    /* wow + flutter shared LFOs (per-voice pitch mod) */
    float    wow_phase;
    float    flutter_phase;
    /* tremolo */
    float    trem_phase;
} fx_state_t;

/* ============================================================================
 * Instance
 * ============================================================================ */
typedef struct {
    /* ---- config ---- */
    float    sr;
    int      current_page;
    /* Two-tier WAV layout: `instruments_root` holds the top-level library
     * folders (Leisureland, Sonic Bloom, …); `instruments_dir` is the
     * currently-active library = root + "/" + lib_list.names[p_sample_lib].
     * The Main-page "Samples Folder" param swaps libraries at runtime. */
    char     instruments_root[512];
    char     instruments_dir[512];

    /* ---- discovered sample libraries (top-level subdirs of root) ---- */
    mello_bank_list_t lib_list;
    int   p_sample_lib;           /* index into lib_list */
    /* lib_first_idx[i] = blist index of the first bank in library i.
     * Used by sample_lib quick-jump to land on a library's first bank.
     * (Library N's banks occupy blist[lib_first_idx[N] .. lib_first_idx[N+1]-1].)
     * Sized to one slot per possible library — far less than MELLO_MAX_BANKS=256
     * which was bank-count-sized in error. */
    int   lib_first_idx[64 + 1];

    /* ---- flattened union bank list across all libraries.
     * Names are "Library/Bank" so bank_a/bank_b enums show every bank at
     * once.  The host caches chain_params options per-instance, so we
     * can't selectively filter on sample_lib change — flat list lets the
     * user reach any bank without a host-side enum refresh. */
    mello_bank_list_t blist;

    /* ---- two playback slots (A/B knob-controlled, equal-power mix) ---- */
    bank_slot_t a, b;
    pthread_mutex_t bank_lock;

    /* tape retract chiff (key-off) pool */
    chiff_voice_t chiff_pool[CHIFF_POOL];

    /* mechanical noise LFO + slow instability wander */
    float mech_phase;
    float instab_state, instab_target;
    uint32_t instab_rng_step;

    /* aftertouch-driven flutter add-on (channel + last poly AT) */
    float at_flutter;
    /* MIDI pitch bend — set by on_midi from 0xE0 messages, ±2 semitones
     * range.  Smoothed per block via sm_pitch_bend_semi, added to the
     * per-sample rate calc alongside the Pitch knob. */
    float p_pitch_bend_semi;
    float sm_pitch_bend_semi;
    /* MIDI mod wheel (CC1) — 0..1.  Added to vibrato amount so the user
     * gets classic Mellotron "wheel up = more wobble" behaviour without
     * touching the Vibrato knob. */
    float p_mod_wheel;
    float sm_mod_wheel;

    /* ---- parameters (raw + smoothed where needed) ---- */
    /* Main page */
    int   p_bank_a, p_bank_b;
    int   p_pitch;                /* -24..+24 semitones */
    float p_ab_mix;
    float p_tone;                 /* 0..1, 0.5 = flat */
    int   p_env_preset;           /* 0..12 (12 = Custom) */
    float p_crunch;
    float p_volume;

    /* Envelope page (live; preset edits write through) */
    float p_env_a, p_env_d, p_env_s, p_env_r, p_env_ac, p_env_rc, p_env_vel;

    /* Tape & Preamp page */
    int   p_tape_style;
    float p_warble;
    float p_flutter;
    float p_degrade;
    float p_tape_sat;
    int   p_preamp_model;
    float p_input_level;
    float p_limiter;
    float p_tape_len;             /* run-out ceiling, seconds (1..16) */
    float p_chiff;                /* key-off tape retract noise amount */
    float p_bias;                 /* 0..1, 0.5 = neutral */
    float p_mech;                 /* mechanical transport noise */

    /* FX page */
    float p_vibrato, p_leslie, p_dj_filter, p_tremolo, p_autowah, p_texture,
          p_delay, p_reverb;
    int   p_fx_chain;             /* enum FX_CHAIN_MODES — routing order */

    /* Menu-only */
    int   p_map_mode;
    int   p_loop_mode;
    int   p_half_speed;
    int   p_auto_retune;          /* 0 Off, 1 On — pitch-correct mis-tuned WAVs */
    int   p_seq_base;             /* -1 = auto, else MIDI offset for *-N */
    int   p_reverse;              /* 0 forward, 1 reverse */
    float p_sample_start;         /* 0..1 fraction of WAV to skip on note-on */
    int   p_xfade_pct;            /* 0..100 — sustain-loop crossfade width
                                   *   0   ≈ 5 ms  (clicky, fastest)
                                   *  50   ≈ 60 ms (default, glitch-free for
                                   *               most sustained content)
                                   * 100   ≈ 130 ms (Blackbox-style very long
                                   *               smear, may sound phasey on
                                   *               percussive content) */

    /* ---- smoothed companions ---- */
    float sm_ab_mix, sm_tone, sm_crunch, sm_volume;
    float sm_pitch_semi;
    float sm_warble, sm_flutter, sm_degrade, sm_tape_sat, sm_input, sm_lim;
    float sm_vibrato, sm_leslie, sm_dj_filter, sm_tremolo, sm_autowah, sm_texture,
          sm_delay, sm_reverb;
    float sm_bias, sm_mech, sm_chiff;

    /* ---- FX state ---- */
    delay_state_t   dly;
    vibrato_state_t vib;
    leslie_state_t  lez;
    autowah_state_t wah;
    texture_state_t tex;
    reverb_state_t  rev;
    fx_state_t      fx;

} mello_t;

/* Forward declarations needed because set_param() (mid-file) reaches into
 * the library-management helpers (defined down in the lifecycle section). */
static int  activate_sample_library(mello_t *m, int idx);

/* ============================================================================
 * Math helpers
 * ============================================================================ */
static inline float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}
static inline float fast_tanh(float x) {
    /* Padé approx — accurate within ±3. The function PEAKS at y=1 at |x|=3
     * and then grows back up (y → x/9 for large |x|). Clamp the input range
     * so the output is bounded to ±1 — without this clamp every "saturation"
     * curve in the file becomes an amplifier above moderate drive levels. */
    if (x >=  3.0f) return  1.0f;
    if (x <= -3.0f) return -1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}
static inline uint32_t xorshift32(uint32_t *s) {
    uint32_t x = *s; if (x == 0) x = 0x12345678u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x; return x;
}
static inline float randf_pm(uint32_t *s) {
    /* -1..+1 */
    return (float)(int32_t)xorshift32(s) / 2147483648.0f;
}
static inline float randf01(uint32_t *s) {
    return (xorshift32(s) >> 8) * (1.0f / 16777216.0f);
}
static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

/* fast 2^x approximation, good for varispeed pitch */
static inline float fast_exp2(float x) {
    float xx = x - floorf(x);
    float frac = xx * (xx * (0.336262f * xx - 0.034889f) + 0.659896f) + 1.000000f;
    int   ix   = (int)floorf(x);
    union { uint32_t u; float f; } u;
    u.u = ((uint32_t)(ix + 127)) << 23;
    return frac * u.f;
}
/* Fast log2 approximation — IEEE-754 exponent extraction + cubic mantissa
 * polynomial.  Used by fast_pow in env_shape.
 *
 * v0.1.25: previous polynomial was wrong — peaked at ~0.52 at m=2 instead
 * of 1.0, so fast_pow(env, e) computed roughly env^(e/2) instead of env^e.
 * That broke the envelope shape curves and produced audible clicks at
 * note release (especially with rc > 0.5, where the curve should taper
 * gracefully but was instead chopping off near zero).  Replaced with a
 * Hermite-interpolant cubic that's exact at m=1, m=2, and matches the
 * log2 derivative at both endpoints — max error ~0.005, inaudible. */
static inline float fast_log2(float x) {
    if (x <= 0.0f) return -127.0f;
    union { float f; uint32_t u; } v;
    v.f = x;
    int   e  = (int)((v.u >> 23) & 0xFF) - 127;
    v.u = (v.u & 0x007FFFFF) | 0x3F800000;         /* mantissa as 1.xxx */
    float m  = v.f;
    /* Hermite cubic for log2(m) on [1,2]:
     *   p(t) = t · (1.44269504 + t · (-0.60673760 + t · 0.16404256))
     * where t = m - 1.  Exact at t=0 and t=1, matches log2'(1) = 1/ln 2
     * and log2'(2) = 1/(2 ln 2). */
    float t  = m - 1.0f;
    float p  = t * (1.44269504f + t * (-0.60673760f + t * 0.16404256f));
    return (float)e + p;
}
/* fast_pow(b, e) for b in (0, ~]: 2^(e · log2(b)).  Good enough for
 * audio-rate envelope curves where small errors (sub-0.001) are inaudible. */
static inline float fast_pow(float b, float e) {
    return fast_exp2(e * fast_log2(b));
}

/* ============================================================================
 * Envelope (per-voice ADSR with shaped curves)
 * ============================================================================ */
static void env_start(voice_t *v, const mello_t *m) {
    v->stage = ENV_ATTACK;
    v->env = 0.0f;
    float a_s = m->p_env_a; if (a_s < 0.0005f) a_s = 0.0005f;
    v->atk_a = 1.0f / (a_s * SR);
    float d_s = m->p_env_d; if (d_s < 0.0005f) d_s = 0.0005f;
    v->dec_rate = 1.0f / (d_s * SR);
    v->sustain = m->p_env_s;
    float r_s = m->p_env_r; if (r_s < 0.0005f) r_s = 0.0005f;
    v->rel_rate = 1.0f / (r_s * SR);
}
static void env_release(voice_t *v, const mello_t *m) {
    if (v->stage == ENV_IDLE) return;
    v->stage = ENV_RELEASE;
    float r_s = m->p_env_r; if (r_s < 0.0005f) r_s = 0.0005f;
    /* Mr-Sample style release scaling: `rel_rate` is the env DECREMENT
     * per sample, so a fixed `1/(r_s*SR)` makes a half-attack note
     * (env=0.5) release in r_s/2 seconds instead of r_s — that abrupt
     * snap was a click source on staccato playing.  Scaling by current
     * env value keeps time-to-zero = r_s seconds regardless of where in
     * the envelope the release was triggered. */
    float env_now = v->env;
    if (env_now < 0.001f) env_now = 0.001f;
    v->rel_rate = env_now / (r_s * SR);
}
static inline float env_step(voice_t *v) {
    switch (v->stage) {
        case ENV_ATTACK:
            v->env += v->atk_a;
            if (v->env >= 1.0f) { v->env = 1.0f; v->stage = ENV_DECAY; }
            break;
        case ENV_DECAY:
            v->env -= v->dec_rate;
            if (v->env <= v->sustain) { v->env = v->sustain; v->stage = ENV_SUSTAIN; }
            break;
        case ENV_SUSTAIN:
            break;
        case ENV_RELEASE:
            v->env -= v->rel_rate;
            if (v->env <= 0.0f) { v->env = 0.0f; v->stage = ENV_IDLE; v->active = 0; }
            break;
        case ENV_IDLE:
        default:
            break;
    }
    return v->env;
}

/* Apply attack/release curve shape via exponent on the [0..1] env value.
 * ac/rc in [0..1]: 0.5 = linear; <0.5 = log/fast-start; >0.5 = exp/slow-start.
 * Uses fast_pow (= fast_exp2 ∘ fast_log2) instead of libm powf — about 3×
 * cheaper per call.  env_shape was the hottest powf in the plugin (called
 * up to NUM_VOICES × frames per block). */
static inline float env_shape(float env, float ac, float rc, env_stage_t s) {
    if (env <= 0.0f) return 0.0f;                  /* fast_log2 needs > 0 */
    float shape = (s == ENV_RELEASE) ? rc : ac;
    /* exponent: 0.3 (fast-start) .. 1.0 (linear) .. 3.0 (slow-start) */
    float e = (shape < 0.5f) ? lerpf(0.3f, 1.0f, shape * 2.0f)
                              : lerpf(1.0f, 3.0f, (shape - 0.5f) * 2.0f);
    return fast_pow(env, e);
}

/* ============================================================================
 * Bank loading (synchronous; called from set_param, NOT from render).
 * The bank_lock protects access during atomic swaps.
 * ============================================================================ */
static void apply_env_preset(mello_t *m, int idx);
static void trigger_chiff(mello_t *m);

/* Synchronous load — used for the initial bank load in create_instance, where
 * we want the bank ready before the first note can be played. NOT used during
 * regular runtime; see request_bank_load_async + the worker thread below. */
static void load_bank_slot(mello_t *m, bank_slot_t *slot, int idx) {
    if (idx < 0 || idx >= m->blist.count) return;
    if (slot->loaded_idx == idx) return;

    pthread_mutex_lock(&m->bank_lock);
    for (int i = 0; i < NUM_VOICES; i++) {
        if (slot->voices[i].active && slot->voices[i].stage != ENV_IDLE) {
            slot->voices[i].stage    = ENV_RELEASE;
            slot->voices[i].rel_rate = 1.0f / (0.020f * SR);
        }
    }
    pthread_mutex_unlock(&m->bank_lock);

    /* blist names are "Library/Bank"; resolve against instruments_root so
     * we can address any bank in any library without re-pointing _dir. */
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", m->instruments_root, m->blist.names[idx]);
    mello_bank_t fresh = {0};
    fresh.lo_note = -1; fresh.hi_note = -1;
    int n = mello_bank_scan(path, &fresh, m->p_seq_base);

    pthread_mutex_lock(&m->bank_lock);
    for (int i = 0; i < NUM_VOICES; i++) slot->voices[i].active = 0;
    mello_bank_free(&slot->bank);
    slot->bank = fresh;
    slot->loaded_idx = (n > 0) ? idx : -1;
    pthread_mutex_unlock(&m->bank_lock);
}

/* Background worker — one per slot. Sleeps on loader_cv until set_param posts
 * a requested_idx, then loads that bank off the audio thread, then publishes
 * into pending_bank for the audio thread to pick up at its next block. */
static void *bank_loader_thread(void *arg) {
    bank_slot_t *slot = (bank_slot_t *)arg;
    mello_t     *m    = (mello_t *)slot->m_ref;
    while (slot->loader_run) {
        pthread_mutex_lock(&slot->loader_mtx);
        while (slot->loader_run && slot->requested_idx < 0) {
            pthread_cond_wait(&slot->loader_cv, &slot->loader_mtx);
        }
        if (!slot->loader_run) {
            pthread_mutex_unlock(&slot->loader_mtx);
            break;
        }
        int idx = slot->requested_idx;
        slot->requested_idx = -1;
        pthread_mutex_unlock(&slot->loader_mtx);

        if (idx < 0 || idx >= m->blist.count) continue;
        mello_bank_t fresh = {0};
        fresh.lo_note = -1; fresh.hi_note = -1;
        /* "Library/Bank" prefix resolves under instruments_root */
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", m->instruments_root, m->blist.names[idx]);
        mello_bank_scan(path, &fresh, m->p_seq_base);

        /* Publish — but if a newer request came in mid-load, drop this one. */
        pthread_mutex_lock(&slot->loader_mtx);
        if (slot->requested_idx >= 0) {
            pthread_mutex_unlock(&slot->loader_mtx);
            mello_bank_free(&fresh);
            continue;
        }
        if (slot->pending_ready) {
            /* prior pending never consumed — replace it */
            mello_bank_free(&slot->pending_bank);
        }
        slot->pending_bank  = fresh;
        slot->pending_idx   = idx;
        slot->pending_ready = 1;
        pthread_mutex_unlock(&slot->loader_mtx);
    }
    return NULL;
}

/* Called from set_param when the user turns the Bank knob. Returns instantly
 * — the file I/O happens on the worker thread. */
static void request_bank_load_async(mello_t *m, bank_slot_t *slot, int idx) {
    if (idx < 0 || idx >= m->blist.count) return;
    if (idx == slot->loaded_idx) return;

    /* Pre-emptive fast release on whatever is sounding — by the time the
     * worker finishes (~500 ms), voices have long since faded silent. */
    pthread_mutex_lock(&m->bank_lock);
    for (int i = 0; i < NUM_VOICES; i++) {
        if (slot->voices[i].active && slot->voices[i].stage != ENV_IDLE) {
            slot->voices[i].stage    = ENV_RELEASE;
            slot->voices[i].rel_rate = 1.0f / (0.020f * SR);
        }
    }
    pthread_mutex_unlock(&m->bank_lock);

    /* Wake the worker with the new target. */
    pthread_mutex_lock(&slot->loader_mtx);
    slot->requested_idx = idx;
    pthread_cond_signal(&slot->loader_cv);
    pthread_mutex_unlock(&slot->loader_mtx);
}

/* Called from render_block at the top — does a quick non-blocking check and,
 * if the worker has finished, atomically swaps the new bank into place. */
static void check_and_swap_pending(mello_t *m, bank_slot_t *slot) {
    if (!slot->pending_ready) return;     /* fast path, no lock */
    pthread_mutex_lock(&slot->loader_mtx);
    if (!slot->pending_ready) {
        pthread_mutex_unlock(&slot->loader_mtx);
        return;
    }
    mello_bank_t fresh = slot->pending_bank;
    int          nidx  = slot->pending_idx;
    slot->pending_ready = 0;
    memset(&slot->pending_bank, 0, sizeof(slot->pending_bank));
    pthread_mutex_unlock(&slot->loader_mtx);

    /* Swap into the live slot, holding the audio-thread bank_lock. Voices are
     * silenced — they had ~500 ms to fade via the pre-load fast-release, so
     * setting active=0 here is inaudible. */
    pthread_mutex_lock(&m->bank_lock);
    for (int i = 0; i < NUM_VOICES; i++) slot->voices[i].active = 0;
    mello_bank_free(&slot->bank);
    slot->bank = fresh;
    slot->loaded_idx = (fresh.count > 0) ? nidx : -1;
    pthread_mutex_unlock(&m->bank_lock);
}

/* ============================================================================
 * Voice allocation (in a single bank slot)
 * ============================================================================ */
static voice_t *alloc_voice(bank_slot_t *slot, uint32_t now) {
    /* free first */
    for (int i = 0; i < NUM_VOICES; i++) {
        if (!slot->voices[i].active) return &slot->voices[i];
    }
    /* steal oldest */
    int best = 0; uint32_t oldest = slot->voices[0].age;
    for (int i = 1; i < NUM_VOICES; i++) {
        if (slot->voices[i].age < oldest) { oldest = slot->voices[i].age; best = i; }
    }
    (void)now;
    return &slot->voices[best];
}

static void release_note(bank_slot_t *slot, int note, mello_t *m) {
    int hit = 0;
    for (int i = 0; i < NUM_VOICES; i++) {
        if (slot->voices[i].active && slot->voices[i].midi_note == note &&
            slot->voices[i].stage != ENV_RELEASE) {
            env_release(&slot->voices[i], m);
            hit = 1;
        }
    }
    if (hit) trigger_chiff(m);
}

static void trigger_note(bank_slot_t *slot, mello_t *m, int note, int vel, uint32_t now) {
    if (!slot->bank.count) return;
    int offset = 0;
    int sl = mello_bank_pick_note(&slot->bank, note, m->p_map_mode, &offset);
    if (sl < 0) return;
    voice_t *v = alloc_voice(slot, now);
    v->active = 1;
    v->age = now;
    v->midi_note = note;
    v->slot = sl;
    v->pitch_offset = offset;
    v->velocity = (float)vel / 127.0f;
    v->reverse = m->p_reverse ? 1 : 0;
    /* sample_start: skip a fraction of the WAV on note-on (Arturia-style),
     * reverse: start from the end and decrement read_pos */
    int frames = slot->bank.notes[sl].frames;
    if (v->reverse) {
        v->read_pos = (double)(frames - 2);
        if (m->p_sample_start > 0.001f && m->p_sample_start < 1.0f)
            v->read_pos = (double)(frames - 2) * (1.0 - (double)m->p_sample_start);
    } else {
        v->read_pos = (m->p_sample_start > 0.001f && m->p_sample_start < 1.0f)
                      ? (double)frames * (double)m->p_sample_start : 0.0;
    }
    /* base rate: original_sr/SR; sign carries reverse direction. */
    float rate = (float)slot->bank.notes[sl].orig_sr / SR;
    rate *= fast_exp2((float)offset / 12.0f);
    /* Auto-detune: apply the per-sample cents correction (set at bank-scan
     * time by detect_pitch_cents — only nonzero if a sample drifted >5 cents
     * from its assigned MIDI note). User can disable via the auto_retune
     * menu param (Off/On). */
    if (m->p_auto_retune) {
        float cents = slot->bank.notes[sl].pitch_cents;
        if (cents != 0.0f) rate *= fast_exp2(cents * (1.0f / 1200.0f));
    }
    if (v->reverse) rate = -rate;
    v->base_rate = rate;
    env_start(v, m);
}

/* spawn a tape-retract noise burst (called from release_note) */
static void trigger_chiff(mello_t *m) {
    if (m->sm_chiff < 0.01f) return;
    for (int i = 0; i < CHIFF_POOL; i++) {
        if (!m->chiff_pool[i].active) {
            m->chiff_pool[i].active = 1;
            m->chiff_pool[i].env = 1.0f;
            /* ~80 ms decay: dec = 1/(0.080*SR) */
            m->chiff_pool[i].dec = 1.0f / (0.080f * SR);
            m->chiff_pool[i].lp = 0.0f;
            return;
        }
    }
}

/* ============================================================================
 * Per-sample bank rendering (one slot → mono float into out[frames])
 * ============================================================================ */
static void render_bank_slot(mello_t *m, bank_slot_t *slot, float *out_mono,
                             int frames, float wow_mod, float flutter_mod_arr[]) {
    memset(out_mono, 0, sizeof(float) * (size_t)frames);
    if (!slot->bank.count || slot->loaded_idx < 0) return;

    /* Pitch knob in semitones, plus half-speed octave-down, plus MIDI
     * pitch-bend wheel value (smoothed). */
    float pitch_semi = m->sm_pitch_semi + m->sm_pitch_bend_semi;
    if (m->p_half_speed) pitch_semi -= 12.0f;

    /* Pitch-multiplier hoisted to block scope — was computed per voice
     * (up to 16× per block) but `pitch_semi` is constant across voices.
     * One fast_exp2 per block now. */
    const float pitch_mul = fast_exp2(pitch_semi * (1.0f / 12.0f));

    /* tape_len ceiling (samples) — when read_pos exceeds, voice goes idle */
    float tape_len_samples = m->p_tape_len * SR;
    /* (Stored in voice-native samples since base_rate already accounts for SR;
     * approximate is fine for a soft ceiling.) */

    for (int vi = 0; vi < NUM_VOICES; vi++) {
        voice_t *v = &slot->voices[vi];
        if (!v->active) continue;
        const mello_wav_t *w = &slot->bank.notes[v->slot];
        if (!w || !w->samples) { v->active = 0; continue; }
        int max_frame = w->frames - 2;
        if (max_frame < 0) { v->active = 0; continue; }

        for (int i = 0; i < frames; i++) {
            /* wow_mod is slow (~0.3 Hz) in cents/16; flutter_mod_arr[i] is fast
             * (~7 Hz + noise) per-sample. Aftertouch adds to flutter depth.
             * Bias affects the tape's HF tracking (subtle pitch drift). */
            float wow_cents = wow_mod * m->sm_warble * 35.0f;
            /* Aftertouch → flutter scaling bumped 0.6 → 1.0 (v0.1.31) so
             * pressing harder on the keyboard produces an audibly
             * stronger tape wobble even when the Flutter knob is at 0.
             * Combined depth still clamped to 1.5 below. */
            float flutter_eff = m->sm_flutter + m->at_flutter * 1.0f;
            if (flutter_eff > 1.5f) flutter_eff = 1.5f;
            float flutter_cents = flutter_mod_arr[i] * flutter_eff * 25.0f;
            /* slow random "instability" wander when flutter > 0.7 */
            float instab_cents = 0.0f;
            if (flutter_eff > 0.7f) {
                instab_cents = m->instab_state * (flutter_eff - 0.7f) * 50.0f;
            }
            float rate = v->base_rate * pitch_mul *
                         fast_exp2((wow_cents + flutter_cents + instab_cents) * (1.0f / 1200.0f));

            /* linear-interp read; handle forward AND reverse direction */
            double rp = v->read_pos;
            int ip = (int)rp;
            int sustain_loop = (m->p_loop_mode == 1) && !v->reverse &&
                               (w->loop_end > w->loop_start + 100);
            if (v->reverse) {
                if (ip <= 0) { env_release(v, m); ip = 0; }
            } else if (!sustain_loop) {
                if (ip >= max_frame) { env_release(v, m); ip = max_frame; }
            } else {
                /* sustain loop active — never release on end. Just keep ip
                 * in range; the wrap happens after we advance read_pos. */
                if (ip > max_frame) ip = max_frame;
            }
            float frac = (float)(rp - (double)ip);
            int ip1 = ip + 1; if (ip1 > max_frame) ip1 = max_frame;
            float s0 = (float)w->samples[ip]  * (1.0f / 32768.0f);
            float s1 = (float)w->samples[ip1] * (1.0f / 32768.0f);
            float s  = s0 + (s1 - s0) * frac;

            /* Sustain loop crossfade — width controlled by p_xfade_pct
             * (0..100 maps to ~240..6240 frames = ~5..130 ms at 48 kHz,
             * Blackbox-style).  Cap by half the loop length so the xfade
             * region never overruns the loop region itself. */
            int xfade_frames = 0;
            if (sustain_loop) {
                int p = m->p_xfade_pct;
                if (p < 0)   p = 0;
                if (p > 100) p = 100;
                xfade_frames = 240 + p * 60;     /* 240 → 6240 samples */
                int half_loop = (w->loop_end - w->loop_start) / 2;
                if (xfade_frames > half_loop) xfade_frames = half_loop;
                if (xfade_frames < 16)        xfade_frames = 16;
            }
            if (sustain_loop && xfade_frames > 0) {
                double xf_zone = (double)w->loop_end - (double)xfade_frames;
                if (v->read_pos >= xf_zone && v->read_pos < (double)w->loop_end) {
                    double into_xf = v->read_pos - xf_zone;
                    int   sip  = w->loop_start + (int)into_xf;
                    float sfrac = (float)(into_xf - (double)(int)into_xf);
                    int   sip1 = sip + 1;
                    if (sip1 < w->frames) {
                        float ss0 = (float)w->samples[sip]  * (1.0f / 32768.0f);
                        float ss1 = (float)w->samples[sip1] * (1.0f / 32768.0f);
                        float ss  = ss0 + (ss1 - ss0) * sfrac;
                        /* Equal-power cosine/sine crossfade (Mr-Sample
                         * style) — sums to constant power across the
                         * blend window, unlike linear which has a -3 dB
                         * dip at xf=0.5.  Audibly smoother on sustained
                         * harmonic content. */
                        float x = (float)(into_xf / (double)xfade_frames);
                        if (x > 1.0f) x = 1.0f;
                        float g_main = cosf(x * 0.5f * (float)M_PI);
                        float g_wrap = sinf(x * 0.5f * (float)M_PI);
                        s = g_main * s + g_wrap * ss;
                    }
                }
            }

            /* envelope (with shape curve) */
            float e_raw = env_step(v);
            float e_shaped = env_shape(e_raw, m->p_env_ac, m->p_env_rc, v->stage);

            /* velocity scaling per Mellotron-authentic depth */
            float vel_amp = 1.0f - m->p_env_vel + m->p_env_vel * v->velocity;

            /* -10 dB voice gain for serious headroom, plus the bank's
             * auto-level-match gain (set at load time so different sample
             * packs play at consistent loudness and the user can drive
             * the preamp/tape stages without immediate clipping). */
            out_mono[i] += s * e_shaped * vel_amp * 0.3f * slot->bank.bank_gain;

            v->read_pos += (double)rate;

            /* sustain loop wrap (after advance). Add `xfade_frames` so the
             * next sample after wrap is loop_start + xfade_len — that's where
             * the crossfade ended last cycle (audio was already blended toward
             * that region). Without this offset, audio jumps back by
             * xfade_len samples at wrap → audible loop click.
             * Uses the same `xfade_frames` value computed above so wrap
             * stays consistent with the crossfade window. */
            if (sustain_loop && v->read_pos >= (double)w->loop_end) {
                v->read_pos -= (double)(w->loop_end - w->loop_start);
                v->read_pos += (double)xfade_frames;
            }

            /* tape run-out: only when NOT looping. Gentle release at wall. */
            if (!v->reverse && !sustain_loop && tape_len_samples > 0 &&
                v->read_pos >= (double)tape_len_samples) {
                env_release(v, m);
            }
        }
    }
}

/* ============================================================================
 * Biquad helpers (RBJ cookbook) — used by tape head-bump and DJ filter
 * ============================================================================ */
static inline float biquad_proc(biquad_t *f, float in) {
    float out = f->b0 * in + f->b1 * f->x1 + f->b2 * f->x2
              - f->a1 * f->y1 - f->a2 * f->y2;
    f->x2 = f->x1; f->x1 = in;
    f->y2 = f->y1; f->y1 = out;
    return out;
}
static void biquad_set_lpf(biquad_t *f, float freq, float q) {
    float w0 = TWO_PI * freq / SR;
    float alpha = sinf(w0) / (2.0f * q);
    float cosw = cosf(w0);
    float a0 = 1.0f + alpha;
    float inv = 1.0f / a0;
    f->b0 = (1.0f - cosw) * 0.5f * inv;
    f->b1 = (1.0f - cosw) * inv;
    f->b2 = f->b0;
    f->a1 = -2.0f * cosw * inv;
    f->a2 = (1.0f - alpha) * inv;
}
static void biquad_set_hpf(biquad_t *f, float freq, float q) {
    float w0 = TWO_PI * freq / SR;
    float alpha = sinf(w0) / (2.0f * q);
    float cosw = cosf(w0);
    float a0 = 1.0f + alpha;
    float inv = 1.0f / a0;
    f->b0 = (1.0f + cosw) * 0.5f * inv;
    f->b1 = -(1.0f + cosw) * inv;
    f->b2 = f->b0;
    f->a1 = -2.0f * cosw * inv;
    f->a2 = (1.0f - alpha) * inv;
}
/* RBJ peaking EQ. gain_db: + boost / - cut. */
static void biquad_set_peaking(biquad_t *f, float freq, float q, float gain_db) {
    float A = powf(10.0f, gain_db / 40.0f);
    float w0 = TWO_PI * freq / SR;
    float alpha = sinf(w0) / (2.0f * q);
    float cosw = cosf(w0);
    float a0 = 1.0f + alpha / A;
    float inv = 1.0f / a0;
    f->b0 = (1.0f + alpha * A) * inv;
    f->b1 = -2.0f * cosw * inv;
    f->b2 = (1.0f - alpha * A) * inv;
    f->a1 = -2.0f * cosw * inv;
    f->a2 = (1.0f - alpha / A) * inv;
}
/* RBJ bandpass (constant skirt gain / peak gain = Q).
 * Used by the Auto-Wah envelope filter. */
static void biquad_set_bpf(biquad_t *f, float freq, float q) {
    float w0 = TWO_PI * freq / SR;
    float alpha = sinf(w0) / (2.0f * q);
    float cosw = cosf(w0);
    float a0 = 1.0f + alpha;
    float inv = 1.0f / a0;
    f->b0 = alpha * inv;
    f->b1 = 0.0f;
    f->b2 = -alpha * inv;
    f->a1 = -2.0f * cosw * inv;
    f->a2 = (1.0f - alpha) * inv;
}

/* ============================================================================
 * Tape saturation shapes (KrautDrums-derived)
 * ============================================================================ */
/* Cubic soft-clip: y = y - y³/6.75, hard-clipped beyond ±1.5.
 * Smoother than tanh, 3rd-harmonic emphasis. */
static inline float tape_cubic(float x, float drive) {
    float y = x * drive;
    if (y >  1.5f) return  1.0f;
    if (y < -1.5f) return -1.0f;
    return y - y * y * y * (1.0f / 6.75f);
}
/* Asymmetric tanh: DC bias in, removed out → 2nd-harmonic warmth.
 * asym≈0.15 sounds tube-like, asym≈0.25 is hotter/dirtier. */
static inline float tape_asym(float x, float drive, float asym) {
    return fast_tanh(x * drive + asym) - fast_tanh(asym);
}

/* ============================================================================
 * Tape stage (post-sampler, pre-preamp)
 *   - tape_style voices the weights AND the saturation curve
 *   - peaking biquad head-bump @ 120 Hz (per-style depth, +sat boost)
 *   - degrade injects hiss + dropouts + grit (gated by signal level)
 *   - degrade also slightly attenuates output (Super Boum 'age' ducking)
 * ============================================================================ */
static inline void tape_style_weights(int style, float *out_warble_w, float *out_flutter_w,
                                      float *out_degrade_w, float *out_sat_w,
                                      float *out_hf_rolloff) {
    /* HF rolloff spread expanded so each style sounds distinctly different at
     * default knob positions (the previous spread of 0..0.3 was too subtle). */
    *out_warble_w = 1.0f; *out_flutter_w = 1.0f;
    *out_degrade_w = 1.0f; *out_sat_w = 1.0f; *out_hf_rolloff = 0.0f;
    switch (style) {
        case 0: /* Off — bypass tape signal-shaping. Wow/flutter (read-ptr) and
                   degrade (hiss/dropouts/grit) still operate, but no head bump,
                   no saturation curve, no HF rolloff. */
            *out_warble_w = 1.0f; *out_flutter_w = 1.0f;
            *out_degrade_w = 1.0f; *out_sat_w = 0.0f; *out_hf_rolloff = 0.0f; break;
        case 1: /* Clean — pristine, no HF cut, no bump */
            *out_warble_w = 0.4f; *out_flutter_w = 0.4f;
            *out_degrade_w = 0.4f; *out_sat_w = 0.6f; *out_hf_rolloff = 0.0f; break;
        case 2: /* M400 — workhorse, mild warmth */
            *out_warble_w = 1.0f; *out_flutter_w = 1.0f;
            *out_degrade_w = 1.0f; *out_sat_w = 1.0f; *out_hf_rolloff = 0.15f; break;
        case 3: /* MkII — tube-y, warmer, more HF cut */
            *out_warble_w = 1.1f; *out_flutter_w = 0.9f;
            *out_degrade_w = 1.0f; *out_sat_w = 1.3f; *out_hf_rolloff = 0.35f; break;
        case 4: /* M300 — brighter than M400 (less HF cut), more flutter */
            *out_warble_w = 0.9f; *out_flutter_w = 1.3f;
            *out_degrade_w = 1.1f; *out_sat_w = 0.9f; *out_hf_rolloff = 0.08f; break;
        case 5: /* Worn — heavily aged, deep HF cut, lots of degrade */
            *out_warble_w = 1.3f; *out_flutter_w = 1.4f;
            *out_degrade_w = 1.7f; *out_sat_w = 1.2f; *out_hf_rolloff = 0.55f; break;
    }
}

static void apply_tape_stage(mello_t *m, float *l, float *r, int frames) {
    float w_warble, w_flutter, w_degrade, w_sat, hf_roll;
    tape_style_weights(m->p_tape_style, &w_warble, &w_flutter, &w_degrade, &w_sat, &hf_roll);

    float deg = clampf(m->sm_degrade * w_degrade, 0.0f, 1.5f);
    float sat = clampf(m->sm_tape_sat * w_sat, 0.0f, 1.5f);
    float input = m->sm_input * 2.0f;

    /* bias: 0.5 = neutral. Low bias → harsher + earlier sat. High bias →
     * darker + cleaner. */
    float bias_off = m->sm_bias - 0.5f;
    sat += bias_off * -0.5f;
    hf_roll += bias_off * 0.4f;
    if (sat < 0.0f) sat = 0.0f;
    if (sat > 1.5f) sat = 1.5f;                     /* upper bound restored after bias add */

    /* drive scaling: ×6 boost. ×12 pushed everything into hard clip at modest
     * input + max sat; ×6 keeps the curve in its sweet spot at usable levels. */
    float drive = 1.0f + sat * sat * 6.0f;

    /* Per-style head-bump (peaking biquad @ 120 Hz, broad Q=0.7). For style 0
     * (Off) the bump is forcibly 0 regardless of sat knob — keeps tape signal
     * shaping fully bypassed. */
    static const float BUMP_BASE_DB[6] = { 0.0f, 0.0f, 3.0f, 4.5f, 2.0f, 5.5f };
    int sty = m->p_tape_style; if (sty < 0) sty = 0; if (sty > 5) sty = 5;
    float bump_db = (sty == 0) ? 0.0f : (BUMP_BASE_DB[sty] + sat * 2.5f);
    int do_bump = bump_db > 0.05f;
    if (do_bump) {
        biquad_set_peaking(&m->fx.head_bump_l, 120.0f, 0.7f, bump_db);
        biquad_set_peaking(&m->fx.head_bump_r, 120.0f, 0.7f, bump_db);
    }

    /* HF rolloff state lives on the instance (was function-static, which
     * caused two Mellos on different tracks to share filter memory).  The
     * denormal guards on the one-pole below prevent ARM-Cortex stalls when
     * the input idles at zero. */
    float roll_c = hf_roll > 0.0f ? hf_roll : 0.0f;
    float age_atten = 1.0f - m->sm_degrade * 0.25f;

    for (int i = 0; i < frames; i++) {
        float sl = l[i] * input;
        float sr = r[i] * input;

        if (do_bump) {
            sl = biquad_proc(&m->fx.head_bump_l, sl);
            sr = biquad_proc(&m->fx.head_bump_r, sr);
        }

        float lvl = fabsf(sl) + fabsf(sr);
        float gate = (lvl > 0.001f) ? (1.0f - expf(-lvl * 12.0f)) : 0.0f;
        if (deg > 0.0001f) {
            float hiss = randf_pm(&m->fx.rng) * (0.003f * deg) * gate;
            sl += hiss; sr += hiss * 0.85f;
        }
        if (deg > 0.001f) {
            uint32_t r1 = xorshift32(&m->fx.rng);
            float drop_prob = deg * 0.00012f;
            if (((float)(r1 >> 8) * (1.0f / 16777216.0f)) < drop_prob) {
                float depth = 0.4f + randf01(&m->fx.rng) * 0.5f;
                sl *= (1.0f - depth); sr *= (1.0f - depth);
            }
        }
        if (deg > 0.5f) {
            uint32_t r2 = xorshift32(&m->fx.rng);
            float cr_prob = (deg - 0.5f) * 0.00018f;
            if (((float)(r2 >> 8) * (1.0f / 16777216.0f)) < cr_prob) {
                float k = randf_pm(&m->fx.rng) * 0.15f;
                sl += k; sr += k;
            }
        }

        /* per-style saturation curve — each style has its own harmonic identity */
        switch (sty) {
            case 0:  /* Off — true bypass, no shaping. The hiss/dropouts/grit
                        above were already added; nothing else to do here. */
                break;
            case 1: {  /* Clean — bypass at sat=0, gentle tanh above. */
                if (sat > 0.001f) {
                    float sat_comp = 0.7f / (fast_tanh(0.7f * drive) + 0.001f);
                    sl = fast_tanh(sl * drive) * sat_comp;
                    sr = fast_tanh(sr * drive) * sat_comp;
                }
                break;
            }
            case 2:  /* M400 — cubic soft-clip (3rd-harmonic) */
                sl = tape_cubic(sl, drive); sr = tape_cubic(sr, drive); break;
            case 3:  /* MkII Tube — asym tanh (2nd-harmonic warmth) */
                sl = tape_asym(sl, drive, 0.15f) * 1.15f;
                sr = tape_asym(sr, drive, 0.15f) * 1.15f; break;
            case 4: {  /* M300 — classic tanh */
                float sat_comp = 0.6f / (fast_tanh(0.6f * drive) + 0.001f);
                sl = fast_tanh(sl * drive) * sat_comp;
                sr = fast_tanh(sr * drive) * sat_comp; break;
            }
            case 5: {  /* Worn — asym + cubic blend, hot */
                float al = tape_asym(sl, drive * 1.10f, 0.22f);
                float ar = tape_asym(sr, drive * 1.10f, 0.22f);
                float cl = tape_cubic(sl, drive * 1.10f);
                float cr = tape_cubic(sr, drive * 1.10f);
                sl = (al + cl) * 0.5f; sr = (ar + cr) * 0.5f; break;
            }
            default: break;
        }

        sl *= age_atten; sr *= age_atten;

        if (roll_c > 0.0001f) {
            m->fx.tape_hf_l += roll_c * 0.4f * (sl - m->fx.tape_hf_l) + 1e-25f;
            m->fx.tape_hf_r += roll_c * 0.4f * (sr - m->fx.tape_hf_r) + 1e-25f;
            sl = lerpf(sl, m->fx.tape_hf_l, roll_c);
            sr = lerpf(sr, m->fx.tape_hf_r, roll_c);
        }

        l[i] = sl; r[i] = sr;
    }

    (void)w_warble; (void)w_flutter;
}

/* ============================================================================
 * Preamp models (driven by Main `crunch`)
 *   - Clean DI       : linear (gain only)
 *   - MkII Tube      : tanh + 2nd-order asymmetry, sag at higher drive
 *   - M400 SS        : harder knee, more odd harmonics
 *   - SK-1 Lo-fi     : bit-decimation + aliasing fold
 * ============================================================================ */
/* Preamp models — each has a distinct TIMBRE (light EQ/voicing) at drive=0,
 * so just switching the model sounds different. Crunch (drive) ramps each
 * model cleanly from clean → saturated using MONOTONIC soft-clips only.
 *
 * Model 0 ("Off") is true bypass — no math, no gain, no character. Used as
 * the default so Mello starts at the cleanest possible setting. */
static inline float preamp_sample(int model, float x, float drive) {
    float y;
    switch (model) {
        case 0: { /* Off — true bypass */
            y = x;
            break;
        }
        case 1: { /* Clean DI — transparent linear gain, no character */
            y = x * (1.0f + drive * 0.6f);
            break;
        }
        case 2: { /* MkII Tube — slight 2nd-harm bias, gentle tanh */
            float k    = 1.0f + drive * 2.0f;
            float bias = 0.04f + drive * 0.06f;     /* subtle baseline asymmetry */
            float a    = fast_tanh((x + bias) * k);
            y = (a - fast_tanh(bias)) * (0.90f / (1.0f + drive * 0.15f));
            break;
        }
        case 3: { /* M400 Solid-State — softclip with TRUE ±1 asymptote */
            float k    = 1.0f + drive * 2.5f;
            float xk   = x * k;
            float clipped = xk / (1.0f + fabsf(xk));            /* → ±1 */
            y = clipped * (1.20f / (1.0f + drive * 0.15f));
            break;
        }
        case 4: { /* SK-1 Lo-fi — always 10-bit, drops to 5-bit at max drive */
            float k    = 1.0f + drive * 1.8f;
            float bits = 10.0f - drive * 5.0f;      /* 10 → 5 bits */
            if (bits < 5.0f) bits = 5.0f;
            float levels = powf(2.0f, bits) * 0.5f;
            float xq = floorf(x * k * levels + 0.5f) / levels;
            xq = xq / (1.0f + fabsf(xq) * 0.3f);
            y = xq * 0.75f;
            break;
        }
        default:
            y = x;
    }
    return y;
}

/* ============================================================================
 * Tone tilt EQ (pivot ~800 Hz; 0.5 = flat, <0.5 darken, >0.5 brighten)
 * Implementation: parallel low-pass + high-pass cross-fade, bipolar around 0.5
 * ============================================================================ */
static inline float tone_tilt_l(mello_t *m, float x) {
    float t = (m->sm_tone - 0.5f) * 2.0f;          /* -1..+1 */
    float c = 0.10f;                                 /* ~800 Hz one-pole */
    m->fx.tone_lo_l += c * (x - m->fx.tone_lo_l) + 1e-25f;
    float hi = x - m->fx.tone_lo_l;
    float lo = m->fx.tone_lo_l;
    /* tilt: t<0 emphasises lo, t>0 emphasises hi */
    return lo * (1.0f - t) + hi * (1.0f + t);
}
static inline float tone_tilt_r(mello_t *m, float x) {
    float t = (m->sm_tone - 0.5f) * 2.0f;
    float c = 0.10f;
    m->fx.tone_lo_r += c * (x - m->fx.tone_lo_r) + 1e-25f;
    float hi = x - m->fx.tone_lo_r;
    float lo = m->fx.tone_lo_r;
    return lo * (1.0f - t) + hi * (1.0f + t);
}

/* ============================================================================
 * FX RACK — 8 macros in fixed order. Each function takes the smoothed param
 * (0..1), 0 = bypass with no work, 1 = max intensity, equal-power dry/wet.
 * ============================================================================ */

/* ---- 1. Vibrato/Chorus ---- */
static void fx_vibrato(mello_t *m, float *l, float *r, int frames) {
    /* ALWAYS-RUN: state updates continuously so the delay buffer stays current
     * with the dry signal. When user turns the knob on, no stale audio
     * surfaces (= no click). Wet output is gated by amt: 0 = dry.
     *
     * MIDI mod wheel adds to the knob — classic Mellotron behaviour where
     * the wheel modulates vibrato.  Clamped at 1.0 so a full-knob +
     * full-wheel combination doesn't break the depth math. */
    float amt = m->sm_vibrato + m->sm_mod_wheel * 0.85f;
    if (amt > 1.0f) amt = 1.0f;

    float depth_samples = (3.0f + amt * 22.0f);
    float rate = 5.0f - amt * 4.5f;                      /* 5 → 0.5 Hz */
    float wet  = amt * 0.85f;                             /* 0 at knob=0, 0.85 at knob=1 */
    float chorus = amt;

    /* Quadrature LFOs — replaces 3 sinf/sample with 3 muls/sample.  Seed
     * (c, s) from the canonical phase counters so block-rate changes still
     * land sample-accurately. */
    float ca = cosf(m->vib.phase_a * TWO_PI), sa = sinf(m->vib.phase_a * TWO_PI);
    float cb = cosf(m->vib.phase_b * TWO_PI), sb = sinf(m->vib.phase_b * TWO_PI);
    float cc = cosf(m->vib.phase_c * TWO_PI), sc = sinf(m->vib.phase_c * TWO_PI);
    float dth_a = rate          * SR_INV * TWO_PI;
    float dth_b = rate * 0.83f  * SR_INV * TWO_PI;
    float dth_c = rate * 1.31f  * SR_INV * TWO_PI;
    float dca = cosf(dth_a), dsa = sinf(dth_a);
    float dcb = cosf(dth_b), dsb = sinf(dth_b);
    float dcc = cosf(dth_c), dsc = sinf(dth_c);

    for (int i = 0; i < frames; i++) {
        m->vib.phase_a += rate * SR_INV;
        m->vib.phase_b += (rate * 0.83f) * SR_INV;
        m->vib.phase_c += (rate * 1.31f) * SR_INV;
        if (m->vib.phase_a > 1.0f) m->vib.phase_a -= 1.0f;
        if (m->vib.phase_b > 1.0f) m->vib.phase_b -= 1.0f;
        if (m->vib.phase_c > 1.0f) m->vib.phase_c -= 1.0f;

        m->vib.buf_l[m->vib.wp] = l[i];
        m->vib.buf_r[m->vib.wp] = r[i];

        float mod_a = sa, mod_b = sb, mod_c = sc;
        /* Advance quadrature oscillators by one sample. */
        float nca = ca * dca - sa * dsa, nsa = sa * dca + ca * dsa;
        float ncb = cb * dcb - sb * dsb, nsb = sb * dcb + cb * dsb;
        float ncc = cc * dcc - sc * dsc, nsc = sc * dcc + cc * dsc;
        ca = nca; sa = nsa; cb = ncb; sb = nsb; cc = ncc; sc = nsc;

        float dly_a = depth_samples + mod_a * depth_samples * 0.9f;
        float dly_b = depth_samples + mod_b * depth_samples * 0.9f;
        float dly_c = depth_samples + mod_c * depth_samples * 0.9f;

        #define VIB_TAP(buf, dly) ({ \
            float rp_f = (float)m->vib.wp - (dly); \
            while (rp_f < 0) rp_f += (float)VIB_DLY_MAX; \
            int rp_i = (int)rp_f; \
            float fr = rp_f - (float)rp_i; \
            int rp1 = (rp_i + 1) % VIB_DLY_MAX; \
            buf[rp_i] + ((buf)[rp1] - (buf)[rp_i]) * fr; })

        float wa_l = VIB_TAP(m->vib.buf_l, dly_a);
        float wa_r = VIB_TAP(m->vib.buf_r, dly_a);
        float wb_l = VIB_TAP(m->vib.buf_l, dly_b);
        float wc_r = VIB_TAP(m->vib.buf_r, dly_c);
        #undef VIB_TAP

        float w_l = wa_l + chorus * 0.6f * wb_l;
        float w_r = wa_r + chorus * 0.6f * wc_r;
        float scale = 1.0f / (1.0f + chorus * 0.6f);
        w_l *= scale; w_r *= scale;

        l[i] = l[i] * (1.0f - wet) + w_l * wet;
        r[i] = r[i] * (1.0f - wet) + w_r * wet;

        m->vib.wp = (m->vib.wp + 1) % VIB_DLY_MAX;
    }
}

/* ---- 2. Rotary (was "Leslie" in v0.1.x) — physical-model port ----
 *
 * v0.1.22 rewrite based on LeslieSim's Faust DSP
 * (github.com/miccio-dk/LeslieSim) cross-referenced with Surge XT's
 * RotarySpeakerEffect.  The earlier version used a tiny doppler delay
 * range (~7 samples) which was why the effect never sounded like an
 * actual rotating speaker — bumping the LFO to 33 Hz only made it
 * scream, not spin.
 *
 * Physical model:
 *   - Horn modelled as a point source rotating on a 19 cm arm, listener
 *     ears at ±22.5° around the rotation axis (45° mic spread).
 *   - Per-sample distance from rotating point to each ear:
 *         x = r·(1 + cosθ),  y = r·(1 + sinθ),  d = √(x² + y²)
 *     This ranges from ~0.41·r (≈ 8 cm) to ~2.41·r (≈ 46 cm).
 *   - Delay length per ear = d_cm × SR / 34300  → 11..64 samples at
 *     48 kHz, 19 cm radius.  The varying delay length IS the doppler —
 *     no separate pitch shifter needed.
 *   - AM via inverse-distance: gain ≈ 1 + 0.35·(√2 − d/r), so when the
 *     horn faces an ear, that ear gets +35 % gain; when it faces away,
 *     −35 %.  Surge XT's dot-product AM does the same thing under a
 *     different parameterisation.
 *   - 800 Hz one-pole crossover splits horn (HF, fast rotation, big AM
 *     swing) from drum (LF, slower rotation, gentler AM).
 *   - Rate curve modelled on a real Leslie: chorale 30 RPM (0.5 Hz),
 *     tremolo 420 RPM (7 Hz).  amt² so slow speeds dominate the lower
 *     half of the knob; this is the "feels right" curve.
 *
 * Per sample: 2 sinf + 2 cosf + 4 sqrtf + 2 linear-interp tap reads.
 * Roughly 60 cycles/sample → ~140 µs per block at 48 kHz.  Affordable.
 */

/* Read a doppler-modulated delay tap from `buf` at `wp`, where the
 * effective delay length is determined by the geometric distance from
 * the rotating source (at angle (c, s) on radius `r_cm`) to the listener
 * at the origin.  AM is folded in via inverse-distance gain. */
static inline float leslie_tap(const float *buf, int wp,
                               float r_cm, float c, float s,
                               float am_depth) {
    /* Source position relative to listener: (r·(1+c), r·(1+s)).
     * Distance ranges roughly r·0.41 .. r·2.41 as θ varies. */
    float xd = r_cm * (1.0f + c);
    float yd = r_cm * (1.0f + s);
    float dist_cm = sqrtf(xd * xd + yd * yd);

    /* Delay length: distance / speed of sound (343 m/s = 34300 cm/s)
     * × sample rate.  Pre-folded constant SR/34300 = 1.399 samp/cm @48k. */
    float delay_f = dist_cm * (SR / 34300.0f);
    if (delay_f < 1.0f)                    delay_f = 1.0f;
    if (delay_f > LESLIE_DLY_MAX - 2.0f)   delay_f = LESLIE_DLY_MAX - 2.0f;

    int   di = (int)delay_f;
    float df = delay_f - (float)di;
    int   r0 = (wp - di     + LESLIE_DLY_MAX) % LESLIE_DLY_MAX;
    int   r1 = (wp - di - 1 + LESLIE_DLY_MAX) % LESLIE_DLY_MAX;
    float v  = buf[r0] + df * (buf[r1] - buf[r0]);

    /* Inverse-distance AM: near-side = louder, far-side = softer.
     * (√2 − d/r) is roughly ±1 as the horn rotates, so gain swings
     * 1 ± am_depth. */
    float am = 1.0f + am_depth * (1.4142136f - dist_cm * (1.0f / 1.0f) / r_cm);
    return v * am;
}

static void fx_leslie(mello_t *m, float *l, float *r, int frames) {
    /* ALWAYS-RUN: delay lines stay current with the dry signal. */
    float amt = m->sm_leslie;

    /* Rate curve — v0.1.30: reverted to the v0.1.26 two-zone curve.  The
     * v0.1.28 extension to 176 Hz produced sounds the user didn't want at
     * any usable knob setting — "now max is really too fast".  Mapping:
     *   0–25 %  : realistic Leslie sweep (0.5 → 11 Hz horn / 0.4 → 9 Hz drum)
     *   25–100 %: above-realistic acceleration up to 44 Hz / 36 Hz.
     * Heterodyne territory was relegated outside the knob range. */
    float horn_target, drum_target;
    if (amt <= 0.25f) {
        float t  = amt * 4.0f;                       /* 0..1 over 0-25 %   */
        float t2 = t * t;
        horn_target = 0.5f + t2 * 10.5f;             /* 0.5 → 11.0 Hz      */
        drum_target = 0.4f + t2 * 8.6f;              /* 0.4 → 9.0 Hz       */
    } else {
        float t  = (amt - 0.25f) * (1.0f / 0.75f);   /* 0..1 over 25-100 % */
        float t2 = t * t;
        horn_target = 11.0f + t2 * 33.0f;            /* 11 → 44 Hz         */
        drum_target =  9.0f + t2 * 27.0f;            /*  9 → 36 Hz         */
    }
    /* Asymmetric speed ramping: 30 ms going DOWN (snappy "kill the spin"
     * for musical control), 60 ms going UP (cabinet spool-up feel).  User
     * specifically asked for faster ramp-down on top of the existing
     * fast spool-up. */
    const float RAMP_UP   = 1.0f - expf(-SR_INV / 0.060f);
    const float RAMP_DOWN = 1.0f - expf(-SR_INV / 0.030f);
    float diff_h = horn_target - m->lez.horn_rate;
    float diff_d = drum_target - m->lez.drum_rate;
    m->lez.horn_rate += (diff_h >= 0.0f ? RAMP_UP : RAMP_DOWN) * diff_h;
    m->lez.drum_rate += (diff_d >= 0.0f ? RAMP_UP : RAMP_DOWN) * diff_d;

    /* 800 Hz one-pole LP coefficient (cached — same on every sample).
     * c = 1 - exp(-2π·800/SR) ≈ 0.0944 at 48 kHz. */
    const float xover_c = 0.0944f;

    /* Listener angles relative to horn axis: ±22.5° → 45° mic spread.
     * Pre-computed rotation matrix constants. */
    const float ROT_CL = 0.92387953f;          /* cos(22.5°) */
    const float ROT_SL = 0.38268343f;          /* sin(22.5°) */

    /* Per-rotor AM depths (horn swings hard, drum is gentler).
     * v0.1.25: AM cranked from 0.40/0.20 → 0.60/0.35 so the tremolo
     * "swoosh" is actually audible — at the previous values the effect
     * sounded chorus-y rather than rotary.  Real Leslie horn directivity
     * produces ~6 dB level swings per rotation; 0.60 maps to ~±8 dB which
     * reads as proper rotary motion. */
    const float HORN_AM = 0.60f;
    const float DRUM_AM = 0.35f;
    /* Horn radius bumped 19 → 23 cm — deeper doppler delay (12..78 samples
     * vs 11..64) gives more obvious pitch wobble.  Real cabinets vary
     * 18–24 cm so this stays inside the realistic envelope. */
    const float HORN_RADIUS = 23.0f;           /* cm */
    const float DRUM_RADIUS = 16.0f;           /* cm */

    /* Wet level: ramps fast to 0.80 over the first 10 % of the knob,
     * then plateaus.  Past 10 %, knob movement just changes the rate /
     * complexity of the rotary, not the wet amount.  Matches the user's
     * "fully wet early on so the effect is committed, then knob just
     * controls speed". */
    float wet;
    if (amt < 0.10f) wet = amt * 8.0f;          /* 0 → 0.80 over 0–10 % */
    else             wet = 0.80f;

    /* Quadrature-oscillator seeding (block-level): re-seed (c, s) from the
     * canonical phase and pre-compute the per-sample rotation delta.  This
     * replaces 4 sinf/cosf calls per sample (one pair for horn, one for
     * drum) with 4 per *block* — a ~120× reduction in trig work.  The
     * phase counter still advances per sample so a rate change between
     * blocks is captured cleanly. */
    float horn_c = cosf(m->lez.horn_phase * TWO_PI);
    float horn_s = sinf(m->lez.horn_phase * TWO_PI);
    float drum_c = cosf(m->lez.drum_phase * TWO_PI);
    float drum_s = sinf(m->lez.drum_phase * TWO_PI);
    float dth_h  = m->lez.horn_rate * SR_INV * TWO_PI;
    float dth_d  = m->lez.drum_rate * SR_INV * TWO_PI;
    float dc_h   = cosf(dth_h), ds_h = sinf(dth_h);
    float dc_d   = cosf(dth_d), ds_d = sinf(dth_d);

    for (int i = 0; i < frames; i++) {
        m->lez.horn_phase += m->lez.horn_rate * SR_INV;
        m->lez.drum_phase += m->lez.drum_rate * SR_INV;
        if (m->lez.horn_phase >= 1.0f) m->lez.horn_phase -= 1.0f;
        if (m->lez.drum_phase >= 1.0f) m->lez.drum_phase -= 1.0f;

        /* Mono pre-rotation input. */
        float mono = 0.5f * (l[i] + r[i]);

        /* 800 Hz crossover: hi = mono - lp, lo = lp. */
        m->lez.xover_lp += xover_c * (mono - m->lez.xover_lp) + 1e-25f;
        float lo = m->lez.xover_lp;
        float hi = mono - lo;

        /* Feed delay lines (mono signal pre-rotation; the rotation lives
         * in the per-listener tap-length variation below). */
        m->lez.horn_buf[m->lez.wp] = hi;
        m->lez.drum_buf[m->lez.wp] = lo;

        /* HORN — derive listener-relative angles by rotating (horn_c, horn_s)
         * by ±22.5°.  Then advance (horn_c, horn_s) by one sample's worth
         * of rotation for the next iteration. */
        float c_hL = horn_c * ROT_CL + horn_s * ROT_SL;
        float s_hL = horn_s * ROT_CL - horn_c * ROT_SL;
        float c_hR = horn_c * ROT_CL - horn_s * ROT_SL;
        float s_hR = horn_s * ROT_CL + horn_c * ROT_SL;

        float horn_L = leslie_tap(m->lez.horn_buf, m->lez.wp,
                                  HORN_RADIUS, c_hL, s_hL, HORN_AM);
        float horn_R = leslie_tap(m->lez.horn_buf, m->lez.wp,
                                  HORN_RADIUS, c_hR, s_hR, HORN_AM);

        /* Advance horn oscillator by dth_h. */
        float nh_c = horn_c * dc_h - horn_s * ds_h;
        float nh_s = horn_s * dc_h + horn_c * ds_h;
        horn_c = nh_c; horn_s = nh_s;

        /* DRUM — same pattern, smaller radius + AM, slower rotation. */
        float c_dL = drum_c * ROT_CL + drum_s * ROT_SL;
        float s_dL = drum_s * ROT_CL - drum_c * ROT_SL;
        float c_dR = drum_c * ROT_CL - drum_s * ROT_SL;
        float s_dR = drum_s * ROT_CL + drum_c * ROT_SL;

        float drum_L = leslie_tap(m->lez.drum_buf, m->lez.wp,
                                  DRUM_RADIUS, c_dL, s_dL, DRUM_AM);
        float drum_R = leslie_tap(m->lez.drum_buf, m->lez.wp,
                                  DRUM_RADIUS, c_dR, s_dR, DRUM_AM);

        float nd_c = drum_c * dc_d - drum_s * ds_d;
        float nd_s = drum_s * dc_d + drum_c * ds_d;
        drum_c = nd_c; drum_s = nd_s;

        /* Combine + makeup gain (the AM averages slightly below unity). */
        float wL = (horn_L + drum_L) * 1.15f;
        float wR = (horn_R + drum_R) * 1.15f;
        wL = fast_tanh(wL);
        wR = fast_tanh(wR);

        l[i] = l[i] * (1.0f - wet) + wL * wet;
        r[i] = r[i] * (1.0f - wet) + wR * wet;

        m->lez.wp = (m->lez.wp + 1) % LESLIE_DLY_MAX;
    }
}

/* ---- 3. DJ Filter (exact port from Essaim — pop-free, fully wet outside
 *      the ±0.05 crossfade zone around center). HP range bumped slightly
 *      so the high-pass sweep is more obvious in DJ-style use. */
static void fx_dj_filter(mello_t *m, float *l, float *r, int frames) {
    /* Per-sample-smoothed cutoff target.  The previous version only
     * updated biquad coefs ONCE per render block, so as the user swept the
     * knob, each block boundary nudged the cutoff by a small step.  The
     * biquad's stored state from the old coefs no longer matched the new
     * coefs, producing a tiny impulse every block — at 48 kHz / 128 frames
     * that's ~375 impulses/sec which the ear hears as a low tone (same
     * mechanism that produced the previous reverb-knob tone).  Smoothing
     * per-sample and updating coefs every 16 samples turns those block-
     * boundary clicks into 3000+ vanishingly-small per-block updates that
     * the ear smooths into a continuous sweep. */
    float target = m->sm_dj_filter;

    for (int i = 0; i < frames; i++) {
        /* one-pole per-sample smoother — ~5 ms time constant.  Much faster
         * than the block-rate smoothing of sm_dj_filter so the filter
         * response keeps up with knob movement, but slow enough that any
         * single-sample coef jump is tiny. */
        m->fx.dj_djf_smooth += 0.005f * (target - m->fx.dj_djf_smooth) + 1e-25f;
        float djf = m->fx.dj_djf_smooth;

        int mode = (djf < 0.5f) ? 0 : 1;
        if (mode != m->fx.prev_dj_mode) {
            for (int s = 0; s < 3; s++) {
                memset(&m->fx.dj_bq_l[s], 0, sizeof(biquad_t));
                memset(&m->fx.dj_bq_r[s], 0, sizeof(biquad_t));
            }
            m->fx.prev_dj_mode = mode;
        }

        /* Crossfade dry/wet near center (±0.05 zone) — Essaim's pop-free
         * recipe.  We compute it per-sample now because djf is smoothed
         * per-sample. */
        float dj_wet = 1.0f;
        if (djf >= 0.45f && djf <= 0.55f) {
            dj_wet = fabsf(djf - 0.5f) / 0.05f;
            if (dj_wet > 1.0f) dj_wet = 1.0f;
        }

        /* Refresh biquad coefs every 16 samples (3 kHz update rate at 48 kHz).
         * Continuous sweep, transparent at center.
         * LP 200 Hz..18 kHz on the dark side.
         * HP 80 Hz..4 kHz on the bright side. */
        if ((i & 15) == 0) {
            if (djf < 0.5f) {
                float t = (0.5f - djf) / 0.5f;
                float lp_f = 18000.0f * powf(200.0f / 18000.0f, t);
                for (int s = 0; s < 3; s++) {
                    biquad_set_lpf(&m->fx.dj_bq_l[s], lp_f, 0.707f);
                    biquad_set_lpf(&m->fx.dj_bq_r[s], lp_f, 0.707f);
                }
            } else {
                float t = (djf - 0.5f) / 0.5f;
                float hp_f = 80.0f * powf(50.0f, t);        /* 80 → 4000 Hz */
                for (int s = 0; s < 3; s++) {
                    biquad_set_hpf(&m->fx.dj_bq_l[s], hp_f, 0.707f);
                    biquad_set_hpf(&m->fx.dj_bq_r[s], hp_f, 0.707f);
                }
            }
        }

        float dry_l = l[i], dry_r = r[i];
        float wl = l[i], wr = r[i];
        for (int s = 0; s < 3; s++) {
            wl = biquad_proc(&m->fx.dj_bq_l[s], wl);
            wr = biquad_proc(&m->fx.dj_bq_r[s], wr);
        }
        l[i] = dry_l + (wl - dry_l) * dj_wet;
        r[i] = dry_r + (wr - dry_r) * dj_wet;
    }
}

/* ---- 4. Tremolo (vintage tube-style amp LFO) ----
 * Previous version had a buggy 5th branch in the soft-square (a "rising
 * ramp" at ph∈[0.95,1.0] that ended at +1, but ph=0 starts at -1 → ~1
 * sample discontinuity = click at the loop rate). Replaced with a clean
 * trapezoid that closes at -1 going into the wrap. Also capped morph at
 * 0.7 so we never get fully square — vintage tube tremolos are biased
 * sine-shaped, not chopped square waves. */
static void fx_tremolo(mello_t *m, float *l, float *r, int frames) {
    float amt = m->sm_tremolo;
    if (amt < 0.001f) return;

    /* Rate curve in three zones so the knob covers a wide musical range:
     *   0 – 10 %  : ULTRA slow tremolo (0.5 → 2 Hz) — useful for a gentle
     *               "breathing" feel under sustained pads, where the
     *               previous 3.5 Hz floor was too fast to disappear into
     *               the background.
     *   10 – 80 % : normal vintage tremolo zone (2 → 11 Hz).
     *   80 – 100 %: quadratic boost up to 22 Hz — the "machine-gun" /
     *               near-ring-mod top end. */
    float rate;
    if (amt < 0.10f) {
        float t = amt / 0.10f;                       /* 0 → 1 over 0–10 %  */
        rate = 0.5f + t * 1.5f;                      /* 0.5 → 2.0 Hz       */
    } else if (amt < 0.80f) {
        float t = (amt - 0.10f) / 0.70f;             /* 0 → 1 over 10–80 % */
        rate = 2.0f + t * 9.0f;                      /* 2.0 → 11.0 Hz      */
    } else {
        float t = (amt - 0.80f) / 0.20f;             /* 0 → 1 over 80–100 %*/
        rate = 11.0f + t * t * 11.0f;                /* 11   → 22.0 Hz (quad) */
    }
    float depth = 0.35f + amt * 0.55f;
    float morph = amt * 0.7f;                        /* cap so it's never pure square */

    for (int i = 0; i < frames; i++) {
        m->fx.trem_phase += rate * SR_INV;
        if (m->fx.trem_phase >= 1.0f) m->fx.trem_phase -= 1.0f;
        float ph = m->fx.trem_phase;
        float sine = sinf(ph * TWO_PI);

        /* Trapezoid: rises 0→ramp, holds 1, falls ramp_mid, holds -1.
         * Closes at -1 at ph→1 so wrap to ph=0 (also -1 just before rise) is
         * continuous. No 5th branch. */
        const float ramp = 0.10f;
        float sq;
        if (ph < ramp)                  sq = -1.0f + (1.0f - cosf(M_PI * ph / ramp));
        else if (ph < 0.5f - ramp)      sq =  1.0f;
        else if (ph < 0.5f + ramp)      sq =  1.0f - (1.0f - cosf(M_PI * (ph - (0.5f - ramp)) / (2.0f * ramp)));
        else                            sq = -1.0f;

        float lfo = sine * (1.0f - morph) + sq * morph;
        float amp = 1.0f - depth * (0.5f - 0.5f * lfo);
        l[i] *= amp; r[i] *= amp;
    }
}

/* ---- 5. Auto-Wah (was Phaser in <=v0.1.11) ---- */
/* Auto-Wah — envelope-controlled resonant bandpass, Crybaby-style.
 *
 * Why Auto-Wah and not a flanger? Every other Mello FX is LFO-driven
 * (vibrato/leslie/tremolo sweep over time) or static (DJ filter). Nothing
 * reacts to *what you play*. Auto-Wah fills that gap — louder notes open
 * the filter further, soft notes leave it dark. Classic Mellotron-into-
 * envelope-filter timbre (Stevie Wonder, Roger Troutman, Air's "Sexy Boy").
 *
 * Crybaby formula: peak-detect → exp-release env follower → resonant BPF
 * with cutoff 450 Hz..4.6 kHz (log mapped on env) and Q 8..2 (highest at
 * the bottom of the sweep for that throaty 'aw' vowel). Knob `amt` is
 * dry/wet only — the env follower runs full-scale at all settings so the
 * filter response is consistent. */
static void fx_autowah(mello_t *m, float *l, float *r, int frames) {
    float amt = m->sm_autowah;
    if (amt < 0.005f) return;

    /* Release time 100 ms — slow enough to track sustained notes through
     * the decay, fast enough to retrigger on legato. */
    const float rel = expf(-1.0f / (0.1f * SR));

    /* Update biquad coeffs every 8 samples — the env moves at audio rate
     * but the filter ear-perceives smoothly with ~6 kHz coef-update rate. */
    for (int i = 0; i < frames; i++) {
        float mono = 0.5f * (l[i] + r[i]);
        float a = fabsf(mono);
        /* Instant attack, exponential release. */
        /* Denormal guard: env idles at 0 between notes; ARM has no FTZ. */
        m->wah.env = (a > m->wah.env) ? a : (rel * m->wah.env + (1.0f - rel) * a + 1e-25f);

        /* Sensitivity: 1.0 baseline + 6× headroom for soft signal to still
         * sweep. Then clamped to [0,1] for log mapping. */
        float w = m->wah.env * 7.0f;
        if (w > 1.0f) w = 1.0f;

        float fc = 450.0f * powf(2.0f, 2.35f * w);          /* 450 Hz → ~4.6 kHz */
        float Q  = 6.5f - 4.5f * w;                          /* 6.5 (throaty) → 2.0 (open) */
        /* Two-tier gain comp:
         *   (1) BPF-loss makeup — `g` scales with how dark the filter is
         *       (closed sweep strips more energy → needs more boost).
         *       Bumped to 4.0..1.8 (from 3.3..1.5) per user feedback that
         *       the wet is still perceptibly quieter than dry.
         *   (2) Knob-dependent extra makeup — `amt_boost` adds up to +50 %
         *       at full knob so a "full-wet" wah feels at least as loud as
         *       the dry signal, matching the user's "louder at higher
         *       values" request. */
        float g         = 1.8f + 2.2f * (1.0f - w);          /* 4.0 → 1.8 */
        float amt_boost = 1.0f + 0.5f * amt;                  /* 1.0 → 1.5 */

        if ((i & 7) == 0) {
            biquad_set_bpf(&m->wah.bq_l, fc, Q);
            biquad_set_bpf(&m->wah.bq_r, fc, Q);
        }
        float yl = g * amt_boost * biquad_proc(&m->wah.bq_l, l[i]);
        float yr = g * amt_boost * biquad_proc(&m->wah.bq_r, r[i]);

        /* Cap the wet at 85 % even at full knob — leaves a 15 % dry leak
         * that keeps transients audible and prevents the wah from sounding
         * like the dry signal vanished. */
        float wet_mix = 0.85f * amt;
        l[i] = (1.0f - wet_mix) * l[i] + wet_mix * yl;
        r[i] = (1.0f - wet_mix) * r[i] + wet_mix * yr;
    }
}

/* ---- 6. Texturizer (WoFi-ish): grain + shimmer + echo + crush ---- */
/* Texturizer — one-knob WoFi-style ribbon, musical at every position.
 * Zones (each transition is smooth, no hard mode boundaries):
 *   0.00–0.05  bypass
 *   0.05–0.30  subtle live grain cloud (mostly unison pitch)
 *   0.30–0.60  + shimmer SVF bandpass feedback (octaves/fifths blend in)
 *   0.60–0.90  + 1-tap echo with soft-sat feedback
 *   0.90–1.00  auto-FREEZE — new grains stop spawning, existing loop forever
 *
 * Grain pitch is power-chord-quantized (unison, ±octave, ±perfect-fifth) per
 * Ambiotica's pitch table — this is what makes grain clouds sound musical
 * instead of cloudy. The quantization probability widens with the knob so low
 * settings stay mostly unison and high settings reharmonize. */
static const float TEX_PITCH_STEPS[5] = {
    1.0f,        /* unison */
    2.0f,        /* +octave */
    0.5f,        /* -octave */
    1.4983f,     /* +perfect fifth (7 semitones) */
    0.6674f      /* -perfect fifth */
};

static void fx_texture(mello_t *m, float *l, float *r, int frames) {
    /* ALWAYS-RUN: grain buffer stays current; turning the knob on doesn't
     * grab stale audio. Output is gated by `wet = amt`, so dry-only at 0. */
    float amt = m->sm_texture;

    /* Zone remap (v0.1.20): freeze pushed up to 96 % so the freeze
     * gesture is a clear "all-the-way commit" rather than a wide top
     * region that's easy to fall into accidentally.  Earlier zones
     * stretched so they still ramp smoothly across 0–96 %:
     *   0.00–0.05  bypass
     *   0.05–0.32  grain cloud spawning rate ramp
     *   0.32–0.64  shimmer feedback ramp
     *   0.64–0.96  echo wet ramp
     *   0.96–1.00  FREEZE (no new grains, existing loop forever) */
    float grain_amt = clampf((amt - 0.05f) * (1.0f / 0.27f), 0.0f, 1.0f);
    float shim_amt  = clampf((amt - 0.32f) * (1.0f / 0.32f), 0.0f, 1.0f);
    float echo_amt  = clampf((amt - 0.64f) * (1.0f / 0.32f), 0.0f, 1.0f);
    int   freeze    = (amt >= 0.96f);
    /* Wet curve — peaks at 60 % knob (max wet 0.85), tapers back to 0.55
     * at 100 % so the freeze zone (≥ 96 %) sits at a sane level relative
     * to the rest of the rack.  Matches the user's "at 60 % should be its
     * maximum" intent. */
    float wet;
    if (amt < 0.60f) {
        wet = amt * (0.85f / 0.60f);            /* 0 → 0.85 over 0-60 %  */
    } else {
        float t = (amt - 0.60f) * (1.0f / 0.40f); /* 0..1 over 60-100 %  */
        wet = 0.85f - t * 0.30f;                /* 0.85 → 0.55           */
    }

    float spawn_interval = 4800.0f - grain_amt * 3600.0f;   /* ~100 ms..25 ms */

    for (int i = 0; i < frames; i++) {
        float in_mono = (l[i] + r[i]) * 0.5f;
        /* In freeze mode, stop recording new audio into the grain buffer too */
        if (!freeze) m->tex.grain_buf[m->tex.wp] = in_mono;

        /* maybe spawn a grain — but not in freeze, where existing grains loop */
        m->tex.spawn_counter++;
        if (!freeze && (float)m->tex.spawn_counter >= spawn_interval) {
            m->tex.spawn_counter = 0;
            for (int g = 0; g < TEX_MAX_GRAINS; g++) {
                if (m->tex.g_age[g] >= m->tex.g_len[g]) {
                    /* Start the read further back (wider range) so pitch>1
                     * grains have headroom before the read outpaces the
                     * write head into stale audio. */
                    float back = randf01(&m->fx.rng) * 6000.0f + 4000.0f;
                    int rp = (m->tex.wp - (int)back + TEX_GRAIN_BUF) % TEX_GRAIN_BUF;
                    m->tex.g_read[g] = (float)rp;
                    m->tex.g_age[g]  = 0;
                    /* Power-chord pitch quantization (Ambiotica). */
                    float roll = randf01(&m->fx.rng);
                    float unison_prob = 1.0f - 0.8f * grain_amt;
                    int idx;
                    if (roll < unison_prob) {
                        idx = 0;                          /* unison */
                    } else {
                        float r2 = randf01(&m->fx.rng);
                        if      (r2 < 0.30f) idx = 1;     /* +octave */
                        else if (r2 < 0.55f) idx = 2;     /* -octave */
                        else if (r2 < 0.80f) idx = 3;     /* +fifth */
                        else                 idx = 4;     /* -fifth */
                    }
                    m->tex.g_pitch[g] = TEX_PITCH_STEPS[idx];
                    m->tex.g_pan[g]   = randf_pm(&m->fx.rng);
                    /* Pick grain length, then CAP IT so the read advance over
                     * the grain's life never exceeds the buffer headroom. For
                     * pitch>1 (up-shifted), the read advances faster than the
                     * write; if the grain runs too long it reads "future"
                     * (stale) audio = audible click. */
                    int target_len = (int)(2400.0f + randf01(&m->fx.rng) * 7200.0f);
                    float pitch_abs = m->tex.g_pitch[g];
                    if (pitch_abs < 1.0f) pitch_abs = 1.0f;
                    int safe_len   = (int)((back - 200.0f) / pitch_abs);
                    if (target_len > safe_len) target_len = safe_len;
                    if (target_len < 1200) target_len = 1200;    /* 25 ms min */
                    m->tex.g_len[g]  = target_len;
                    break;
                }
            }
        }
        /* In freeze: as grains finish, re-spawn them at the same read offset
         * so the cloud sustains indefinitely. */
        if (freeze) {
            for (int g = 0; g < TEX_MAX_GRAINS; g++) {
                if (m->tex.g_age[g] >= m->tex.g_len[g] && m->tex.g_len[g] > 0) {
                    m->tex.g_age[g] = 0;   /* loop this grain */
                }
            }
        }

        /* render all live grains */
        float gl = 0.0f, gr = 0.0f;
        for (int g = 0; g < TEX_MAX_GRAINS; g++) {
            int len = m->tex.g_len[g];
            int age = m->tex.g_age[g];
            if (age >= len) continue;
            /* hann window */
            float t = (float)age / (float)len;
            float win = 0.5f - 0.5f * cosf(t * TWO_PI);
            /* read */
            float rp = m->tex.g_read[g];
            int ri = (int)rp;
            float fr = rp - (float)ri;
            int ri1 = (ri + 1) % TEX_GRAIN_BUF;
            float s = m->tex.grain_buf[ri & (TEX_GRAIN_BUF - 1)] * (1.0f - fr) +
                      m->tex.grain_buf[ri1 & (TEX_GRAIN_BUF - 1)] * fr;
            float pan = m->tex.g_pan[g];
            gl += s * win * (0.7f - 0.3f * pan);
            gr += s * win * (0.7f + 0.3f * pan);
            /* advance */
            m->tex.g_read[g] = rp + m->tex.g_pitch[g];
            if (m->tex.g_read[g] >= (float)TEX_GRAIN_BUF) m->tex.g_read[g] -= (float)TEX_GRAIN_BUF;
            m->tex.g_age[g]++;
        }
        /* Grain-cloud level — 0.70× sits between v0.1.17's 0.60 (too
         * quiet in freeze) and v0.1.18's 1.00 (too loud vs other FX).
         * Freeze mode is still useful and the cloud no longer
         * over-dominates the rack. */
        gl *= 0.70f; gr *= 0.70f;

        /* Shimmer: SVF bandpass ~6 kHz with feedback. Now ALWAYS runs (state
         * always-tracked) — only the OUTPUT scales by shim_amt. The previous
         * `if (shim_amt > 0.01f)` gate left stale filter state when shim_amt
         * crossed 0; turning the knob up replayed that stale state as a click. */
        {
            float f = 0.6f;
            float q = 0.15f;
            m->tex.shim_lp_l += f * m->tex.shim_bp_l + 1e-25f;
            float hp_l = gl - m->tex.shim_lp_l - q * m->tex.shim_bp_l;
            m->tex.shim_bp_l += f * hp_l + 1e-25f;
            m->tex.shim_lp_r += f * m->tex.shim_bp_r + 1e-25f;
            float hp_r = gr - m->tex.shim_lp_r - q * m->tex.shim_bp_r;
            m->tex.shim_bp_r += f * hp_r + 1e-25f;
            /* Feedback capped at 0.85 (was 0.95) AND soft-saturated inside
             * the loop so a large transient can't drive |bp| to extreme
             * values that ring for seconds at full Q.  Without the in-loop
             * limit the SVF could sustain ±N peaks indefinitely. */
            float fb = 0.5f + shim_amt * 0.35f;        /* 0.50 → 0.85 */
            m->tex.shim_bp_l = fast_tanh(m->tex.shim_bp_l * fb);
            m->tex.shim_bp_r = fast_tanh(m->tex.shim_bp_r * fb);
            gl += m->tex.shim_bp_l * 0.7f * shim_amt;
            gr += m->tex.shim_bp_r * 0.7f * shim_amt;
        }

        /* Echo — ALWAYS writes/reads the ring (no on/off transitions). The
         * feedback amount and the wet output scale by echo_amt, so when the
         * knob is below the echo zone the audible effect is zero but the
         * buffer keeps "tracking" the dry signal silently. */
        {
            int tap = 8000;                                          /* 167 ms */
            int rp = (m->tex.echo_wp - tap + 8192) & 8191;
            float e = m->tex.echo_buf[rp];
            float fb_amt = (0.35f + echo_amt * 0.35f) * echo_amt;    /* scaled by amt */
            float fb_sig = e * fb_amt;
            fb_sig = fb_sig / (1.0f + fabsf(fb_sig * 0.8f));
            m->tex.echo_buf[m->tex.echo_wp] = (gl + gr) * 0.5f + fb_sig;
            m->tex.echo_wp = (m->tex.echo_wp + 1) & 8191;
            gl += e * 0.5f * echo_amt;
            gr += e * 0.5f * echo_amt;
        }

        /* Vintage warmth — one-pole LP on the grain output, rolls off
         * above ~4 kHz so the cloud feels analog-tape warm instead of
         * digital-crisp.  The up-pitch grains (+octave, +fifth) get the
         * most softening, which masks their occasionally-harsh quality
         * while leaving the unison + down-pitch grains intact in body.
         * c = 1 − exp(−2π·4000/48000) ≈ 0.41. */
        const float warmth_c = 0.41f;
        m->tex.warmth_l += warmth_c * (gl - m->tex.warmth_l) + 1e-25f;
        m->tex.warmth_r += warmth_c * (gr - m->tex.warmth_r) + 1e-25f;
        gl = m->tex.warmth_l;
        gr = m->tex.warmth_r;

        /* mix into output */
        l[i] = l[i] * (1.0f - wet) + gl * wet;
        r[i] = r[i] * (1.0f - wet) + gr * wet;

        m->tex.wp = (m->tex.wp + 1) & (TEX_GRAIN_BUF - 1);
    }
}

/* ---- 7. Delay (BBD-style, gain-staged like Schwung TapeDelay) ----
 * Key fix from the prior version: the OLD delay wrote (input + feedback)
 * unbounded, which let buf accumulate to ±1.6+ for sustained loud input —
 * tap-back was hot, wet additive output pushed past the output soft-clip,
 * and the result was the "distorted/glitching" tail the user reported.
 *
 * Two changes:
 *   (a) input to buffer is GAIN-STAGED — it scales DOWN as feedback rises
 *       (`input_gain = 1 - fb_amt*0.92`), keeping the steady-state level
 *       of the delay line at ~±0.5 regardless of feedback.
 *   (b) the BUFFER WRITE itself is soft-clipped (`x/(1+|x|)`), so even an
 *       extreme transient can never accumulate past ±1. */
static void fx_delay(mello_t *m, float *l, float *r, int frames) {
    /* ALWAYS-RUN: BBD delay buffer stays current with dry signal so re-engaging
     * doesn't surface stale taps. target_wet = amt * 0.85 → silent at knob=0. */
    float amt = m->sm_delay;

    /* Target params for this block. Smoothed per-sample below so knob moves
     * don't jump the tap position or feedback level. */
    float time_ms        = 240.0f + amt * 360.0f;     /* 240..600 ms */
    float target_dly     = time_ms * SR / 1000.0f;
    if (target_dly < 1.0f) target_dly = 1.0f;
    float max_dly = (float)((int)DELAY_MAX_SAMPLES - 4);
    if (target_dly > max_dly) target_dly = max_dly;
    float target_fb      = clampf(0.20f + amt * 0.55f, 0.0f, 0.75f);
    float target_wet     = amt * 0.85f;          /* louder per user — was 0.50 */
    float target_in_gain = 1.0f - target_fb * 0.92f;

    /* Smoothing coefficient: ~60 ms time-constant. Tape capstan inertia. */
    const float sm_c = 0.0007f;

    for (int i = 0; i < frames; i++) {
        m->dly.dly_smooth        += sm_c * (target_dly     - m->dly.dly_smooth);
        m->dly.fb_smooth         += sm_c * (target_fb      - m->dly.fb_smooth);
        m->dly.wet_smooth        += sm_c * (target_wet     - m->dly.wet_smooth);
        m->dly.input_gain_smooth += sm_c * (target_in_gain - m->dly.input_gain_smooth);

        m->dly.jitter_phase += 0.15f * SR_INV;
        if (m->dly.jitter_phase > 1.0f) m->dly.jitter_phase -= 1.0f;
        float jit = sinf(m->dly.jitter_phase * TWO_PI) * 0.04f;
        float base_c = 0.15f + (1.0f - amt) * 0.20f;
        float lp_c   = clampf(base_c + jit, 0.10f, 0.50f);

        /* Fractional tap read with linear interpolation. As dly_smooth glides
         * toward its target, the tap position glides — no jump = no click. */
        float dly_f  = m->dly.dly_smooth;
        float rp_f   = (float)m->dly.wp - dly_f;
        while (rp_f < 0.0f) rp_f += (float)DELAY_MAX_SAMPLES;
        int   rp_i   = (int)rp_f;
        if (rp_i >= (int)DELAY_MAX_SAMPLES) rp_i -= (int)DELAY_MAX_SAMPLES;
        float frac_d = rp_f - (float)rp_i;
        int   rp_i1  = (rp_i + 1) % (int)DELAY_MAX_SAMPLES;
        float tap_l  = m->dly.buf_l[rp_i] + frac_d * (m->dly.buf_l[rp_i1] - m->dly.buf_l[rp_i]);
        float tap_r  = m->dly.buf_r[rp_i] + frac_d * (m->dly.buf_r[rp_i1] - m->dly.buf_r[rp_i]);

        /* 2-pole LP cascade in feedback (denormal-guarded) */
        m->dly.lp1_l += lp_c * (tap_l - m->dly.lp1_l) + 1e-25f;
        m->dly.lp2_l += lp_c * (m->dly.lp1_l - m->dly.lp2_l) + 1e-25f;
        m->dly.lp1_r += lp_c * (tap_r - m->dly.lp1_r) + 1e-25f;
        m->dly.lp2_r += lp_c * (m->dly.lp1_r - m->dly.lp2_r) + 1e-25f;

        float fb_l = m->dly.lp2_l * m->dly.fb_smooth;
        float fb_r = m->dly.lp2_r * m->dly.fb_smooth;

        /* (a) gain-staged input + (b) soft-clip on the buffer write — keeps
         * the delay line strictly bounded to |buf| < 1 at all times. */
        float to_buf_l = l[i] * m->dly.input_gain_smooth + fb_l;
        float to_buf_r = r[i] * m->dly.input_gain_smooth + fb_r;
        to_buf_l = to_buf_l / (1.0f + fabsf(to_buf_l));   /* asymptote ±1 */
        to_buf_r = to_buf_r / (1.0f + fabsf(to_buf_r));

        m->dly.buf_l[m->dly.wp] = to_buf_l;
        m->dly.buf_r[m->dly.wp] = to_buf_r;
        m->dly.wp = (m->dly.wp + 1) % (int)DELAY_MAX_SAMPLES;

        /* mix wet, with gentle dry attenuation so the sum stays under ~1.2 */
        float wet_now = m->dly.wet_smooth;
        float dry_atten = 1.0f - 0.25f * wet_now;
        l[i] = l[i] * dry_atten + tap_l * wet_now;
        r[i] = r[i] * dry_atten + tap_r * wet_now;
    }
}

/* ---- 8. Reverb (small plate, vintage character) ---- */
static void fx_reverb(mello_t *m, float *l, float *r, int frames) {
    /* ALWAYS-RUN: the recirculating tail tracks the dry signal continuously,
     * so when the knob is turned on the reverb sounds like it was always
     * there — no click from suddenly engaging a stale buffer. */
    float amt = m->sm_reverb;

    /* Tap delays span 108–600 ms. Decay capped under unity so the tail can't
     * compound; wet level tamed and buffer-write soft-clipped so even long
     * delayed playing can't cause the limiter to choke the whole instrument
     * (user-reported "sound cuts off when reverb is loud + I play with delay
     * quickly"). */
    static const int DELAYS_L[6] = {  5163,  7919,  11689, 16097, 21379, 28723 };
    static const int DELAYS_R[6] = {  4749,  7211,  11003, 15583, 20767, 27583 };
    /* Decay capped at 0.80 — previous 0.90 ceiling held enough loop energy
     * for the comb network's natural mode to ring through wet-mix changes
     * as an audible tone.  At 0.80 the resonance dies fast enough that the
     * LFO smear (below) can break it before it builds. */
    float decay = 0.45f + amt * 0.35f;             /* 0.45 → 0.80 */
    float wet   = amt * 0.80f;                     /* 0 → 0.80 */
    float damp  = 0.10f + amt * 0.30f;

    /* Async LFOs at slightly faster, still incommensurate rates so the
     * modulation actively smears the comb resonance instead of glacially
     * drifting around it.  Mod depth doubled: ±15..±40 samples = ~0.3 %
     * to ~0.8 % pitch wobble per tap — well within the "chorussed reverb"
     * range musicians actually want, and big enough that no single comb
     * mode can dominate the spectrum long enough to ring. */
    static const float TAP_LFO_HZ[6] = { 0.71f, 1.03f, 1.37f, 1.79f, 2.11f, 2.53f };
    int   mod_depth = (int)(15.0f + amt * 25.0f);   /* ±15..±40 samples */
    float lfo_inc[6];
    for (int t = 0; t < 6; t++) lfo_inc[t] = TAP_LFO_HZ[t] * SR_INV;

    /* Quadrature LFOs — seed (c, s) from canonical phase, precompute per-
     * sample rotation deltas.  Was the heaviest single trig load in the
     * plugin (6 sinf calls per sample → 6 muls per sample now).
     * The phase counter still advances per sample so block-to-block rate
     * changes resync cleanly via the seeding cosf/sinf at block top. */
    float lfo_c[6], lfo_s[6], dc[6], ds[6];
    for (int t = 0; t < 6; t++) {
        float th0 = m->rev.tap_lfo_phase[t] * TWO_PI;
        lfo_c[t]  = cosf(th0);
        lfo_s[t]  = sinf(th0);
        float dth = lfo_inc[t] * TWO_PI;
        dc[t]     = cosf(dth);
        ds[t]     = sinf(dth);
    }

    for (int i = 0; i < frames; i++) {
        /* Per-sample LFO advance: each LFO contributes a single 's' value
         * (= sin θ).  Phase counter incremented in parallel so re-seeding
         * at the next block stays sample-accurate. */
        float lfo[6];
        for (int t = 0; t < 6; t++) {
            m->rev.tap_lfo_phase[t] += lfo_inc[t];
            if (m->rev.tap_lfo_phase[t] >= 1.0f) m->rev.tap_lfo_phase[t] -= 1.0f;
            lfo[t] = lfo_s[t];
            /* Rotate (c, s) by (dc, ds): the rotation-matrix advance. */
            float nc = lfo_c[t] * dc[t] - lfo_s[t] * ds[t];
            float ns = lfo_s[t] * dc[t] + lfo_c[t] * ds[t];
            lfo_c[t] = nc;
            lfo_s[t] = ns;
        }

        /* single-write-head, 6 read taps with LFO-wobbled offsets. (Six
         * independent write heads collapse to one because all advanced
         * together — DSP review caught the degenerate FDN. With a single
         * write head this is a coherent multi-tap diffuse reverb, not a real
         * FDN, but it sounds bigger than the old broken version.) */
        float sum_l = 0.0f, sum_r = 0.0f;
        for (int t = 0; t < 6; t++) {
            int off = (int)(lfo[t] * (float)mod_depth);
            int rp_l = (m->rev.wp - DELAYS_L[t] + off + REVERB_DLY_MAX) % REVERB_DLY_MAX;
            int rp_r = (m->rev.wp - DELAYS_R[t] - off + REVERB_DLY_MAX) % REVERB_DLY_MAX;
            sum_l += m->rev.buf_l[rp_l];
            sum_r += m->rev.buf_r[rp_r];
        }
        /* Sum scaling 0.33 instead of 1/6 — old version was too quiet
         * because dividing the 6-tap sum by 6 left output at unity tap level
         * regardless of how loud the recirculation got. Boosting to 0.33
         * gives ~2× the wet presence. */
        sum_l *= 0.33f; sum_r *= 0.33f;

        /* HF damp (+ denormal guard) */
        m->rev.lp_l += damp * (sum_l - m->rev.lp_l) + 1e-25f;
        m->rev.lp_r += damp * (sum_r - m->rev.lp_r) + 1e-25f;
        float fb_l = m->rev.lp_l * decay;
        float fb_r = m->rev.lp_r * decay;

        /* Allpass diffusion — 4 stages with VARYING but moderate coefficients.
         * Earlier values (0.50/0.62/0.71/0.78) made the loop self-oscillate
         * at the comb network's natural resonance — when the user wiggled
         * the knob, the wet ramp exposed that always-on resonance as a
         * "ringing tone, always the same frequency". Reduced coefficients
         * cut the self-Q so the diffuser smears instead of rings. */
        static const float AP_A[4] = { 0.42f, 0.50f, 0.58f, 0.65f };
        for (int s = 0; s < 4; s++) {
            float a = AP_A[s];
            float al = a * fb_l + m->rev.ap_l[s];
            m->rev.ap_l[s] = fb_l - a * al;
            fb_l = al;
            float ar = a * fb_r + m->rev.ap_r[s];
            m->rev.ap_r[s] = fb_r - a * ar;
            fb_r = ar;
        }

        /* Gate the loop INPUT by amt — when the knob is at zero, no fresh
         * signal enters the recirculating buffer, so the tail decays away
         * naturally and the user can't hear "the reverb that was always
         * there underneath" the moment they touch the knob. */
        float in_gain = amt * 0.5f;
        float drive_in = 0.5f * (l[i] + r[i]) * in_gain;

        /* 120 Hz one-pole HPF on the loop input — kills sub-bass buildup
         * that accumulates when sustained low notes are sent into a long
         * decay reverb.  r = exp(-2π·120/SR) ≈ 0.9844.  Standard zero-at-DC
         * differentiator + leaky integrator. */
        const float HPF_R = 0.9844f;
        float hpf_out = drive_in - m->rev.hpf_x1 + HPF_R * m->rev.hpf_y1;
        m->rev.hpf_x1 = drive_in;
        m->rev.hpf_y1 = hpf_out;

        float to_buf_l = hpf_out + fb_r;
        float to_buf_r = hpf_out + fb_l;
        to_buf_l = to_buf_l / (1.0f + fabsf(to_buf_l));   /* asymptote ±1 */
        to_buf_r = to_buf_r / (1.0f + fabsf(to_buf_r));
        m->rev.buf_l[m->rev.wp] = to_buf_l;
        m->rev.buf_r[m->rev.wp] = to_buf_r;
        m->rev.wp = (m->rev.wp + 1) % REVERB_DLY_MAX;

        l[i] = l[i] + sum_l * wet;
        r[i] = r[i] + sum_r * wet;
    }

    /* Off-state detector: when amt sits below threshold for >100ms,
     * zero out the entire loop so residual content from earlier playing
     * can't keep ringing the comb network's natural mode.  Threshold
     * 0.005 (effectively "off"); reset counter when knob comes back up. */
    if (amt < 0.005f) {
        m->rev.off_counter += frames;
        if (m->rev.off_counter >= (int)(SR * 0.10f)) {
            memset(m->rev.buf_l, 0, sizeof(m->rev.buf_l));
            memset(m->rev.buf_r, 0, sizeof(m->rev.buf_r));
            memset(m->rev.ap_l,  0, sizeof(m->rev.ap_l));
            memset(m->rev.ap_r,  0, sizeof(m->rev.ap_r));
            m->rev.lp_l = m->rev.lp_r = 0.0f;
            m->rev.hpf_x1 = m->rev.hpf_y1 = 0.0f;
            /* keep counter pinned so we don't re-zero every block */
            m->rev.off_counter = (int)(SR * 0.10f);
        }
    } else {
        m->rev.off_counter = 0;
    }
}

/* ============================================================================
 * Limiter (output protection)
 * ============================================================================ */
static void apply_limiter(mello_t *m, float *l, float *r, int frames) {
    float amt = m->sm_lim;
    if (amt < 0.001f) return;
    float threshold = 0.95f - amt * 0.25f;            /* 0.95 down to 0.70 */
    float ratio = 4.0f + amt * 16.0f;
    float att_coeff = 1.0f - expf(-1.0f / (0.0005f * SR));   /* 0.5 ms */
    float rel_coeff = 1.0f - expf(-1.0f / (0.080f * SR));    /* 80 ms */

    for (int i = 0; i < frames; i++) {
        float peak = fmaxf(fabsf(l[i]), fabsf(r[i]));
        /* Denormal guard: limiter env decays exponentially toward 0 during
         * silence and lives near zero most of the time. */
        if (peak > m->fx.lim_env) m->fx.lim_env += att_coeff * (peak - m->fx.lim_env) + 1e-25f;
        else                       m->fx.lim_env += rel_coeff * (peak - m->fx.lim_env) + 1e-25f;
        float gain = 1.0f;
        if (m->fx.lim_env > threshold) {
            float over = m->fx.lim_env - threshold;
            float comp = over / ratio;
            float target = threshold + comp;
            gain = target / (m->fx.lim_env + 1e-6f);
        }
        l[i] *= gain; r[i] *= gain;
    }
}

/* ============================================================================
 * Parameter registry / set / get
 * ============================================================================ */
enum reg_type { T_F, T_I, T_E };
typedef struct { const char *key; int type; void *p; const char **opt; int nopt; } reg_t;
#define R_COUNT 48

static void build_registry(mello_t *m, reg_t *r, int *n) {
    int i = 0;
    #define RF(K,P) r[i++] = (reg_t){K, T_F, &m->P, NULL, 0}
    #define RI(K,P) r[i++] = (reg_t){K, T_I, &m->P, NULL, 0}
    #define RE(K,P,O,N) r[i++] = (reg_t){K, T_E, &m->P, O, N}
    RI("bank_a", p_bank_a); RI("bank_b", p_bank_b);
    RF("ab_mix", p_ab_mix); RI("pitch", p_pitch);
    RF("tone", p_tone); RE("env_preset", p_env_preset, ENV_PRESETS, ENV_PRESET_COUNT);
    RF("crunch", p_crunch); RF("volume", p_volume);
    RF("env_a", p_env_a); RF("env_d", p_env_d); RF("env_s", p_env_s); RF("env_r", p_env_r);
    RF("env_ac", p_env_ac); RF("env_rc", p_env_rc); RF("env_vel", p_env_vel);
    RE("tape_style", p_tape_style, TAPE_STYLES, TAPE_STYLE_COUNT);
    RF("warble", p_warble); RF("flutter", p_flutter); RF("degrade", p_degrade);
    RF("tape_sat", p_tape_sat);
    RE("preamp_model", p_preamp_model, PREAMP_MODELS, PREAMP_COUNT);
    RF("input_level", p_input_level); RF("limiter", p_limiter);
    RF("tape_len", p_tape_len); RF("chiff", p_chiff);
    RF("vibrato", p_vibrato); RF("rotary", p_leslie); RF("dj_filter", p_dj_filter);
    RF("tremolo", p_tremolo); RF("autowah", p_autowah); RF("texture", p_texture);
    RF("delay", p_delay); RF("reverb", p_reverb);
    RE("fx_chain", p_fx_chain, FX_CHAIN_MODES, FX_CHAIN_COUNT);
    RE("map_mode", p_map_mode, MAP_MODES, 2); RE("loop_mode", p_loop_mode, LOOP_MODES, 2);
    RE("half_speed", p_half_speed, ONOFF, 2);
    RI("xfade_pct", p_xfade_pct);
    RE("auto_retune", p_auto_retune, ONOFF, 2);
    RI("seq_base", p_seq_base);
    RE("reverse", p_reverse, ONOFF, 2);
    RF("sample_start", p_sample_start);
    RF("bias", p_bias);
    RF("mech", p_mech);
    #undef RF
    #undef RI
    #undef RE
    *n = i;
}

static int enum_index(const char **opt, int n, const char *val) {
    for (int i = 0; i < n; i++) if (!strcmp(opt[i], val)) return i;
    char *end; long v = strtol(val, &end, 10);
    if (end != val && v >= 0 && v < n) return (int)v;
    return -1;
}

static int page_from_level(const char *v) {
    /* v0.1.15 reorder: FX is now menu 2, Envelope menu 3 (was the other
     * way round).  Tape & Preamp stays last on menu 4. */
    if (!strcmp(v, "FX")) return 1;
    if (!strcmp(v, "Envelope")) return 2;
    if (!strcmp(v, "Tape") || !strcmp(v, "Tape & Preamp")) return 3;
    return 0;
}

static const char *KNOB_MAIN[8] = {"bank_a","bank_b","ab_mix","pitch","tone","env_preset","crunch","volume"};
static const char *KNOB_FX  [8] = {"autowah","vibrato","tremolo","rotary","dj_filter","texture","delay","reverb"};
static const char *KNOB_ENV [8] = {"env_preset","env_a","env_d","env_s","env_r","env_ac","env_rc","env_vel"};
static const char *KNOB_TAPE[8] = {"tape_style","warble","flutter","degrade","tape_sat","preamp_model","input_level","limiter"};
static const char *const *page_knobs(int p) {
    switch (p) { case 1: return KNOB_FX; case 2: return KNOB_ENV; case 3: return KNOB_TAPE; default: return KNOB_MAIN; }
}

static void apply_env_preset(mello_t *m, int idx) {
    if (idx < 0 || idx >= 22) return;     /* 22 = "Custom" sentinel */
    const env_def_t *e = &ENV_DEFS[idx];
    m->p_env_a = e->a; m->p_env_d = e->d; m->p_env_s = e->s; m->p_env_r = e->r;
    m->p_env_ac = e->ac; m->p_env_rc = e->rc; m->p_env_vel = e->vel;
    /* (no smoothed env values to snap — envelope rates are recomputed on note-on) */
}

static void set_param(void *inst, const char *key, const char *val) {
    mello_t *m = (mello_t *)inst;
    if (!m || !key || !val) return;

    if (!strcmp(key, "_level") || !strcmp(key, "current_level")) {
        if (!strcmp(val, "Mello") || !strcmp(val, "root")) m->current_page = 0;
        else m->current_page = page_from_level(val);
        return;
    }

    /* Schwung sometimes routes through knob_N_adjust as a delta. We treat it
     * as an absolute set on the current page's knob (Schwung's overlay sends
     * the new value, not a delta, for sound generator knobs). */
    if (!strncmp(key, "knob_", 5) && strstr(key, "_adjust")) {
        int idx = atoi(key + 5) - 1;
        if (idx < 0 || idx > 7) return;
        const char *pk = page_knobs(m->current_page)[idx];
        set_param(inst, pk, val);
        return;
    }

    /* bulk state restore */
    if (!strcmp(key, "state")) {
        /* simple whitespace-separated key=value parser */
        char buf[2048]; strncpy(buf, val, sizeof(buf) - 1); buf[sizeof(buf) - 1] = '\0';
        char *tok = strtok(buf, " \t\n");
        while (tok) {
            char *eq = strchr(tok, '=');
            if (eq) {
                *eq = '\0';
                set_param(inst, tok, eq + 1);
            }
            tok = strtok(NULL, " \t\n");
        }
        return;
    }

    /* sample_lib — picking a library is a QUICK-JUMP: bank_a and bank_b
     * snap to the first bank of that library in the flattened union list.
     * The bank_a / bank_b enums themselves are NOT re-filtered — the host
     * caches enum options per-instance, so we can't dynamically narrow
     * them.  Instead the user scrolls Bank A and sees every library's
     * banks alphabetically grouped (all "Leisureland/…" together, then
     * all "Sonic Bloom/…" together). */
    if (!strcmp(key, "sample_lib")) {
        int idx = -1;
        for (int i = 0; i < m->lib_list.count; i++)
            if (!strcmp(val, m->lib_list.names[i])) { idx = i; break; }
        if (idx < 0) {
            char *end; long v = strtol(val, &end, 10);
            if (end != val && v >= 0 && v < m->lib_list.count) idx = (int)v;
        }
        if (idx < 0) return;
        int first = activate_sample_library(m, idx);
        if (first >= 0 && first < m->blist.count) {
            if (first != m->p_bank_a) {
                m->p_bank_a = first;
                request_bank_load_async(m, &m->a, first);
            }
            if (first != m->p_bank_b) {
                m->p_bank_b = first;
                request_bank_load_async(m, &m->b, first);
            }
        }
        return;
    }

    /* bank_a / bank_b are enums of scanned folder names — Schwung sends the
     * option string, not a numeric index. Match against the blist and load. */
    if (!strcmp(key, "bank_a") || !strcmp(key, "bank_b")) {
        int idx = -1;
        for (int i = 0; i < m->blist.count; i++)
            if (!strcmp(val, m->blist.names[i])) { idx = i; break; }
        if (idx < 0) {                              /* numeric fallback */
            char *end; long v = strtol(val, &end, 10);
            if (end != val && v >= 0 && v < m->blist.count) idx = (int)v;
        }
        if (idx < 0) return;
        if (!strcmp(key, "bank_a")) {
            if (idx != m->p_bank_a) {
                m->p_bank_a = idx;
                /* fire-and-forget async load; returns instantly so the
                 * Schwung event thread isn't blocked on file I/O */
                request_bank_load_async(m, &m->a, idx);
            }
        } else {
            if (idx != m->p_bank_b) {
                m->p_bank_b = idx;
                request_bank_load_async(m, &m->b, idx);
            }
        }
        return;
    }

    reg_t reg[R_COUNT]; int n; build_registry(m, reg, &n);
    reg_t *r = NULL;
    for (int i = 0; i < n; i++) if (!strcmp(reg[i].key, key)) { r = &reg[i]; break; }
    if (!r) return;

    if (r->type == T_E) {
        int e = enum_index(r->opt, r->nopt, val);
        if (e >= 0) {
            *(int *)r->p = e;
            if (!strcmp(key, "env_preset") && e < 22) apply_env_preset(m, e);
        }
    } else if (r->type == T_I) {
        *(int *)r->p = (int)strtol(val, NULL, 10);
    } else {
        *(float *)r->p = (float)atof(val);
        if (!strncmp(key, "env_", 4) && strcmp(key, "env_preset") != 0)
            m->p_env_preset = 22;       /* mark Custom (sentinel index) */
    }
}

/* ---- ui_hierarchy is served from get_param (sound generator rule) ---- */
static const char *UI_HIERARCHY =
"{\"modes\":null,\"levels\":{"
 "\"root\":{\"name\":\"Mello\",\"knobs\":[\"bank_a\",\"bank_b\",\"ab_mix\",\"pitch\",\"tone\",\"env_preset\",\"crunch\",\"volume\"],"
   "\"params\":[{\"level\":\"Main\",\"label\":\"Main\"},{\"level\":\"FX\",\"label\":\"FX\"},{\"level\":\"Envelope\",\"label\":\"Envelope\"},{\"level\":\"Tape\",\"label\":\"Tape & Preamp\"}]},"
 "\"Main\":{\"label\":\"Main\",\"knobs\":[\"bank_a\",\"bank_b\",\"ab_mix\",\"pitch\",\"tone\",\"env_preset\",\"crunch\",\"volume\"],\"params\":[\"sample_lib\",\"bank_a\",\"bank_b\",\"ab_mix\",\"pitch\",\"tone\",\"env_preset\",\"crunch\",\"volume\",\"reverse\",\"half_speed\",\"sample_start\",\"xfade_pct\",\"loop_mode\",\"map_mode\",\"auto_retune\",\"seq_base\"]},"
 "\"FX\":{\"label\":\"FX\",\"knobs\":[\"autowah\",\"vibrato\",\"tremolo\",\"rotary\",\"dj_filter\",\"texture\",\"delay\",\"reverb\"],\"params\":[\"fx_chain\",\"autowah\",\"vibrato\",\"tremolo\",\"rotary\",\"dj_filter\",\"texture\",\"delay\",\"reverb\"]},"
 "\"Envelope\":{\"label\":\"Envelope\",\"knobs\":[\"env_preset\",\"env_a\",\"env_d\",\"env_s\",\"env_r\",\"env_ac\",\"env_rc\",\"env_vel\"],\"params\":[\"env_preset\",\"env_a\",\"env_d\",\"env_s\",\"env_r\",\"env_ac\",\"env_rc\",\"env_vel\"]},"
 "\"Tape\":{\"label\":\"Tape & Preamp\",\"knobs\":[\"tape_style\",\"warble\",\"flutter\",\"degrade\",\"tape_sat\",\"preamp_model\",\"input_level\",\"limiter\"],\"params\":[\"tape_style\",\"warble\",\"flutter\",\"degrade\",\"tape_sat\",\"preamp_model\",\"input_level\",\"limiter\",\"tape_len\",\"chiff\",\"bias\",\"mech\"]}"
"}}";

/* ---- chain_params built dynamically so bank_a/bank_b enums reflect the scan ---- */
static int build_chain_params_json(const mello_t *m, char *buf, int buf_len) {
    int p = 0;
    p += snprintf(buf + p, buf_len - p, "[");
    /* sample_lib — dynamic enum of library folder names */
    p += snprintf(buf + p, buf_len - p,
                  "{\"key\":\"sample_lib\",\"name\":\"Samples Folder\",\"type\":\"enum\",\"options\":[");
    if (m->lib_list.count == 0) p += snprintf(buf + p, buf_len - p, "\"(empty)\"");
    for (int i = 0; i < m->lib_list.count; i++) {
        p += snprintf(buf + p, buf_len - p, "%s\"%s\"", i == 0 ? "" : ",", m->lib_list.names[i]);
    }
    p += snprintf(buf + p, buf_len - p, "]},");
    /* bank_a / bank_b enums (use scanned folder names; fall back to a single
     * "(empty)" option if instruments/ has no folders) */
    p += snprintf(buf + p, buf_len - p,
                  "{\"key\":\"bank_a\",\"name\":\"Bank A\",\"type\":\"enum\",\"options\":[");
    if (m->blist.count == 0) p += snprintf(buf + p, buf_len - p, "\"(empty)\"");
    for (int i = 0; i < m->blist.count; i++) {
        p += snprintf(buf + p, buf_len - p, "%s\"%s\"", i == 0 ? "" : ",", m->blist.names[i]);
    }
    p += snprintf(buf + p, buf_len - p, "]},");
    p += snprintf(buf + p, buf_len - p,
                  "{\"key\":\"bank_b\",\"name\":\"Bank B\",\"type\":\"enum\",\"options\":[");
    if (m->blist.count == 0) p += snprintf(buf + p, buf_len - p, "\"(empty)\"");
    for (int i = 0; i < m->blist.count; i++) {
        p += snprintf(buf + p, buf_len - p, "%s\"%s\"", i == 0 ? "" : ",", m->blist.names[i]);
    }
    p += snprintf(buf + p, buf_len - p, "]},");
    /* remaining params (static) */
    p += snprintf(buf + p, buf_len - p,
        "{\"key\":\"ab_mix\",\"name\":\"A Mix B\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
        "{\"key\":\"pitch\",\"name\":\"Pitch\",\"type\":\"int\",\"min\":-24,\"max\":24,\"step\":1},"
        "{\"key\":\"tone\",\"name\":\"Tone\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
        "{\"key\":\"env_preset\",\"name\":\"Envelope\",\"type\":\"enum\",\"options\":[\"Tape Organ\",\"Strings\",\"Slow Pad\",\"Choir Swell\",\"Pluck\",\"Stab\",\"Bowed Swell\",\"Gate\",\"Reverse Swell\",\"Staccato\",\"Drone\",\"Soft Keys\",\"Cinematic Rise\",\"Pluck Bass\",\"Soft Lead\",\"Pad Wash\",\"Marcato\",\"Hammer\",\"Vox Ahh\",\"Brass Hit\",\"Slow Drone\",\"Pizzicato\",\"Custom\"]},"
        "{\"key\":\"crunch\",\"name\":\"Crunch\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
        "{\"key\":\"volume\",\"name\":\"Volume\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
        "{\"key\":\"env_a\",\"name\":\"Attack\",\"type\":\"float\",\"min\":0,\"max\":5,\"step\":0.01},"
        "{\"key\":\"env_d\",\"name\":\"Decay\",\"type\":\"float\",\"min\":0,\"max\":3,\"step\":0.01},"
        "{\"key\":\"env_s\",\"name\":\"Sustain\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
        "{\"key\":\"env_r\",\"name\":\"Release\",\"type\":\"float\",\"min\":0,\"max\":5,\"step\":0.01},"
        "{\"key\":\"env_ac\",\"name\":\"Atk Curve\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
        "{\"key\":\"env_rc\",\"name\":\"Rel Curve\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
        "{\"key\":\"env_vel\",\"name\":\"Velocity\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
        "{\"key\":\"tape_style\",\"name\":\"Tape Style\",\"type\":\"enum\",\"options\":[\"Off\",\"Clean\",\"M400\",\"MkII\",\"M300\",\"Worn\"]},"
        "{\"key\":\"warble\",\"name\":\"Warble\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
        "{\"key\":\"flutter\",\"name\":\"Flutter\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
        "{\"key\":\"degrade\",\"name\":\"Degrade\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
        "{\"key\":\"tape_sat\",\"name\":\"Saturation\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
        "{\"key\":\"preamp_model\",\"name\":\"Preamp\",\"type\":\"enum\",\"options\":[\"Off\",\"Clean DI\",\"MkII Tube\",\"M400 Solid-State\",\"SK-1 Lo-fi\"]},"
        "{\"key\":\"input_level\",\"name\":\"Input\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
        "{\"key\":\"limiter\",\"name\":\"Limiter\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
        "{\"key\":\"tape_len\",\"name\":\"Tape Len\",\"type\":\"float\",\"min\":1,\"max\":16,\"step\":0.5},"
        "{\"key\":\"chiff\",\"name\":\"Chiff\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
        "{\"key\":\"vibrato\",\"name\":\"Vibrato\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
        "{\"key\":\"rotary\",\"name\":\"Rotary\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
        "{\"key\":\"dj_filter\",\"name\":\"DJ Filter\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
        "{\"key\":\"tremolo\",\"name\":\"Tremolo\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
        "{\"key\":\"autowah\",\"name\":\"Auto-Wah\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
        "{\"key\":\"texture\",\"name\":\"Texturizer\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
        "{\"key\":\"delay\",\"name\":\"Delay\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
        "{\"key\":\"reverb\",\"name\":\"Reverb\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
        "{\"key\":\"fx_chain\",\"name\":\"FX Chain\",\"type\":\"enum\",\"options\":[\"Classic\",\"Pedalboard\",\"Wet Swirl\",\"Ambient Wash\",\"Dub\",\"Reverse\"]},"
        "{\"key\":\"map_mode\",\"name\":\"Map Mode\",\"type\":\"enum\",\"options\":[\"Tape (exact)\",\"Full range (stretched)\"]},"
        "{\"key\":\"loop_mode\",\"name\":\"Loop Mode\",\"type\":\"enum\",\"options\":[\"Tape (no loop)\",\"Sustain (loop)\"]},"
        "{\"key\":\"half_speed\",\"name\":\"Half Speed\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"]},"
        "{\"key\":\"xfade_pct\",\"name\":\"Crossfade %\",\"type\":\"int\",\"min\":0,\"max\":100,\"step\":1},"
        "{\"key\":\"auto_retune\",\"name\":\"Auto Retune\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"]},"
        "{\"key\":\"seq_base\",\"name\":\"Seq Base\",\"type\":\"int\",\"min\":-1,\"max\":127,\"step\":1},"
        "{\"key\":\"reverse\",\"name\":\"Reverse\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"]},"
        "{\"key\":\"sample_start\",\"name\":\"Sample Start\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
        "{\"key\":\"bias\",\"name\":\"Bias\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
        "{\"key\":\"mech\",\"name\":\"Mech Noise\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01}"
        "]");
    return p;
}

static int get_param(void *inst, const char *key, char *buf, int buf_len) {
    mello_t *m = (mello_t *)inst;
    if (!m || !key || !buf || buf_len <= 0) return -1;

    if (!strcmp(key, "ui_hierarchy")) {
        int n = (int)strlen(UI_HIERARCHY);
        if (n >= buf_len) n = buf_len - 1;
        memcpy(buf, UI_HIERARCHY, n); buf[n] = '\0';
        return n;
    }
    if (!strcmp(key, "chain_params")) {
        return build_chain_params_json(m, buf, buf_len);
    }
    if (!strcmp(key, "state")) {
        return snprintf(buf, buf_len,
            "ab_mix=%.4f tone=%.4f crunch=%.4f volume=%.4f pitch=%d env_preset=%d "
            "env_a=%.4f env_d=%.4f env_s=%.4f env_r=%.4f env_ac=%.4f env_rc=%.4f env_vel=%.4f "
            "tape_style=%d warble=%.4f flutter=%.4f degrade=%.4f tape_sat=%.4f preamp_model=%d "
            "input_level=%.4f limiter=%.4f tape_len=%.4f chiff=%.4f "
            "vibrato=%.4f rotary=%.4f dj_filter=%.4f tremolo=%.4f autowah=%.4f texture=%.4f delay=%.4f reverb=%.4f "
            "sample_lib=%d bank_a=%d bank_b=%d fx_chain=%d map_mode=%d loop_mode=%d half_speed=%d xfade_pct=%d auto_retune=%d seq_base=%d",
            m->p_ab_mix, m->p_tone, m->p_crunch, m->p_volume, m->p_pitch, m->p_env_preset,
            m->p_env_a, m->p_env_d, m->p_env_s, m->p_env_r, m->p_env_ac, m->p_env_rc, m->p_env_vel,
            m->p_tape_style, m->p_warble, m->p_flutter, m->p_degrade, m->p_tape_sat, m->p_preamp_model,
            m->p_input_level, m->p_limiter, m->p_tape_len, m->p_chiff,
            m->p_vibrato, m->p_leslie, m->p_dj_filter, m->p_tremolo, m->p_autowah, m->p_texture, m->p_delay, m->p_reverb,
            m->p_sample_lib, m->p_bank_a, m->p_bank_b, m->p_fx_chain, m->p_map_mode, m->p_loop_mode, m->p_half_speed, m->p_xfade_pct, m->p_auto_retune, m->p_seq_base);
    }

    /* knob overlay name/value for the current page */
    if (!strncmp(key, "knob_", 5) && (strstr(key, "_name") || strstr(key, "_value"))) {
        int idx = atoi(key + 5) - 1;
        if (idx < 0 || idx > 7) return -1;
        const char *pk = page_knobs(m->current_page)[idx];
        if (strstr(key, "_name")) {
            /* friendly labels for the overlay */
            static const struct { const char *k; const char *l; } LABEL[] = {
                {"bank_a","Bank A"}, {"bank_b","Bank B"}, {"ab_mix","A · B"}, {"pitch","Pitch"},
                {"tone","Tone"}, {"env_preset","Env"}, {"crunch","Crunch"}, {"volume","Volume"},
                {"env_a","Attack"}, {"env_d","Decay"}, {"env_s","Sustain"}, {"env_r","Release"},
                {"env_ac","Atk Crv"}, {"env_rc","Rel Crv"}, {"env_vel","Vel"},
                {"tape_style","Style"}, {"warble","Warble"}, {"flutter","Flutter"},
                {"degrade","Degrade"}, {"tape_sat","Sat"}, {"preamp_model","Preamp"},
                {"input_level","Input"}, {"limiter","Limiter"},
                {"vibrato","Vibrato"}, {"rotary","Rotary"}, {"dj_filter","DJ Filter"},
                {"tremolo","Tremolo"}, {"autowah","Auto-Wah"}, {"texture","Texturizer"},
                {"delay","Delay"}, {"reverb","Reverb"}, {NULL, NULL}
            };
            for (int li = 0; LABEL[li].k; li++)
                if (!strcmp(LABEL[li].k, pk)) return snprintf(buf, buf_len, "%s", LABEL[li].l);
            return snprintf(buf, buf_len, "%s", pk);
        }
        key = pk;
    }

    /* special: bank_a/b/c → return folder name (not numeric index) */
    if (!strcmp(key, "bank_a")) {
        if (m->p_bank_a >= 0 && m->p_bank_a < m->blist.count)
            return snprintf(buf, buf_len, "%s", m->blist.names[m->p_bank_a]);
        return snprintf(buf, buf_len, "(empty)");
    }
    if (!strcmp(key, "bank_b")) {
        if (m->p_bank_b >= 0 && m->p_bank_b < m->blist.count)
            return snprintf(buf, buf_len, "%s", m->blist.names[m->p_bank_b]);
        return snprintf(buf, buf_len, "(empty)");
    }
    /* sample_lib → return library folder name */
    if (!strcmp(key, "sample_lib")) {
        if (m->p_sample_lib >= 0 && m->p_sample_lib < m->lib_list.count)
            return snprintf(buf, buf_len, "%s", m->lib_list.names[m->p_sample_lib]);
        return snprintf(buf, buf_len, "(empty)");
    }

    reg_t reg[R_COUNT]; int n; build_registry(m, reg, &n);
    reg_t *r = NULL;
    for (int i = 0; i < n; i++) if (!strcmp(reg[i].key, key)) { r = &reg[i]; break; }
    if (!r) return -1;

    if (r->type == T_E) {
        int e = *(int *)r->p; if (e < 0 || e >= r->nopt) e = 0;
        return snprintf(buf, buf_len, "%s", r->opt[e]);
    } else if (r->type == T_I) {
        return snprintf(buf, buf_len, "%d", *(int *)r->p);
    }
    return snprintf(buf, buf_len, "%.4f", *(float *)r->p);
}

/* ============================================================================
 * MIDI
 * ============================================================================ */
static void on_midi(void *inst, const uint8_t *msg, int len, int src) {
    mello_t *m = (mello_t *)inst;
    if (!m || !msg || len < 1) return;
    (void)src;
    uint8_t status = msg[0] & 0xF0;
    /* Some MIDI bridges/HALs reject 1-byte system messages; require a
     * valid status nibble (0x80–0xE0) to proceed. */
    if (status < 0x80 || status >= 0xF0) return;

    uint8_t note   = (len > 1) ? msg[1] : 0;
    uint8_t vel    = (len > 2) ? msg[2] : 0;

    int n_eff = (int)note;
    if (n_eff < 0 || n_eff > 127) return;
    static uint32_t now = 1;
    now++;

    if (status == 0x90 && vel > 0) {
        pthread_mutex_lock(&m->bank_lock);
        /* Same-note retrigger guard: if a voice is already playing this
         * midi_note (e.g. previous note-off was dropped by USB MIDI),
         * release it first so we don't accumulate phantom stuck voices.
         * This is the #1 cause of "stuck notes when playing with a MIDI
         * keyboard" symptoms. */
        release_note(&m->a, n_eff, m);
        release_note(&m->b, n_eff, m);
        trigger_note(&m->a, m, n_eff, vel, now);
        trigger_note(&m->b, m, n_eff, vel, now);
        pthread_mutex_unlock(&m->bank_lock);
    } else if (status == 0x80 || (status == 0x90 && vel == 0)) {
        pthread_mutex_lock(&m->bank_lock);
        release_note(&m->a, n_eff, m);
        release_note(&m->b, n_eff, m);
        pthread_mutex_unlock(&m->bank_lock);
    } else if (status == 0xA0) {
        /* polyphonic aftertouch — map pressure to flutter add-on */
        m->at_flutter = (float)vel / 127.0f;
    } else if (status == 0xD0) {
        /* channel aftertouch — value sits in note byte (only 2 bytes) */
        m->at_flutter = (float)note / 127.0f;
    } else if (status == 0xE0) {
        /* Pitch Bend — 14-bit LSB then MSB, centered at 0x2000. Mapped
         * to ±2 semitones (standard default range).  Stored as float,
         * smoothed per block, added to the per-sample rate calc. */
        if (len >= 3) {
            int lsb = msg[1] & 0x7F;
            int msb = msg[2] & 0x7F;
            int raw = (msb << 7) | lsb;            /* 0..16383, 8192=center */
            float bend = ((float)raw - 8192.0f) * (1.0f / 8192.0f);  /* -1..+1 */
            m->p_pitch_bend_semi = bend * 2.0f;     /* ±2 semitones */
        }
    } else if (status == 0xB0) {
        /* Control Change — mod wheel + panic CCs */
        if (len >= 3) {
            int cc  = msg[1] & 0x7F;
            int val = msg[2] & 0x7F;
            if (cc == 1) {
                /* Mod wheel — classic Mellotron mapping: wheel adds to
                 * Vibrato amount.  Up to +1.0 contribution; the receiving
                 * FX clamps the sum so going past 1.0 doesn't break. */
                m->p_mod_wheel = (float)val / 127.0f;
            }
            /* CC 120 = All Sound Off (kill voices instantly).
             * CC 123 = All Notes Off (release everything gracefully).
             * Both standard MIDI spec — keyboards send these as "panic". */
            else if (cc == 120 || cc == 123) {
                pthread_mutex_lock(&m->bank_lock);
                for (int i = 0; i < NUM_VOICES; i++) {
                    if (m->a.voices[i].active) env_release(&m->a.voices[i], m);
                    if (m->b.voices[i].active) env_release(&m->b.voices[i], m);
                }
                pthread_mutex_unlock(&m->bank_lock);
            }
        }
    }
}

/* ============================================================================
 * render_block — the audio core
 * ============================================================================ */
static void smooth_param(float *sm, float target, float coeff) {
    *sm += coeff * (target - *sm);
}

static void render_block(void *inst, int16_t *out_lr, int frames) {
    mello_t *m = (mello_t *)inst;
    if (!m || !out_lr || frames <= 0) { if (out_lr) memset(out_lr, 0, sizeof(int16_t) * 2 * (size_t)frames); return; }
    if (frames > MAX_BLOCK) frames = MAX_BLOCK;

    /* 20 ms-ish smoothing per block */
    const float SC = 0.10f;
    smooth_param(&m->sm_ab_mix,    m->p_ab_mix,    SC);
    smooth_param(&m->sm_tone,      m->p_tone,      SC);
    smooth_param(&m->sm_crunch,    m->p_crunch,    SC);
    smooth_param(&m->sm_volume,    m->p_volume,    SC);
    smooth_param(&m->sm_pitch_semi,(float)m->p_pitch, SC);
    /* MIDI pitch bend smoothed too — keeps wheel wiggles click-free. */
    smooth_param(&m->sm_pitch_bend_semi, m->p_pitch_bend_semi, SC);
    /* Mod wheel smoothed for click-free wheel sweeps */
    smooth_param(&m->sm_mod_wheel,       m->p_mod_wheel,        SC);
    smooth_param(&m->sm_warble,    m->p_warble,    SC);
    smooth_param(&m->sm_flutter,   m->p_flutter,   SC);
    smooth_param(&m->sm_degrade,   m->p_degrade,   SC);
    smooth_param(&m->sm_tape_sat,  m->p_tape_sat,  SC);
    smooth_param(&m->sm_input,     m->p_input_level, SC);
    smooth_param(&m->sm_lim,       m->p_limiter,   SC);
    smooth_param(&m->sm_vibrato,   m->p_vibrato,   SC);
    smooth_param(&m->sm_leslie,    m->p_leslie,    SC);
    smooth_param(&m->sm_dj_filter, m->p_dj_filter, SC);
    smooth_param(&m->sm_tremolo,   m->p_tremolo,   SC);
    smooth_param(&m->sm_autowah,   m->p_autowah,   SC);
    smooth_param(&m->sm_texture,   m->p_texture,   SC);
    smooth_param(&m->sm_delay,     m->p_delay,     SC);
    smooth_param(&m->sm_reverb,    m->p_reverb,    SC);
    smooth_param(&m->sm_bias,      m->p_bias,      SC);
    smooth_param(&m->sm_mech,      m->p_mech,      SC);
    smooth_param(&m->sm_chiff,     m->p_chiff,     SC);

    /* aftertouch flutter naturally decays back toward 0 (pressure release) */
    m->at_flutter *= 0.97f;
    if (m->at_flutter < 0.001f) m->at_flutter = 0.0f;

    /* slow instability random walk (~0.05 Hz update) — bipolar -1..+1 */
    m->instab_rng_step += (uint32_t)frames;
    if (m->instab_rng_step > (uint32_t)(SR * 0.20f)) {
        m->instab_rng_step = 0;
        m->instab_target = randf_pm(&m->fx.rng);
    }
    m->instab_state += 0.02f * (m->instab_target - m->instab_state) + 1e-25f;

    /* per-block wow LFO sample (slow ~0.4 Hz) */
    m->fx.wow_phase += 0.4f * (float)frames * SR_INV;
    if (m->fx.wow_phase > 1.0f) m->fx.wow_phase -= 1.0f;
    float wow_lfo = sinf(m->fx.wow_phase * TWO_PI);

    /* per-sample flutter LFO buffer (fast 6.5 Hz + noise) */
    float flutter_buf[MAX_BLOCK];
    for (int i = 0; i < frames; i++) {
        m->fx.flutter_phase += 6.5f * SR_INV;
        if (m->fx.flutter_phase > 1.0f) m->fx.flutter_phase -= 1.0f;
        float fl = sinf(m->fx.flutter_phase * TWO_PI);
        float ns = randf_pm(&m->fx.rng) * 0.3f;
        flutter_buf[i] = fl * 0.7f + ns;
    }

    /* If a background bank load has finished, swap it in before rendering.
     * Non-blocking when there's no pending bank. */
    check_and_swap_pending(m, &m->a);
    check_and_swap_pending(m, &m->b);

    /* render both bank slots into float mono buffers */
    float buf_a[MAX_BLOCK], buf_b[MAX_BLOCK];
    pthread_mutex_lock(&m->bank_lock);
    render_bank_slot(m, &m->a, buf_a, frames, wow_lfo, flutter_buf);
    render_bank_slot(m, &m->b, buf_b, frames, wow_lfo, flutter_buf);
    pthread_mutex_unlock(&m->bank_lock);

    /* equal-power A/B mix → stereo */
    float l_buf[MAX_BLOCK], r_buf[MAX_BLOCK];
    float mix = m->sm_ab_mix;
    float ga  = cosf(mix * (float)M_PI * 0.5f);
    float gb  = sinf(mix * (float)M_PI * 0.5f);
    for (int i = 0; i < frames; i++) {
        float v = buf_a[i] * ga + buf_b[i] * gb;
        l_buf[i] = v; r_buf[i] = v;
    }

    /* chiff (tape-retract) pool — mixed in pre-tape */
    for (int p = 0; p < CHIFF_POOL; p++) {
        chiff_voice_t *cv = &m->chiff_pool[p];
        if (!cv->active) continue;
        for (int i = 0; i < frames; i++) {
            float n = randf_pm(&m->fx.rng);
            cv->lp += 0.18f * (n - cv->lp) + 1e-25f;   /* ~2 kHz LP, denormal-guarded */
            float s = cv->lp * cv->env * m->sm_chiff * 0.35f;
            l_buf[i] += s; r_buf[i] += s;
            cv->env -= cv->dec;
            if (cv->env <= 0.0f) { cv->env = 0.0f; cv->active = 0; break; }
        }
    }

    /* mechanical transport noise — sub-300 Hz rumble + occasional click */
    if (m->sm_mech > 0.001f) {
        for (int i = 0; i < frames; i++) {
            m->mech_phase += 80.0f * SR_INV;
            if (m->mech_phase > 1.0f) m->mech_phase -= 1.0f;
            float rumble = sinf(m->mech_phase * TWO_PI) * 0.4f
                         + sinf(m->mech_phase * TWO_PI * 1.7f) * 0.2f;
            rumble *= (0.7f + 0.3f * randf_pm(&m->fx.rng));
            /* click */
            float click = 0.0f;
            uint32_t r1 = xorshift32(&m->fx.rng);
            float prob = m->sm_mech * 0.00015f;
            if (((float)(r1 >> 8) * (1.0f / 16777216.0f)) < prob) {
                click = randf_pm(&m->fx.rng) * 0.5f;
            }
            float n = (rumble * 0.015f + click * 0.12f) * m->sm_mech;
            l_buf[i] += n; r_buf[i] += n;
        }
    }

    /* tape stage */
    apply_tape_stage(m, l_buf, r_buf, frames);

    /* preamp (Crunch drives the model curve) */
    /* Preamp runs whenever a non-Off model is selected (so the model's own
     * baseline character is heard even at Crunch=0). Model 0 = Off = full
     * bypass — skip the loop entirely. */
    if (m->p_preamp_model > 0) {
        for (int i = 0; i < frames; i++) {
            l_buf[i] = preamp_sample(m->p_preamp_model, l_buf[i], m->sm_crunch);
            r_buf[i] = preamp_sample(m->p_preamp_model, r_buf[i], m->sm_crunch);
        }
    }

    /* tone tilt EQ */
    if (fabsf(m->sm_tone - 0.5f) > 0.005f) {
        for (int i = 0; i < frames; i++) {
            l_buf[i] = tone_tilt_l(m, l_buf[i]);
            r_buf[i] = tone_tilt_r(m, r_buf[i]);
        }
    }

    /* FX chain — routing order chosen by the FX-page `fx_chain` menu.
     * Every effect always RUNS (always-on state, keeps no-click behaviour);
     * the order changes which one tints which.  Each preset is
     * meaningfully distinct so an ear-test reveals the difference. */
    switch (m->p_fx_chain) {
    case 1: /* Pedalboard — wah opens, then the full mod stack (vibrato,
             *              tremolo, leslie), grain texture, then EQ
             *              filter, finally delay+reverb */
        fx_autowah  (m, l_buf, r_buf, frames);
        fx_vibrato  (m, l_buf, r_buf, frames);
        fx_tremolo  (m, l_buf, r_buf, frames);
        fx_leslie   (m, l_buf, r_buf, frames);
        fx_texture  (m, l_buf, r_buf, frames);
        fx_dj_filter(m, l_buf, r_buf, frames);
        fx_delay    (m, l_buf, r_buf, frames);
        fx_reverb   (m, l_buf, r_buf, frames);
        break;
    case 2: /* Wet Swirl — time early, then mod sweeps the wet tail */
        fx_texture  (m, l_buf, r_buf, frames);
        fx_delay    (m, l_buf, r_buf, frames);
        fx_reverb   (m, l_buf, r_buf, frames);
        fx_vibrato  (m, l_buf, r_buf, frames);
        fx_leslie   (m, l_buf, r_buf, frames);
        fx_autowah  (m, l_buf, r_buf, frames);
        fx_dj_filter(m, l_buf, r_buf, frames);
        fx_tremolo  (m, l_buf, r_buf, frames);
        break;
    case 3: /* Ambient Wash — delay+reverb up front, everything else colours
             *                the wash (radical: cathedral that's also a
             *                Leslie cabinet and a wah pedal) */
        fx_delay    (m, l_buf, r_buf, frames);
        fx_reverb   (m, l_buf, r_buf, frames);
        fx_texture  (m, l_buf, r_buf, frames);
        fx_vibrato  (m, l_buf, r_buf, frames);
        fx_leslie   (m, l_buf, r_buf, frames);
        fx_autowah  (m, l_buf, r_buf, frames);
        fx_dj_filter(m, l_buf, r_buf, frames);
        fx_tremolo  (m, l_buf, r_buf, frames);
        break;
    case 4: /* Dub — dub-console workflow.  DJ filter carves the dry signal
             *      first (HP-sweep an organ note for that filtered-stab
             *      effect), then delay+reverb generate the wet field, and
             *      modulation breathes life into the late tail.  Texture
             *      sits last so any granular shimmer feeds off the
             *      filtered-and-delayed signal. */
        fx_dj_filter(m, l_buf, r_buf, frames);
        fx_delay    (m, l_buf, r_buf, frames);
        fx_reverb   (m, l_buf, r_buf, frames);
        fx_tremolo  (m, l_buf, r_buf, frames);
        fx_vibrato  (m, l_buf, r_buf, frames);
        fx_leslie   (m, l_buf, r_buf, frames);
        fx_autowah  (m, l_buf, r_buf, frames);
        fx_texture  (m, l_buf, r_buf, frames);
        break;
    case 5: /* Reverse — reverb FIRST so the dry note is instantly soaked
             *          in ambience, then delay echoes that ambient blob,
             *          autowah + tremolo + filter dynamically sculpt the
             *          wet field, leslie+vibrato add rotation, and the
             *          texture grain-cloud at the very END chops the
             *          fully-effected signal into freeze/shimmer.  Best
             *          for "preserved-in-amber" Mellotron pads. */
        fx_reverb   (m, l_buf, r_buf, frames);
        fx_delay    (m, l_buf, r_buf, frames);
        fx_autowah  (m, l_buf, r_buf, frames);
        fx_tremolo  (m, l_buf, r_buf, frames);
        fx_dj_filter(m, l_buf, r_buf, frames);
        fx_leslie   (m, l_buf, r_buf, frames);
        fx_vibrato  (m, l_buf, r_buf, frames);
        fx_texture  (m, l_buf, r_buf, frames);
        break;
    case 0: /* Classic — Texturizer FIRST so the granular cloud is coloured
             *           by all the modulation/filter/time-based FX that
             *           follow.  This is the v0.1.14 default. */
    default:
        fx_texture  (m, l_buf, r_buf, frames);
        fx_vibrato  (m, l_buf, r_buf, frames);
        fx_leslie   (m, l_buf, r_buf, frames);
        fx_dj_filter(m, l_buf, r_buf, frames);
        fx_tremolo  (m, l_buf, r_buf, frames);
        fx_autowah  (m, l_buf, r_buf, frames);
        fx_delay    (m, l_buf, r_buf, frames);
        fx_reverb   (m, l_buf, r_buf, frames);
        break;
    }

    /* limiter */
    apply_limiter(m, l_buf, r_buf, frames);

    /* volume + soft-clip + int16 stereo write. Soft tanh saturation at the
     * output prevents the brittle hard-clip "distortion" that hit the moment
     * signal × volume exceeded 1.0. tanh knee at 0.85 stays linear in the
     * useful range and saturates gracefully above. */
    float vol = m->sm_volume;
    for (int i = 0; i < frames; i++) {
        float l = l_buf[i] * vol;
        float r = r_buf[i] * vol;
        l = fast_tanh(l * 1.1f) * 0.91f;   /* knee around ±0.9, max ±0.91 */
        r = fast_tanh(r * 1.1f) * 0.91f;
        out_lr[i * 2]     = (int16_t)(l * 32767.0f);
        out_lr[i * 2 + 1] = (int16_t)(r * 32767.0f);
    }
}

/* ============================================================================
 * Lifecycle
 * ============================================================================ */
static void detect_instruments_dir(mello_t *m, const char *id) {
    /* Try the two known install bases; pick whichever exists.  This now
     * resolves the library ROOT — the directory whose immediate children are
     * library folders (Leisureland, Sonic Bloom, …).  `instruments_dir` is
     * filled in later by activate_sample_library() once we know which
     * library to use. */
    const char *bases[] = {
        "/data/UserData/schwung/modules/sound_generators",
        "/data/UserData/move-anything/modules/sound_generators",
        NULL
    };
    for (int i = 0; bases[i]; i++) {
        snprintf(m->instruments_root, sizeof(m->instruments_root),
                 "%s/%s/instruments", bases[i], id ? id : "mello");
        struct stat st;
        if (stat(m->instruments_root, &st) == 0 && (st.st_mode & S_IFDIR)) return;
    }
    /* fallback: relative path (development / desktop test) */
    snprintf(m->instruments_root, sizeof(m->instruments_root), "%s",
             INSTRUMENTS_BASE_DEFAULT);
}

/* Build the flattened union bank list across all libraries.  Each bank
 * shows up as "Library/Bank" so the chain_params enum lists every bank
 * regardless of which library is "active".  Per-library start indices are
 * cached so sample_lib selection can quick-jump to that library's first
 * bank.  Called once at instance start. */
static void scan_all_libraries(mello_t *m) {
    memset(&m->blist, 0, sizeof(m->blist));
    m->lib_first_idx[0] = 0;
    for (int L = 0; L < m->lib_list.count && m->blist.count < MELLO_MAX_BANKS; L++) {
        char lib_dir[768];
        snprintf(lib_dir, sizeof(lib_dir), "%s/%s",
                 m->instruments_root, m->lib_list.names[L]);
        mello_bank_list_t one = {0};
        mello_list_banks(lib_dir, &one);
        for (int b = 0; b < one.count && m->blist.count < MELLO_MAX_BANKS; b++) {
            /* Prefix the bank name with its library so the host's enum
             * shows the grouping at a glance. */
            snprintf(m->blist.names[m->blist.count], MELLO_BANK_NAME_MAX,
                     "%s/%s", m->lib_list.names[L], one.names[b]);
            m->blist.count++;
        }
        m->lib_first_idx[L + 1] = m->blist.count;
    }
}

/* sample_lib quick-jump: stash the library selection and return the union
 * index of that library's first bank, so callers can route bank_a / bank_b
 * there.  The blist itself does NOT get re-filtered (the host caches enum
 * options — we'd have no way to refresh them).  Returns -1 on bad idx. */
static int activate_sample_library(mello_t *m, int idx) {
    if (idx < 0 || idx >= m->lib_list.count) return -1;
    m->p_sample_lib = idx;
    /* Reflect the active library in instruments_dir too for cleanliness;
     * it isn't used to resolve bank paths anymore (load_bank_slot now
     * builds the path from instruments_root + the prefixed blist name). */
    snprintf(m->instruments_dir, sizeof(m->instruments_dir),
             "%s/%s", m->instruments_root, m->lib_list.names[idx]);
    return m->lib_first_idx[idx];
}

static void *create_instance(const char *id, const char *cfg) {
    (void)cfg;
    mello_t *m = (mello_t *)calloc(1, sizeof(mello_t));
    if (!m) return NULL;
    m->sr = SR;
    m->current_page = 0;

    /* defaults (mirror module.json) */
    m->p_bank_a = 0; m->p_bank_b = 0; m->p_pitch = 0;
    m->p_ab_mix = 0.0f; m->p_tone = 0.5f; m->p_crunch = 0.0f; m->p_volume = 0.5f;
    m->p_env_preset = 0;
    apply_env_preset(m, 0);
    /* Defaults: tape signal-shaping Off (samples were already recorded to
     * tape — doubling sounded like over-tape-d mush). Preamp = M400 SS as
     * the most "musical" preamp out of the box. Wow/flutter/degrade still
     * default to 0 so the static character is fully bypassed too. */
    m->p_tape_style = 0; m->p_preamp_model = 3;
    m->p_input_level = 0.5f; m->p_limiter = 0.3f;
    m->p_tape_len = 8.0f; m->p_chiff = 0.0f;
    m->p_dj_filter = 0.5f;
    m->p_map_mode = 0; m->p_loop_mode = 1; m->p_half_speed = 0;
    m->p_xfade_pct = 50;          /* ~60 ms crossfade — glitch-free default */
    m->p_auto_retune = 0;   /* OFF by default — user opts in */
    m->p_seq_base = 40;  /* Leisureland M400 default */
    m->p_reverse = 0; m->p_sample_start = 0.0f;
    m->p_fx_chain = 0;   /* "Classic" — preserve v0.1.14 default routing */
    m->p_bias = 0.5f; m->p_mech = 0.0f;
    m->sm_bias = m->p_bias; m->sm_mech = m->p_mech;
    m->sm_chiff = m->p_chiff;
    m->instab_rng_step = 0;

    /* smoothed init */
    m->sm_ab_mix = m->p_ab_mix; m->sm_tone = m->p_tone;
    m->sm_crunch = m->p_crunch; m->sm_volume = m->p_volume;
    m->sm_pitch_semi = (float)m->p_pitch;
    m->sm_dj_filter = m->p_dj_filter;
    m->sm_input = m->p_input_level; m->sm_lim = m->p_limiter;

    m->a.loaded_idx = -1; m->a.requested_idx = -1;
    m->b.loaded_idx = -1; m->b.requested_idx = -1;
    m->a.bank.lo_note = -1; m->a.bank.hi_note = -1;
    m->b.bank.lo_note = -1; m->b.bank.hi_note = -1;
    pthread_mutex_init(&m->bank_lock, NULL);

    /* Per-slot async-loader scaffolding. The workers sleep on loader_cv until
     * set_param posts a request via request_bank_load_async(). */
    m->a.m_ref = m;  m->b.m_ref = m;
    m->a.pending_ready = 0;  m->b.pending_ready = 0;
    m->a.loader_run    = 1;  m->b.loader_run    = 1;
    pthread_mutex_init(&m->a.loader_mtx, NULL);
    pthread_mutex_init(&m->b.loader_mtx, NULL);
    pthread_cond_init (&m->a.loader_cv,  NULL);
    pthread_cond_init (&m->b.loader_cv,  NULL);
    pthread_create(&m->a.loader_tid, NULL, bank_loader_thread, &m->a);
    pthread_create(&m->b.loader_tid, NULL, bank_loader_thread, &m->b);

    m->fx.rng = 0xC0FFEE17u;
    m->fx.prev_dj_mode = 0;
    m->lez.horn_rate = 1.0f; m->lez.drum_rate = 0.7f;

    /* Resolve library root, list libraries, build the flattened union bank
     * list (every library's banks concatenated with "Library/Bank" prefix).
     * Then pick a default library — prefer Leisureland if present so the
     * v0.1.x factory behaviour is preserved. */
    detect_instruments_dir(m, id);
    mello_list_banks(m->instruments_root, &m->lib_list);
    scan_all_libraries(m);

    /* Boot bank preference: SB/MK2 Flute first (Sonic Bloom's MK2 Flute
     * sounds notably better than the Leisureland version, so it's the
     * sonic anchor).  Fall back to the first bank in any library that
     * exists if SB isn't installed. */
    int default_bank = 0;
    for (int i = 0; i < m->blist.count; i++) {
        if (!strcmp(m->blist.names[i], "SB/MK2 Flute")) { default_bank = i; break; }
    }
    m->p_bank_a = default_bank;
    m->p_bank_b = default_bank;

    /* Samples Folder preference: try "MT" (full MellowTrawn / Leisureland
     * archive, 129 banks) first, then "LL" (legacy Leisureland subset,
     * 34 banks — kept as a recognised name so anyone who organised their
     * own Leisureland folder pre-v0.1.2 keeps working).  Whichever wins
     * is overridden below so the displayed Samples Folder matches the
     * actual default bank's library — keeps the UI consistent (no
     * "folder says MT, bank is from SB" confusion). */
    int default_lib = 0;
    static const char *LIB_PREFERENCE[] = { "MT", "LL", NULL };
    int found = 0;
    for (int p = 0; LIB_PREFERENCE[p] && !found; p++) {
        for (int i = 0; i < m->lib_list.count; i++) {
            if (!strcmp(m->lib_list.names[i], LIB_PREFERENCE[p])) {
                default_lib = i; found = 1; break;
            }
        }
    }
    /* If a real default bank was picked (e.g. SB/MK2 Flute), point the
     * Samples Folder param at that bank's owning library so the menu
     * label and the playing bank agree. */
    for (int i = 0; i < m->lib_list.count; i++) {
        int lo = m->lib_first_idx[i];
        int hi = (i + 1 < (int)(sizeof(m->lib_first_idx)/sizeof(int)))
                 ? m->lib_first_idx[i + 1]
                 : m->blist.count;
        if (default_bank >= lo && default_bank < hi) { default_lib = i; break; }
    }
    activate_sample_library(m, default_lib);

    /* preload bank A (and B if different from A) — SYNCHRONOUS for the
     * initial load so the first note has samples ready. */
    if (m->blist.count > 0) {
        /* Load the *chosen* default bank (set above to SB/MK2 Flute when
         * present).  Previously this was hard-coded to index 0, which meant
         * the param value displayed correctly but the actual WAVs loaded
         * were whatever the alphabetically-first union entry happened to
         * be (e.g. 90/8VOICE CHOIR), producing a "param says Mkii-Flute
         * but it sounds like a choir" disconnect. */
        load_bank_slot(m, &m->a, m->p_bank_a);
        load_bank_slot(m, &m->b, m->p_bank_b);
    }

    return m;
}

static void destroy_instance(void *inst) {
    mello_t *m = (mello_t *)inst;
    if (!m) return;

    /* Tell the worker threads to exit, then join them. */
    pthread_mutex_lock(&m->a.loader_mtx);
    m->a.loader_run = 0;
    pthread_cond_signal(&m->a.loader_cv);
    pthread_mutex_unlock(&m->a.loader_mtx);
    pthread_mutex_lock(&m->b.loader_mtx);
    m->b.loader_run = 0;
    pthread_cond_signal(&m->b.loader_cv);
    pthread_mutex_unlock(&m->b.loader_mtx);
    pthread_join(m->a.loader_tid, NULL);
    pthread_join(m->b.loader_tid, NULL);

    /* Drain any pending-but-unconsumed banks so we don't leak. */
    if (m->a.pending_ready) mello_bank_free(&m->a.pending_bank);
    if (m->b.pending_ready) mello_bank_free(&m->b.pending_bank);

    mello_bank_free(&m->a.bank);
    mello_bank_free(&m->b.bank);
    pthread_mutex_destroy(&m->a.loader_mtx);
    pthread_mutex_destroy(&m->b.loader_mtx);
    pthread_cond_destroy(&m->a.loader_cv);
    pthread_cond_destroy(&m->b.loader_cv);
    pthread_mutex_destroy(&m->bank_lock);
    free(m);
}

/* ============================================================================
 * Entry
 * ============================================================================ */
plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host) {
    g_host = host;
    static plugin_api_v2_t api = {
        2,
        create_instance,
        destroy_instance,
        on_midi,
        set_param,
        get_param,
        NULL,           /* get_error */
        render_block,
    };
    return &api;
}
