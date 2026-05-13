/**
 * KrautDrums — 16-voice tonal drum machine for Ableton Move
 * Modeled on the 1969 Elka Drummer One.
 *
 * Author:  Vincent Filliforme
 * License: MIT
 *
 * Architecture:
 *   16 always-allocated voices (9 base + 7 variations).
 *   Each voice = 1 or 2 high-Q biquad bandpass resonators + AD envelope.
 *   Triggered by impulse injection (one-sample delta).
 *
 * Pad layout:
 *   Left 16 pads  (notes 36-39, 44-47, 52-55, 60-63) = 16 latching rhythm patterns.
 *   Right 16 pads (notes 40-43, 48-51, 56-59, 64-67) = 16 momentary voice triggers.
 *
 * Master signal chain:
 *   voices_sum → attitude (preamp+tape+EQ+SVF+phaser) → bus_comp → delay
 *              → reverb → master_vol → limiter → out
 *
 * Audio: 44100 Hz, 128 frames/block, stereo interleaved int16 output.
 * API:   plugin_api_v2_t  (8 fields including get_error = NULL).
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

#define SAMPLE_RATE       44100.0f
#define BLOCK_SIZE        128
#define NUM_VOICES        16
#define NUM_RHYTHMS       16
#define MAX_RHYTHM_STEPS  32
#define DELAY_BUF_SIZE    98304      /* 2.23 sec @ 44.1k — comfortably exceeds 1000ms max delay
                                       * (DELAY_BUF_SIZE/2 = 49152 samples ≈ 1115 ms cap, > 1000ms spec) */
#define NUM_RHYTHM_OPTS   17    /* Off + 16 rhythm patterns */
#define NUM_RHYTHM_SLOTS  8     /* simultaneous rhythm channels on Rhythms page */
#define REV_COMB_COUNT    4
#define REV_AP_COUNT      2
#define REV_DISP_COUNT    6    /* dispersion allpasses for BX20 spring (Parker 2011) */
#define REV_DISP_BUFLEN   256  /* max delay per dispersion allpass (longer = sharper chirp slope) */
#define REV_ER_BUFLEN     2400 /* ~54 ms — tap delay for chamber early reflections */
#define REV_ER_TAPS       6

/* Dattorro 1997 plate reverb topology — sample lengths from the original
 * AES paper, scaled from 29.761 kHz → 44.1 kHz (×1.482).  These are the
 * tank "magic numbers" Dattorro published; they're reused by Mutable
 * Instruments clouds, Faust dattorro_rev, and most modern plate emulations.
 *
 * Buffer sizes are next-power-of-2 above the actual lengths so the index
 * wrap is a cheap mask.                                                       */
#define DAT_PRE_LEN          240   /* ~5.4 ms input pre-delay */
#define DAT_DIF_A_LEN        211
#define DAT_DIF_B_LEN        159
#define DAT_DIF_C_LEN        562
#define DAT_DIF_D_LEN        411
#define DAT_TA_MOD_AP_LEN    996
#define DAT_TA_DELAY1_LEN    6598
#define DAT_TA_STATIC_AP_LEN 2667
#define DAT_TA_DELAY2_LEN    5512
#define DAT_TB_MOD_AP_LEN    1322
#define DAT_TB_DELAY1_LEN    6249
#define DAT_TB_STATIC_AP_LEN 3936
#define DAT_TB_DELAY2_LEN    4687

/* Anti-denormal constant: ~1e-25 escapes ARM's denormal range (smallest normal
 * float32 is ~1.18e-38). Adding to filter states prevents 30-50× CPU stalls
 * during long decay tails. The DC offset introduced is below -500 dB → inaudible. */
#define DENORM_EPS  1.0e-25f

/* ──────────────────────────────────────────────────────────────────────────────
 * Forward declarations and small helpers
 * ────────────────────────────────────────────────────────────────────────────── */

static inline float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

static inline float fast_tanh(float x) {
    /* Cheap rational approx, monotonic and bounded. */
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

/* Simple fast LCG random for analog drift — deterministic per instance */
static uint32_t rng_state = 0xC0FFEE42u;
static inline float rand_bipolar(void) {
    rng_state = rng_state * 1664525u + 1013904223u;
    return ((float)rng_state / (float)0x80000000u) - 1.0f;  /* -1 .. +1 */
}
static inline float rand_unipolar(void) {
    rng_state = rng_state * 1664525u + 1013904223u;
    return (float)rng_state / (float)0xFFFFFFFFu;  /* 0 .. +1 */
}
/* Approximate Gaussian via Central Limit Theorem (sum of 3 uniforms).
 * Output stddev ≈ 1.0, output range ≈ ±2 (clamped by sum). Cheaper than Box-Muller
 * (no logf/sqrtf), and the slight tail truncation is acoustically irrelevant.
 * Real component drift on 1969 ceramic caps is closer to Gaussian than uniform —
 * most hits cluster near nominal, occasional outliers feel like real instability. */
static inline float rand_gaussian(void) {
    float u1 = rand_bipolar();
    float u2 = rand_bipolar();
    float u3 = rand_bipolar();
    return (u1 + u2 + u3) * 0.5773502691896258f;  /* 1/sqrt(3) → unit stddev */
}

/* ──────────────────────────────────────────────────────────────────────────────
 * Biquad bandpass — the heart of every drum voice
 *   Cookbook BPF (constant 0 dB peak gain at f0).
 *   Triggered by impulse → rings at f0 with decay τ ≈ Q/(π·f0).
 * ────────────────────────────────────────────────────────────────────────────── */

typedef struct {
    float b0, b1, b2;   /* normalized */
    float a1, a2;
    float z1, z2;       /* state */
} biquad_t;

static void biquad_set_bpf(biquad_t *bq, float f0, float Q, float fs) {
    if (f0 < 20.0f) f0 = 20.0f;
    if (f0 > fs * 0.45f) f0 = fs * 0.45f;
    if (Q < 0.5f) Q = 0.5f;

    float w0    = 2.0f * M_PI * f0 / fs;
    float cos_w = cosf(w0);
    float sin_w = sinf(w0);
    float alpha = sin_w / (2.0f * Q);

    float b0 = alpha;
    float b1 = 0.0f;
    float b2 = -alpha;
    float a0 = 1.0f + alpha;
    float a1 = -2.0f * cos_w;
    float a2 = 1.0f - alpha;

    float inv_a0 = 1.0f / a0;
    bq->b0 = b0 * inv_a0;
    bq->b1 = b1 * inv_a0;
    bq->b2 = b2 * inv_a0;
    bq->a1 = a1 * inv_a0;
    bq->a2 = a2 * inv_a0;
}

/* High-pass biquad (Audio EQ Cookbook). Q ~0.707 = Butterworth response. */
static void biquad_set_hpf(biquad_t *bq, float f0, float Q, float fs) {
    if (f0 < 20.0f) f0 = 20.0f;
    if (f0 > fs * 0.45f) f0 = fs * 0.45f;
    if (Q < 0.5f) Q = 0.5f;

    float w0    = 2.0f * M_PI * f0 / fs;
    float cos_w = cosf(w0);
    float sin_w = sinf(w0);
    float alpha = sin_w / (2.0f * Q);

    float b0 = (1.0f + cos_w) * 0.5f;
    float b1 = -(1.0f + cos_w);
    float b2 = (1.0f + cos_w) * 0.5f;
    float a0 = 1.0f + alpha;
    float a1 = -2.0f * cos_w;
    float a2 = 1.0f - alpha;

    float inv_a0 = 1.0f / a0;
    bq->b0 = b0 * inv_a0;
    bq->b1 = b1 * inv_a0;
    bq->b2 = b2 * inv_a0;
    bq->a1 = a1 * inv_a0;
    bq->a2 = a2 * inv_a0;
}

/* Low-shelf biquad (Audio EQ Cookbook). gain_db = ±N dB shelf around f0. */
static void biquad_set_lowshelf(biquad_t *bq, float f0, float gain_db, float fs) {
    if (f0 < 20.0f) f0 = 20.0f;
    if (f0 > fs * 0.45f) f0 = fs * 0.45f;

    float A     = powf(10.0f, gain_db / 40.0f);
    float w0    = 2.0f * M_PI * f0 / fs;
    float cos_w = cosf(w0);
    float sin_w = sinf(w0);
    /* S=1 for shelf slope (standard) */
    float alpha = sin_w * 0.5f * sqrtf((A + 1.0f / A) * (1.0f / 1.0f - 1.0f) + 2.0f);
    /* simplification: alpha = sin_w/2 * sqrt((A + 1/A)*(1/S - 1) + 2)  with S=1 → sqrt(2) */
    alpha = sin_w * 0.7071067811865476f;

    float two_sqrtA_alpha = 2.0f * sqrtf(A) * alpha;

    float b0 =       A * ((A + 1.0f) - (A - 1.0f) * cos_w + two_sqrtA_alpha);
    float b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cos_w);
    float b2 =       A * ((A + 1.0f) - (A - 1.0f) * cos_w - two_sqrtA_alpha);
    float a0 =           ((A + 1.0f) + (A - 1.0f) * cos_w + two_sqrtA_alpha);
    float a1 = -2.0f *   ((A - 1.0f) + (A + 1.0f) * cos_w);
    float a2 =           ((A + 1.0f) + (A - 1.0f) * cos_w - two_sqrtA_alpha);

    float inv_a0 = 1.0f / a0;
    bq->b0 = b0 * inv_a0;
    bq->b1 = b1 * inv_a0;
    bq->b2 = b2 * inv_a0;
    bq->a1 = a1 * inv_a0;
    bq->a2 = a2 * inv_a0;
}

/* High-shelf biquad (Audio EQ Cookbook). gain_db = ±N dB shelf above f0. */
static void biquad_set_highshelf(biquad_t *bq, float f0, float gain_db, float fs) {
    if (f0 < 20.0f) f0 = 20.0f;
    if (f0 > fs * 0.45f) f0 = fs * 0.45f;

    float A     = powf(10.0f, gain_db / 40.0f);
    float w0    = 2.0f * M_PI * f0 / fs;
    float cos_w = cosf(w0);
    float sin_w = sinf(w0);
    float alpha = sin_w * 0.7071067811865476f;  /* S=1 */

    float two_sqrtA_alpha = 2.0f * sqrtf(A) * alpha;

    float b0 =       A * ((A + 1.0f) + (A - 1.0f) * cos_w + two_sqrtA_alpha);
    float b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cos_w);
    float b2 =       A * ((A + 1.0f) + (A - 1.0f) * cos_w - two_sqrtA_alpha);
    float a0 =           ((A + 1.0f) - (A - 1.0f) * cos_w + two_sqrtA_alpha);
    float a1 =  2.0f *   ((A - 1.0f) - (A + 1.0f) * cos_w);
    float a2 =           ((A + 1.0f) - (A - 1.0f) * cos_w - two_sqrtA_alpha);

    float inv_a0 = 1.0f / a0;
    bq->b0 = b0 * inv_a0;
    bq->b1 = b1 * inv_a0;
    bq->b2 = b2 * inv_a0;
    bq->a1 = a1 * inv_a0;
    bq->a2 = a2 * inv_a0;
}

/* Peaking EQ biquad (Audio EQ Cookbook). Bell shape centered on f0.
 * Used for the EP-3 head-bump shelf and Studer A80 head-bump.            */
static void biquad_set_peaking(biquad_t *bq, float f0, float Q, float gain_db, float fs) {
    if (f0 < 20.0f)         f0 = 20.0f;
    if (f0 > fs * 0.45f)    f0 = fs * 0.45f;
    if (Q < 0.1f)           Q = 0.1f;

    float A     = powf(10.0f, gain_db / 40.0f);
    float w0    = 2.0f * M_PI * f0 / fs;
    float cos_w = cosf(w0);
    float sin_w = sinf(w0);
    float alpha = sin_w / (2.0f * Q);

    float b0 = 1.0f + alpha * A;
    float b1 = -2.0f * cos_w;
    float b2 = 1.0f - alpha * A;
    float a0 = 1.0f + alpha / A;
    float a1 = -2.0f * cos_w;
    float a2 = 1.0f - alpha / A;

    float inv_a0 = 1.0f / a0;
    bq->b0 = b0 * inv_a0;
    bq->b1 = b1 * inv_a0;
    bq->b2 = b2 * inv_a0;
    bq->a1 = a1 * inv_a0;
    bq->a2 = a2 * inv_a0;
}

static inline float biquad_tick(biquad_t *bq, float in) {
    /* Direct Form II Transposed — numerically robust */
    float out = bq->b0 * in + bq->z1;
    bq->z1   = bq->b1 * in - bq->a1 * out + bq->z2;
    bq->z2   = bq->b2 * in - bq->a2 * out + DENORM_EPS;  /* anti-denormal */
    return out;
}

static inline void biquad_reset(biquad_t *bq) {
    bq->z1 = bq->z2 = 0.0f;
}

/* ──────────────────────────────────────────────────────────────────────────────
 * Synthi-style diode-ladder low-pass filter
 *
 * 4-stage cascaded one-pole topology with feedback for resonance, plus a
 * tanh nonlinearity in the feedback path to model the asymmetric
 * conduction of the EMS Synthi VCS3's transistor-pair "diode" filter
 * (Krautrock period-correct: Faust's Wümme studio, Tangerine Dream,
 * Cluster all used Synthi/VCS3-class filters extensively).
 *
 * `g` is the per-stage one-pole coefficient (0..1, ≈ tan(π·fc/fs) prewarp
 * collapsed). `k` is the resonance feedback amount, 0 = clean, ~3.9 = on
 * the edge of self-oscillation. The tanh on the feedback path softens
 * the squelch and adds the recognisable VCS3 grit when pushed.
 * ────────────────────────────────────────────────────────────────────────────── */
typedef struct {
    float z[4];
} ladder_t;

static inline void ladder_reset(ladder_t *l) {
    l->z[0] = l->z[1] = l->z[2] = l->z[3] = 0.0f;
}

static inline float ladder_process(ladder_t *l, float in, float g, float k) {
    /* tanh-clipped feedback from the last stage — Synthi diode character */
    float fb = fast_tanh(l->z[3] * 0.7f) * 1.4f;
    float u  = in - k * fb;
    /* Soft clip the input too, so the filter never goes numerically wild
     * at extreme drive — also part of the VCS3 grit signature.            */
    u = fast_tanh(u * 0.5f) * 2.0f;

    l->z[0] += g * (u        - l->z[0]) + DENORM_EPS;
    l->z[1] += g * (l->z[0]  - l->z[1]) + DENORM_EPS;
    l->z[2] += g * (l->z[1]  - l->z[2]) + DENORM_EPS;
    l->z[3] += g * (l->z[2]  - l->z[3]) + DENORM_EPS;
    return l->z[3];
}

/* ──────────────────────────────────────────────────────────────────────────────
 * Voice — bridged-T resonator simulation
 *
 * Voice indices (panel order, left to right):
 *   0=Bass-drum  1=Conga      2=Tom1     3=Tom2      4=Claves
 *   5=Snare      6=ShortCym   7=LongCym  8=CowBell
 *   9=Bass2     10=Tom3      11=Conga2  12=Claves2
 *  13=Snare2   14=MidCym    15=CowBell2
 * ────────────────────────────────────────────────────────────────────────────── */

#define VOICE_GROUP_BASS    0
#define VOICE_GROUP_CONGA   1
#define VOICE_GROUP_TOM1    2
#define VOICE_GROUP_TOM2    3
#define VOICE_GROUP_CLAVES  4
#define VOICE_GROUP_SNARE   5
#define VOICE_GROUP_COWBELL 6
#define VOICE_GROUP_CYMBALS 7
#define NUM_GROUPS          8

typedef struct {
    const char *name;
    float       f0_a;          /* Hz — primary resonator */
    float       Q_a;
    float       f0_b;          /* Hz — secondary, 0 if unused */
    float       Q_b;
    float       res_mix;       /* mix b vs a (0..1, only when f0_b > 0) */
    float       decay_ms;      /* envelope AD decay */
    int         group;         /* VOICE_GROUP_* index for level control */
    float       fm_amount;     /* Plaits-style attack-FM depth (0=no chirp, 0.4=strong kick chirp) */
    float       drift_amt;     /* diode-starve pitch drift coefficient (0=none, 0.06=strong) */
} voice_def_t;

/* Per-voice FM/drift values — calibrated for the actual 1969 Drummer One,
 * NOT for TR-808-style "thump-chirp" kicks.
 *
 * The Drummer One has NO explicit pitch-envelope circuit (unlike the 808's
 * Q3 retrigger). Each voice is just a bridged-T resonator with a diode in
 * the feedback path. The diode produces TWO related effects from the same
 * physics:
 *   - A tiny natural upward chirp at attack (fm_amount, ≤0.08)
 *   - A larger downward decay drift (drift_amt, scales with envelope)
 * Both come from forward-voltage-drop with current. Setting fm_amount any
 * larger than this turns the kicks into 808-thumps that the real machine
 * doesn't have — drives the "Krautrock" toward "post-disco" character.
 *
 * Reference recordings: Cluster *Zuckerzeit*, *Sowiesoso*; Harmonia *Deluxe*;
 * Roedelius solo work. Listen for kicks that *thud* without chirping. */
static const voice_def_t VOICES[NUM_VOICES] = {
    /* name             f0_a     Q_a    f0_b     Q_b   mix   ms     group              fm    drift */
    {"Bass-drum",      71.6f,   8.0f,    0.0f,   0.0f, 0.0f,  250.0f, VOICE_GROUP_BASS,    0.06f, 0.060f},
    {"Conga",         102.8f,  12.0f,    0.0f,   0.0f, 0.0f,  200.0f, VOICE_GROUP_CONGA,   0.02f, 0.040f},
    {"Tom 1",         228.3f,  10.0f,    0.0f,   0.0f, 0.0f,  250.0f, VOICE_GROUP_TOM1,    0.05f, 0.050f},
    {"Tom 2",         350.0f,  10.0f,    0.0f,   0.0f, 0.0f,  260.0f, VOICE_GROUP_TOM2,    0.05f, 0.050f},
    {"Claves",       2210.5f,  25.0f,    0.0f,   0.0f, 0.0f,   30.0f, VOICE_GROUP_CLAVES,  0.02f, 0.005f},
    {"Snare",         192.9f,   6.0f,  310.0f,   8.0f, 0.4f,  220.0f, VOICE_GROUP_SNARE,   0.05f, 0.030f},
    {"Short cym",    4074.3f,  30.0f, 5400.0f,  25.0f, 0.4f,   90.0f, VOICE_GROUP_CYMBALS, 0.02f, 0.005f},
    {"Long cym",     3461.4f,  35.0f, 4900.0f,  30.0f, 0.4f, 1200.0f, VOICE_GROUP_CYMBALS, 0.02f, 0.005f},
    {"Cow Bell",      536.1f,  18.0f,  800.0f,  18.0f, 0.4f,  350.0f, VOICE_GROUP_COWBELL, 0.02f, 0.020f},
    /* 7 hand-tuned variations */
    {"Bass 2",         60.0f,   6.0f,    0.0f,   0.0f, 0.0f,  180.0f, VOICE_GROUP_BASS,    0.08f, 0.070f},
    {"Tom 3",         723.4f,  11.0f,    0.0f,   0.0f, 0.0f,  220.0f, VOICE_GROUP_TOM2,    0.05f, 0.050f},
    {"Conga 2",       150.0f,  14.0f,    0.0f,   0.0f, 0.0f,  180.0f, VOICE_GROUP_CONGA,   0.02f, 0.040f},
    {"Claves 2",     2800.0f,  28.0f,    0.0f,   0.0f, 0.0f,   25.0f, VOICE_GROUP_CLAVES,  0.02f, 0.005f},
    {"Snare 2",       220.0f,   7.0f,  360.0f,   9.0f, 0.4f,  200.0f, VOICE_GROUP_SNARE,   0.05f, 0.030f},
    {"Mid cym",      3700.0f,  32.0f, 5100.0f,  28.0f, 0.4f,  500.0f, VOICE_GROUP_CYMBALS, 0.02f, 0.005f},
    {"Cow Bell 2",    640.0f,  18.0f,  880.0f,  18.0f, 0.4f,  320.0f, VOICE_GROUP_COWBELL, 0.02f, 0.020f},
};

/* Plaits-style shaped trigger pulse durations (in samples @ 44.1kHz):
 *   1ms diode-clipped trigger pulse + 6ms FM pulse (raises pitch on attack).
 * These mirror the values in Émilie Gillet's Plaits 808 BD model. */
#define TRIG_PULSE_SAMPLES   44       /* ~1 ms diode-clipped trigger */
#define TRIG_FM_SAMPLES      264      /* ~6 ms FM pulse */
#define TRIG_RETRIG_SAMPLES  2205     /* ~50 ms retrig hold */

typedef struct {
    biquad_t res_a;
    biquad_t res_b;          /* used only if VOICES[idx].f0_b > 0 */
    float    env;             /* current envelope value 0..1 */
    int      env_active;
    float    decay_coef;     /* per-sample decay multiplier (computed from decay_ms × all_decay) */
    float    velocity;        /* last trigger velocity 0..1 */

    /* Per-trigger frequencies AFTER Gaussian jitter (used for diode-starve drift each block) */
    float    f0_a_trig;
    float    f0_b_trig;

    /* Plaits-style shaped trigger pulse state (replaces the old single-sample impulse) */
    int      pulse_remaining;       /* samples left of the 1 ms hard pulse */
    int      fm_pulse_remaining;     /* samples left of the 6 ms FM pulse */
    float    pulse_height;           /* peak value of the trigger pulse (set from velocity) */
    float    pulse_value;            /* current decayed pulse value */
    float    pulse_lp;               /* one-pole LPF state for diode shaping */
    float    fm_pulse_lp;            /* one-pole LPF on the FM pulse (smooths chirp edge) */
    float    retrig_pulse;           /* slow undershoot post-FM pulse — adds retrigger character */
} voice_state_t;

/* ──────────────────────────────────────────────────────────────────────────────
 * Pad MIDI mapping (Move Drum Kit template)
 *
 * The Drum Kit template numbers the 32 pads as two 4×4 blocks of 16:
 *   Left  half (cols 0-3, all rows): notes 36-51 — 16 drum voice triggers
 *   Right half (cols 4-7, all rows): notes 52-67 — 16 rhythm pattern toggles
 *
 *   Right block layout (rhythms in panel order):
 *     Row 4 (top):    [Rock  ][SlowRck][Shake ][R&Blues]   notes 64-67
 *     Row 3:          [Rumba ][Samba  ][Mambo ][BossaN ]   notes 60-63
 *     Row 2:          [Swing ][Slow   ][Beguin][ChaCha ]   notes 56-59
 *     Row 1 (bot):    [Waltz ][Tango  ][Polka ][PasoD  ]   notes 52-55
 *
 * Voice triggers are momentary (one-shot). Rhythm pads are LATCHING — first
 * press activates, second press deactivates. Multiple rhythms can be active
 * simultaneously; their step events are OR-merged with density × 1/√N gating.
 * ────────────────────────────────────────────────────────────────────────────── */

/* Map note → voice index (0..15) if it's a left-half pad, else -1 */
static int note_to_voice_idx(int note) {
    if (note < 36 || note > 51) return -1;
    return note - 36;
}

/* Map note → rhythm index (0..15) if it's a right-half pad, else -1 */
static int note_to_rhythm_idx(int note) {
    if (note < 52 || note > 67) return -1;
    return note - 52;
}

/* ──────────────────────────────────────────────────────────────────────────────
 * Rhythm patterns
 *
 * Each rhythm = 16 steps × 9 voice slots (each slot is the *base* voice in its
 * group; variations are not sequenced from rhythm patterns). Step value = velocity
 * (0-127). When multiple rhythms are active, velocities are MAX-merged across
 * rhythms, then density-scaled.
 *
 * Voice slot indices in patterns:
 *   0=Bass  1=Conga  2=Tom1  3=Tom2  4=Claves  5=Snare  6=ShortCym  7=LongCym  8=CowBell
 *
 * NOTE: These are starting-point grooves. Vincent should tune the velocities and
 * step counts by ear against the original Drummer One reference recordings.
 * ────────────────────────────────────────────────────────────────────────────── */

#define R_(...) {__VA_ARGS__}

typedef struct {
    const char *name;
    int   len_steps;                /* 12, 16, or up to MAX_RHYTHM_STEPS */
    /* Step velocities per voice slot. 0 = silent, 1-127 = velocity. */
    uint8_t steps[9][MAX_RHYTHM_STEPS];
} rhythm_def_t;

/* ── 16 Drummer-One rhythms in panel order ───────────────────────────────────
 *   Pad layout (left half of Move grid):
 *     ROW 4 (top):  [Rock  ][SlowRck][Shake ][R&Blues]   indices 12-15
 *     ROW 3:        [Rumba ][Samba  ][Mambo ][BossaN ]   indices  8-11
 *     ROW 2:        [Swing ][Slow   ][Beguin][ChaCha ]   indices  4-7
 *     ROW 1 (bot):  [Waltz ][Tango  ][Polka ][PasoD  ]   indices  0-3
 *
 * Patterns are 16 steps (1 bar of 4/4 in 16ths) by default. Waltz is 12
 * (3/4 in 16ths). Slow Rock is 12 (12/8 triplet feel). Patterns shorter
 * than 16 loop within sequencer_step's `step % len_steps` modulo.
 *
 * Velocity map: ~110 = strong, ~80 = normal, ~60 = ghost, 0 = silent.
 * ───────────────────────────────────────────────────────────────────────────── */

static const rhythm_def_t RHYTHMS[NUM_RHYTHMS] = {
    /* ──────────────────────────────────── 0 — Waltz (3/4, 12 steps) ───
     * Classic ballroom waltz: bass on 1, light snare brushes on 2 and 3,
     * cymbal on every beat. */
    {"Waltz", 12, {
        /* Bass    */ R_(110,0,0,0, 0,0,0,0, 0,0,0,0),
        /* Conga   */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* Tom 1   */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* Tom 2   */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* Claves  */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* Snare   */ R_(0,0,0,0, 90,0,0,0, 90,0,0,0),
        /* ShortCy */ R_(80,0,0,0, 80,0,0,0, 80,0,0,0),
        /* LongCy  */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* CowBell */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0),
    }},

    /* ──────────────────────────────────── 1 — Tango (4/4, 16 steps) ───
     * Habanera figure: dotted-quarter + sixteenth + eighth + eighth.
     * Bass plays the habanera, claves trace tango clave (1, ee, 3, 4). */
    {"Tango", 16, {
        /* Bass    */ R_(110,0,0,80, 0,0,0,0, 100,0,0,0, 0,0,0,0),
        /* Conga   */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* Tom 1   */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* Tom 2   */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* Claves  */ R_(100,0,0,80, 0,0,100,0, 100,0,0,0, 100,0,0,0),
        /* Snare   */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 90,0,0,0),
        /* ShortCy */ R_(70,0,70,0, 70,0,70,0, 70,0,70,0, 70,0,70,0),
        /* LongCy  */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* CowBell */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
    }},

    /* ──────────────────────────────────── 2 — Polka (2/4 × 2, 16 steps) ──
     * Bouncy oom-pah-oom-pah, fast tempo. Bass on 1 of each 2/4 bar,
     * snare on 2 of each. */
    {"Polka", 16, {
        /* Bass    */ R_(110,0,0,0, 0,0,0,0, 100,0,0,0, 0,0,0,0),
        /* Conga   */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* Tom 1   */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* Tom 2   */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* Claves  */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* Snare   */ R_(0,0,0,0, 100,0,0,0, 0,0,0,0, 100,0,0,0),
        /* ShortCy */ R_(80,0,80,0, 80,0,80,0, 80,0,80,0, 80,0,80,0),
        /* LongCy  */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* CowBell */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
    }},

    /* ──────────────────────────────────── 3 — Paso Doble (2/4 march × 2) ──
     * Spanish bullfight march. Strong bass on every quarter, military snare,
     * crash cymbal on the 1 of each bar, tom pickup at the end. */
    {"Paso Doble", 16, {
        /* Bass    */ R_(110,0,0,0, 100,0,0,0, 110,0,0,0, 100,0,0,0),
        /* Conga   */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* Tom 1   */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,80,90),
        /* Tom 2   */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* Claves  */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* Snare   */ R_(0,0,0,0, 90,0,0,0, 0,0,0,0, 90,0,0,0),
        /* ShortCy */ R_(80,0,0,0, 80,0,0,0, 80,0,0,0, 80,0,0,0),
        /* LongCy  */ R_(110,0,0,0, 0,0,0,0, 110,0,0,0, 0,0,0,0),
        /* CowBell */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
    }},

    /* ──────────────────────────────────── 4 — Swing (4/4 swung, 16 steps) ──
     * Big-band swing. Feathered bass on 1 and 3, snare on 2 and 4 (HH-pedal
     * style), ride cymbal "ding ding-a ding-a" pattern. The swung 8th-note
     * feel is implicit; on a 16-step grid it's approximated with off-beat
     * ghost ride hits at velocity 60. */
    {"Swing", 16, {
        /* Bass    */ R_(80,0,0,0, 0,0,0,0, 80,0,0,0, 0,0,0,0),
        /* Conga   */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* Tom 1   */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* Tom 2   */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* Claves  */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* Snare   */ R_(0,0,0,0, 90,0,0,0, 0,0,0,0, 90,0,0,0),
        /* ShortCy */ R_(100,0,60,0, 100,60,0,0, 100,0,60,0, 100,60,0,0),
        /* LongCy  */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* CowBell */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
    }},

    /* ──────────────────────────────────── 5 — Slow (4/4 ballad, 16 steps) ──
     * Generic slow ballad pattern. Kick on 1 and 3, snare on 2 and 4,
     * steady 8th-note cymbal. Distinct from "Slow Rock" (which is 12/8). */
    {"Slow", 16, {
        /* Bass    */ R_(100,0,0,0, 0,0,0,0, 90,0,0,0, 0,0,0,0),
        /* Conga   */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* Tom 1   */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* Tom 2   */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* Claves  */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* Snare   */ R_(0,0,0,0, 100,0,0,0, 0,0,0,0, 100,0,0,0),
        /* ShortCy */ R_(70,0,70,0, 70,0,70,0, 70,0,70,0, 70,0,70,0),
        /* LongCy  */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* CowBell */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
    }},

    /* ──────────────────────────────────── 6 — Beguine (4/4, 16 steps) ──
     * Caribbean ballroom rhythm from Martinique. Characteristic accent on
     * AND of 1 (rumba-like). Bass plays 1 + AND-of-3, claves play a rumba
     * clave variant, conga drives the off-beats. */
    {"Beguine", 16, {
        /* Bass    */ R_(110,0,0,0, 0,0,0,0, 0,0,100,0, 0,0,0,0),
        /* Conga   */ R_(0,0,60,0, 0,0,60,0, 0,0,60,0, 0,0,60,0),
        /* Tom 1   */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* Tom 2   */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* Claves  */ R_(100,0,0,90, 0,0,100,0, 0,0,100,0, 100,0,0,0),
        /* Snare   */ R_(0,0,0,0, 80,0,0,0, 0,0,0,0, 80,0,0,0),
        /* ShortCy */ R_(60,0,60,0, 60,0,60,0, 60,0,60,0, 60,0,60,0),
        /* LongCy  */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* CowBell */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
    }},

    /* ──────────────────────────────────── 7 — Cha-Cha (4/4, 16 steps) ──
     * "One, two, cha-cha-cha". The cha-cha-cha hits land on 4-AND-1.
     * Steady cha-cha cowbell on every quarter, with the trademark cha-cha-cha
     * on claves. */
    {"Cha-Cha", 16, {
        /* Bass    */ R_(110,0,0,0, 100,0,0,0, 100,0,0,0, 100,0,100,0),
        /* Conga   */ R_(0,0,60,0, 0,0,60,0, 0,0,60,0, 0,0,60,0),
        /* Tom 1   */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* Tom 2   */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* Claves  */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 100,0,100,0),
        /* Snare   */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* ShortCy */ R_(80,0,80,0, 80,0,80,0, 80,0,80,0, 80,0,80,0),
        /* LongCy  */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* CowBell */ R_(110,0,0,0, 100,0,0,0, 100,0,0,0, 100,0,100,0),
    }},

    /* ──────────────────────────────────── 8 — Rumba (4/4, 16 steps) ──
     * Forward (3-2) son clave: positions 0, 3, 6, 10, 12 in 16ths.
     * Conga plays tumbao, light snare on 2 and 4. */
    {"Rumba", 16, {
        /* Bass    */ R_(100,0,0,0, 0,0,90,0, 0,0,0,0, 100,0,0,0),
        /* Conga   */ R_(0,0,0,60, 60,0,0,60, 60,0,0,60, 60,0,0,60),
        /* Tom 1   */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* Tom 2   */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* Claves  */ R_(100,0,0,90, 0,0,100,0, 0,0,100,0, 100,0,0,0),
        /* Snare   */ R_(0,0,0,0, 70,0,0,0, 0,0,0,0, 70,0,0,0),
        /* ShortCy */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* LongCy  */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* CowBell */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
    }},

    /* ──────────────────────────────────── 9 — Samba (2/4 fast, 16 steps) ──
     * Brazilian samba in the 16-step view = 2 bars of 2/4. Surdo bass plays
     * heavy on "1" with prep notes on AND-of-2, conga on a partido-alto
     * pattern, 16th-note shaker (ShortCy) drives the engine. */
    {"Samba", 16, {
        /* Bass    */ R_(110,0,0,0, 0,0,100,0, 0,0,0,0, 0,0,100,0),
        /* Conga   */ R_(0,0,70,0, 0,70,0,0, 70,0,0,70, 0,0,70,0),
        /* Tom 1   */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* Tom 2   */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* Claves  */ R_(100,0,0,0, 90,0,0,100, 0,0,100,0, 0,0,90,0),
        /* Snare   */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* ShortCy */ R_(70,70,70,70, 70,70,70,70, 70,70,70,70, 70,70,70,70),
        /* LongCy  */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* CowBell */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
    }},

    /* ──────────────────────────────────── 10 — Mambo (4/4, 16 steps) ──
     * Cuban mambo with 2-3 son clave (positions 2, 5, 8, 11, 13).
     * Cowbell plays cascara, conga on tumbao, bass syncopated. */
    {"Mambo", 16, {
        /* Bass    */ R_(100,0,0,0, 0,0,90,0, 0,0,100,0, 0,0,90,0),
        /* Conga   */ R_(60,0,60,0, 0,0,60,0, 60,0,60,0, 0,0,60,0),
        /* Tom 1   */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* Tom 2   */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* Claves  */ R_(0,0,100,0, 0,100,0,0, 100,0,0,100, 0,100,0,0),
        /* Snare   */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* ShortCy */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* LongCy  */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* CowBell */ R_(110,0,100,0, 110,0,100,0, 110,0,100,0, 110,0,100,0),
    }},

    /* ──────────────────────────────────── 11 — Bossa Nova (4/4, 16 steps) ──
     * One bar of the classic 2-bar bossa pattern. Surdo bass on 1 and 3 with
     * soft prep notes on AND-of-2 and AND-of-4. Bossa 3-2 clave on claves.
     * 16th-note cabasa figure drives the cymbal. */
    {"Bossa-Nova", 16, {
        /* Bass    */ R_(110,0,0,0, 0,0,0,60, 100,0,0,0, 0,0,0,60),
        /* Conga   */ R_(0,0,60,0, 0,60,0,0, 0,60,0,0, 0,60,0,0),
        /* Tom 1   */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* Tom 2   */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* Claves  */ R_(100,0,0,90, 0,0,100,0, 0,0,100,0, 100,0,0,0),
        /* Snare   */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* ShortCy */ R_(70,70,70,70, 70,70,70,70, 70,70,70,70, 70,70,70,70),
        /* LongCy  */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* CowBell */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
    }},

    /* ──────────────────────────────────── 12 — Rock (4/4, 16 steps) ──
     * Standard rock beat. Kick on 1 and 3 with extra kick on AND-of-3,
     * snare on 2 and 4, 8th-note hi-hat. */
    {"Rock", 16, {
        /* Bass    */ R_(110,0,0,0, 0,0,0,0, 100,0,100,0, 0,0,0,0),
        /* Conga   */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* Tom 1   */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* Tom 2   */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* Claves  */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* Snare   */ R_(0,0,0,0, 110,0,0,0, 0,0,0,0, 110,0,0,0),
        /* ShortCy */ R_(80,0,80,0, 80,0,80,0, 80,0,80,0, 80,0,80,0),
        /* LongCy  */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* CowBell */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
    }},

    /* ──────────────────────────────────── 13 — Slow Rock (12/8, 12 steps) ──
     * Triplet-based ballad (each step = 8th-note triplet). Bass on beats 1
     * and 3, snare on beats 2 and 4. Cymbal plays every triplet 8th with
     * accents on the downbeats. */
    {"Slow Rock", 12, {
        /* Bass    */ R_(110,0,0,0, 0,0,100,0, 0,0,0,0),
        /* Conga   */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* Tom 1   */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* Tom 2   */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* Claves  */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* Snare   */ R_(0,0,0,100, 0,0,0,0, 0,100,0,0),
        /* ShortCy */ R_(90,60,60,90, 60,60,90,60, 60,90,60,60),
        /* LongCy  */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* CowBell */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0),
    }},

    /* ──────────────────────────────────── 14 — Shake (4/4 fast, 16 steps) ──
     * 1960s shake/twist beat. 4-on-the-floor kick, hard backbeat snare,
     * heavy 8th-note tambourine on cymbal with strong/weak alternation,
     * crash on the 1. */
    {"Shake", 16, {
        /* Bass    */ R_(100,0,0,0, 100,0,0,0, 100,0,0,0, 100,0,0,0),
        /* Conga   */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* Tom 1   */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* Tom 2   */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* Claves  */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* Snare   */ R_(0,0,0,0, 100,0,0,0, 0,0,0,0, 100,0,0,0),
        /* ShortCy */ R_(100,60,100,60, 100,60,100,60, 100,60,100,60, 100,60,100,60),
        /* LongCy  */ R_(110,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* CowBell */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
    }},

    /* ──────────────────────────────────── 15 — R & Blues (4/4 shuffle, 16 steps) ──
     * 60s R&B / Stax-style shuffle. Syncopated bass on 1, &-of-2, 3, &-of-4.
     * Hard backbeat snare on 2 and 4. Hi-hat 8ths (the shuffle is felt in
     * the swung tempo, not encoded in the grid). */
    {"R & Blues", 16, {
        /* Bass    */ R_(110,0,0,0, 0,0,90,0, 100,0,0,0, 0,0,90,0),
        /* Conga   */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* Tom 1   */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* Tom 2   */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* Claves  */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* Snare   */ R_(0,0,0,0, 100,0,0,0, 0,0,0,0, 100,0,0,0),
        /* ShortCy */ R_(80,0,80,0, 80,0,80,0, 80,0,80,0, 80,0,80,0),
        /* LongCy  */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
        /* CowBell */ R_(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0),
    }},
};

/* ──────────────────────────────────────────────────────────────────────────────
 * Master FX — saturation, multi-mode tape delay, spring reverb
 *
 * The delay supports three authentic voicings selected by `delay_type`:
 *
 *   0 — TAPE      → Maestro Echoplex EP-3   (single head, FET-preamp brightness)
 *   1 — MAGNETIC  → Binson Echorec          (4-tap drum delay, tube warmth)
 *   2 — SPACE     → Roland RE-201 Space Echo (3-tap tape, solid-state, modulated)
 *
 * Each voicing differs in:
 *   - Number and ratios of read taps (multi-head simulation)
 *   - Wow & flutter modulation depth (transport stability)
 *   - HF rolloff per repeat (head/tape generation loss)
 *   - LF rolloff per repeat (DC blocking + AC coupling)
 *   - Saturation curve shape (FET / tube / solid-state-into-tape)
 *   - Feedback cap (each unit's signature self-oscillation behavior)
 *   - Wet preamp coloration (the "EP-3 sweetening" applied to repeats)
 *
 * Sources: Echoplex EP-3 schematic (Schematic Heaven), LTSpice analyses (Seymour
 * Duncan forum), Binson Echorec service manual (Effectrode knowledge base),
 * Roland RE-201 service manual analysis (FreeStompBoxes), Catalinbread tonal
 * descriptions, BlakeC27's EP-3 LTSpice frequency-response measurement
 * (~+6.4 dB peak at 9.5 kHz, slight LF roll-off below 100 Hz).
 * ────────────────────────────────────────────────────────────────────────────── */

#define MAX_TAPS    4

typedef struct {
    int   num_taps;
    float tap_ratios[MAX_TAPS];   /* multipliers of base delay_samples */
    float tap_levels[MAX_TAPS];   /* mix levels (sum normalized inside process) */
    float wow_depth_ms;            /* peak wow modulation depth in ms */
    float flutter_depth_ms;        /* peak flutter modulation depth in ms */
    int   wow_shape;               /* 0 = sine (tape), 1 = drum-eccentric (Echorec dual-sine) */
    float hf_rolloff_coef;         /* one-pole LPF coef in feedback (0..1) — higher = brighter */
    float lf_rolloff_coef;         /* one-pole HPF coef in feedback (0..1) — higher = more LF cut */
    float sat_drive;               /* pre-saturation gain into tanh */
    float sat_asymmetry;           /* +/- bias for even-harmonic content (0=symmetric) */
    float fb_cap;                   /* hard cap on feedback (Tape/Mag ≤0.95, Space 1.05 = self-osc) */
    /* Wet head-bump peaking biquad — replaces the old flat treble/bass scalars.
     * EP-3: pronounced bump at 9.5 kHz from FET preamp. Echorec: small mid-bump.
     * RE-201: nearly flat (the Space Echo's tone is dominated by tape hysteresis,
     * not preamp coloration — the spring reverb on the original is separate). */
    float bump_freq;
    float bump_q;
    float bump_gain_db;
} delay_voicing_t;

/* The three vintage units, calibrated from schematic / measurement data. */
static const delay_voicing_t DELAY_VOICINGS[3] = {
    /* ── 0: Echoplex EP-3 (Tape) ────────────────────────────────────────────
     * Single tape head, FET preamp adds ~+4 dB peak at 9.5 kHz with slight
     * LF cut. Worn pinch roller introduces audible wow ~0.7 Hz (sinusoidal),
     * light flutter ~6.3 Hz. Repeats stay bright (catalinbread: "bright,
     * percussive repeats — NOT dark"). Asymmetric FET clipping when pushed.
     * Head-bump source: BlakeC27 LTSpice analysis of EP-3 preamp + jatin-
     * chowdhury18/AnalogTapeModel head-bump model (peaking biquad, not shelf). */
    {
        .num_taps         = 1,
        .tap_ratios       = {1.0f, 0.0f, 0.0f, 0.0f},
        .tap_levels       = {1.0f, 0.0f, 0.0f, 0.0f},
        .wow_depth_ms     = 0.45f,
        .flutter_depth_ms = 0.10f,
        .wow_shape        = 0,        /* sine */
        .hf_rolloff_coef  = 0.55f,    /* gentle LPF — repeats stay bright */
        .lf_rolloff_coef  = 0.012f,   /* slight HPF (AC coupling) */
        .sat_drive        = 1.4f,
        .sat_asymmetry    = 0.08f,    /* mild even-harmonic, FET asymmetry */
        .fb_cap           = 0.93f,
        .bump_freq        = 9500.0f,  /* EP-3 head-bump peak */
        .bump_q           = 1.2f,
        .bump_gain_db     = 4.0f,
    },
    /* ── 1: Binson Echorec (Magnetic) ───────────────────────────────────────
     * Multi-tap magnetic drum: 4 fixed playback heads at 74, 148, 222, 296 ms
     * (perfect 1:2:3:4 ratio of the user-set base delay time). Drum is far
     * MORE stable than tape but has eccentricity wobble — modeled as a
     * dual-sine LFO (fundamental + 2nd-harmonic offset @ 30% amplitude),
     * which is what gives the Echorec its "almost steady" character.
     * Tube preamp (6× 12AX7) adds warmth, mild mid-bump @ 4 kHz. */
    {
        .num_taps         = 4,
        .tap_ratios       = {0.25f, 0.50f, 0.75f, 1.00f},
        .tap_levels       = {0.55f, 0.70f, 0.85f, 1.00f},   /* later heads stronger (Binson swell) */
        .wow_depth_ms     = 0.08f,
        .flutter_depth_ms = 0.04f,
        .wow_shape        = 1,        /* drum-eccentric dual-sine */
        .hf_rolloff_coef  = 0.32f,    /* steeper LPF — wire/bias HF limit */
        .lf_rolloff_coef  = 0.008f,
        .sat_drive        = 1.7f,
        .sat_asymmetry    = 0.18f,    /* tube-style even-harmonic emphasis */
        .fb_cap           = 0.91f,
        .bump_freq        = 4000.0f,  /* Echorec tube-stage mid-bump */
        .bump_q           = 0.9f,
        .bump_gain_db     = 1.5f,
    },
    /* ── 2: Roland RE-201 Space Echo (Space) ────────────────────────────────
     * 3 fixed playback heads (no spring reverb here — the global reverb
     * stage handles that). Free-floating tape transport → noticeable wow
     * and flutter. Solid-state preamp at 17V, mostly clean but the input
     * clips characteristically when pushed. Tape compression is more
     * audible than EP-3. */
    {
        .num_taps         = 3,
        .tap_ratios       = {0.333f, 0.667f, 1.0f},
        .tap_levels       = {0.65f, 0.82f, 1.0f},
        .wow_depth_ms     = 0.65f,    /* free-floating tape — most modulated */
        .flutter_depth_ms = 0.18f,
        .wow_shape        = 0,        /* sine */
        .hf_rolloff_coef  = 0.45f,
        .lf_rolloff_coef  = 0.010f,
        .sat_drive        = 1.85f,    /* pronounced tape compression */
        .sat_asymmetry    = 0.05f,    /* mostly symmetric (tape) */
        .fb_cap           = 1.05f,    /* >1: SELF-OSCILLATES at high feedback —
                                       * the legendary RE-201 build-up sound. The
                                       * tanh `delay_saturate` inside the feedback
                                       * loop is what bounds the runaway: at low
                                       * signal levels loop gain >1 → oscillation
                                       * grows, at saturation tanh limits to ~0.95
                                       * → controlled steady-state self-osc.       */
        .bump_freq        = 6000.0f,  /* RE-201 has near-flat preamp; tiny mid-tilt */
        .bump_q           = 0.7f,
        .bump_gain_db     = 0.5f,
    },
};

typedef struct {
    float buf_l[DELAY_BUF_SIZE];
    float buf_r[DELAY_BUF_SIZE];
    int   write_idx;
    /* Per-channel feedback path filter states (one-pole LPF + HPF) */
    float fb_lpf_l, fb_lpf_r;
    float fb_hpf_l, fb_hpf_r;
    /* Wow + flutter LFO state (updated per block, applied per sample) */
    float wow_phase;
    float flutter_phase;
    /* Wet head-bump peaking biquads (per voicing — coefs recomputed per block) */
    biquad_t bump_l, bump_r;
} delay_state_t;

/* ──────────────────────────────────────────────────────────────────────────────
 * Multi-mode reverb modeled after three studio classics
 *
 *   0 — PLATE     → EMT 140 (1957)        4×8-foot suspended steel plate.
 *                                         Dense modal cluster, no early reflections,
 *                                         dispersive, slightly metallic upper-mids.
 *                                         Heard on: Autobahn, Phaedra, Cluster, Neu! 75.
 *
 *   1 — SPRING    → AKG BX20 (1965)        Twin torsion-spring tank with motional
 *                                         feedback damping. Distinctive chirp from
 *                                         dispersive wave propagation. Brighter,
 *                                         rattlier, more attack than plate.
 *
 *   2 — CHAMBER   → Live Echo Chamber      Conny Plank / Hansa Meistersaal-style
 *                                         basement/stairwell with speaker→mic.
 *                                         Distinct early reflections + diffuse
 *                                         tail. 3D, asymmetric, less metallic.
 *
 * All three share the same comb+allpass back-end (Schroeder/Moorer); each adds a
 * front-end stage to shape early-time behavior:
 *
 *   Plate:    Dattorro 1997 figure-8 tank — true plate density, stereo out
 *   Spring:   [dispersion allpass cascade (Parker) → comb bank → allpass]
 *   Chamber:  [tapped ER → comb bank → allpass]   (discrete + diffuse)
 *
 * Sources: Dattorro 1997 "Effect Design Part 1: Reverberator and Other Filters"
 * (JAES 45/9); Parker 2011 "Spring Reverb Emulation Using Dispersive Allpass
 * Filters" (DAFx); Bilbao 2009; BlakeC27 EMT 140 LTSpice analysis; UA AKG
 * BX20 / EMT 140 manuals; Schroeder 1962; Moorer 1979.
 * ────────────────────────────────────────────────────────────────────────────── */

typedef struct {
    /* === Spring + Chamber tail: classic Schroeder comb bank + 2 allpass diffusers === */
    float comb_buf[REV_COMB_COUNT][1400];   /* sized for longest comb (1356) + headroom */
    int   comb_idx[REV_COMB_COUNT];
    float comb_lpf[REV_COMB_COUNT];          /* one-pole LPF state in feedback */
    int   comb_len[REV_COMB_COUNT];
    float ap_buf[REV_AP_COUNT][800];
    int   ap_idx[REV_AP_COUNT];
    int   ap_len[REV_AP_COUNT];

    /* === Spring mode: dispersive allpass cascade for BX20 chirp === */
    float disp_buf[REV_DISP_COUNT][REV_DISP_BUFLEN];
    int   disp_idx[REV_DISP_COUNT];
    int   disp_len[REV_DISP_COUNT];

    /* === Chamber mode: tapped early-reflection delay line === */
    float er_buf[REV_ER_BUFLEN];
    int   er_idx;

    /* === Plate mode: Dattorro 1997 figure-8 tank === */
    float dat_pre[256];          int dat_pre_idx;     /* input pre-delay (5.4 ms)        */
    float dat_dif_a[256];        int dat_dif_a_idx;   /* input diffuser 1                */
    float dat_dif_b[256];        int dat_dif_b_idx;   /* input diffuser 2                */
    float dat_dif_c[768];        int dat_dif_c_idx;   /* input diffuser 3                */
    float dat_dif_d[512];        int dat_dif_d_idx;   /* input diffuser 4                */
    float dat_ta_mod_ap[1024];   int dat_ta_mod_ap_idx; /* tank A modulated allpass     */
    float dat_ta_delay1[8192];   int dat_ta_delay1_idx; /* tank A delay 1               */
    float dat_ta_lpf;                                   /* tank A damping LPF state     */
    float dat_ta_static_ap[3072];int dat_ta_static_ap_idx; /* tank A static allpass     */
    float dat_ta_delay2[8192];   int dat_ta_delay2_idx; /* tank A delay 2               */
    float dat_tb_mod_ap[2048];   int dat_tb_mod_ap_idx; /* tank B modulated allpass     */
    float dat_tb_delay1[8192];   int dat_tb_delay1_idx; /* tank B delay 1               */
    float dat_tb_lpf;                                   /* tank B damping LPF state     */
    float dat_tb_static_ap[4096];int dat_tb_static_ap_idx; /* tank B static allpass     */
    float dat_tb_delay2[8192];   int dat_tb_delay2_idx; /* tank B delay 2               */
    float dat_mod_phase;                                /* shared LFO for mod allpasses */
    float dat_cross_a, dat_cross_b;                     /* cross-feedback last-sample   */
} reverb_state_t;

/* Comb delays — primes-ish, classic Schroeder/Moorer values, work for all modes */
static const int COMB_LENGTHS[REV_COMB_COUNT] = {1117, 1188, 1277, 1356};
static const int AP_LENGTHS[REV_AP_COUNT]      = {225, 556};

/* Spring dispersion allpass delays — Parker 2011 / Bilbao 2009 BX20 model.
 * Lengths spread across 30-200 samples (mutually-prime), allpass coefficient
 * 0.75. This produces the *frequency-dependent group delay* characteristic
 * of torsion-spring wave propagation, where high frequencies arrive sooner
 * than low frequencies — the recognizable "downward chirp" of BX20-class
 * tanks. Six longer cascaded allpasses give the right slope without the
 * thinness that 12 short ones produce.                                        */
static const int DISP_LENGTHS[REV_DISP_COUNT] = { 31, 47, 73, 109, 157, 199 };

/* Chamber early-reflection tap positions (samples) and per-tap gains.
 * Times: ~7.0, 13.5, 21, 28.5, 38, 51 ms — typical for a small 4×8m room with
 * speaker→mic at ~3-5m distance, irregular wall positions. Gains decrease with
 * delay (~1/r law for direct→wall→mic distance increases) */
static const int   ER_TAP_SAMPLES[REV_ER_TAPS] = {310, 595, 925, 1255, 1675, 2245};
static const float ER_TAP_GAINS[REV_ER_TAPS]   = {0.85f, 0.70f, 0.55f, 0.45f, 0.35f, 0.25f};

/* Forward-declared state structs for Attitude and bus compressor (functions
 * implemented further down to keep the file's high-level grouping). */
typedef struct {
    /* V72/V76 transformer stage — HP-coupled saturation models the input
     * transformer's bottom-end thickening (the "iron" feel of the V72).      */
    float xfmr_state_l, xfmr_state_r;
    /* Phaser allpass states — 4 stages × 2 channels */
    float ap_x[4], ap_y[4];      /* left */
    float ap_x_r[4], ap_y_r[4];  /* right */
    float lfo_phase;              /* 0..1 over the LFO period */
    /* EQ biquads (per-channel state) */
    biquad_t hpf_l, hpf_r;
    biquad_t body_l, body_r;
    biquad_t air_l, air_r;
    /* Studer A80 head-bump peaking biquads (gain scales with tape_amount) */
    biquad_t tape_bump_l, tape_bump_r;
    /* Synthi-style diode-ladder LPF (replaces the previous ZDF SVF —
     * period-correct for Krautrock: VCS3, Wümme, Tangerine Dream).           */
    ladder_t ladder_l, ladder_r;
    /* Tape post-rolloff one-pole LPF */
    float tape_lpf_l, tape_lpf_r;
} attitude_state_t;

typedef struct {
    float env;       /* envelope follower state (linear, peak detector) */
    float gain;      /* current applied gain (smoothed) */
} compressor_state_t;

/* ──────────────────────────────────────────────────────────────────────────────
 * Instance state
 * ────────────────────────────────────────────────────────────────────────────── */

typedef struct {
    /* === voices === */
    voice_state_t voices[NUM_VOICES];

    /* === sequencer ===
     * rhythm_slots: 8 independent "rhythm channels", each holding either -1
     * (Off) or a rhythm index 0..15. The sequencer ORs together the patterns
     * referenced by all active slots — duplicates are naturally merged into
     * one. Right pads manipulate slots: pressing rhythm N → if N is already
     * in a slot, that slot is cleared; otherwise N takes the first free slot.
     * The 8-slot cap means you can have at most 8 distinct rhythms running.   */
    int           rhythm_slots[NUM_RHYTHM_SLOTS];
    int           clock_step;             /* 0..MAX_RHYTHM_STEPS-1, advances at step rate */
    float         clock_phase;            /* 0..1, advances per-sample, wraps each step */
    float         clock_inc;              /* per-sample increment based on tempo */
    int           sync_to_host;           /* 0 = free, 1 = sync (uses host MIDI clock) */
    int           midi_clock_pulses;      /* counter for 24 PPQN host clock */

    /* === group levels (Menu 1) === */
    float         level[NUM_GROUPS];

    /* === FX (Menu 2) === */
    float         delay_mix, delay_time_norm, delay_fb;
    float         reverb_mix, reverb_decay, reverb_tone;
    float         bus_comp;              /* 0..1 — EMT 156 / Neumann broadcast limiter macro */
    float         all_decay;            /* 0..1 → 1.0× to 4.0× decay multiplier */
    delay_state_t delay;
    reverb_state_t reverb;
    compressor_state_t bus_comp_state;

    /* === Attitude (Menu 3) — pre-FX studio chain coloration === */
    float         preamp_drive;          /* 0..1 — V72/V76 tube preamp drive */
    float         tape_amount;           /* 0..1 — Studer A80 tape print */
    float         hpf_freq;              /* Hz — 30..500 */
    float         eq_body;               /* dB — -6..+6 (low shelf 80 Hz) */
    float         eq_air;                /* dB — -6..+6 (high shelf 6 kHz) */
    float         filter_cutoff;         /* Hz — 100..15000 */
    float         filter_reso;           /* 0..1 — Q from 0.5..10 */
    float         phaser_amount;         /* 0..1 — Phase 90 macro (depth+rate+wet) */
    attitude_state_t attitude;

    /* === General (Menu 4) === */
    float         tempo_bpm;
    int           limiter_on;
    int           delay_type;            /* 0=Tape, 1=Magnetic, 2=Space */
    float         drift_amount;
    /* Continuous slow drift baseline — random walk that the whole machine drifts
     * on, like a tape transport's pitch slowly wandering during a take. The
     * per-trigger Gaussian jitter (in voice_retune) perturbs around this baseline. */
    float         drift_slow_target;
    float         drift_slow_value;       /* current slewed value, ±1 nominal */
    int           reverb_type;            /* 0=Plate, 1=Spring, 2=Chamber */
    float         master_vol;
    float         density;

    /* === Delay sync mode (menu-only) === */
    int           delay_sync;            /* 0 = Free (ms), 1 = Synced (tempo divisions) */

    /* === current page (for knob overlay routing) === */
    int           current_page;          /* 0=KrautDrums, 1=FX, 2=Attitude, 3=General */

    /* === Smoothed knob params (10 ms time constant @ block rate) ===
     * Each smoothed value tracks its target via a one-pole filter applied
     * per-block in render_block. All audio-rate compute paths read the
     * *_s versions so knob movements never click.                              */
    float         level_s[NUM_GROUPS];
    float         delay_mix_s, delay_time_s, delay_fb_s;
    float         reverb_mix_s, reverb_decay_s, reverb_tone_s;
    float         bus_comp_s, all_decay_s;
    float         preamp_drive_s, tape_amount_s;
    float         hpf_freq_s, eq_body_s, eq_air_s;
    float         filter_cutoff_s, filter_reso_s, phaser_amount_s;
    float         master_vol_s, density_s;
} kd_inst_t;

/* ──────────────────────────────────────────────────────────────────────────────
 * Voice helpers
 * ────────────────────────────────────────────────────────────────────────────── */

static float compute_decay_coef(float decay_ms, float all_decay_norm) {
    /* all_decay_norm 0..1 maps to 1.0×..4.0× multiplier */
    float mult = 1.0f + all_decay_norm * 3.0f;
    float dt   = decay_ms * 0.001f * mult;
    if (dt < 0.001f) dt = 0.001f;
    return expf(-1.0f / (dt * SAMPLE_RATE));
}

static void voice_retune(kd_inst_t *inst, int i) {
    const voice_def_t *def = &VOICES[i];
    voice_state_t *v = &inst->voices[i];

    /* Three-layer analog drift model:
     *   1. Slow continuous baseline (±3 cents @ ~0.1 Hz) — the whole machine
     *      drifts on a slow random walk, like tape transport speed wander.
     *   2. Per-trigger Gaussian jitter (±25¢ at 100% drift) — sum-of-3-uniforms
     *      gives a Gaussian-shaped distribution: most hits cluster near nominal,
     *      occasional outliers feel like real component instability.
     *   3. Independent jitter per resonator — multi-resonator voices (snare,
     *      cymbal, cowbell) have res_a and res_b drift separately, modeling
     *      independently-aged 1969 ceramic capacitors. The slight inharmonicity
     *      this creates breathes life into otherwise static sounds. */
    float baseline = inst->drift_slow_value * 3.0f;  /* ±3 ¢ baseline */
    float jitter_a = inst->drift_amount * 25.0f * rand_gaussian();
    float jitter_b = inst->drift_amount * 25.0f * rand_gaussian();  /* independent */
    float drift_a_mult = powf(2.0f, (baseline + jitter_a) / 1200.0f);
    float drift_b_mult = powf(2.0f, (baseline + jitter_b) / 1200.0f);

    /* Store the post-jitter trigger frequencies — the per-block diode-starve
     * drift in render_block reads these to compute the *current* tuning. */
    v->f0_a_trig = def->f0_a * drift_a_mult;
    v->f0_b_trig = (def->f0_b > 0.0f) ? def->f0_b * drift_b_mult : 0.0f;

    biquad_set_bpf(&v->res_a, v->f0_a_trig, def->Q_a, SAMPLE_RATE);
    if (def->f0_b > 0.0f) {
        biquad_set_bpf(&v->res_b, v->f0_b_trig, def->Q_b, SAMPLE_RATE);
    }

    v->decay_coef = compute_decay_coef(def->decay_ms, inst->all_decay_s);
}

/* Plaits-style shaped trigger pulse, replacing the previous one-sample impulse.
 * Émilie Gillet's 808-BD model uses:
 *   1. A 1ms hard pulse, asymmetrically diode-clipped, that falls into a fast
 *      RC decay → this excites the resonator with realistic edge content.
 *   2. A separate 6ms FM pulse that briefly lifts the resonator's centre
 *      frequency at attack — this is the "chirp" that makes a kick feel like
 *      a kick (not a static test tone). Per-voice amount in voice_def_t.
 *   3. A short undershoot post-FM pulse that mimics the diode "retrigger"
 *      behaviour of the bridged-T circuit's RC discharge.
 * See pichenettes/eurorack/plaits/dsp/drums/analog_bass_drum.h for the canonical
 * implementation. The asymmetric diode + self-FM gives the diode-starve
 * frequency drift Vincent's design spec calls for, naturally.                   */
static inline float voice_diode(float x) {
    if (x >= 0.0f) return x;
    x *= 2.0f;
    return 0.7f * x / (1.0f + fabsf(x));
}

static void voice_trigger(kd_inst_t *inst, int i, float velocity) {
    if (i < 0 || i >= NUM_VOICES) return;
    voice_state_t *v = &inst->voices[i];

    voice_retune(inst, i);

    /* Reset filter states to prevent transient chaining */
    biquad_reset(&v->res_a);
    biquad_reset(&v->res_b);

    v->velocity    = clampf(velocity, 0.0f, 1.0f);
    v->env         = 1.0f;
    v->env_active  = 1;

    /* Set up Plaits-style shaped trigger:
     *   pulse_height scales with velocity (3..10 like Plaits' kPulseHeight),
     *   pulse_remaining = 44 samples (1 ms) of hard pulse, then RC decay,
     *   fm_pulse_remaining = 264 samples (6 ms) of attack-FM that adds
     *   ~fm_amount × 1.7 of upward pitch lift via render_block.                */
    v->pulse_height       = 3.0f + 7.0f * v->velocity;
    v->pulse_value        = 0.0f;
    v->pulse_lp           = 0.0f;
    v->fm_pulse_lp        = 0.0f;
    v->retrig_pulse       = 0.0f;
    v->pulse_remaining    = TRIG_PULSE_SAMPLES;
    v->fm_pulse_remaining = TRIG_FM_SAMPLES;
}

/* ──────────────────────────────────────────────────────────────────────────────
 * Tempo / clock helpers
 * ────────────────────────────────────────────────────────────────────────────── */

static void update_clock_inc(kd_inst_t *inst) {
    /* One full revolution per 16th note → 16 steps per beat.
     * step_hz = bpm/60 × 4   (4 sixteenths per beat)
     * inc = step_hz / Fs */
    float step_hz = inst->tempo_bpm * 4.0f / 60.0f;
    inst->clock_inc = step_hz / SAMPLE_RATE;
}

/* ──────────────────────────────────────────────────────────────────────────────
 * Sequencer step — fire all voices that should trigger on this step
 * ────────────────────────────────────────────────────────────────────────────── */

static void sequencer_step(kd_inst_t *inst, int step) {
    /* Derive the active-pattern mask from the 8 rhythm slots. Duplicates
     * across slots collapse into one bit (OR semantics), so the user can
     * safely set the same rhythm on multiple knobs without doubling triggers. */
    uint32_t mask = 0;
    int n_active = 0;
    for (int s = 0; s < NUM_RHYTHM_SLOTS; s++) {
        int r = inst->rhythm_slots[s];
        if (r >= 0 && r < NUM_RHYTHMS && !(mask & (1u << r))) {
            mask |= (1u << r);
            n_active++;
        }
    }
    if (n_active == 0) return;

    /* Density: probabilistic trigger gating (NOT velocity scaling).
     *   p_keep = density × (1 / sqrt(N_active))                                */
    float p_keep = inst->density_s;
    if (n_active > 1) p_keep *= 1.0f / sqrtf((float)n_active);

    /* For each voice slot 0..8, take max velocity across active rhythms at this step */
    for (int vslot = 0; vslot < 9; vslot++) {
        uint8_t max_vel = 0;
        for (int p = 0; p < NUM_RHYTHMS; p++) {
            if (!(mask & (1u << p))) continue;
            const rhythm_def_t *r = &RHYTHMS[p];
            int s = step % r->len_steps;
            uint8_t vel = r->steps[vslot][s];
            if (vel > max_vel) max_vel = vel;
        }
        if (max_vel > 0) {
            if (rand_unipolar() > p_keep) continue;
            voice_trigger(inst, vslot, max_vel / 127.0f);
        }
    }
}

/* ──────────────────────────────────────────────────────────────────────────────
 * Attitude — pre-FX studio-chain coloration (V72/V76 preamp + Studer A80 tape +
 * EQ + diode-ladder LPF + Phase 90 phaser)
 *
 * Signal flow:  in → V72 transformer stage → V72 tube saturation
 *                  → A80 tape saturation → A80 head-bump biquad → tape HF rolloff
 *                  → HPF → low-shelf (body) → high-shelf (air)
 *                  → diode-ladder LPF (Synthi-class) → 4-stage phaser → out
 *
 * Modeling notes (post upgrade, sources cited in code):
 *  - V72 transformer:  HP-coupled fast tanh on the 50 Hz one-pole HP residue,
 *                       blended back with the original — mimics the input
 *                       transformer's bottom-end thickening (the "iron" feel).
 *                       This is the single piece that distinguishes a real
 *                       Telefunken V72 from a generic asymmetric saturator.
 *  - V72 tube:          DC-bias asymmetric tanh — adds 2nd-harmonic correlated
 *                       to the input (the audible "tube" signature). Combined
 *                       with the transformer stage above we get the full V72.
 *  - A80 tape:          Cubic-clip soft saturation + drive-scaled HF rolloff
 *                       (18 kHz → 8 kHz with tape_amount) + peaking head-bump
 *                       at ~120 Hz (gain scales 0..+2 dB with tape_amount).
 *                       The head bump is the missing low-mid thickness that
 *                       gives "tape feel" — sourced from CHOWTapeModel's
 *                       calcHeadBumpFilter() with A80-typical values.
 *  - Diode ladder LPF:  4-stage one-pole cascade with tanh-clipped feedback
 *                       (Stilson/Smith ZDF ladder). Period-correct for
 *                       Krautrock — Faust's Wümme had a Synthi A, Tangerine
 *                       Dream used VCS3 extensively, Cluster used Moog
 *                       ladders. Replaces the previous ZDF SVF.
 *  - Phaser:            4-stage 1-pole allpass cascade. Single Amount knob
 *                       maps to BOTH rate (log 0.05 → 0.6 Hz) and depth+wet:
 *                       0–50% → slow rate growing depth, 50–100% → faster
 *                       rate + full depth. Krautrock-vibe behaviour: vibe
 *                       intensity *and* speed grow together.
 * ────────────────────────────────────────────────────────────────────────────── */

/* (attitude_state_t struct moved above kd_inst_t for forward use) */

static void attitude_init(attitude_state_t *a) {
    memset(a, 0, sizeof(*a));
    /* EQ + filter biquads — render_block recomputes coefs per block based on
     * current param values. Initial coefs are identity-ish so the first block
     * after instantiation produces predictable output. */
    biquad_set_hpf      (&a->hpf_l,       60.0f,   0.707f, SAMPLE_RATE);
    biquad_set_hpf      (&a->hpf_r,       60.0f,   0.707f, SAMPLE_RATE);
    biquad_set_lowshelf (&a->body_l,      80.0f,   0.0f,   SAMPLE_RATE);
    biquad_set_lowshelf (&a->body_r,      80.0f,   0.0f,   SAMPLE_RATE);
    biquad_set_highshelf(&a->air_l,     6000.0f,   0.0f,   SAMPLE_RATE);
    biquad_set_highshelf(&a->air_r,     6000.0f,   0.0f,   SAMPLE_RATE);
    biquad_set_peaking  (&a->tape_bump_l, 120.0f,  1.5f,   0.0f, SAMPLE_RATE);
    biquad_set_peaking  (&a->tape_bump_r, 120.0f,  1.5f,   0.0f, SAMPLE_RATE);
    ladder_reset(&a->ladder_l);
    ladder_reset(&a->ladder_r);
}

/* V72/V76 input transformer stage — high-pass coupled tanh shaped through
 * a slow integrator. The HP residue is saturated to add 2nd-harmonic
 * thickening on transients while leaving steady-state bass linear.
 * This produces the bottom-end "iron" feel that distinguishes real V72s.       */
static inline float v72_transformer(float x, float *state) {
    float hp  = x - *state;
    *state   += 0.005f * hp + DENORM_EPS;       /* ~35 Hz one-pole integrator */
    /* Saturate the HP residue, then re-add the integrated bass.
     * 0.6 mix keeps the effect subtle at default drive.                         */
    return fast_tanh(hp * 1.2f) * 0.6f + (*state);
}

/* V72/V76 tube stage — DC-bias asymmetric tanh, pulls in 2nd-harmonic content
 * correlated with the input. The output is NOT normalized by drive — driving
 * the stage harder should make it both LOUDER and saturated, matching how a
 * real tube preamp behaves (output transformer compresses dynamics, perceived
 * loudness rises with input level).                                            */
static inline float preamp_saturate(float x, float drive) {
    if (drive < 1.001f) return x;                       /* bypass at knob 0% */
    float biased = x * drive + 0.10f;
    return fast_tanh(biased) - fast_tanh(0.10f);
}

/* Studer A80 tape stage — cubic soft-clip emphasising 3rd-harmonic content
 * (dominant tape-hysteresis product). Like the preamp, no post-normalization:
 * the gain IS part of the tape character.                                      */
static inline float tape_saturate(float x, float drive) {
    if (drive < 1.001f) return x;                       /* bypass at knob 0% */
    float y = x * drive;
    if (y > 1.5f)       y = 1.0f;
    else if (y < -1.5f) y = -1.0f;
    else                y = y - (y * y * y) * (1.0f / 6.75f);
    return y;
}

/* 4-stage 1-pole allpass cascade — the Phase 90 / Maestro PS-1A model.
 * coef = (1 - g) / (1 + g) where g = tan(pi * f / sr). f sweeps with LFO. */
static inline float phaser_4stage(float in, float coef, float *x, float *y) {
    float v = in;
    for (int i = 0; i < 4; i++) {
        float new_y = -coef * v + x[i] + coef * y[i];
        x[i] = v;
        y[i] = new_y + DENORM_EPS;
        v = new_y;
    }
    return v;
}

/* Process one stereo sample through the full Attitude chain. All expensive
 * coefficients (HPF, body/air shelves, tape head-bump, diode-ladder g/k,
 * phaser coefs) are precomputed once per block in render_block and passed
 * in here to keep per-sample cost transcendental-free.                          */
static inline void attitude_process(attitude_state_t *a,
                                    float *l, float *r,
                                    float preamp_pregain, float tape_pregain,
                                    float tape_lpf_coef,
                                    float ladder_g, float ladder_k,
                                    float phaser_coef_l, float phaser_coef_r,
                                    float phaser_wet) {
    /* === 1. V72 transformer stage (low-end iron) === */
    float xl = v72_transformer(*l, &a->xfmr_state_l);
    float xr = v72_transformer(*r, &a->xfmr_state_r);

    /* === 2. V72 tube saturation (asymmetric tanh) === */
    xl = preamp_saturate(xl, preamp_pregain);
    xr = preamp_saturate(xr, preamp_pregain);

    /* === 3. A80 tape saturation (cubic clip) === */
    xl = tape_saturate(xl, tape_pregain);
    xr = tape_saturate(xr, tape_pregain);

    /* === 4. A80 tape head-bump (peaking biquad ~120 Hz, gain scales w/ tape) === */
    xl = biquad_tick(&a->tape_bump_l, xl);
    xr = biquad_tick(&a->tape_bump_r, xr);

    /* === 5. Tape HF rolloff (one-pole LPF, coef scales with tape_amount) === */
    a->tape_lpf_l += tape_lpf_coef * (xl - a->tape_lpf_l) + DENORM_EPS;
    a->tape_lpf_r += tape_lpf_coef * (xr - a->tape_lpf_r) + DENORM_EPS;
    xl = a->tape_lpf_l;
    xr = a->tape_lpf_r;

    /* === 6. HPF (Butterworth) === */
    xl = biquad_tick(&a->hpf_l, xl);
    xr = biquad_tick(&a->hpf_r, xr);

    /* === 7. Body — low shelf at 80 Hz === */
    xl = biquad_tick(&a->body_l, xl);
    xr = biquad_tick(&a->body_r, xr);

    /* === 8. Air — high shelf at 6 kHz === */
    xl = biquad_tick(&a->air_l, xl);
    xr = biquad_tick(&a->air_r, xr);

    /* === 9. Synthi-style diode-ladder LPF (replaces previous SVF) === */
    xl = ladder_process(&a->ladder_l, xl, ladder_g, ladder_k);
    xr = ladder_process(&a->ladder_r, xr, ladder_g, ladder_k);

    /* === 10. Phase 90 phaser (rate + depth grow with amount) === */
    if (phaser_wet > 0.001f) {
        float phl = phaser_4stage(xl, phaser_coef_l, a->ap_x,   a->ap_y);
        float phr = phaser_4stage(xr, phaser_coef_r, a->ap_x_r, a->ap_y_r);
        xl = xl * (1.0f - phaser_wet * 0.5f) + phl * phaser_wet * 0.5f;
        xr = xr * (1.0f - phaser_wet * 0.5f) + phr * phaser_wet * 0.5f;
    }

    *l = xl;
    *r = xr;
}

/* ──────────────────────────────────────────────────────────────────────────────
 * Bus Compressor — EMT 156 / Neumann broadcast limiter character
 *
 * Single-knob macro feedback peak compressor:
 *   - Stereo-linked detection (max of |L|, |R|) for coherent gain reduction
 *   - Slow attack (~8 ms) lets drum transients pass before clamping body
 *   - Medium release (~150 ms) — "lifts" the natural decay of the drums
 *   - Soft 4:1 ratio with auto-makeup gain
 *   - Threshold descends from 0 dB → -12 dB as amount knob rises
 *   - Slight 4kHz emphasis recovers air after compression (transformer trait)
 *
 * The EMT 156 was a transformer-coupled feedback compressor used widely in
 * European broadcast/studio settings. Neumann's PEM 8 / W444STA channel-strip
 * limiters were similar program-dependent peak limiters built into consoles.
 * Both prized for their "musical glue" rather than aggressive pumping.
 * ────────────────────────────────────────────────────────────────────────────── */

/* (compressor_state_t struct moved above kd_inst_t for forward use) */

static void compressor_init(compressor_state_t *c) {
    c->env  = 0.0f;
    c->gain = 1.0f;
}

static inline void compressor_process(compressor_state_t *c,
                                      float *l, float *r,
                                      float amount) {
    if (amount < 0.001f) return;  /* bypass */

    /* Feed-forward peak detection — for drum bus material with transients
     * in the 0.2..0.5 range, FF is more responsive than FB.                    */
    float det = fmaxf(fabsf(*l), fabsf(*r));

    /* Sub-millisecond attack range, slow release for "broadcast limiter" pump */
    float attack_ms     = 1.0f - amount * 0.95f;       /* 1.0 .. 0.05 ms */
    float attack_coef   = 1.0f - expf(-1.0f / (attack_ms * 0.001f * SAMPLE_RATE));
    const float release_coef = 0.000151f;              /* 150 ms */
    float coef = (det > c->env) ? attack_coef : release_coef;
    c->env += coef * (det - c->env) + DENORM_EPS;

    /* Hard peak limiter (∞:1 ratio) + amount-scaled makeup. This is the EMT
     * 156 / Neumann broadcast-limiter character: anything above threshold is
     * clamped to threshold, then a global makeup boost pumps everything back
     * up. Quiet sections get boosted (knob effect audible at all levels),
     * loud transients get crushed (pumping = the comp signature).
     *
     *   threshold sweep: 0.40 → 0.02  (heavy comp at 100%)
     *   makeup        : 1× → 4×       (+12 dB at 100% — restores loudness)    */
    float threshold = 0.40f - amount * 0.38f;
    float makeup    = 1.0f + amount * 3.0f;

    float target_gain = makeup;          /* boost applied even when below threshold */
    if (c->env > threshold) {
        target_gain = (threshold / c->env) * makeup;
    }

    /* Faster gain smoothing so attack actually reaches the limiter target
     * (~0.1 ms time const at coef=0.2). */
    c->gain += 0.2f * (target_gain - c->gain);

    *l *= c->gain;
    *r *= c->gain;
}

/* ──────────────────────────────────────────────────────────────────────────────
 * FX — multi-tap modeled delay (EP-3 / Echorec / RE-201)
 * ────────────────────────────────────────────────────────────────────────────── */

static void delay_init(delay_state_t *d) {
    memset(d, 0, sizeof(*d));
}

static int delay_samples_from_norm(float time_norm) {
    /* Exponential 10ms..1000ms (the user's "base" delay time, which is the
     * longest tap on multi-tap modes) */
    float t = 0.010f * powf(100.0f, time_norm);
    int n = (int)(t * SAMPLE_RATE);
    if (n < 16) n = 16;
    if (n >= DELAY_BUF_SIZE / 2) n = DELAY_BUF_SIZE / 2;
    return n;
}

/* Asymmetric tanh-based saturation modeling each unit's preamp character.
 *   - Tape (FET):  mild even-harmonic asymmetry, gentle compression
 *   - Tube:        more even-harmonic content, smoother knee
 *   - SS+tape:     near-symmetric with audible compression when pushed
 * The asymmetry is implemented by adding a DC bias before tanh and removing
 * it after — produces 2nd-harmonic content musically correlated with input. */
static inline float delay_saturate(float x, float drive, float asym) {
    float biased = x * drive + asym;
    float clipped = fast_tanh(biased);
    /* Remove the DC introduced by the bias */
    return (clipped - fast_tanh(asym)) / drive;
}

/* Linear-interpolated read from a circular buffer. read_pos may be fractional
 * and may wrap (negative or >= BUF_SIZE). */
static inline float delay_tap_read(const float *buf, float read_pos) {
    /* Wrap to [0, DELAY_BUF_SIZE) */
    while (read_pos < 0.0f)              read_pos += (float)DELAY_BUF_SIZE;
    while (read_pos >= (float)DELAY_BUF_SIZE) read_pos -= (float)DELAY_BUF_SIZE;
    int   idx0 = (int)read_pos;
    float frac = read_pos - (float)idx0;
    int   idx1 = idx0 + 1;
    if (idx1 >= DELAY_BUF_SIZE) idx1 = 0;
    return buf[idx0] * (1.0f - frac) + buf[idx1] * frac;
}

/* Process one stereo sample through the modeled delay. `wow_mod` and
 * `flutter_mod` are LFO values in [-1, +1] computed once per block. */
static void delay_process(delay_state_t *d,
                          float in_l, float in_r,
                          float *out_l, float *out_r,
                          int base_delay_samples, float feedback,
                          const delay_voicing_t *v,
                          float wow_mod, float flutter_mod) {
    /* Combined modulation in samples, applied to all taps */
    float wow_samps     = (v->wow_depth_ms     * 0.001f * SAMPLE_RATE) * wow_mod;
    float flutter_samps = (v->flutter_depth_ms * 0.001f * SAMPLE_RATE) * flutter_mod;
    float mod = wow_samps + flutter_samps;

    /* Sum the read taps. Compute total tap level for output normalization. */
    float echo_l = 0.0f, echo_r = 0.0f;
    float total_level = 0.0f;
    for (int t = 0; t < v->num_taps; t++) {
        float tap_delay = (float)base_delay_samples * v->tap_ratios[t] + mod;
        if (tap_delay < 1.0f) tap_delay = 1.0f;
        float read_pos_l = (float)d->write_idx - tap_delay;
        float read_pos_r = (float)d->write_idx - tap_delay;
        echo_l += delay_tap_read(d->buf_l, read_pos_l) * v->tap_levels[t];
        echo_r += delay_tap_read(d->buf_r, read_pos_r) * v->tap_levels[t];
        total_level += v->tap_levels[t];
    }
    /* Normalize the multi-tap sum so the wet level is consistent across modes */
    float norm = (total_level > 0.0f) ? 1.0f / total_level : 1.0f;
    echo_l *= norm;
    echo_r *= norm;

    /* Wet head-bump biquad — peaking EQ models each unit's preamp signature:
     *   EP-3:    +4 dB peak at 9.5 kHz (Q=1.2)  — the FET-preamp shimmer
     *   Echorec: +1.5 dB peak at 4 kHz (Q=0.9)  — tube-stage mid-bump
     *   RE-201:  +0.5 dB peak at 6 kHz (Q=0.7)  — nearly flat preamp
     * Coefficients are recomputed once per block in render_block when the
     * voicing changes; we just tick the biquads here.                          */
    float out_l_pre = biquad_tick(&d->bump_l, echo_l);
    float out_r_pre = biquad_tick(&d->bump_r, echo_r);
    *out_l = out_l_pre;
    *out_r = out_r_pre;

    /* Feedback-path tone shaping: HPF → LPF → saturation
     * 1) HPF (one-pole "Direct Form I" style): state += coef*(in - state); out = in - state */
    d->fb_hpf_l += v->lf_rolloff_coef * (echo_l - d->fb_hpf_l) + DENORM_EPS;
    d->fb_hpf_r += v->lf_rolloff_coef * (echo_r - d->fb_hpf_r) + DENORM_EPS;
    float hp_l = echo_l - d->fb_hpf_l;
    float hp_r = echo_r - d->fb_hpf_r;
    /* 2) LPF (HF generation loss per repeat) */
    d->fb_lpf_l += v->hf_rolloff_coef * (hp_l - d->fb_lpf_l) + DENORM_EPS;
    d->fb_lpf_r += v->hf_rolloff_coef * (hp_r - d->fb_lpf_r) + DENORM_EPS;
    /* 3) Saturation models the preamp character */
    float fb_l = delay_saturate(d->fb_lpf_l, v->sat_drive, v->sat_asymmetry);
    float fb_r = delay_saturate(d->fb_lpf_r, v->sat_drive, v->sat_asymmetry);

    /* Cap feedback per unit's signature behavior */
    float fb_eff = feedback * v->fb_cap;

    /* Cross-feedback for ping-pong stereo movement */
    d->buf_l[d->write_idx] = in_l + fb_r * fb_eff;
    d->buf_r[d->write_idx] = in_r + fb_l * fb_eff;

    d->write_idx++;
    if (d->write_idx >= DELAY_BUF_SIZE) d->write_idx = 0;
}

/* ──────────────────────────────────────────────────────────────────────────────
 * FX — multi-mode reverb (EMT 140 / AKG BX20 / Echo Chamber)
 * ────────────────────────────────────────────────────────────────────────────── */

static void reverb_init(reverb_state_t *r) {
    memset(r, 0, sizeof(*r));
    for (int i = 0; i < REV_COMB_COUNT; i++) r->comb_len[i] = COMB_LENGTHS[i];
    for (int i = 0; i < REV_AP_COUNT; i++)    r->ap_len[i]   = AP_LENGTHS[i];
    for (int i = 0; i < REV_DISP_COUNT; i++)  r->disp_len[i] = DISP_LENGTHS[i];
}

static inline float comb_process(reverb_state_t *r, int i, float in, float fb_gain, float damp) {
    int idx = r->comb_idx[i];
    int len = r->comb_len[i];
    if (idx >= len) idx = 0;
    float delayed = r->comb_buf[i][idx];
    /* One-pole LPF in feedback for damping (denormal-guarded) */
    r->comb_lpf[i] += damp * (delayed - r->comb_lpf[i]) + DENORM_EPS;
    r->comb_buf[i][idx] = in + r->comb_lpf[i] * fb_gain;
    idx++;
    if (idx >= len) idx = 0;
    r->comb_idx[i] = idx;
    return delayed;
}

static inline float ap_process(reverb_state_t *r, int i, float in) {
    const float ap_gain = 0.5f;
    int idx = r->ap_idx[i];
    int len = r->ap_len[i];
    if (idx >= len) idx = 0;
    float delayed = r->ap_buf[i][idx];
    float out = -in + delayed;
    /* Denormal-guarded write */
    r->ap_buf[i][idx] = in + delayed * ap_gain + DENORM_EPS;
    idx++;
    if (idx >= len) idx = 0;
    r->ap_idx[i] = idx;
    return out;
}

/* Schroeder allpass for spring dispersion cascade.
 * Each stage: y[n] = -g*x[n] + x[n-N] + g*y[n-N], with N samples of delay.
 * Cascading produces the characteristic "chirp" of dispersive wave propagation. */
static inline float disp_ap_process(reverb_state_t *r, int i, float in, float g) {
    int idx = r->disp_idx[i];
    int len = r->disp_len[i];
    if (idx >= len) idx = 0;
    float delayed = r->disp_buf[i][idx];
    float out = -g * in + delayed;
    r->disp_buf[i][idx] = in + g * out + DENORM_EPS;
    idx++;
    if (idx >= len) idx = 0;
    r->disp_idx[i] = idx;
    return out;
}

/* Run the common comb-bank + 2 series allpass tail.
 * Used by all three modes; differences come from input pre-processing. */
static inline float reverb_tail(reverb_state_t *r, float in, float fb_gain, float damp) {
    float comb_sum = 0.0f;
    for (int i = 0; i < REV_COMB_COUNT; i++) {
        comb_sum += comb_process(r, i, in, fb_gain, damp);
    }
    comb_sum *= 0.25f;  /* normalize 4 comb sum */
    float ap = ap_process(r, 0, comb_sum);
    ap = ap_process(r, 1, ap);
    return ap;
}

/* ── 0: PLATE (EMT 140) — Dattorro 1997 figure-8 tank ──────────────────────
 * Topology (per Dattorro JAES 1997 §3):
 *   in → pre-delay → 4 input diffusers (allpass cascade)
 *      → split into two cross-coupled tanks. Each tank is:
 *          [modulated_ap → delay1 → damping_lpf → static_ap → delay2]
 *      Tank A's output (×decay) feeds tank B's input, and vice-versa.
 *      Stereo output = weighted multi-tap sum from both tanks (Dattorro's
 *      published tap positions, scaled to 44.1 kHz).
 * The figure-8 cross-coupling is what gives this its true "plate" density —
 * the tail is dense and slightly metallic from sample 0, with no early-
 * reflection comb-filter "chorus-y" signature.                                */
static void reverb_process_plate_stereo(reverb_state_t *r, float in,
                                        float decay_norm, float tone_norm,
                                        float *out_l, float *out_r) {
    /* Decay range: 0.10..0.95 — much wider than Dattorro's stock 0.5..0.85.
     * At 0.10 the cross-feedback is weak → tail dies in ~250 ms (audible but
     * snappy). At 0.95 the tank approaches self-sustain → 10 sec EMT-140
     * lush tail Cluster favoured. Floor pushed low so even "minimum" decay
     * is genuinely short for a drum machine context.                          */
    float decay = 0.10f + decay_norm * 0.85f;
    /* Damping LPF coefficient — controls dark→bright tone of the tail.
     * tone_norm=0 → heavy damping (dark/dub plate), tone_norm=1 → bright EMT */
    float damp  = 0.05f + (1.0f - tone_norm) * 0.55f;

    /* Input diffuser allpass coefficients (Dattorro §3 fig 4) */
    const float dif_g_ab = 0.75f;
    const float dif_g_cd = 0.625f;
    /* Tank allpass coefficients */
    const float ta_mod_g    = -0.7f;
    const float ta_static_g =  0.5f;
    const float tb_mod_g    = -0.7f;
    const float tb_static_g =  0.5f;

    /* === Pre-delay === */
    r->dat_pre[r->dat_pre_idx] = in;
    int read = r->dat_pre_idx - DAT_PRE_LEN;
    if (read < 0) read += 256;
    float x = r->dat_pre[read];
    r->dat_pre_idx = (r->dat_pre_idx + 1) & 255;

    /* === 4 input diffusers (series allpass cascade) — schroeder allpass
     *   y[n] = -g·x[n] + d[n-N];   d[n] = x[n] + g·y[n]                       */
    {
        int idx = r->dat_dif_a_idx;
        int rd  = idx - DAT_DIF_A_LEN; if (rd < 0) rd += 256;
        float d = r->dat_dif_a[rd];
        float y = -dif_g_ab * x + d;
        r->dat_dif_a[idx] = x + dif_g_ab * y + DENORM_EPS;
        r->dat_dif_a_idx  = (idx + 1) & 255;
        x = y;
    }
    {
        int idx = r->dat_dif_b_idx;
        int rd  = idx - DAT_DIF_B_LEN; if (rd < 0) rd += 256;
        float d = r->dat_dif_b[rd];
        float y = -dif_g_ab * x + d;
        r->dat_dif_b[idx] = x + dif_g_ab * y + DENORM_EPS;
        r->dat_dif_b_idx  = (idx + 1) & 255;
        x = y;
    }
    {
        int idx = r->dat_dif_c_idx;
        int rd  = idx - DAT_DIF_C_LEN; if (rd < 0) rd += 768;
        float d = r->dat_dif_c[rd];
        float y = -dif_g_cd * x + d;
        r->dat_dif_c[idx] = x + dif_g_cd * y + DENORM_EPS;
        r->dat_dif_c_idx  = (idx + 1) % 768;
        x = y;
    }
    {
        int idx = r->dat_dif_d_idx;
        int rd  = idx - DAT_DIF_D_LEN; if (rd < 0) rd += 512;
        float d = r->dat_dif_d[rd];
        float y = -dif_g_cd * x + d;
        r->dat_dif_d[idx] = x + dif_g_cd * y + DENORM_EPS;
        r->dat_dif_d_idx  = (idx + 1) & 511;
        x = y;
    }

    /* === LFO advance for the two modulated allpasses (Dattorro §3.3)         */
    r->dat_mod_phase += 1.0f / SAMPLE_RATE;       /* ~1 Hz LFO */
    if (r->dat_mod_phase >= 1.0f) r->dat_mod_phase -= 1.0f;
    float lfo  = sinf(r->dat_mod_phase * 2.0f * M_PI);
    int   mod_a_off = (int)(lfo * 8.0f);          /* ±8 sample mod */
    int   mod_b_off = (int)(-lfo * 8.0f);

    /* === Tank A === */
    float ta_in = x + r->dat_cross_b * decay;     /* cross-feedback from tank B */
    /* modulated allpass */
    {
        int idx = r->dat_ta_mod_ap_idx;
        int rd  = idx - (DAT_TA_MOD_AP_LEN + mod_a_off);
        rd = ((rd % 1024) + 1024) % 1024;
        float d = r->dat_ta_mod_ap[rd];
        float y = -ta_mod_g * ta_in + d;
        r->dat_ta_mod_ap[idx] = ta_in + ta_mod_g * y + DENORM_EPS;
        r->dat_ta_mod_ap_idx  = (idx + 1) & 1023;
        ta_in = y;
    }
    /* delay 1 */
    r->dat_ta_delay1[r->dat_ta_delay1_idx] = ta_in + DENORM_EPS;
    int ta_d1_rd = r->dat_ta_delay1_idx - DAT_TA_DELAY1_LEN;
    if (ta_d1_rd < 0) ta_d1_rd += 8192;
    float ta_d1 = r->dat_ta_delay1[ta_d1_rd];
    r->dat_ta_delay1_idx = (r->dat_ta_delay1_idx + 1) & 8191;
    /* damping LPF (one-pole) */
    r->dat_ta_lpf += damp * (ta_d1 - r->dat_ta_lpf) + DENORM_EPS;
    float ta_post_lpf = r->dat_ta_lpf;
    /* static allpass */
    float ta_post_ap;
    {
        int idx = r->dat_ta_static_ap_idx;
        int rd  = idx - DAT_TA_STATIC_AP_LEN;
        if (rd < 0) rd += 3072;
        float d = r->dat_ta_static_ap[rd];
        float y = -ta_static_g * ta_post_lpf + d;
        r->dat_ta_static_ap[idx] = ta_post_lpf + ta_static_g * y + DENORM_EPS;
        r->dat_ta_static_ap_idx  = (idx + 1) % 3072;
        ta_post_ap = y;
    }
    /* delay 2 → cross-feedback to tank B */
    r->dat_ta_delay2[r->dat_ta_delay2_idx] = ta_post_ap + DENORM_EPS;
    int ta_d2_rd = r->dat_ta_delay2_idx - DAT_TA_DELAY2_LEN;
    if (ta_d2_rd < 0) ta_d2_rd += 8192;
    r->dat_cross_a = r->dat_ta_delay2[ta_d2_rd];
    r->dat_ta_delay2_idx = (r->dat_ta_delay2_idx + 1) & 8191;

    /* === Tank B === */
    float tb_in = x + r->dat_cross_a * decay;     /* cross-feedback from tank A */
    {
        int idx = r->dat_tb_mod_ap_idx;
        int rd  = idx - (DAT_TB_MOD_AP_LEN + mod_b_off);
        rd = ((rd % 2048) + 2048) % 2048;
        float d = r->dat_tb_mod_ap[rd];
        float y = -tb_mod_g * tb_in + d;
        r->dat_tb_mod_ap[idx] = tb_in + tb_mod_g * y + DENORM_EPS;
        r->dat_tb_mod_ap_idx  = (idx + 1) & 2047;
        tb_in = y;
    }
    r->dat_tb_delay1[r->dat_tb_delay1_idx] = tb_in + DENORM_EPS;
    int tb_d1_rd = r->dat_tb_delay1_idx - DAT_TB_DELAY1_LEN;
    if (tb_d1_rd < 0) tb_d1_rd += 8192;
    float tb_d1 = r->dat_tb_delay1[tb_d1_rd];
    r->dat_tb_delay1_idx = (r->dat_tb_delay1_idx + 1) & 8191;
    r->dat_tb_lpf += damp * (tb_d1 - r->dat_tb_lpf) + DENORM_EPS;
    float tb_post_lpf = r->dat_tb_lpf;
    float tb_post_ap;
    {
        int idx = r->dat_tb_static_ap_idx;
        int rd  = idx - DAT_TB_STATIC_AP_LEN;
        if (rd < 0) rd += 4096;
        float d = r->dat_tb_static_ap[rd];
        float y = -tb_static_g * tb_post_lpf + d;
        r->dat_tb_static_ap[idx] = tb_post_lpf + tb_static_g * y + DENORM_EPS;
        r->dat_tb_static_ap_idx  = (idx + 1) & 4095;
        tb_post_ap = y;
    }
    r->dat_tb_delay2[r->dat_tb_delay2_idx] = tb_post_ap + DENORM_EPS;
    int tb_d2_rd = r->dat_tb_delay2_idx - DAT_TB_DELAY2_LEN;
    if (tb_d2_rd < 0) tb_d2_rd += 8192;
    r->dat_cross_b = r->dat_tb_delay2[tb_d2_rd];
    r->dat_tb_delay2_idx = (r->dat_tb_delay2_idx + 1) & 8191;

    /* === Output: Dattorro's published multi-tap weighted sum (44.1k-scaled) ===
     *   y_l = a[394] + a[4401] - b[2831] + c[2954] - d[2945] - e[277] - f[1578]
     *   y_r = (mirror)
     * Where a..f are tank delays/allpasses. Tap weights are all 0.6.           */
    int   tA_d1_idx = r->dat_ta_delay1_idx;
    int   tA_d2_idx = r->dat_ta_delay2_idx;
    int   tB_d1_idx = r->dat_tb_delay1_idx;
    int   tB_d2_idx = r->dat_tb_delay2_idx;
    int   tA_ap_idx = r->dat_ta_static_ap_idx;
    int   tB_ap_idx = r->dat_tb_static_ap_idx;

    #define TAP_TA_DELAY1(o) ({ int i = tA_d1_idx - (o); if (i<0) i += 8192; r->dat_ta_delay1[i]; })
    #define TAP_TA_DELAY2(o) ({ int i = tA_d2_idx - (o); if (i<0) i += 8192; r->dat_ta_delay2[i]; })
    #define TAP_TB_DELAY1(o) ({ int i = tB_d1_idx - (o); if (i<0) i += 8192; r->dat_tb_delay1[i]; })
    #define TAP_TB_DELAY2(o) ({ int i = tB_d2_idx - (o); if (i<0) i += 8192; r->dat_tb_delay2[i]; })
    #define TAP_TA_STATIC(o) ({ int i = tA_ap_idx - (o); if (i<0) i += 3072; r->dat_ta_static_ap[i]; })
    #define TAP_TB_STATIC(o) ({ int i = tB_ap_idx - (o); if (i<0) i += 4096; r->dat_tb_static_ap[i]; })

    float yL =  TAP_TA_DELAY1(394)  + TAP_TA_DELAY1(4401)
              - TAP_TA_STATIC(2831) + TAP_TA_DELAY2(2954)
              - TAP_TB_DELAY1(2945) - TAP_TB_STATIC(277)
              - TAP_TB_DELAY2(1578);
    float yR =  TAP_TB_DELAY1(522)  + TAP_TB_DELAY1(5368)
              - TAP_TB_STATIC(1817) + TAP_TB_DELAY2(3956)
              - TAP_TA_DELAY1(3124) - TAP_TA_STATIC(496)
              - TAP_TA_DELAY2(179);

    #undef TAP_TA_DELAY1
    #undef TAP_TA_DELAY2
    #undef TAP_TB_DELAY1
    #undef TAP_TB_DELAY2
    #undef TAP_TA_STATIC
    #undef TAP_TB_STATIC

    *out_l = yL * 0.45f;   /* dropped from 0.6 — Dattorro tap sum is naturally
                            * dense; lower scaling keeps the wet from
                            * overwhelming the dry at moderate reverb_mix     */
    *out_r = yR * 0.45f;
}

/* ── 1: SPRING (AKG BX20) ───────────────────────────────────────────────────
 * Cascaded short Schroeder allpasses produce the dispersive chirp characteristic
 * of torsion-spring wave propagation (Smith/Pakarinen, Parker). The cascade
 * output then feeds the comb bank for the diffuse late tail. The result is a
 * brighter, "rattlier" impulse with the recognizable spring attack — but
 * smoother than a classic Hammond-style longitudinal spring (BX20 was prized
 * for its dense, plate-like late tail despite the spring front-end). */
static float reverb_process_spring(reverb_state_t *r, float in,
                                   float decay_norm, float tone_norm) {
    /* Allpass coefficient pushed to 0.75 (Parker 2011 recommendation for
     * BX20-class tanks): produces sharper dispersive chirp slope so high
     * frequencies clearly arrive before low ones — the recognizable spring
     * "boing" attack. Combined with the longer 30-200 sample dispersion
     * delays, this gives a much more authentic torsion-spring character
     * than the previous short-allpass cascade.                                 */
    const float disp_g = 0.75f;

    /* Run the cascade (chirp generator) */
    float x = in;
    for (int i = 0; i < REV_DISP_COUNT; i++) {
        x = disp_ap_process(r, i, x, disp_g);
    }

    /* Spring decay range — slightly tighter than plate (motional feedback damping) */
    float fb_gain = 0.62f + decay_norm * 0.28f;
    /* Spring is naturally brighter — push damp range toward less attenuation */
    float damp    = 0.08f + (1.0f - tone_norm) * 0.35f;

    return reverb_tail(r, x, fb_gain, damp);
}

/* ── 2: CHAMBER (Live Echo Room) ────────────────────────────────────────────
 * Tapped early reflection delay line provides the discrete first-order
 * reflections (speaker → walls → mic at multiple distances). The tail bus then
 * feeds the comb bank for the diffuse late field. The combination gives the
 * "3D" feel that distinguishes a real chamber from electromechanical reverbs. */
static float reverb_process_chamber(reverb_state_t *r, float in,
                                    float decay_norm, float tone_norm) {
    /* Write input to ER delay buffer */
    r->er_buf[r->er_idx] = in;
    /* Sum the early reflection taps */
    float er_sum = 0.0f;
    for (int t = 0; t < REV_ER_TAPS; t++) {
        int read_idx = r->er_idx - ER_TAP_SAMPLES[t];
        while (read_idx < 0) read_idx += REV_ER_BUFLEN;
        er_sum += r->er_buf[read_idx] * ER_TAP_GAINS[t];
    }
    /* Advance er_idx */
    r->er_idx++;
    if (r->er_idx >= REV_ER_BUFLEN) r->er_idx = 0;
    /* Normalize ER sum */
    er_sum *= 0.35f;  /* scale so summed taps don't dwarf the diffuse tail */

    /* Chamber tail — longer decay, brighter (hard reflective walls) */
    float fb_gain = 0.70f + decay_norm * 0.25f;
    float damp    = 0.04f + (1.0f - tone_norm) * 0.30f;
    /* Mix the input direct + ER taps as the tail input */
    float tail_in = in * 0.4f + er_sum;
    float tail = reverb_tail(r, tail_in, fb_gain, damp);

    /* Output = ER + tail. Both contribute to the spatial sense. */
    return er_sum + tail;
}

/* Dispatcher — selects the modeled topology per reverb_type. Output is stereo:
 * Plate (Dattorro) is natively stereo. Spring and Chamber are mono internally;
 * we duplicate to both channels so the rest of the chain stays uniform.        */
static void reverb_process(reverb_state_t *r, int reverb_type, float in,
                           float decay_norm, float tone_norm,
                           float *out_l, float *out_r) {
    if (reverb_type == 1) {
        float m = reverb_process_spring(r, in, decay_norm, tone_norm);
        *out_l = m; *out_r = m;
        return;
    }
    if (reverb_type == 2) {
        float m = reverb_process_chamber(r, in, decay_norm, tone_norm);
        *out_l = m; *out_r = m;
        return;
    }
    reverb_process_plate_stereo(r, in, decay_norm, tone_norm, out_l, out_r);
}

/* ──────────────────────────────────────────────────────────────────────────────
 * Knob mapping table — for popup overlay (knob_N_name / knob_N_value)
 * ────────────────────────────────────────────────────────────────────────────── */

typedef struct { const char *key; const char *label; } knob_def_t;

static const knob_def_t KNOB_MAP_PAGE[5][8] = {
    /* Page 0 — KrautDrums (voice levels) */
    {
        {"lvl_bass",    "Bass"},
        {"lvl_conga",   "Conga"},
        {"lvl_tom1",    "Tom 1"},
        {"lvl_tom2",    "Tom 2"},
        {"lvl_claves",  "Claves"},
        {"lvl_snare",   "Snare"},
        {"lvl_cowbell", "Cow Bell"},
        {"lvl_cymbals", "Cymbals"},
    },
    /* Page 1 — FX */
    {
        {"delay_mix",    "Delay Mix"},
        {"delay_time",   "Delay Time"},
        {"delay_fb",     "Delay Fdbk"},
        {"reverb_mix",   "Reverb Mix"},
        {"reverb_decay", "Reverb Dcy"},
        {"reverb_tone",  "Reverb Tone"},
        {"bus_comp",     "Bus Comp"},
        {"all_decay",    "All Decay"},
    },
    /* Page 2 — Attitude (V72/V76 preamp + Studer tape + EQ + filter + phaser) */
    {
        {"preamp_drive", "Drive"},
        {"tape_amount",  "Tape"},
        {"hpf_freq",     "HPF"},
        {"eq_body",      "Body"},
        {"eq_air",       "Air"},
        {"filter_cutoff","Cutoff"},
        {"filter_reso",  "Reso"},
        {"phaser_amount","Phaser"},
    },
    /* Page 3 — General. delay_type and reverb_type moved to the FX menu as
     * menu-only params, freeing knobs 5 and 6. master_vol and density shift
     * up to fill those slots; knobs 7 and 8 are unused on this page.          */
    {
        {"tempo",        "Tempo"},
        {"tempo_mode",   "Tempo Mode"},
        {"drift",        "Drift"},
        {"limiter",      "Limiter"},
        {"master_vol",   "M.Volume"},
        {"density",      "Density"},
        {"",             ""},        /* unused */
        {"",             ""},        /* unused */
    },
    /* Page 4 — Rhythms. 8 enum knobs, each independently selects a rhythm
     * pattern (or Off). Active mask is OR'd from non-Off slots. Right pads
     * (notes 52-67) also manipulate these slots — pad press toggles a
     * rhythm into the first free slot (or clears it if already present).     */
    {
        {"rhythm_1", "Rhythm 1"},
        {"rhythm_2", "Rhythm 2"},
        {"rhythm_3", "Rhythm 3"},
        {"rhythm_4", "Rhythm 4"},
        {"rhythm_5", "Rhythm 5"},
        {"rhythm_6", "Rhythm 6"},
        {"rhythm_7", "Rhythm 7"},
        {"rhythm_8", "Rhythm 8"},
    },
};

/* ──────────────────────────────────────────────────────────────────────────────
 * Lifecycle
 * ────────────────────────────────────────────────────────────────────────────── */

static void *create_instance(const char *module_dir, const char *json_defaults) {
    (void)module_dir;
    (void)json_defaults;
    kd_inst_t *inst = (kd_inst_t *)calloc(1, sizeof(kd_inst_t));
    if (!inst) return NULL;

    /* Default group levels */
    inst->level[VOICE_GROUP_BASS]    = 0.85f;
    inst->level[VOICE_GROUP_CONGA]   = 0.65f;
    inst->level[VOICE_GROUP_TOM1]    = 0.70f;
    inst->level[VOICE_GROUP_TOM2]    = 0.70f;
    inst->level[VOICE_GROUP_CLAVES]  = 0.55f;
    inst->level[VOICE_GROUP_SNARE]   = 0.80f;
    inst->level[VOICE_GROUP_COWBELL] = 0.60f;
    inst->level[VOICE_GROUP_CYMBALS] = 0.55f;

    /* Default FX — fully dry by default; user dials in delay/reverb to taste */
    inst->delay_mix       = 0.0f;
    inst->delay_time_norm = 0.35f;
    inst->delay_fb        = 0.35f;
    inst->reverb_mix      = 0.0f;
    inst->reverb_decay    = 0.55f;
    inst->reverb_tone     = 0.50f;
    inst->bus_comp        = 0.0f;     /* off by default — let user dial in */
    inst->all_decay       = 0.0f;

    /* Default Attitude — clean by default; Drive/Tape stay off until user opts in */
    inst->preamp_drive    = 0.0f;
    inst->tape_amount     = 0.25f;
    inst->hpf_freq        = 60.0f;     /* Hz */
    inst->eq_body         = 0.0f;      /* dB */
    inst->eq_air          = 0.0f;      /* dB */
    inst->filter_cutoff   = 15000.0f;  /* Hz — effectively open */
    inst->filter_reso     = 0.20f;
    inst->phaser_amount   = 0.0f;      /* off */

    /* Default General */
    inst->tempo_bpm       = 120.0f;
    inst->sync_to_host    = 1;     /* Sync */
    inst->limiter_on      = 1;     /* On */
    inst->delay_type      = 0;     /* Tape (Echoplex EP-3) */
    inst->reverb_type     = 0;     /* Plate (EMT 140) */
    inst->drift_amount    = 0.15f;
    inst->master_vol      = 0.85f;
    inst->density         = 1.00f;     /* every step fires when 1 pattern is active;
                                          higher-N combinations get thinned by 1/sqrt(N) */
    inst->delay_sync      = 0;         /* Free (ms) by default */
    for (int i = 0; i < NUM_RHYTHM_SLOTS; i++) inst->rhythm_slots[i] = -1;  /* all slots Off */

    /* Seed smoothed companions to target values so first block uses correct
     * parameters (no startup zip from zero → target).                         */
    for (int i = 0; i < NUM_GROUPS; i++) inst->level_s[i] = inst->level[i];
    inst->delay_mix_s     = inst->delay_mix;
    inst->delay_time_s    = inst->delay_time_norm;
    inst->delay_fb_s      = inst->delay_fb;
    inst->reverb_mix_s    = inst->reverb_mix;
    inst->reverb_decay_s  = inst->reverb_decay;
    inst->reverb_tone_s   = inst->reverb_tone;
    inst->bus_comp_s      = inst->bus_comp;
    inst->all_decay_s     = inst->all_decay;
    inst->preamp_drive_s  = inst->preamp_drive;
    inst->tape_amount_s   = inst->tape_amount;
    inst->hpf_freq_s      = inst->hpf_freq;
    inst->eq_body_s       = inst->eq_body;
    inst->eq_air_s        = inst->eq_air;
    inst->filter_cutoff_s = inst->filter_cutoff;
    inst->filter_reso_s   = inst->filter_reso;
    inst->phaser_amount_s = inst->phaser_amount;
    inst->master_vol_s    = inst->master_vol;
    inst->density_s       = inst->density;

    /* Compute initial coefficients for each voice */
    for (int i = 0; i < NUM_VOICES; i++) {
        biquad_set_bpf(&inst->voices[i].res_a, VOICES[i].f0_a, VOICES[i].Q_a, SAMPLE_RATE);
        if (VOICES[i].f0_b > 0.0f) {
            biquad_set_bpf(&inst->voices[i].res_b, VOICES[i].f0_b, VOICES[i].Q_b, SAMPLE_RATE);
        }
        inst->voices[i].decay_coef = compute_decay_coef(VOICES[i].decay_ms, inst->all_decay);
    }

    /* Init FX + Attitude + Compressor */
    delay_init(&inst->delay);
    reverb_init(&inst->reverb);
    attitude_init(&inst->attitude);
    compressor_init(&inst->bus_comp_state);

    update_clock_inc(inst);

    return inst;
}

static void destroy_instance(void *instance) {
    free(instance);
}

/* ──────────────────────────────────────────────────────────────────────────────
 * MIDI handling
 * ────────────────────────────────────────────────────────────────────────────── */

static void on_midi(void *instance, const uint8_t *msg, int len, int source) {
    (void)source;
    kd_inst_t *inst = (kd_inst_t *)instance;
    if (len < 1) return;

    uint8_t status = msg[0];

    /* MIDI clock messages */
    if (status == 0xF8) {
        /* 24 PPQN clock pulse */
        inst->midi_clock_pulses++;
        if (inst->sync_to_host) {
            /* 6 pulses per 16th note (24/4) */
            if (inst->midi_clock_pulses >= 6) {
                inst->midi_clock_pulses = 0;
                /* Advance step on the next sample boundary in render_block */
                inst->clock_phase = 1.0f;  /* trigger immediate step at top of next block */
            }
        }
        return;
    }
    if (status == 0xFA) {
        /* Start */
        inst->clock_step = 0;
        inst->clock_phase = 0.0f;
        inst->midi_clock_pulses = 0;
        return;
    }
    if (status == 0xFC) {
        /* Stop */
        return;
    }

    if (len < 3) return;
    uint8_t cmd   = status & 0xF0;
    uint8_t data1 = msg[1];
    uint8_t data2 = msg[2];

    if (cmd == 0x90 && data2 > 0) {
        /* Note On — Drum Kit template:
         *   left half  (36-51) → momentary voice trigger
         *   right half (52-67) → latching rhythm-pattern toggle via slots      */
        int voice = note_to_voice_idx(data1);
        if (voice >= 0) {
            voice_trigger(inst, voice, data2 / 127.0f);
            return;
        }
        int rhythm = note_to_rhythm_idx(data1);
        if (rhythm >= 0) {
            /* If rhythm is already in a slot, clear that slot (toggle-off).
             * Otherwise assign to the first free slot (toggle-on). When all
             * 8 slots are taken, the press is ignored.                          */
            int found = -1;
            for (int s = 0; s < NUM_RHYTHM_SLOTS; s++) {
                if (inst->rhythm_slots[s] == rhythm) { found = s; break; }
            }
            if (found >= 0) {
                inst->rhythm_slots[found] = -1;
            } else {
                for (int s = 0; s < NUM_RHYTHM_SLOTS; s++) {
                    if (inst->rhythm_slots[s] < 0) {
                        inst->rhythm_slots[s] = rhythm;
                        break;
                    }
                }
            }
            return;
        }
    }
    /* Note Off ignored: voices are one-shots, rhythm pads are toggle-on-press */
}

/* ──────────────────────────────────────────────────────────────────────────────
 * Parameter dispatch
 * ────────────────────────────────────────────────────────────────────────────── */

static int parse_enum(const char *val, const char *const *opts, int n_opts) {
    for (int i = 0; i < n_opts; i++) {
        if (strcmp(val, opts[i]) == 0) return i;
    }
    /* Numeric fallback, bounded — defends against corrupted state files
     * that might serialize an out-of-range index. */
    int i = atoi(val);
    if (i < 0 || i >= n_opts) i = 0;
    return i;
}

static const char *TEMPO_MODE_OPTS[2]  = {"Free", "Sync"};
static const char *LIMITER_OPTS[2]     = {"Off",  "On"};
static const char *DELAY_TYPE_OPTS[3]  = {"Tape", "Magnetic", "Space"};
static const char *REVERB_TYPE_OPTS[3] = {"Plate", "Spring", "Chamber"};
static const char *DELAY_SYNC_OPTS[2]  = {"Free", "Synced"};

/* Rhythm enum — index 0 = "Off", 1..16 = pattern indices (RHYTHMS[0..15]).
 * (NUM_RHYTHM_SLOTS is also declared in the top-of-file constants block so
 * kd_inst_t can reference it.) */
static const char *RHYTHM_OPTS[NUM_RHYTHM_OPTS] = {
    "Off", "Waltz", "Tango", "Polka", "Paso Doble", "Swing", "Slow", "Beguine",
    "Cha-Cha", "Rumba", "Samba", "Mambo", "Bossa-Nova", "Rock", "Slow Rock",
    "Shake", "R & Blues"
};

/* Tempo-divisions table for synced delay. notes_per_beat: 1/4 note = 1.0.
 *   delay_ms = 60000 / (bpm × notes_per_beat)
 * E.g. 1/4 @ 120 BPM = 500 ms; 1/16T @ 120 BPM = 83.3 ms.
 *
 * Ordered SHORTEST → LONGEST so knob position 0 = quickest echo (1/64),
 * knob position 1 = longest echo (1/1 = whole note). Matches user intuition
 * of "low knob = short time, high knob = long time".                          */
#define NUM_DIVISIONS 12
static const char *DIV_NAMES[NUM_DIVISIONS] = {
    "1/64", "1/32T", "1/32", "1/16T", "1/16", "1/8T",
    "1/8",  "1/4T",  "1/4",  "1/2T",  "1/2",  "1/1"
};
static const float DIV_NOTES_PER_BEAT[NUM_DIVISIONS] = {
    16.0f, 12.0f, 8.0f, 6.0f, 4.0f, 3.0f,
    2.0f,  1.5f,  1.0f, 0.75f, 0.5f, 0.25f
};

/* Map a normalized knob value (0..1) to a tempo division index */
static inline int knob_to_div_idx(float norm) {
    int idx = (int)(norm * (float)(NUM_DIVISIONS - 1) + 0.5f);
    if (idx < 0) idx = 0;
    if (idx >= NUM_DIVISIONS) idx = NUM_DIVISIONS - 1;
    return idx;
}
static void set_param(void *instance, const char *key, const char *val) {
    kd_inst_t *inst = (kd_inst_t *)instance;
    if (!key || !val) return;

    /* Page navigation: Schwung sends set_param("_level", "<PageName>") */
    if (strcmp(key, "_level") == 0) {
        if      (strcmp(val, "Kraut") == 0)      inst->current_page = 0;
        else if (strcmp(val, "FX") == 0)         inst->current_page = 1;
        else if (strcmp(val, "Attitude") == 0)   inst->current_page = 2;
        else if (strcmp(val, "General") == 0)    inst->current_page = 3;
        else if (strcmp(val, "Rhythms") == 0)    inst->current_page = 4;
        return;
    }

    /* Voice levels */
    if (strcmp(key, "lvl_bass")    == 0) { inst->level[VOICE_GROUP_BASS]    = clampf((float)atof(val), 0, 1); return; }
    if (strcmp(key, "lvl_conga")   == 0) { inst->level[VOICE_GROUP_CONGA]   = clampf((float)atof(val), 0, 1); return; }
    if (strcmp(key, "lvl_tom1")    == 0) { inst->level[VOICE_GROUP_TOM1]    = clampf((float)atof(val), 0, 1); return; }
    if (strcmp(key, "lvl_tom2")    == 0) { inst->level[VOICE_GROUP_TOM2]    = clampf((float)atof(val), 0, 1); return; }
    if (strcmp(key, "lvl_claves")  == 0) { inst->level[VOICE_GROUP_CLAVES]  = clampf((float)atof(val), 0, 1); return; }
    if (strcmp(key, "lvl_snare")   == 0) { inst->level[VOICE_GROUP_SNARE]   = clampf((float)atof(val), 0, 1); return; }
    if (strcmp(key, "lvl_cowbell") == 0) { inst->level[VOICE_GROUP_COWBELL] = clampf((float)atof(val), 0, 1); return; }
    if (strcmp(key, "lvl_cymbals") == 0) { inst->level[VOICE_GROUP_CYMBALS] = clampf((float)atof(val), 0, 1); return; }

    /* FX */
    if (strcmp(key, "delay_mix")    == 0) { inst->delay_mix       = clampf((float)atof(val), 0, 1); return; }
    if (strcmp(key, "delay_time")   == 0) { inst->delay_time_norm = clampf((float)atof(val), 0, 1); return; }
    if (strcmp(key, "delay_fb")     == 0) { inst->delay_fb        = clampf((float)atof(val), 0, 0.95f); return; }
    if (strcmp(key, "reverb_mix")   == 0) { inst->reverb_mix      = clampf((float)atof(val), 0, 1); return; }
    if (strcmp(key, "reverb_decay") == 0) { inst->reverb_decay    = clampf((float)atof(val), 0, 1); return; }
    if (strcmp(key, "reverb_tone")  == 0) { inst->reverb_tone     = clampf((float)atof(val), 0, 1); return; }
    if (strcmp(key, "bus_comp")     == 0) { inst->bus_comp        = clampf((float)atof(val), 0, 1); return; }
    if (strcmp(key, "all_decay")    == 0) {
        inst->all_decay = clampf((float)atof(val), 0, 1);
        /* render_block re-computes each active voice's decay_coef per-block
         * from the smoothed all_decay_s, so the knob audibly affects voices
         * that are already ringing — no need to recompute synchronously here. */
        return;
    }

    /* Attitude */
    if (strcmp(key, "preamp_drive") == 0) { inst->preamp_drive = clampf((float)atof(val), 0, 1); return; }
    if (strcmp(key, "tape_amount")  == 0) { inst->tape_amount  = clampf((float)atof(val), 0, 1); return; }
    if (strcmp(key, "hpf_freq")     == 0) { inst->hpf_freq     = clampf((float)atof(val), 30.0f, 500.0f); return; }
    if (strcmp(key, "eq_body")      == 0) { inst->eq_body      = clampf((float)atof(val), -6.0f, 6.0f); return; }
    if (strcmp(key, "eq_air")       == 0) { inst->eq_air       = clampf((float)atof(val), -6.0f, 6.0f); return; }
    if (strcmp(key, "filter_cutoff")== 0) { inst->filter_cutoff= clampf((float)atof(val), 100.0f, 15000.0f); return; }
    if (strcmp(key, "filter_reso")  == 0) { inst->filter_reso  = clampf((float)atof(val), 0, 1); return; }
    if (strcmp(key, "phaser_amount")== 0) { inst->phaser_amount= clampf((float)atof(val), 0, 1); return; }

    /* General */
    if (strcmp(key, "tempo") == 0) {
        inst->tempo_bpm = clampf((float)atof(val), 60.0f, 200.0f);
        update_clock_inc(inst);
        return;
    }
    if (strcmp(key, "tempo_mode") == 0) {
        inst->sync_to_host = parse_enum(val, TEMPO_MODE_OPTS, 2);
        return;
    }
    if (strcmp(key, "drift")        == 0) { inst->drift_amount   = clampf((float)atof(val), 0, 1); return; }
    if (strcmp(key, "limiter")      == 0) { inst->limiter_on     = parse_enum(val, LIMITER_OPTS, 2); return; }
    if (strcmp(key, "delay_type")   == 0) { inst->delay_type     = parse_enum(val, DELAY_TYPE_OPTS, 3); return; }
    if (strcmp(key, "reverb_type")  == 0) { inst->reverb_type    = parse_enum(val, REVERB_TYPE_OPTS, 3); return; }
    if (strcmp(key, "master_vol")   == 0) { inst->master_vol     = clampf((float)atof(val), 0, 1); return; }
    if (strcmp(key, "density")      == 0) { inst->density        = clampf((float)atof(val), 0, 1); return; }
    if (strcmp(key, "delay_sync")   == 0) { inst->delay_sync     = parse_enum(val, DELAY_SYNC_OPTS, 2); return; }

    /* Rhythm slots — 8 enum knobs, each holds Off (idx 0) or a rhythm 1..16. */
    if (strncmp(key, "rhythm_", 7) == 0) {
        int slot = atoi(key + 7) - 1;
        if (slot < 0 || slot >= NUM_RHYTHM_SLOTS) return;
        int idx = parse_enum(val, RHYTHM_OPTS, NUM_RHYTHM_OPTS);
        inst->rhythm_slots[slot] = (idx <= 0) ? -1 : (idx - 1);
        return;
    }

    /* State serialization (deserialize). 41 fields = 32 original + delay_sync
     * + 8 rhythm slots. Older state strings parse the first 32 or 33 cleanly,
     * leaving newer fields at their defaults.                                  */
    if (strcmp(key, "state") == 0) {
        int tempo_mode_i, limiter_i, delay_type_i, reverb_type_i, delay_sync_i = 0;
        int rs[NUM_RHYTHM_SLOTS] = {-1, -1, -1, -1, -1, -1, -1, -1};
        int n = sscanf(val,
            "%f %f %f %f %f %f %f %f "    /* 8 levels */
            "%f %f %f %f %f %f %f %f "    /* 8 fx */
            "%f %f %f %f %f %f %f %f "    /* 8 attitude */
            "%f %d %f %d %d %d %f %f "    /* 8 general */
            "%d "                          /* delay_sync */
            "%d %d %d %d %d %d %d %d",    /* 8 rhythm slots */
            &inst->level[0], &inst->level[1], &inst->level[2], &inst->level[3],
            &inst->level[4], &inst->level[5], &inst->level[6], &inst->level[7],
            &inst->delay_mix, &inst->delay_time_norm, &inst->delay_fb,
            &inst->reverb_mix, &inst->reverb_decay, &inst->reverb_tone,
            &inst->bus_comp, &inst->all_decay,
            &inst->preamp_drive, &inst->tape_amount, &inst->hpf_freq,
            &inst->eq_body, &inst->eq_air,
            &inst->filter_cutoff, &inst->filter_reso, &inst->phaser_amount,
            &inst->tempo_bpm, &tempo_mode_i, &inst->drift_amount,
            &limiter_i, &delay_type_i, &reverb_type_i,
            &inst->master_vol, &inst->density,
            &delay_sync_i,
            &rs[0], &rs[1], &rs[2], &rs[3], &rs[4], &rs[5], &rs[6], &rs[7]);
        if (n >= 32) {
            inst->sync_to_host = tempo_mode_i;
            inst->limiter_on   = limiter_i;
            inst->delay_type   = delay_type_i;
            inst->reverb_type  = reverb_type_i;
            update_clock_inc(inst);
            for (int i = 0; i < NUM_VOICES; i++) {
                inst->voices[i].decay_coef = compute_decay_coef(VOICES[i].decay_ms, inst->all_decay);
            }
            if (n >= 33) {
                if (delay_sync_i < 0 || delay_sync_i > 1) delay_sync_i = 0;
                inst->delay_sync = delay_sync_i;
            }
            if (n >= 41) {
                for (int s = 0; s < NUM_RHYTHM_SLOTS; s++) {
                    if (rs[s] < -1 || rs[s] >= NUM_RHYTHMS) rs[s] = -1;
                    inst->rhythm_slots[s] = rs[s];
                }
            }
        }
        return;
    }
}

/* ──────────────────────────────────────────────────────────────────────────────
 * get_param — return values, knob overlays, chain_params, etc.
 * ────────────────────────────────────────────────────────────────────────────── */

/* The full chain_params JSON is too big for stack-allocated snprintf chains.
 * We embed it as a static const char* and use memcpy. */
static const char CHAIN_PARAMS_JSON[] =
"["
  "{\"key\":\"lvl_bass\",\"name\":\"Bass\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
  "{\"key\":\"lvl_conga\",\"name\":\"Conga\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
  "{\"key\":\"lvl_tom1\",\"name\":\"Tom 1\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
  "{\"key\":\"lvl_tom2\",\"name\":\"Tom 2\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
  "{\"key\":\"lvl_claves\",\"name\":\"Claves\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
  "{\"key\":\"lvl_snare\",\"name\":\"Snare\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
  "{\"key\":\"lvl_cowbell\",\"name\":\"Cow Bell\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
  "{\"key\":\"lvl_cymbals\",\"name\":\"Cymbals\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
  "{\"key\":\"delay_mix\",\"name\":\"Delay Mix\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
  "{\"key\":\"delay_time\",\"name\":\"Delay Time\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
  "{\"key\":\"delay_fb\",\"name\":\"Delay Fdbk\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
  "{\"key\":\"reverb_mix\",\"name\":\"Reverb Mix\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
  "{\"key\":\"reverb_decay\",\"name\":\"Reverb Dcy\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
  "{\"key\":\"reverb_tone\",\"name\":\"Reverb Tone\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
  "{\"key\":\"bus_comp\",\"name\":\"Bus Comp\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
  "{\"key\":\"all_decay\",\"name\":\"All Decay\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
  "{\"key\":\"preamp_drive\",\"name\":\"Drive\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
  "{\"key\":\"tape_amount\",\"name\":\"Tape\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
  "{\"key\":\"hpf_freq\",\"name\":\"HPF\",\"type\":\"float\",\"min\":30,\"max\":500,\"step\":1},"
  "{\"key\":\"eq_body\",\"name\":\"Body\",\"type\":\"float\",\"min\":-6,\"max\":6,\"step\":0.1},"
  "{\"key\":\"eq_air\",\"name\":\"Air\",\"type\":\"float\",\"min\":-6,\"max\":6,\"step\":0.1},"
  "{\"key\":\"filter_cutoff\",\"name\":\"Cutoff\",\"type\":\"float\",\"min\":100,\"max\":15000,\"step\":50},"
  "{\"key\":\"filter_reso\",\"name\":\"Reso\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
  "{\"key\":\"phaser_amount\",\"name\":\"Phaser\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
  "{\"key\":\"tempo\",\"name\":\"Tempo\",\"type\":\"float\",\"min\":60,\"max\":200,\"step\":1},"
  "{\"key\":\"tempo_mode\",\"name\":\"Tempo Mode\",\"type\":\"enum\",\"options\":[\"Free\",\"Sync\"]},"
  "{\"key\":\"drift\",\"name\":\"Drift\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
  "{\"key\":\"limiter\",\"name\":\"Limiter\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"]},"
  "{\"key\":\"delay_type\",\"name\":\"Delay Type\",\"type\":\"enum\",\"options\":[\"Tape\",\"Magnetic\",\"Space\"]},"
  "{\"key\":\"reverb_type\",\"name\":\"Reverb Mode\",\"type\":\"enum\",\"options\":[\"Plate\",\"Spring\",\"Chamber\"]},"
  "{\"key\":\"master_vol\",\"name\":\"M.Volume\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
  "{\"key\":\"density\",\"name\":\"Density\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
  "{\"key\":\"delay_sync\",\"name\":\"Delay Sync\",\"type\":\"enum\",\"options\":[\"Free\",\"Synced\"]},"
  "{\"key\":\"rhythm_1\",\"name\":\"Rhythm 1\",\"type\":\"enum\",\"options\":[\"Off\",\"Waltz\",\"Tango\",\"Polka\",\"Paso Doble\",\"Swing\",\"Slow\",\"Beguine\",\"Cha-Cha\",\"Rumba\",\"Samba\",\"Mambo\",\"Bossa-Nova\",\"Rock\",\"Slow Rock\",\"Shake\",\"R & Blues\"]},"
  "{\"key\":\"rhythm_2\",\"name\":\"Rhythm 2\",\"type\":\"enum\",\"options\":[\"Off\",\"Waltz\",\"Tango\",\"Polka\",\"Paso Doble\",\"Swing\",\"Slow\",\"Beguine\",\"Cha-Cha\",\"Rumba\",\"Samba\",\"Mambo\",\"Bossa-Nova\",\"Rock\",\"Slow Rock\",\"Shake\",\"R & Blues\"]},"
  "{\"key\":\"rhythm_3\",\"name\":\"Rhythm 3\",\"type\":\"enum\",\"options\":[\"Off\",\"Waltz\",\"Tango\",\"Polka\",\"Paso Doble\",\"Swing\",\"Slow\",\"Beguine\",\"Cha-Cha\",\"Rumba\",\"Samba\",\"Mambo\",\"Bossa-Nova\",\"Rock\",\"Slow Rock\",\"Shake\",\"R & Blues\"]},"
  "{\"key\":\"rhythm_4\",\"name\":\"Rhythm 4\",\"type\":\"enum\",\"options\":[\"Off\",\"Waltz\",\"Tango\",\"Polka\",\"Paso Doble\",\"Swing\",\"Slow\",\"Beguine\",\"Cha-Cha\",\"Rumba\",\"Samba\",\"Mambo\",\"Bossa-Nova\",\"Rock\",\"Slow Rock\",\"Shake\",\"R & Blues\"]},"
  "{\"key\":\"rhythm_5\",\"name\":\"Rhythm 5\",\"type\":\"enum\",\"options\":[\"Off\",\"Waltz\",\"Tango\",\"Polka\",\"Paso Doble\",\"Swing\",\"Slow\",\"Beguine\",\"Cha-Cha\",\"Rumba\",\"Samba\",\"Mambo\",\"Bossa-Nova\",\"Rock\",\"Slow Rock\",\"Shake\",\"R & Blues\"]},"
  "{\"key\":\"rhythm_6\",\"name\":\"Rhythm 6\",\"type\":\"enum\",\"options\":[\"Off\",\"Waltz\",\"Tango\",\"Polka\",\"Paso Doble\",\"Swing\",\"Slow\",\"Beguine\",\"Cha-Cha\",\"Rumba\",\"Samba\",\"Mambo\",\"Bossa-Nova\",\"Rock\",\"Slow Rock\",\"Shake\",\"R & Blues\"]},"
  "{\"key\":\"rhythm_7\",\"name\":\"Rhythm 7\",\"type\":\"enum\",\"options\":[\"Off\",\"Waltz\",\"Tango\",\"Polka\",\"Paso Doble\",\"Swing\",\"Slow\",\"Beguine\",\"Cha-Cha\",\"Rumba\",\"Samba\",\"Mambo\",\"Bossa-Nova\",\"Rock\",\"Slow Rock\",\"Shake\",\"R & Blues\"]},"
  "{\"key\":\"rhythm_8\",\"name\":\"Rhythm 8\",\"type\":\"enum\",\"options\":[\"Off\",\"Waltz\",\"Tango\",\"Polka\",\"Paso Doble\",\"Swing\",\"Slow\",\"Beguine\",\"Cha-Cha\",\"Rumba\",\"Samba\",\"Mambo\",\"Bossa-Nova\",\"Rock\",\"Slow Rock\",\"Shake\",\"R & Blues\"]}"
"]";

/* ui_hierarchy: returned from get_param so the Shadow UI gets it via DSP fallback path.
 * (The same JSON is also in module.json — both paths are needed.) */
static const char UI_HIERARCHY_JSON[] =
"{"
  "\"modes\":null,"
  "\"levels\":{"
    "\"root\":{"
      "\"name\":\"KD\","
      "\"knobs\":[\"lvl_bass\",\"lvl_conga\",\"lvl_tom1\",\"lvl_tom2\",\"lvl_claves\",\"lvl_snare\",\"lvl_cowbell\",\"lvl_cymbals\"],"
      "\"params\":["
        "{\"level\":\"Kraut\",\"label\":\"Kraut\"},"
        "{\"level\":\"FX\",\"label\":\"FX\"},"
        "{\"level\":\"Attitude\",\"label\":\"Attitude\"},"
        "{\"level\":\"General\",\"label\":\"General\"},"
        "{\"level\":\"Rhythms\",\"label\":\"Rhythms\"}"
      "]"
    "},"
    "\"Kraut\":{"
      "\"label\":\"Kraut\","
      "\"knobs\":[\"lvl_bass\",\"lvl_conga\",\"lvl_tom1\",\"lvl_tom2\",\"lvl_claves\",\"lvl_snare\",\"lvl_cowbell\",\"lvl_cymbals\"],"
      "\"params\":["
        "{\"key\":\"lvl_bass\",\"label\":\"Bass\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
        "{\"key\":\"lvl_conga\",\"label\":\"Conga\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
        "{\"key\":\"lvl_tom1\",\"label\":\"Tom 1\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
        "{\"key\":\"lvl_tom2\",\"label\":\"Tom 2\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
        "{\"key\":\"lvl_claves\",\"label\":\"Claves\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
        "{\"key\":\"lvl_snare\",\"label\":\"Snare\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
        "{\"key\":\"lvl_cowbell\",\"label\":\"Cow Bell\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
        "{\"key\":\"lvl_cymbals\",\"label\":\"Cymbals\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01}"
      "]"
    "},"
    "\"FX\":{"
      "\"label\":\"FX\","
      "\"knobs\":[\"delay_mix\",\"delay_time\",\"delay_fb\",\"reverb_mix\",\"reverb_decay\",\"reverb_tone\",\"bus_comp\",\"all_decay\"],"
      "\"params\":["
        "{\"key\":\"delay_mix\",\"label\":\"Delay Mix\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
        "{\"key\":\"delay_time\",\"label\":\"Delay Time\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
        "{\"key\":\"delay_fb\",\"label\":\"Delay Fdbk\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
        "{\"key\":\"reverb_mix\",\"label\":\"Reverb Mix\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
        "{\"key\":\"reverb_decay\",\"label\":\"Reverb Dcy\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
        "{\"key\":\"reverb_tone\",\"label\":\"Reverb Tone\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
        "{\"key\":\"bus_comp\",\"label\":\"Bus Comp\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
        "{\"key\":\"all_decay\",\"label\":\"All Decay\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
        "{\"key\":\"delay_sync\",\"label\":\"Delay Sync\",\"type\":\"enum\",\"options\":[\"Free\",\"Synced\"]},"
        "{\"key\":\"delay_type\",\"label\":\"Delay Type\",\"type\":\"enum\",\"options\":[\"Tape\",\"Magnetic\",\"Space\"]},"
        "{\"key\":\"reverb_type\",\"label\":\"Reverb Mode\",\"type\":\"enum\",\"options\":[\"Plate\",\"Spring\",\"Chamber\"]}"
      "]"
    "},"
    "\"Attitude\":{"
      "\"label\":\"Attitude\","
      "\"knobs\":[\"preamp_drive\",\"tape_amount\",\"hpf_freq\",\"eq_body\",\"eq_air\",\"filter_cutoff\",\"filter_reso\",\"phaser_amount\"],"
      "\"params\":["
        "{\"key\":\"preamp_drive\",\"label\":\"Drive\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
        "{\"key\":\"tape_amount\",\"label\":\"Tape\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
        "{\"key\":\"hpf_freq\",\"label\":\"HPF\",\"type\":\"float\",\"min\":30,\"max\":500,\"step\":1},"
        "{\"key\":\"eq_body\",\"label\":\"Body\",\"type\":\"float\",\"min\":-6,\"max\":6,\"step\":0.1},"
        "{\"key\":\"eq_air\",\"label\":\"Air\",\"type\":\"float\",\"min\":-6,\"max\":6,\"step\":0.1},"
        "{\"key\":\"filter_cutoff\",\"label\":\"Cutoff\",\"type\":\"float\",\"min\":100,\"max\":15000,\"step\":50},"
        "{\"key\":\"filter_reso\",\"label\":\"Reso\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
        "{\"key\":\"phaser_amount\",\"label\":\"Phaser\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01}"
      "]"
    "},"
    "\"General\":{"
      "\"label\":\"General\","
      "\"knobs\":[\"tempo\",\"tempo_mode\",\"drift\",\"limiter\",\"master_vol\",\"density\"],"
      "\"params\":["
        "{\"key\":\"tempo\",\"label\":\"Tempo\",\"type\":\"float\",\"min\":60,\"max\":200,\"step\":1},"
        "{\"key\":\"tempo_mode\",\"label\":\"Tempo Mode\",\"type\":\"enum\",\"options\":[\"Free\",\"Sync\"]},"
        "{\"key\":\"drift\",\"label\":\"Drift\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
        "{\"key\":\"limiter\",\"label\":\"Limiter\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"]},"
        "{\"key\":\"master_vol\",\"label\":\"M.Volume\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
        "{\"key\":\"density\",\"label\":\"Density\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01}"
      "]"
    "},"
    /* === Rhythms page — 8 enum knobs, each picks an active rhythm pattern */
    "\"Rhythms\":{"
      "\"label\":\"Rhythms\","
      "\"knobs\":[\"rhythm_1\",\"rhythm_2\",\"rhythm_3\",\"rhythm_4\",\"rhythm_5\",\"rhythm_6\",\"rhythm_7\",\"rhythm_8\"],"
      "\"params\":["
        "{\"key\":\"rhythm_1\",\"label\":\"Rhythm 1\",\"type\":\"enum\",\"options\":[\"Off\",\"Waltz\",\"Tango\",\"Polka\",\"Paso Doble\",\"Swing\",\"Slow\",\"Beguine\",\"Cha-Cha\",\"Rumba\",\"Samba\",\"Mambo\",\"Bossa-Nova\",\"Rock\",\"Slow Rock\",\"Shake\",\"R & Blues\"]},"
        "{\"key\":\"rhythm_2\",\"label\":\"Rhythm 2\",\"type\":\"enum\",\"options\":[\"Off\",\"Waltz\",\"Tango\",\"Polka\",\"Paso Doble\",\"Swing\",\"Slow\",\"Beguine\",\"Cha-Cha\",\"Rumba\",\"Samba\",\"Mambo\",\"Bossa-Nova\",\"Rock\",\"Slow Rock\",\"Shake\",\"R & Blues\"]},"
        "{\"key\":\"rhythm_3\",\"label\":\"Rhythm 3\",\"type\":\"enum\",\"options\":[\"Off\",\"Waltz\",\"Tango\",\"Polka\",\"Paso Doble\",\"Swing\",\"Slow\",\"Beguine\",\"Cha-Cha\",\"Rumba\",\"Samba\",\"Mambo\",\"Bossa-Nova\",\"Rock\",\"Slow Rock\",\"Shake\",\"R & Blues\"]},"
        "{\"key\":\"rhythm_4\",\"label\":\"Rhythm 4\",\"type\":\"enum\",\"options\":[\"Off\",\"Waltz\",\"Tango\",\"Polka\",\"Paso Doble\",\"Swing\",\"Slow\",\"Beguine\",\"Cha-Cha\",\"Rumba\",\"Samba\",\"Mambo\",\"Bossa-Nova\",\"Rock\",\"Slow Rock\",\"Shake\",\"R & Blues\"]},"
        "{\"key\":\"rhythm_5\",\"label\":\"Rhythm 5\",\"type\":\"enum\",\"options\":[\"Off\",\"Waltz\",\"Tango\",\"Polka\",\"Paso Doble\",\"Swing\",\"Slow\",\"Beguine\",\"Cha-Cha\",\"Rumba\",\"Samba\",\"Mambo\",\"Bossa-Nova\",\"Rock\",\"Slow Rock\",\"Shake\",\"R & Blues\"]},"
        "{\"key\":\"rhythm_6\",\"label\":\"Rhythm 6\",\"type\":\"enum\",\"options\":[\"Off\",\"Waltz\",\"Tango\",\"Polka\",\"Paso Doble\",\"Swing\",\"Slow\",\"Beguine\",\"Cha-Cha\",\"Rumba\",\"Samba\",\"Mambo\",\"Bossa-Nova\",\"Rock\",\"Slow Rock\",\"Shake\",\"R & Blues\"]},"
        "{\"key\":\"rhythm_7\",\"label\":\"Rhythm 7\",\"type\":\"enum\",\"options\":[\"Off\",\"Waltz\",\"Tango\",\"Polka\",\"Paso Doble\",\"Swing\",\"Slow\",\"Beguine\",\"Cha-Cha\",\"Rumba\",\"Samba\",\"Mambo\",\"Bossa-Nova\",\"Rock\",\"Slow Rock\",\"Shake\",\"R & Blues\"]},"
        "{\"key\":\"rhythm_8\",\"label\":\"Rhythm 8\",\"type\":\"enum\",\"options\":[\"Off\",\"Waltz\",\"Tango\",\"Polka\",\"Paso Doble\",\"Swing\",\"Slow\",\"Beguine\",\"Cha-Cha\",\"Rumba\",\"Samba\",\"Mambo\",\"Bossa-Nova\",\"Rock\",\"Slow Rock\",\"Shake\",\"R & Blues\"]}"
      "]"
    "}"
  "}"
"}";

static int get_param(void *instance, const char *key, char *buf, int buf_len) {
    kd_inst_t *inst = (kd_inst_t *)instance;
    if (!key || !buf || buf_len <= 0) return -1;

    /* Large JSON returns via memcpy */
    if (strcmp(key, "chain_params") == 0) {
        int n = (int)sizeof(CHAIN_PARAMS_JSON) - 1;
        if (n > buf_len - 1) n = buf_len - 1;
        memcpy(buf, CHAIN_PARAMS_JSON, n);
        buf[n] = '\0';
        return n;
    }
    if (strcmp(key, "ui_hierarchy") == 0) {
        int n = (int)sizeof(UI_HIERARCHY_JSON) - 1;
        if (n > buf_len - 1) n = buf_len - 1;
        memcpy(buf, UI_HIERARCHY_JSON, n);
        buf[n] = '\0';
        return n;
    }

    /* Knob overlay: knob_N_name / knob_N_value (page-aware) */
    if (strncmp(key, "knob_", 5) == 0) {
        int idx = atoi(key + 5) - 1;  /* 1-indexed */
        if (idx < 0 || idx > 7) return 0;
        int p = inst->current_page;
        if (p < 0 || p > 4) p = 0;

        const char *target = strchr(key, '_');
        if (!target) return 0;
        target = strchr(target + 1, '_');
        if (!target) return 0;
        target++;  /* point past the second underscore */

        if (strcmp(target, "name") == 0) {
            /* Empty slot (knob not mapped on this page) → no label */
            if (KNOB_MAP_PAGE[p][idx].key[0] == '\0') return snprintf(buf, buf_len, "—");
            return snprintf(buf, buf_len, "%s", KNOB_MAP_PAGE[p][idx].label);
        }
        if (strcmp(target, "value") == 0) {
            const char *pkey = KNOB_MAP_PAGE[p][idx].key;
            if (pkey[0] == '\0') return snprintf(buf, buf_len, "—");
            /* Rhythm slots show the selected pattern name (or "Off") */
            if (strncmp(pkey, "rhythm_", 7) == 0) {
                int slot = atoi(pkey + 7) - 1;
                if (slot < 0 || slot >= NUM_RHYTHM_SLOTS) return snprintf(buf, buf_len, "Off");
                int r = inst->rhythm_slots[slot];
                int rid = (r < 0) ? 0 : (r + 1);
                if (rid >= NUM_RHYTHM_OPTS) rid = 0;
                return snprintf(buf, buf_len, "%s", RHYTHM_OPTS[rid]);
            }
            /* Get the value display based on the parameter type */
            if (strcmp(pkey, "tempo") == 0)
                return snprintf(buf, buf_len, "%.0f BPM", inst->tempo_bpm);
            if (strcmp(pkey, "tempo_mode") == 0)
                return snprintf(buf, buf_len, "%s", inst->sync_to_host ? "Sync" : "Free");
            if (strcmp(pkey, "limiter") == 0)
                return snprintf(buf, buf_len, "%s", inst->limiter_on ? "On" : "Off");
            if (strcmp(pkey, "delay_type") == 0)
                return snprintf(buf, buf_len, "%s", DELAY_TYPE_OPTS[inst->delay_type]);
            if (strcmp(pkey, "reverb_type") == 0)
                return snprintf(buf, buf_len, "%s", REVERB_TYPE_OPTS[inst->reverb_type]);
            if (strcmp(pkey, "delay_time") == 0) {
                if (inst->delay_sync) {
                    /* Synced mode: knob value is a tempo-division index */
                    int idx = knob_to_div_idx(inst->delay_time_norm);
                    return snprintf(buf, buf_len, "%s", DIV_NAMES[idx]);
                } else {
                    float ms = 0.010f * powf(100.0f, inst->delay_time_norm) * 1000.0f;
                    return snprintf(buf, buf_len, "%.0f ms", ms);
                }
            }
            if (strcmp(pkey, "all_decay") == 0)
                return snprintf(buf, buf_len, "%.2fx", 1.0f + inst->all_decay * 3.0f);
            /* Attitude — natural units */
            if (strcmp(pkey, "hpf_freq") == 0)
                return snprintf(buf, buf_len, "%.0f Hz", inst->hpf_freq);
            if (strcmp(pkey, "eq_body") == 0)
                return snprintf(buf, buf_len, "%+.1f dB", inst->eq_body);
            if (strcmp(pkey, "eq_air") == 0)
                return snprintf(buf, buf_len, "%+.1f dB", inst->eq_air);
            if (strcmp(pkey, "filter_cutoff") == 0) {
                if (inst->filter_cutoff >= 1000.0f)
                    return snprintf(buf, buf_len, "%.1f kHz", inst->filter_cutoff * 0.001f);
                return snprintf(buf, buf_len, "%.0f Hz", inst->filter_cutoff);
            }
            /* Default percent display */
            float v = 0.0f;
            if      (strcmp(pkey, "lvl_bass")     == 0) v = inst->level[VOICE_GROUP_BASS];
            else if (strcmp(pkey, "lvl_conga")    == 0) v = inst->level[VOICE_GROUP_CONGA];
            else if (strcmp(pkey, "lvl_tom1")     == 0) v = inst->level[VOICE_GROUP_TOM1];
            else if (strcmp(pkey, "lvl_tom2")     == 0) v = inst->level[VOICE_GROUP_TOM2];
            else if (strcmp(pkey, "lvl_claves")   == 0) v = inst->level[VOICE_GROUP_CLAVES];
            else if (strcmp(pkey, "lvl_snare")    == 0) v = inst->level[VOICE_GROUP_SNARE];
            else if (strcmp(pkey, "lvl_cowbell")  == 0) v = inst->level[VOICE_GROUP_COWBELL];
            else if (strcmp(pkey, "lvl_cymbals")  == 0) v = inst->level[VOICE_GROUP_CYMBALS];
            else if (strcmp(pkey, "delay_mix")    == 0) v = inst->delay_mix;
            else if (strcmp(pkey, "delay_fb")     == 0) v = inst->delay_fb;
            else if (strcmp(pkey, "reverb_mix")   == 0) v = inst->reverb_mix;
            else if (strcmp(pkey, "reverb_decay") == 0) v = inst->reverb_decay;
            else if (strcmp(pkey, "reverb_tone")  == 0) v = inst->reverb_tone;
            else if (strcmp(pkey, "bus_comp")     == 0) v = inst->bus_comp;
            else if (strcmp(pkey, "preamp_drive") == 0) v = inst->preamp_drive;
            else if (strcmp(pkey, "tape_amount")  == 0) v = inst->tape_amount;
            else if (strcmp(pkey, "filter_reso")  == 0) v = inst->filter_reso;
            else if (strcmp(pkey, "phaser_amount")== 0) v = inst->phaser_amount;
            else if (strcmp(pkey, "drift")        == 0) v = inst->drift_amount;
            else if (strcmp(pkey, "master_vol")   == 0) v = inst->master_vol;
            else if (strcmp(pkey, "density")      == 0) v = inst->density;
            return snprintf(buf, buf_len, "%d%%", (int)(v * 100.0f));
        }
    }

    /* Direct param reads (for state serialization compatibility) */
    if (strcmp(key, "lvl_bass")    == 0) return snprintf(buf, buf_len, "%.4f", inst->level[VOICE_GROUP_BASS]);
    if (strcmp(key, "lvl_conga")   == 0) return snprintf(buf, buf_len, "%.4f", inst->level[VOICE_GROUP_CONGA]);
    if (strcmp(key, "lvl_tom1")    == 0) return snprintf(buf, buf_len, "%.4f", inst->level[VOICE_GROUP_TOM1]);
    if (strcmp(key, "lvl_tom2")    == 0) return snprintf(buf, buf_len, "%.4f", inst->level[VOICE_GROUP_TOM2]);
    if (strcmp(key, "lvl_claves")  == 0) return snprintf(buf, buf_len, "%.4f", inst->level[VOICE_GROUP_CLAVES]);
    if (strcmp(key, "lvl_snare")   == 0) return snprintf(buf, buf_len, "%.4f", inst->level[VOICE_GROUP_SNARE]);
    if (strcmp(key, "lvl_cowbell") == 0) return snprintf(buf, buf_len, "%.4f", inst->level[VOICE_GROUP_COWBELL]);
    if (strcmp(key, "lvl_cymbals") == 0) return snprintf(buf, buf_len, "%.4f", inst->level[VOICE_GROUP_CYMBALS]);

    if (strcmp(key, "delay_mix")    == 0) return snprintf(buf, buf_len, "%.4f", inst->delay_mix);
    if (strcmp(key, "delay_time")   == 0) return snprintf(buf, buf_len, "%.4f", inst->delay_time_norm);
    if (strcmp(key, "delay_fb")     == 0) return snprintf(buf, buf_len, "%.4f", inst->delay_fb);
    if (strcmp(key, "reverb_mix")   == 0) return snprintf(buf, buf_len, "%.4f", inst->reverb_mix);
    if (strcmp(key, "reverb_decay") == 0) return snprintf(buf, buf_len, "%.4f", inst->reverb_decay);
    if (strcmp(key, "reverb_tone")  == 0) return snprintf(buf, buf_len, "%.4f", inst->reverb_tone);
    if (strcmp(key, "bus_comp")     == 0) return snprintf(buf, buf_len, "%.4f", inst->bus_comp);
    if (strcmp(key, "all_decay")    == 0) return snprintf(buf, buf_len, "%.4f", inst->all_decay);

    /* Attitude */
    if (strcmp(key, "preamp_drive") == 0) return snprintf(buf, buf_len, "%.4f", inst->preamp_drive);
    if (strcmp(key, "tape_amount")  == 0) return snprintf(buf, buf_len, "%.4f", inst->tape_amount);
    if (strcmp(key, "hpf_freq")     == 0) return snprintf(buf, buf_len, "%.2f", inst->hpf_freq);
    if (strcmp(key, "eq_body")      == 0) return snprintf(buf, buf_len, "%.2f", inst->eq_body);
    if (strcmp(key, "eq_air")       == 0) return snprintf(buf, buf_len, "%.2f", inst->eq_air);
    if (strcmp(key, "filter_cutoff")== 0) return snprintf(buf, buf_len, "%.2f", inst->filter_cutoff);
    if (strcmp(key, "filter_reso")  == 0) return snprintf(buf, buf_len, "%.4f", inst->filter_reso);
    if (strcmp(key, "phaser_amount")== 0) return snprintf(buf, buf_len, "%.4f", inst->phaser_amount);

    if (strcmp(key, "tempo")        == 0) return snprintf(buf, buf_len, "%.1f", inst->tempo_bpm);
    if (strcmp(key, "tempo_mode")   == 0) return snprintf(buf, buf_len, "%s", inst->sync_to_host ? "Sync" : "Free");
    if (strcmp(key, "drift")        == 0) return snprintf(buf, buf_len, "%.4f", inst->drift_amount);
    if (strcmp(key, "limiter")      == 0) return snprintf(buf, buf_len, "%s", inst->limiter_on ? "On" : "Off");
    if (strcmp(key, "delay_type")   == 0) return snprintf(buf, buf_len, "%s", DELAY_TYPE_OPTS[inst->delay_type]);
    if (strcmp(key, "reverb_type")  == 0) return snprintf(buf, buf_len, "%s", REVERB_TYPE_OPTS[inst->reverb_type]);
    if (strcmp(key, "master_vol")   == 0) return snprintf(buf, buf_len, "%.4f", inst->master_vol);
    if (strcmp(key, "density")      == 0) return snprintf(buf, buf_len, "%.4f", inst->density);
    if (strcmp(key, "delay_sync")   == 0) return snprintf(buf, buf_len, "%s", DELAY_SYNC_OPTS[inst->delay_sync]);
    if (strncmp(key, "rhythm_", 7) == 0) {
        int slot = atoi(key + 7) - 1;
        if (slot < 0 || slot >= NUM_RHYTHM_SLOTS) return -1;
        int r = inst->rhythm_slots[slot];
        int idx = (r < 0) ? 0 : (r + 1);
        if (idx >= NUM_RHYTHM_OPTS) idx = 0;
        return snprintf(buf, buf_len, "%s", RHYTHM_OPTS[idx]);
    }

    /* State (serialize) — 41 fields: 32 original + delay_sync + 8 rhythm slots */
    if (strcmp(key, "state") == 0) {
        return snprintf(buf, buf_len,
            "%f %f %f %f %f %f %f %f "    /* 8 levels */
            "%f %f %f %f %f %f %f %f "    /* 8 fx */
            "%f %f %f %f %f %f %f %f "    /* 8 attitude */
            "%f %d %f %d %d %d %f %f "    /* 8 general */
            "%d "                          /* delay_sync */
            "%d %d %d %d %d %d %d %d",    /* 8 rhythm slots */
            inst->level[0], inst->level[1], inst->level[2], inst->level[3],
            inst->level[4], inst->level[5], inst->level[6], inst->level[7],
            inst->delay_mix, inst->delay_time_norm, inst->delay_fb,
            inst->reverb_mix, inst->reverb_decay, inst->reverb_tone,
            inst->bus_comp, inst->all_decay,
            inst->preamp_drive, inst->tape_amount, inst->hpf_freq,
            inst->eq_body, inst->eq_air,
            inst->filter_cutoff, inst->filter_reso, inst->phaser_amount,
            inst->tempo_bpm, inst->sync_to_host, inst->drift_amount,
            inst->limiter_on, inst->delay_type, inst->reverb_type,
            inst->master_vol, inst->density,
            inst->delay_sync,
            inst->rhythm_slots[0], inst->rhythm_slots[1], inst->rhythm_slots[2],
            inst->rhythm_slots[3], inst->rhythm_slots[4], inst->rhythm_slots[5],
            inst->rhythm_slots[6], inst->rhythm_slots[7]);
    }

    /* Runtime module name — used by Shadow UI for the knob-popup prefix
     * ("KD: Bass" rather than "KrautDrums: Bass"). The picker/catalog name
     * stays "KrautDrums" via module.json's static `name` field.                */
    if (strcmp(key, "name") == 0) return snprintf(buf, buf_len, "KD");

    /* CRITICAL: -1 for unknown keys, NOT 0 — see /move-synth */
    return -1;
}

/* ──────────────────────────────────────────────────────────────────────────────
 * Audio rendering
 * ────────────────────────────────────────────────────────────────────────────── */

static void render_block(void *instance, int16_t *out_lr, int frames) {
    kd_inst_t *inst = (kd_inst_t *)instance;

    /* === 10 ms knob smoothing (per-block one-pole filter) ===
     * SMOOTH_COEF = 1 - exp(-T_block / τ) with T_block = 128/44100 = 2.9 ms
     * and τ = 10 ms → coefficient ≈ 0.252. Settles to within ~5% in ~10 ms
     * (≈4 blocks). All audio-rate computations below read the *_s fields
     * so knob movements never click.                                          */
    const float SMOOTH = 0.252f;
    for (int i = 0; i < NUM_GROUPS; i++)
        inst->level_s[i]      += SMOOTH * (inst->level[i]         - inst->level_s[i]);
    inst->delay_mix_s     += SMOOTH * (inst->delay_mix         - inst->delay_mix_s);
    inst->delay_time_s    += SMOOTH * (inst->delay_time_norm   - inst->delay_time_s);
    inst->delay_fb_s      += SMOOTH * (inst->delay_fb          - inst->delay_fb_s);
    inst->reverb_mix_s    += SMOOTH * (inst->reverb_mix        - inst->reverb_mix_s);
    inst->reverb_decay_s  += SMOOTH * (inst->reverb_decay      - inst->reverb_decay_s);
    inst->reverb_tone_s   += SMOOTH * (inst->reverb_tone       - inst->reverb_tone_s);
    inst->bus_comp_s      += SMOOTH * (inst->bus_comp          - inst->bus_comp_s);
    inst->all_decay_s     += SMOOTH * (inst->all_decay         - inst->all_decay_s);
    inst->preamp_drive_s  += SMOOTH * (inst->preamp_drive      - inst->preamp_drive_s);
    inst->tape_amount_s   += SMOOTH * (inst->tape_amount       - inst->tape_amount_s);
    inst->hpf_freq_s      += SMOOTH * (inst->hpf_freq          - inst->hpf_freq_s);
    inst->eq_body_s       += SMOOTH * (inst->eq_body           - inst->eq_body_s);
    inst->eq_air_s        += SMOOTH * (inst->eq_air            - inst->eq_air_s);
    inst->filter_cutoff_s += SMOOTH * (inst->filter_cutoff     - inst->filter_cutoff_s);
    inst->filter_reso_s   += SMOOTH * (inst->filter_reso       - inst->filter_reso_s);
    inst->phaser_amount_s += SMOOTH * (inst->phaser_amount     - inst->phaser_amount_s);
    inst->master_vol_s    += SMOOTH * (inst->master_vol        - inst->master_vol_s);
    inst->density_s       += SMOOTH * (inst->density           - inst->density_s);

    /* Pre-compute group level lookup for voice mixing — uses smoothed values */
    float voice_level[NUM_VOICES];
    for (int i = 0; i < NUM_VOICES; i++) {
        voice_level[i] = inst->level_s[VOICES[i].group];
    }

    /* Pre-compute delay parameters. When delay_sync == 1, the Delay Time knob
     * is interpreted as a tempo-division index instead of a continuous ms
     * value. Tempo source: host MIDI clock if sync_to_host, else inst->tempo_bpm. */
    int delay_samples;
    if (inst->delay_sync) {
        int div_idx = knob_to_div_idx(inst->delay_time_s);
        float ms    = 60000.0f / (inst->tempo_bpm * DIV_NOTES_PER_BEAT[div_idx]);
        delay_samples = (int)(ms * 0.001f * SAMPLE_RATE);
        if (delay_samples < 16) delay_samples = 16;
        if (delay_samples >= DELAY_BUF_SIZE / 2) delay_samples = DELAY_BUF_SIZE / 2;
    } else {
        delay_samples = delay_samples_from_norm(inst->delay_time_s);
    }
    int   delay_type_idx = inst->delay_type;
    if (delay_type_idx < 0) delay_type_idx = 0;
    if (delay_type_idx > 2) delay_type_idx = 2;
    const delay_voicing_t *voicing = &DELAY_VOICINGS[delay_type_idx];

    /* Wow & flutter LFO advance per block (cheap: no per-sample sinf calls).
     *   Wow ≈ 0.7 Hz (slow drift, e.g., worn pinch roller / rotating drum eccentricity)
     *   Flutter ≈ 6.3 Hz (fast — capstan slip, motor ripple)
     * The phases advance by (rate × frames / Fs) per block; we sample sin once
     * per block and let the per-sample modulation be approximately linear over
     * 128 frames (= 2.9 ms, well below the LFO period).                         */
    const float wow_rate_hz     = 0.7f;
    const float flutter_rate_hz = 6.3f;
    inst->delay.wow_phase     += wow_rate_hz     * (float)frames / SAMPLE_RATE;
    inst->delay.flutter_phase += flutter_rate_hz * (float)frames / SAMPLE_RATE;
    if (inst->delay.wow_phase     >= 1.0f) inst->delay.wow_phase     -= 1.0f;
    if (inst->delay.flutter_phase >= 1.0f) inst->delay.flutter_phase -= 1.0f;
    /* Per-voicing wow LFO shape — sine for tape units, dual-sine "drum
     * eccentricity" for the Echorec (its rotating-drum geometry produces a
     * non-sinusoidal wobble: dominant cycle plus a small 2nd-harmonic offset
     * from imperfect head spacing). The 0.3 amplitude on the second sine and
     * 0.3 rad phase offset give the recognisable "almost steady" Echorec feel.  */
    float wow_phi  = inst->delay.wow_phase * 2.0f * M_PI;
    float wow_mod;
    if (voicing->wow_shape == 1) {
        wow_mod = sinf(wow_phi) + 0.3f * sinf(2.0f * wow_phi + 0.3f);
    } else {
        wow_mod = sinf(wow_phi);
    }
    float flutter_mod = sinf(inst->delay.flutter_phase * 2.0f * M_PI);

    /* Per-block: recompute the wet head-bump biquad for the active voicing.
     * EP-3 → 9.5 kHz @ +4 dB, Echorec → 4 kHz @ +1.5 dB, RE-201 → 6 kHz @ +0.5 dB.
     * Computing once per block keeps cosf/sinf out of the per-sample loop.       */
    biquad_set_peaking(&inst->delay.bump_l, voicing->bump_freq, voicing->bump_q,
                       voicing->bump_gain_db, SAMPLE_RATE);
    biquad_set_peaking(&inst->delay.bump_r, voicing->bump_freq, voicing->bump_q,
                       voicing->bump_gain_db, SAMPLE_RATE);

    /* Slow continuous drift baseline — per-block update.
     * Denis-style slewed random walk: pick a new random target occasionally,
     * slew the current value toward it. Operates in normalized [-1, +1] units;
     * voice_retune scales by ±3 cents. The slew coefficient gives ~10-second
     * time constant. Re-targets on average every ~8 seconds.
     * 44100/128 ≈ 344 blocks/sec; p = 1/(8 × 344) ≈ 0.00036. */
    if (rand_unipolar() < 0.00036f) {
        inst->drift_slow_target = rand_gaussian() * 0.7f;  /* keep mostly in [-1,+1] */
    }
    inst->drift_slow_value += 0.003f * (inst->drift_slow_target - inst->drift_slow_value);

    /* === Attitude — per-block coefficient setup (reads smoothed knobs) ===
     * Recompute filter coefs once per block (~2.9 ms), avoids per-sample cosf/tanf.
     * Smoothing in the *_s values prevents knob clicks from propagating to coefs. */
    biquad_set_hpf      (&inst->attitude.hpf_l,  inst->hpf_freq_s,  0.707f,           SAMPLE_RATE);
    biquad_set_hpf      (&inst->attitude.hpf_r,  inst->hpf_freq_s,  0.707f,           SAMPLE_RATE);
    biquad_set_lowshelf (&inst->attitude.body_l, 80.0f,             inst->eq_body_s,  SAMPLE_RATE);
    biquad_set_lowshelf (&inst->attitude.body_r, 80.0f,             inst->eq_body_s,  SAMPLE_RATE);
    biquad_set_highshelf(&inst->attitude.air_l,  6000.0f,           inst->eq_air_s,   SAMPLE_RATE);
    biquad_set_highshelf(&inst->attitude.air_r,  6000.0f,           inst->eq_air_s,   SAMPLE_RATE);

    /* A80 tape head-bump @ ~120 Hz, Q=1.5, gain scales with tape_amount (0..+4 dB) */
    float tape_bump_db = inst->tape_amount_s * 4.0f;
    biquad_set_peaking(&inst->attitude.tape_bump_l, 120.0f, 1.5f, tape_bump_db, SAMPLE_RATE);
    biquad_set_peaking(&inst->attitude.tape_bump_r, 120.0f, 1.5f, tape_bump_db, SAMPLE_RATE);

    /* Preamp + tape pre-gains (drive into saturation curves) */
    float preamp_pregain = 1.0f + inst->preamp_drive_s * 5.0f;   /* 1× to 6× (15 dB) */
    float tape_pregain   = 1.0f + inst->tape_amount_s  * 3.0f;   /* 1× to 4× (12 dB) */

    /* Tape post-rolloff one-pole LPF coef (cutoff sweeps 18 kHz → 5 kHz with tape) */
    float tape_cutoff_hz = 18000.0f - inst->tape_amount_s * 13000.0f;
    if (tape_cutoff_hz > 18000.0f) tape_cutoff_hz = 18000.0f;
    if (tape_cutoff_hz < 1000.0f)  tape_cutoff_hz = 1000.0f;
    float tape_lpf_coef = 1.0f - expf(-2.0f * M_PI * tape_cutoff_hz / SAMPLE_RATE);
    if (tape_lpf_coef > 1.0f) tape_lpf_coef = 1.0f;

    /* Synthi-style diode-ladder LPF — per-stage coefficient + reso feedback */
    float ladder_freq = inst->filter_cutoff_s;
    if (ladder_freq > SAMPLE_RATE * 0.45f) ladder_freq = SAMPLE_RATE * 0.45f;
    float ladder_g    = 1.0f - expf(-2.0f * M_PI * ladder_freq / SAMPLE_RATE);
    if (ladder_g > 0.99f) ladder_g = 0.99f;
    float ladder_k    = inst->filter_reso_s * 3.95f;

    /* Phaser amount → both rate AND depth/wet (logarithmic rate taper)        */
    float pa = inst->phaser_amount_s;
    float phaser_rate_hz = 0.05f * powf(12.0f, pa);   /* 0.05 → 0.60 Hz log */
    inst->attitude.lfo_phase += phaser_rate_hz * (float)frames / SAMPLE_RATE;
    if (inst->attitude.lfo_phase >= 1.0f) inst->attitude.lfo_phase -= 1.0f;
    float lfo_l = sinf(inst->attitude.lfo_phase * 2.0f * M_PI);
    float lfo_r = sinf((inst->attitude.lfo_phase + 0.25f) * 2.0f * M_PI);  /* 90° offset */
    /* Wider sweep range (50..1500 Hz) for deeper Krautrock-style movement.
     * Depth grows from 0 → 600 Hz across the knob.                             */
    float sweep_depth = pa * 600.0f;
    float ph_freq_l = 400.0f + lfo_l * sweep_depth;
    float ph_freq_r = 400.0f + lfo_r * sweep_depth;
    if (ph_freq_l < 50.0f) ph_freq_l = 50.0f;
    if (ph_freq_r < 50.0f) ph_freq_r = 50.0f;
    /* Convert to allpass coefficient: a = (1-g)/(1+g) where g = tan(pi*f/sr) */
    float gph_l = tanf(M_PI * ph_freq_l / SAMPLE_RATE);
    float gph_r = tanf(M_PI * ph_freq_r / SAMPLE_RATE);
    float phaser_coef_l = (1.0f - gph_l) / (1.0f + gph_l);
    float phaser_coef_r = (1.0f - gph_r) / (1.0f + gph_r);
    float phaser_wet = pa;

    /* Per-block voice update: diode-starve drift + AllDecay-scaled Q.
     *
     * AllDecay = "drone/wash" — extends the audible decay by raising the
     * resonator Q. Decay time τ = Q/(π·f₀), so Q×16 → audible ring × 16.
     * For a bass drum at f₀=72 Hz: τ goes from 35 ms (Q=8) to 567 ms
     * (Q=128) → 60 dB decay extends from ~244 ms to ~3.9 sec. Cymbals at
     * Q=560 ring ~350 ms instead of ~22 ms. The intentional side effect is
     * that very high Q narrows the BPF bandwidth → drum hits soften into
     * pitched "pings", exactly the *drone/wash texture* in CLAUDE.md.
     *
     * The decay_coef recompute (envelope rate) is also smoothed via
     * all_decay_s so the envelope auto-extends in lock-step with Q.            */
    float Q_mult = 1.0f + inst->all_decay_s * 15.0f;   /* 1× to 16× */
    for (int vi = 0; vi < NUM_VOICES; vi++) {
        voice_state_t *v = &inst->voices[vi];
        if (!v->env_active) continue;
        v->decay_coef = compute_decay_coef(VOICES[vi].decay_ms, inst->all_decay_s);
        float drift = 1.0f - VOICES[vi].drift_amt * (1.0f - v->env);
        biquad_set_bpf(&v->res_a, v->f0_a_trig * drift, VOICES[vi].Q_a * Q_mult, SAMPLE_RATE);
        if (v->f0_b_trig > 0.0f) {
            biquad_set_bpf(&v->res_b, v->f0_b_trig * drift, VOICES[vi].Q_b * Q_mult, SAMPLE_RATE);
        }
    }

    /* Pulse RC tail — kept short. Adding to all_decay here doesn't help the
     * BPF (the slow decaying pulse_value is mostly DC, which the BPF blocks);
     * the audible-decay extension comes from the Q scaling above.              */
    const float kPulseDecayCoef = 0.889f;   /* 1 - 1/9, ~0.2 ms RC tail */

    for (int i = 0; i < frames; i++) {
        /* === Sequencer clock === */
        if (!inst->sync_to_host) {
            inst->clock_phase += inst->clock_inc;
        }
        if (inst->clock_phase >= 1.0f) {
            inst->clock_phase -= 1.0f;
            inst->clock_step = (inst->clock_step + 1) % MAX_RHYTHM_STEPS;
            sequencer_step(inst, inst->clock_step);
        }

        /* === Voices ===
         * Each active voice runs a Plaits-style shaped trigger pulse:
         *   1. Hard pulse for ~1 ms, asymmetrically diode-clipped (more edge
         *      content than a clean impulse — what excites a real bridged-T
         *      circuit when its trigger transistor switches).
         *   2. RC-decay tail for ~5 ms after the hard pulse ends.
         *   3. A separate FM pulse running for ~6 ms that lifts the resonator
         *      centre frequency by `fm_amount × 1.7` of itself — the chirp
         *      that makes a kick punch and a tom thump.
         * Combined with the per-block diode-starve drift above, the resonator
         * gets a realistic excitation profile and pitch envelope.              */
        float mix = 0.0f;
        const float kPulseFilterCoef  = 1.0f / 5.0f;   /* ~0.1 ms LPF on pulse  */
        /* kPulseDecayCoef is now dynamic (depends on all_decay_s) — computed per-block above */
        const float kRetrigDecayCoef  = 1.0f - 1.0f / (float)TRIG_RETRIG_SAMPLES;

        for (int vi = 0; vi < NUM_VOICES; vi++) {
            voice_state_t *v = &inst->voices[vi];
            if (!v->env_active) continue;

            /* --- 1. Hard trigger pulse (1 ms) + RC tail --- */
            float pulse;
            if (v->pulse_remaining > 0) {
                v->pulse_remaining--;
                /* Pulse drops by 1.0 in the very last sample (Plaits' trick:
                 * the falling edge becomes part of the diode-shaped excitation). */
                pulse = (v->pulse_remaining > 0) ? v->pulse_height : v->pulse_height - 1.0f;
                v->pulse_value = pulse;
            } else {
                v->pulse_value *= kPulseDecayCoef;
                pulse = v->pulse_value;
            }
            /* One-pole LPF + diode shaping (Plaits Q39/Q40 emulation) */
            v->pulse_lp += kPulseFilterCoef * (pulse - v->pulse_lp) + DENORM_EPS;
            float exciter = voice_diode((pulse - v->pulse_lp) + pulse * 0.044f);

            /* --- 2. FM pulse (6 ms) → resonator centre-frequency lift --- */
            float fm_pulse_raw;
            if (v->fm_pulse_remaining > 0) {
                v->fm_pulse_remaining--;
                fm_pulse_raw = 1.0f;
                v->retrig_pulse = (v->fm_pulse_remaining > 0) ? 0.0f : -0.8f;
            } else {
                v->retrig_pulse *= kRetrigDecayCoef;
                fm_pulse_raw = 0.0f;
            }
            v->fm_pulse_lp += kPulseFilterCoef * (fm_pulse_raw - v->fm_pulse_lp) + DENORM_EPS;

            /* Apply attack-FM to the resonator: temporarily nudge biquad pole
             * via a parallel feedback path. Cheapest correct approximation:
             * scale the exciter by (1 + fm_lpf*1.7*fm_amount) — the pole's
             * natural response to a louder excitation IS a momentary upward
             * pitch shift in a self-oscillating resonator (Werner et al.,
             * "Bridged-T Self-Oscillator", 2014). Combined with the slight
             * negative retrig_pulse below, the chirp profile matches a real
             * bridged-T's trigger response.                                    */
            float fm = v->fm_pulse_lp * 1.7f * VOICES[vi].fm_amount;
            float in = exciter * (1.0f + fm) - v->retrig_pulse * 0.2f;

            float out;
            if (VOICES[vi].f0_b > 0.0f) {
                float a = biquad_tick(&v->res_a, in);
                float b = biquad_tick(&v->res_b, in);
                out = a * (1.0f - VOICES[vi].res_mix) + b * VOICES[vi].res_mix;
            } else {
                out = biquad_tick(&v->res_a, in);
            }

            /* Apply envelope */
            v->env *= v->decay_coef;
            if (v->env < 0.0001f) {
                v->env = 0.0f;
                v->env_active = 0;
                v->pulse_remaining = 0;
                v->fm_pulse_remaining = 0;
                biquad_reset(&v->res_a);
                biquad_reset(&v->res_b);
            }

            /* The shaped pulse already encodes velocity in pulse_height (3..10).
             * No extra scaling here — the cookbook BPF's intrinsic gain
             * (b0 ≈ sin(ω0)/(2Q)) and the resonant ring-up over the 1 ms pulse
             * give a peak response in the right ballpark. Master pre-fader
             * gain below normalizes across voices.                              */
            mix += out * v->env * voice_level[vi];
        }

        /* Pre-fader gain — empirical. Cookbook BPF response to a 1 ms /
         * pulse_height=10 excitation depends strongly on f0/Q (bass-drum at
         * Q=8/71Hz rings up only 7% of one period during the pulse → small
         * output, while cymbals at Q=30/4kHz get 4 cycles of ring-up).
         * ×4.0 brings single-voice transients to ~−10 dB peak; tutti (16
         * simultaneous voices) gets soft-clipped by the master limiter.        */
        mix *= 4.0f;

        /* === Stereo split — everything from here is stereo === */
        float att_l = mix;
        float att_r = mix;

        /* === Attitude — pre-FX studio chain (V72 + A80 + EQ + ladder + Phase 90) === */
        attitude_process(&inst->attitude, &att_l, &att_r,
                         preamp_pregain, tape_pregain, tape_lpf_coef,
                         ladder_g, ladder_k,
                         phaser_coef_l, phaser_coef_r, phaser_wet);

        /* === Bus Compressor (EMT 156 / Neumann broadcast limiter macro) === */
        compressor_process(&inst->bus_comp_state, &att_l, &att_r, inst->bus_comp_s);

        float dry_l = att_l;
        float dry_r = att_r;

        /* === Delay (insert: linear dry/wet crossfade) === */
        float dly_l = 0.0f, dly_r = 0.0f;
        delay_process(&inst->delay, dry_l, dry_r, &dly_l, &dly_r,
                      delay_samples, inst->delay_fb_s, voicing,
                      wow_mod, flutter_mod);
        float dly_dry = 1.0f - inst->delay_mix_s;
        float post_dly_l = dry_l * dly_dry + dly_l * inst->delay_mix_s;
        float post_dly_r = dry_r * dly_dry + dly_r * inst->delay_mix_s;

        /* === Reverb (insert: linear dry/wet crossfade) ===                    */
        float rev_in = (post_dly_l + post_dly_r) * 0.5f;
        float rev_l = 0.0f, rev_r = 0.0f;
        reverb_process(&inst->reverb, inst->reverb_type, rev_in,
                       inst->reverb_decay_s, inst->reverb_tone_s, &rev_l, &rev_r);
        float rev_dry = 1.0f - inst->reverb_mix_s;
        float l = post_dly_l * rev_dry + rev_l * inst->reverb_mix_s;
        float r = post_dly_r * rev_dry + rev_r * inst->reverb_mix_s;

        /* === Master volume === */
        l *= inst->master_vol_s;
        r *= inst->master_vol_s;

        /* === Limiter (gentle soft-clip) === */
        if (inst->limiter_on) {
            l = fast_tanh(l * 0.9f);
            r = fast_tanh(r * 0.9f);
        }

        /* Final clamp + int16 output */
        l = clampf(l, -1.0f, 1.0f);
        r = clampf(r, -1.0f, 1.0f);
        out_lr[i * 2]     = (int16_t)(l * 32767.0f);
        out_lr[i * 2 + 1] = (int16_t)(r * 32767.0f);
    }
}

/* ──────────────────────────────────────────────────────────────────────────────
 * Plugin API export
 * ────────────────────────────────────────────────────────────────────────────── */

typedef struct {
    uint32_t api_version;
    void* (*create_instance)(const char *, const char *);
    void  (*destroy_instance)(void *);
    void  (*on_midi)(void *, const uint8_t *, int, int);
    void  (*set_param)(void *, const char *, const char *);
    int   (*get_param)(void *, const char *, char *, int);
    int   (*get_error)(void *, char *, int);
    void  (*render_block)(void *, int16_t *, int);
} plugin_api_v2_t;

__attribute__((visibility("default")))
plugin_api_v2_t *move_plugin_init_v2(const void *host) {
    (void)host;
    static plugin_api_v2_t api = {
        .api_version      = 2,
        .create_instance  = create_instance,
        .destroy_instance = destroy_instance,
        .on_midi          = on_midi,
        .set_param        = set_param,
        .get_param        = get_param,
        .get_error        = NULL,         /* MUST exist as NULL — 8-field struct */
        .render_block     = render_block,
    };
    return &api;
}
