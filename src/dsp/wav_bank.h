/* ============================================================================
 * wav_bank.h — Mello in-house WAV sampler
 *
 * Loads raw .wav files (16-bit mono/stereo PCM, any sample rate) from a
 * folder into a 128-slot bank indexed by MIDI note. No SFZ, no DecentSampler,
 * no submodules — just a small read-and-pitch sampler tailored to Mellotron
 * one-tape-per-key playback semantics.
 *
 * Filename → MIDI note parsing tolerates three conventions:
 *   1. Note name + octave : "MkII Violins A2.wav", "C#3.wav", "Bb4.wav"
 *   2. Bare MIDI number   : "60.wav", "note_72.wav"   (within 21..108)
 *   3. Leisureland sequential : "M4008Choir-8.wav" → file_index 8 + seq_base
 *      (Leisureland M400/MkII default seq_base = 40, so -1 = F2 / MIDI 41,
 *      -8 = C3, -20 = C4, -32 = C5, -37 = F5)
 * ============================================================================ */
#ifndef MELLO_WAV_BANK_H
#define MELLO_WAV_BANK_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* one loaded sample, mono int16, owned heap buffer */
typedef struct {
    int16_t *samples;   /* NULL if slot empty */
    int      frames;    /* count of mono frames in `samples` */
    int      orig_sr;   /* sample rate in Hz (e.g. 44100) */
    int      loop_start;/* auto-detected sustain loop start (frame index) */
    int      loop_end;  /* auto-detected sustain loop end (exclusive) */
    float    pitch_cents;/* auto-detune correction (cents to add to playback) */
} mello_wav_t;

/* a bank = up to 128 mapped notes + range metadata */
#define MELLO_BANK_NAME_MAX 64
typedef struct {
    mello_wav_t notes[128];
    int  lo_note;        /* lowest mapped MIDI note, -1 if empty */
    int  hi_note;        /* highest mapped MIDI note, -1 if empty */
    int  count;          /* number of mapped notes */
    char name[MELLO_BANK_NAME_MAX];
    /* Auto-level-match gain: peak-normalises the bank to a target headroom
     * so different sample packs play at consistent loudness AND the user
     * can drive the preamp/tape distortion stages harder without immediate
     * clipping.  Computed in mello_bank_scan() after all WAVs are loaded:
     * gain = target_peak / detected_peak (clamped to [0.1, 4.0]).  Apply
     * during voice render — typically `out *= bank_gain`. */
    float bank_gain;
} mello_bank_t;

/* discover bank folders inside `root`. Sorted by folder name.
 * 256 entries supports the flattened union list (every library's banks
 * concatenated with `Library/Bank` prefix names), with headroom for users
 * adding a few more sample packs. */
#define MELLO_MAX_BANKS 256
typedef struct {
    char names[MELLO_MAX_BANKS][MELLO_BANK_NAME_MAX];
    int  count;
} mello_bank_list_t;

/* ----- WAV file I/O (16-bit PCM only; stereo is downmixed to mono) ----- */
int  mello_wav_load(const char *path, mello_wav_t *out);
void mello_wav_free(mello_wav_t *w);

/* ----- bank scan / load / free ----- */
/* List sub-folders of `root` (one folder = one bank). Returns count. */
int  mello_list_banks(const char *root, mello_bank_list_t *out);

/* Scan `dir` for .wav files, parse MIDI notes, load each into the bank.
 * `seq_base`: starting MIDI for *-N.wav sequential filenames (-1 = disabled,
 *             40 = Leisureland default). The scanner auto-detects sequential
 *             naming if no note-name files are found.
 * Returns: number of slots loaded, or -1 on error. Caller must free with
 * mello_bank_free(). Safe to call repeatedly (frees previous contents). */
int  mello_bank_scan(const char *dir, mello_bank_t *bank, int seq_base);
void mello_bank_free(mello_bank_t *bank);

/* For an incoming MIDI note, pick the actual sample slot to play, returning
 * the semitone offset that must be applied via varispeed pitch.
 *
 * Behaviour: "Tape exact with chromatic fallback above/below" (design §2):
 *   - If the note has its own sample → return that note, offset 0.
 *   - If outside the bank range → pitch-shift the nearest endpoint sample
 *     (the lowest pitched down, the highest pitched up).
 *   - Inside the range with a gap (rare for Leisureland) → pitch-shift the
 *     nearest mapped neighbour.
 *
 * `mode`: 0 = exact-with-fallback (default), 1 = full-range stretched (every
 *         note gets the nearest sample, useful for sparse packs).
 *
 * Returns the slot index (0..127) to play, writing the semitone offset into
 * *out_pitch_offset. Returns -1 if the bank is empty. */
int  mello_bank_pick_note(const mello_bank_t *bank, int midi_note,
                          int mode, int *out_pitch_offset);

/* ----- filename → MIDI note (exposed for testing) ----- */
int  mello_parse_midi_note(const char *filename, int seq_base);

#ifdef __cplusplus
}
#endif

#endif /* MELLO_WAV_BANK_H */
