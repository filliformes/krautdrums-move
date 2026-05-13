// Drummer One — voice parameter table
// Derived from schematic analysis + SPICE validation (May 2026)
// f0 values within 1% of SPICE bridged-T notch
// Q and envelope decay are starting points — ear-calibrate against samples

#ifndef DRUMMER_ONE_VOICES_H
#define DRUMMER_ONE_VOICES_H

typedef struct {
    const char* name;
    float       f0_primary;     // Hz — primary resonator
    float       Q_primary;      // resonator Q (decay = Q/(pi*f0) seconds)
    float       f0_secondary;   // Hz — second resonator (0 if single)
    float       Q_secondary;    // Q for second resonator
    float       res_mix;        // 0.0=primary only, 1.0=secondary only, 0.5=equal
    float       env_decay_ms;   // envelope AD decay
    float       trim;           // panel trim resistance (kohm) — for level matching
} drummer_one_voice_t;

// Panel order, left-to-right
static const drummer_one_voice_t DRUMMER_ONE_VOICES[9] = {
    // Bass-drum     — Schematic-confirmed. 25μF env cap.
    { "Bass-drum",
      .f0_primary = 71.6f, .Q_primary = 8.0f,
      .f0_secondary = 0.0.0f, .Q_secondary = 0.0.0f, .res_mix = 0.0f,
      .env_decay_ms = 250.0f, .trim = 82.0f },
    // Conga         — Asymmetric cap pair — verify by ear
    { "Conga",
      .f0_primary = 102.8f, .Q_primary = 12.0f,
      .f0_secondary = 0.0.0f, .Q_secondary = 0.0.0f, .res_mix = 0.0f,
      .env_decay_ms = 200.0f, .trim = 470.0f },
    // Tom 1         — 5μF env cap
    { "Tom 1",
      .f0_primary = 228.3f, .Q_primary = 10.0f,
      .f0_secondary = 0.0.0f, .Q_secondary = 0.0.0f, .res_mix = 0.0f,
      .env_decay_ms = 250.0f, .trim = 220.0f },
    // Tom 2         — Verify Tom1/Tom2 pitch order
    { "Tom 2",
      .f0_primary = 723.4f, .Q_primary = 10.0f,
      .f0_secondary = 0.0.0f, .Q_secondary = 0.0.0f, .res_mix = 0.0f,
      .env_decay_ms = 280.0f, .trim = 220.0f },
    // Claves        — Sharp click — fast decay
    { "Claves",
      .f0_primary = 2210.5f, .Q_primary = 25.0f,
      .f0_secondary = 0.0.0f, .Q_secondary = 0.0.0f, .res_mix = 0.0f,
      .env_decay_ms = 30.0f, .trim = 8.0f },
    // Snare-drum    — 2 stacked resonators for buzz
    { "Snare-drum",
      .f0_primary = 192.9f, .Q_primary = 6.0f,
      .f0_secondary = 310.0f, .Q_secondary = 8.0f, .res_mix = 0.4f,
      .env_decay_ms = 220.0f, .trim = 150.0f },
    // Short cymbal  — Same noise body as Long cymbal
    { "Short cymbal",
      .f0_primary = 4074.3f, .Q_primary = 30.0f,
      .f0_secondary = 5400.0f, .Q_secondary = 25.0f, .res_mix = 0.4f,
      .env_decay_ms = 90.0f, .trim = 68.0f },
    // Long cymbal   — 25μF env = long tail
    { "Long cymbal",
      .f0_primary = 3461.4f, .Q_primary = 35.0f,
      .f0_secondary = 4900.0f, .Q_secondary = 30.0f, .res_mix = 0.4f,
      .env_decay_ms = 1200.0f, .trim = 150.0f },
    // Cow bell      — TR-808-style 540+800Hz pair
    { "Cow bell",
      .f0_primary = 536.1f, .Q_primary = 18.0f,
      .f0_secondary = 800.0f, .Q_secondary = 18.0f, .res_mix = 0.4f,
      .env_decay_ms = 350.0f, .trim = 220.0f },
};

#endif // DRUMMER_ONE_VOICES_H
