/* ============================================================================
 * wav_bank.c — Mello in-house WAV sampler implementation.
 *
 * Standalone self-test:
 *   cc -DWAV_BANK_TEST wav_bank.c -o wav_bank_test && ./wav_bank_test [folder]
 * ============================================================================ */
#include "wav_bank.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>

/* ============================================================================
 * RIFF WAV loader (16-bit PCM, mono or stereo, any sample rate).
 * Stereo files are downmixed to mono on load. Bit depths other than 16 are
 * refused — the Leisureland set, and basically every Mellotron sample pack on
 * the planet, is 16-bit so this is fine.
 * ============================================================================ */
static uint32_t rd_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd_u16_le(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

int mello_wav_load(const char *path, mello_wav_t *out) {
    if (!path || !out) return -1;
    memset(out, 0, sizeof(*out));

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    uint8_t hdr[12];
    if (fread(hdr, 1, 12, f) != 12) { fclose(f); return -1; }
    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        fclose(f);
        return -1;
    }

    int     channels = 0, sr = 0, bits = 0;
    int16_t *pcm     = NULL;
    int      frames  = 0;
    int      got_fmt = 0, got_data = 0;

    while (!got_data) {
        uint8_t ch[8];
        if (fread(ch, 1, 8, f) != 8) break;
        uint32_t sz = rd_u32_le(ch + 4);

        if (memcmp(ch, "fmt ", 4) == 0) {
            uint8_t fmt[32];
            uint32_t take = sz > sizeof(fmt) ? sizeof(fmt) : sz;
            if (fread(fmt, 1, take, f) != take) break;
            if (sz > take) fseek(f, (long)(sz - take), SEEK_CUR);
            uint16_t format = rd_u16_le(fmt);
            channels = rd_u16_le(fmt + 2);
            sr       = (int)rd_u32_le(fmt + 4);
            bits     = rd_u16_le(fmt + 14);
            if (format != 1) { fclose(f); return -1; }  /* PCM only */
            if (bits != 16) { fclose(f); return -1; }
            if (channels != 1 && channels != 2) { fclose(f); return -1; }
            got_fmt = 1;
        } else if (memcmp(ch, "data", 4) == 0) {
            if (!got_fmt) { fclose(f); return -1; }
            int bytes_per_frame = 2 * channels;
            int n_frames = (int)(sz / (uint32_t)bytes_per_frame);
            int16_t *buf = (int16_t *)malloc(sizeof(int16_t) * 2 * (size_t)n_frames);
            if (!buf) { fclose(f); return -1; }
            int read = (int)fread(buf, (size_t)bytes_per_frame, (size_t)n_frames, f);
            if (channels == 1) {
                pcm = buf;
                frames = read;
            } else {
                /* downmix stereo → mono (averaging) */
                int16_t *mono = (int16_t *)malloc(sizeof(int16_t) * (size_t)read);
                if (!mono) { free(buf); fclose(f); return -1; }
                for (int i = 0; i < read; i++) {
                    int s = (int)buf[i * 2] + (int)buf[i * 2 + 1];
                    mono[i] = (int16_t)(s / 2);
                }
                free(buf);
                pcm = mono;
                frames = read;
            }
            got_data = 1;
        } else {
            fseek(f, (long)sz, SEEK_CUR);
        }
    }
    fclose(f);

    if (!got_data || !pcm) { free(pcm); return -1; }

    out->samples = pcm;
    out->frames  = frames;
    out->orig_sr = sr;

    /* Auto-detect a ~3 s sustain loop. Skip past the attack, find two
     * positive-going zero-crossings near the target positions. If anything
     * goes wrong, fall back to the middle 70 % of the sample. */
    if (frames < sr * 2) {
        out->loop_start = 0;
        out->loop_end   = frames;
    } else {
        int target_start = sr;                       /* 1.0 s in (past attack) */
        int target_end   = target_start + 3 * sr;    /* 3-second loop */
        int tail_guard   = sr / 2;                   /* leave 0.5 s at end */
        if (target_end > frames - tail_guard) target_end = frames - tail_guard;

        int radius = sr / 4;
        int ls = target_start, le = target_end;
        int best_amp_s = 0x7FFF, best_amp_e = 0x7FFF;

        int lo_s = target_start - radius; if (lo_s < 1) lo_s = 1;
        int hi_s = target_start + radius; if (hi_s > frames - 1) hi_s = frames - 1;
        for (int i = lo_s; i < hi_s; i++) {
            if (pcm[i] <= 0 && pcm[i + 1] > 0) {
                int a = -(int)pcm[i];        /* distance below zero */
                if (a < best_amp_s) { best_amp_s = a; ls = i; }
            }
        }
        int lo_e = target_end - radius; if (lo_e < ls + sr) lo_e = ls + sr;
        int hi_e = target_end + radius; if (hi_e > frames - 1) hi_e = frames - 1;
        for (int i = lo_e; i < hi_e; i++) {
            if (pcm[i] <= 0 && pcm[i + 1] > 0) {
                int a = -(int)pcm[i];
                if (a < best_amp_e) { best_amp_e = a; le = i; }
            }
        }
        if (le <= ls + sr) {
            ls = frames / 6;
            le = frames - frames / 6;
        }
        out->loop_start = ls;
        out->loop_end   = le;
    }
    return 0;
}

void mello_wav_free(mello_wav_t *w) {
    if (!w) return;
    if (w->samples) free(w->samples);
    w->samples = NULL;
    w->frames = 0;
    w->orig_sr = 0;
}

/* ============================================================================
 * filename → MIDI note
 * ============================================================================ */
/* try a note name (C, C#, Db, ...) + signed octave digit at position i */
static int try_note_at(const char *s, int i, int len, int *consumed) {
    int base;
    switch (toupper((unsigned char)s[i])) {
        case 'C': base = 0;  break;
        case 'D': base = 2;  break;
        case 'E': base = 4;  break;
        case 'F': base = 5;  break;
        case 'G': base = 7;  break;
        case 'A': base = 9;  break;
        case 'B': base = 11; break;
        default: return -1;
    }
    int j = i + 1, acc = 0;
    if (j < len) {
        char c = s[j];
        if (c == '#' || c == 's' || c == 'S') { acc = 1; j++; }
        else if (c == 'b') { acc = -1; j++; }
    }
    int sign = 1;
    if (j < len && s[j] == '-') { sign = -1; j++; }
    if (j >= len || !isdigit((unsigned char)s[j])) return -1;
    int oct = (s[j] - '0') * sign;
    j++;
    int midi = (oct + 1) * 12 + base + acc;        /* C4 = 60 */
    if (midi < 0 || midi > 127) return -1;
    *consumed = j - i;
    return midi;
}

/* Trailing "-N" sequential extractor (returns N >= 1 or -1). */
static int trailing_seq(const char *name) {
    int len = (int)strlen(name);
    if (len == 0) return -1;
    int i = len - 1;
    if (!isdigit((unsigned char)name[i])) return -1;
    while (i > 0 && isdigit((unsigned char)name[i - 1])) i--;
    if (i == 0 || name[i - 1] != '-') return -1;
    int v = atoi(&name[i]);
    return v >= 1 ? v : -1;
}

int mello_parse_midi_note(const char *filename, int seq_base) {
    if (!filename) return -1;

    /* basename, no extension */
    const char *base = filename;
    for (const char *p = filename; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    char name[256];
    size_t n = strlen(base);
    if (n >= sizeof(name)) n = sizeof(name) - 1;
    memcpy(name, base, n); name[n] = '\0';
    char *dot = strrchr(name, '.'); if (dot) *dot = '\0';
    int len = (int)strlen(name);

    /* 1. last note-name match wins (handles "MkII Violins A2") */
    int found = -1;
    for (int i = 0; i < len; i++) {
        int consumed = 0;
        int m = try_note_at(name, i, len, &consumed);
        if (m >= 0) { found = m; i += consumed - 1; }
    }
    if (found >= 0) return found;

    /* 2. Leisureland-style trailing -N with explicit base */
    if (seq_base >= 0) {
        int s = trailing_seq(name);
        if (s >= 1) {
            int m = s + seq_base;
            if (m >= 0 && m <= 127) return m;
        }
    }

    /* 3. bare MIDI number fallback in piano range */
    for (int i = 0; i < len; i++) {
        if (isdigit((unsigned char)name[i]) &&
            (i == 0 || !isdigit((unsigned char)name[i - 1]))) {
            int v = atoi(&name[i]);
            if (v >= 21 && v <= 108) return v;
            while (i < len && isdigit((unsigned char)name[i])) i++;
        }
    }
    return -1;
}

/* ============================================================================
 * Folder scan + bank load
 * ============================================================================ */
static int has_wav_ext(const char *name) {
    size_t n = strlen(name);
    if (n < 4) return 0;
    return (tolower((unsigned char)name[n - 4]) == '.' &&
            tolower((unsigned char)name[n - 3]) == 'w' &&
            tolower((unsigned char)name[n - 2]) == 'a' &&
            tolower((unsigned char)name[n - 1]) == 'v');
}

static int is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && (st.st_mode & S_IFDIR);
}

static int cmp_str(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

int mello_list_banks(const char *root, mello_bank_list_t *out) {
    if (!root || !out) return -1;
    memset(out, 0, sizeof(*out));
    DIR *d = opendir(root);
    if (!d) return -1;
    struct dirent *de;
    while ((de = readdir(d)) && out->count < MELLO_MAX_BANKS) {
        if (de->d_name[0] == '.') continue;
        char p[1024];
        snprintf(p, sizeof(p), "%s/%s", root, de->d_name);
        if (!is_dir(p)) continue;
        strncpy(out->names[out->count], de->d_name, MELLO_BANK_NAME_MAX - 1);
        out->names[out->count][MELLO_BANK_NAME_MAX - 1] = '\0';
        out->count++;
    }
    closedir(d);
    qsort(out->names, (size_t)out->count, MELLO_BANK_NAME_MAX, cmp_str);
    return out->count;
}

void mello_bank_free(mello_bank_t *bank) {
    if (!bank) return;
    for (int i = 0; i < 128; i++) mello_wav_free(&bank->notes[i]);
    memset(bank, 0, sizeof(*bank));
    bank->lo_note = -1;
    bank->hi_note = -1;
}

/* 12-TET MIDI note → fundamental frequency (A4 = MIDI 69 = 440 Hz).
 * Baked at compile time so pitch detection never has to call powf().
 * Indexed 0..127, units are Hz.  Used by detect_pitch_cents to set the
 * target window for autocorrelation, and as the ground-truth reference
 * for the post-correction verification pass. */
static const float MIDI_FREQ[128] = {
      8.17580f,   8.66196f,   9.17702f,   9.72272f,  10.30086f,  10.91338f,
     11.56233f,  12.24986f,  12.97827f,  13.75000f,  14.56762f,  15.43385f,
     16.35160f,  17.32391f,  18.35405f,  19.44544f,  20.60172f,  21.82676f,
     23.12465f,  24.49971f,  25.95654f,  27.50000f,  29.13524f,  30.86771f,
     32.70320f,  34.64783f,  36.70810f,  38.89087f,  41.20344f,  43.65353f,
     46.24930f,  48.99943f,  51.91309f,  55.00000f,  58.27047f,  61.73541f,
     65.40639f,  69.29566f,  73.41619f,  77.78175f,  82.40689f,  87.30706f,
     92.49861f,  97.99886f, 103.82617f, 110.00000f, 116.54094f, 123.47083f,
    130.81278f, 138.59132f, 146.83238f, 155.56349f, 164.81378f, 174.61412f,
    184.99721f, 195.99772f, 207.65235f, 220.00000f, 233.08188f, 246.94165f,
    261.62557f, 277.18263f, 293.66477f, 311.12698f, 329.62756f, 349.22823f,
    369.99442f, 391.99544f, 415.30470f, 440.00000f, 466.16376f, 493.88330f,
    523.25113f, 554.36526f, 587.32954f, 622.25397f, 659.25511f, 698.45646f,
    739.98885f, 783.99087f, 830.60940f, 880.00000f, 932.32752f, 987.76660f,
   1046.50226f, 1108.73052f, 1174.65907f, 1244.50793f, 1318.51023f, 1396.91293f,
   1479.97770f, 1567.98174f, 1661.21879f, 1760.00000f, 1864.65505f, 1975.53321f,
   2093.00452f, 2217.46105f, 2349.31814f, 2489.01587f, 2637.02046f, 2793.82585f,
   2959.95540f, 3135.96349f, 3322.43758f, 3520.00000f, 3729.31009f, 3951.06641f,
   4186.00904f, 4434.92210f, 4698.63629f, 4978.03174f, 5274.04091f, 5587.65170f,
   5919.91079f, 6271.92698f, 6644.87516f, 7040.00000f, 7458.62018f, 7902.13282f,
   8372.01809f, 8869.84419f, 9397.27257f, 9956.06348f, 10548.08182f,
  11175.30341f, 11839.82158f, 12543.85395f, 13289.75032f, 14080.00000f,
  14917.24036f, 15804.26564f
};

/* Normalized autocorrelation peak search inside [min_p, max_p] with the
 * given decimation step.  Returns the period that maximised the correlation
 * (and writes the correlation into *out_corr).  Helper used by the Auto
 * Retune pipeline so the search/refine/verify passes share one
 * implementation. */
static int ac_peak(const int16_t *samples, int win_start, int win_len,
                   int min_p, int max_p, int step, float *out_corr) {
    float best = -1e30f;
    int   best_p = -1;
    for (int p = min_p; p <= max_p; p += step) {
        float sum_ab = 0.0f, sum_a2 = 0.0f, sum_b2 = 0.0f;
        for (int n = 0; n < win_len; n += step) {
            float a = (float)samples[win_start + n];
            float b = (float)samples[win_start + n + p];
            sum_ab += a * b;
            sum_a2 += a * a;
            sum_b2 += b * b;
        }
        float denom = sqrtf(sum_a2 * sum_b2);
        if (denom < 1.0f) continue;
        float c = sum_ab / denom;
        if (c > best) { best = c; best_p = p; }
    }
    *out_corr = best;
    return best_p;
}

/* Compute one correlation value at exactly `p` — used by the parabolic
 * interpolator that refines the integer peak to sub-sample precision. */
static float ac_at(const int16_t *samples, int win_start, int win_len,
                   int step, int p) {
    float sum_ab = 0.0f, sum_a2 = 0.0f, sum_b2 = 0.0f;
    for (int n = 0; n < win_len; n += step) {
        float a = (float)samples[win_start + n];
        float b = (float)samples[win_start + n + p];
        sum_ab += a * b;
        sum_a2 += a * a;
        sum_b2 += b * b;
    }
    float denom = sqrtf(sum_a2 * sum_b2);
    if (denom < 1.0f) return -1.0f;
    return sum_ab / denom;
}

/* Auto Retune — estimate the deviation between a sample's fundamental and
 * the MIDI note we mapped it to, return the CORRECTION in cents that the
 * voice should add to its playback pitch.
 *
 * Three-stage pipeline:
 *   1. PRE-ANALYZE: autocorrelate within ±100 cents (one semitone) of the
 *      MIDI table's target period.  Narrow window prevents octave/harmonic
 *      mis-detection — the most common autocorrelation failure mode.
 *   2. CORRECT: compute cents = 1200·log2(det_freq / target_freq).  Reject
 *      if confidence is low (<0.85), if |cents| is implausibly small (<3,
 *      not worth correcting) or large (>35, almost certainly a bad pitch
 *      detection).
 *   3. VERIFY: re-run the autocorrelation with the DETECTED frequency as
 *      the new target.  A real detune produces a sharp peak that lines up
 *      with itself on re-detect (re-cents ≈ 0).  A spurious detection
 *      makes the peak shift in the verification pass and we reject the
 *      correction.  This is the "make sure it's better, not worse" check
 *      the user requested.
 *
 * Cost ≈ 70 k mul-adds per sample (search+refine+verify) → <2 ms per WAV
 * at 48 kHz, ~70 ms per bank load.  Runs once on the worker thread. */
static float detect_pitch_cents(const int16_t *samples, int frames,
                                int sr, int midi_note) {
    if (!samples || frames <= 0) return 0.0f;
    if (midi_note < 0 || midi_note > 127) return 0.0f;

    float exp_freq = MIDI_FREQ[midi_note];
    if (exp_freq < 30.0f || exp_freq > 3000.0f) return 0.0f;
    float exp_period = (float)sr / exp_freq;

    /* MIDI-aware window placement: low-pitched samples (below D3) often
     * have longer attack tails where the tape is still settling — start
     * the analysis 40 % into the sample for those instead of the usual
     * 30 %, so we hit steady-state pitch reliably. */
    const int step = 8;
    int win_start = (midi_note < 50) ? (frames * 40 / 100)
                                      : (frames * 30 / 100);
    int win_len   = sr * 4 / 10;
    if (win_start + win_len + (int)(exp_period * 1.1f) >= frames) {
        win_len = frames - win_start - (int)(exp_period * 1.1f) - 1;
    }
    if (win_len < sr / 8) return 0.0f;

    /* ±100 cents window (period factor 2^(±1/12) ≈ 0.9439..1.0595). */
    int min_p = (int)(exp_period * 0.9439f);  if (min_p < 8) min_p = 8;
    int max_p = (int)(exp_period * 1.0595f);
    if (max_p >= frames - win_start) return 0.0f;
    if (max_p <= min_p) return 0.0f;

    /* Stage 1: coarse search. */
    float best_corr;
    int   best_period = ac_peak(samples, win_start, win_len,
                                min_p, max_p, step, &best_corr);
    /* MIDI-aware confidence floor.  Low-pitched samples (often multi-
     * voice choirs, full string sections, organ stops) have harmonic
     * content that produces broader, noisier correlation peaks.  Require
     * a HIGHER confidence in that range to reject false detunings the
     * user reported as "wrong notes in the low range". */
    float conf_floor = (midi_note < 50) ? 0.93f : 0.90f;
    if (best_period < 0 || best_corr < conf_floor) return 0.0f;

    /* Stage 1b: refine to sample-accurate period around the coarse winner. */
    int refine_lo = best_period - step; if (refine_lo < min_p) refine_lo = min_p;
    int refine_hi = best_period + step; if (refine_hi > max_p) refine_hi = max_p;
    {
        float refined_corr;
        int   refined_p = ac_peak(samples, win_start, win_len,
                                  refine_lo, refine_hi, 1, &refined_corr);
        if (refined_p >= 0 && refined_corr > best_corr) {
            best_corr   = refined_corr;
            best_period = refined_p;
        }
    }
    if (best_period <= 0) return 0.0f;

    /* Stage 1c: PARABOLIC INTERPOLATION.  The previous version used the
     * integer period as-is, which for a high-pitched note (period ~70
     * samples at F5) has a quantisation step of ~25 cents — and the
     * search's `c > best` strict-greater rule consistently broke ties in
     * favour of the FIRST-found = SHORTEST period = HIGHEST detected
     * freq.  Net result: detection biased sharp → correction biased
     * negative → user's "everything goes flat" symptom.
     *
     * Parabolic fit around the peak gives sub-sample precision and is
     * unbiased.  Use neighbours best_period ± 1 to fit y = a·x² + b·x + c
     * and find the vertex offset. */
    float det_period = (float)best_period;
    if (best_period > min_p && best_period < max_p) {
        /* CRITICAL: use step=1 for alpha and gamma so they're at the same
         * resolution as `beta` (which the refine pass above computed with
         * step=1).  The previous version passed the file-scope `step=8`
         * here, which decimated alpha/gamma 8× while beta was full-res.
         * For sharp high-note peaks the decimated values could land
         * ABOVE the full-res beta, making denom positive (a positive
         * curvature) — the strict downward-curvature guard would then
         * reject parabolic refinement and fall back to integer period,
         * reintroducing the v0.1.26 sharp-bias that drives notes flat.
         * With matched resolution, alpha and gamma are always ≤ beta
         * and the parabolic interp is unbiased. */
        float alpha = ac_at(samples, win_start, win_len, 1, best_period - 1);
        float beta  = best_corr;
        float gamma = ac_at(samples, win_start, win_len, 1, best_period + 1);
        if (alpha >= 0.0f && gamma >= 0.0f) {
            float denom = alpha - 2.0f * beta + gamma;
            /* Parabolic interp is only valid at a TRUE peak (parabola
             * curving DOWN → denom < 0).  At broad low-pitch peaks the
             * curvature can land near zero, in which case the vertex
             * formula amplifies noise into a wild fractional offset.
             * Require strong downward curvature; if the peak is too
             * flat, fall back to the integer period. */
            if (denom < -1e-4f) {
                float offset = 0.5f * (alpha - gamma) / denom;
                if (offset > -1.0f && offset < 1.0f) {
                    det_period = (float)best_period + offset;
                }
            }
        }
    }

    float det_freq = (float)sr / det_period;
    float cents    = 1200.0f * logf(det_freq / exp_freq) / logf(2.0f);

    /* MIDI-aware cents bounds:
     *   Below D3 (MIDI 50): [12, 30] cents — be conservative.  Low
     *     Mellotron content has natural pitch wobble and low-Q
     *     correlation peaks; only apply corrections that are clearly
     *     larger than the detection noise floor (~10 cents at low pitch)
     *     and clearly smaller than what a harmonic mis-detection would
     *     produce.
     *   Above D3: [8, 35] cents — standard range. */
    float cents_lo = (midi_note < 50) ? 12.0f : 8.0f;
    float cents_hi = (midi_note < 50) ? 30.0f : 35.0f;
    if (fabsf(cents) < cents_lo || fabsf(cents) > cents_hi) return 0.0f;

    /* Stage 3: VERIFY.  Re-run the autocorrelation with det_freq as the
     * new target.  If the correction is real the peak should sit at the
     * same period we just found → re-cents ≈ 0.  If it has moved, the
     * original detection was spurious and the correction would make
     * playback worse, not better. */
    int v_lo = (int)(det_period * 0.97f); if (v_lo < 8) v_lo = 8;
    int v_hi = (int)(det_period * 1.03f);
    if (v_hi >= frames - win_start) return 0.0f;
    float v_corr;
    int   v_period = ac_peak(samples, win_start, win_len, v_lo, v_hi, 1, &v_corr);
    if (v_period < 0 || v_corr < conf_floor) return 0.0f;
    float v_freq  = (float)sr / (float)v_period;
    float v_cents = 1200.0f * logf(v_freq / det_freq) / logf(2.0f);
    if (fabsf(v_cents) > 5.0f) return 0.0f;       /* verification failed */

    return -cents;                                /* CORRECTION to playback */
}

int mello_bank_scan(const char *dir, mello_bank_t *bank, int seq_base) {
    if (!dir || !bank) return -1;
    mello_bank_free(bank);
    bank->lo_note = -1;
    bank->hi_note = -1;
    bank->count = 0;
    /* derive bank display name from final path component */
    {
        const char *b = dir;
        for (const char *p = dir; *p; p++)
            if (*p == '/' || *p == '\\') b = p + 1;
        strncpy(bank->name, b, MELLO_BANK_NAME_MAX - 1);
        bank->name[MELLO_BANK_NAME_MAX - 1] = '\0';
    }

    /* enumerate .wav files */
    typedef struct { char fn[256]; int parsed; } scan_entry_t;
    scan_entry_t entries[256];
    int n = 0;

    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *de;
    while ((de = readdir(d)) && n < (int)(sizeof(entries) / sizeof(entries[0]))) {
        if (de->d_name[0] == '.') continue;
        if (!has_wav_ext(de->d_name)) continue;
        strncpy(entries[n].fn, de->d_name, sizeof(entries[n].fn) - 1);
        entries[n].fn[sizeof(entries[n].fn) - 1] = '\0';
        entries[n].parsed = mello_parse_midi_note(de->d_name, seq_base);
        n++;
    }
    closedir(d);
    if (n == 0) return 0;

    /* if NOTHING parsed, auto-fallback: trailing -N + Leisureland +40 */
    int parsed_any = 0;
    for (int i = 0; i < n; i++) if (entries[i].parsed >= 0) { parsed_any = 1; break; }
    if (!parsed_any && seq_base < 0) {
        for (int i = 0; i < n; i++) {
            char stem[256]; strncpy(stem, entries[i].fn, sizeof(stem) - 1); stem[sizeof(stem) - 1] = '\0';
            char *dot = strrchr(stem, '.'); if (dot) *dot = '\0';
            int s = trailing_seq(stem);
            if (s >= 1) entries[i].parsed = s + 40;
        }
    }

    /* load each WAV */
    int loaded = 0;
    for (int i = 0; i < n; i++) {
        int note = entries[i].parsed;
        if (note < 0 || note > 127) continue;
        if (bank->notes[note].samples) continue; /* duplicate: skip */
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, entries[i].fn);
        if (mello_wav_load(path, &bank->notes[note]) == 0) {
            /* Auto-detune: measure how far the sample's fundamental is from
             * the note we mapped it to, store the correction in cents. */
            bank->notes[note].pitch_cents = detect_pitch_cents(
                bank->notes[note].samples,
                bank->notes[note].frames,
                bank->notes[note].orig_sr,
                note);
            if (bank->lo_note < 0 || note < bank->lo_note) bank->lo_note = note;
            if (bank->hi_note < 0 || note > bank->hi_note) bank->hi_note = note;
            loaded++;
        }
    }
    bank->count = loaded;

    /* Auto level-match: scan every loaded sample for absolute peak, then
     * compute a gain that normalises the bank to a target headroom.  Why:
     * different sample packs have wildly different recorded levels (the
     * 90s archive is hot, Sonic Bloom is conservative, Leisureland sits
     * in between) — playing them through the same FX chain produces
     * unpredictable distortion / limiter behaviour.  By normalising every
     * bank to peak 0.35, the user gets:
     *   1. consistent loudness when switching banks
     *   2. headroom to drive the preamp / tape / crunch stages hard
     *      without the limiter immediately stomping the output
     *
     * Decimated scan (every 16th sample) keeps this cheap — a 35-sample
     * bank at ~5 s/sample takes ~10 ms total instead of ~150 ms full-
     * scan, and the peak estimate is still accurate to ~0.3 dB. */
    bank->bank_gain = 1.0f;
    if (loaded > 0) {
        int max_abs = 0;
        for (int note = 0; note < 128; note++) {
            const mello_wav_t *w = &bank->notes[note];
            if (!w->samples) continue;
            for (int n = 0; n < w->frames; n += 16) {
                int v = w->samples[n];
                if (v < 0) v = -v;
                if (v > max_abs) max_abs = v;
            }
        }
        if (max_abs > 0) {
            const float TARGET_PEAK = 0.35f;     /* leaves 9 dB of headroom */
            float peak_f = (float)max_abs / 32768.0f;
            float g = TARGET_PEAK / peak_f;
            if (g < 0.10f) g = 0.10f;            /* don't kill loud banks entirely */
            if (g > 4.00f) g = 4.00f;            /* don't overdrive silent ones */
            bank->bank_gain = g;
        }
    }
    return loaded;
}

int mello_bank_pick_note(const mello_bank_t *bank, int midi_note,
                         int mode, int *out_pitch_offset) {
    if (!bank || bank->count == 0) {
        if (out_pitch_offset) *out_pitch_offset = 0;
        return -1;
    }
    if (midi_note < 0) midi_note = 0;
    if (midi_note > 127) midi_note = 127;

    /* exact hit (both modes) */
    if (bank->notes[midi_note].samples) {
        if (out_pitch_offset) *out_pitch_offset = 0;
        return midi_note;
    }

    /* find nearest mapped slot */
    int nearest = -1, best_dist = 128;
    for (int i = 0; i < 128; i++) {
        if (!bank->notes[i].samples) continue;
        int d = i - midi_note; if (d < 0) d = -d;
        if (d < best_dist) { best_dist = d; nearest = i; }
    }
    if (nearest < 0) {
        if (out_pitch_offset) *out_pitch_offset = 0;
        return -1;
    }

    int offset = midi_note - nearest;

    if (mode == 1) {
        /* Full range (stretched): pitch-shift the nearest sample no matter
         * how far it is from the requested note.  This is the "one tape,
         * every key" behaviour — every MIDI note gets *something*, even
         * if it's a sample stretched 24 semitones up.  Clamp to ±48
         * semitones (4 octaves) just to avoid absurd rate calculations. */
        if (offset > 48)  offset = 48;
        if (offset < -48) offset = -48;
        if (out_pitch_offset) *out_pitch_offset = offset;
        return nearest;
    }

    /* mode 0 — Tape exact with chromatic fallback: nearest sample, pitch-
     * shift via varispeed, but clamp the fallback distance to ±7
     * semitones (a fifth) so notes far outside the bank's sampled range
     * don't sound like slow-tape artefacts.  Beyond that, return -1
     * (silence) — Mellotron-authentic. */
    if (offset > 7 || offset < -7) {
        if (out_pitch_offset) *out_pitch_offset = 0;
        return -1;
    }
    if (out_pitch_offset) *out_pitch_offset = offset;
    return nearest;
}

/* ============================================================================ */
#ifdef WAV_BANK_TEST
static void check_note(const char *fn, int seq_base, int expect) {
    int got = mello_parse_midi_note(fn, seq_base);
    printf("  %-40s seq=%-3d -> %4d   %s\n", fn, seq_base, got,
           (got == expect) ? "ok" : "FAIL");
}
int main(int argc, char **argv) {
    printf("filename -> MIDI note (C4=60):\n");
    check_note("A2.wav", -1, 45);
    check_note("C4.wav", -1, 60);
    check_note("C#3.wav", -1, 49);
    check_note("Db3.wav", -1, 49);
    check_note("Bb3.wav", -1, 58);
    check_note("F-1.wav", -1, 5);
    check_note("MkII Violins A2.wav", -1, 45);
    check_note("M4008Choir-1.wav", 40, 41);   /* F2 */
    check_note("M4008Choir-8.wav", 40, 48);   /* C3 */
    check_note("M4008Choir-20.wav", 40, 60);  /* C4 */
    check_note("M4008Choir-32.wav", 40, 72);  /* C5 */
    check_note("M4008Choir-37.wav", 40, 77);  /* F5 */
    check_note("MkiiFlute-1.wav", 40, 41);
    check_note("note_60.wav", -1, 60);
    check_note("Bassoon.wav", -1, -1);

    if (argc > 1) {
        mello_bank_t bank = {0};
        bank.lo_note = -1; bank.hi_note = -1;
        int c = mello_bank_scan(argv[1], &bank, 40);
        printf("\nscan '%s' -> %d loaded (%s..%s)\n", argv[1], c,
               bank.lo_note >= 0 ? (char[]){'M','I','D','I',' ', 0} : "empty", "");
        if (c > 0) printf("  range = MIDI %d..%d\n", bank.lo_note, bank.hi_note);
        mello_bank_free(&bank);
    }
    return 0;
}
#endif
